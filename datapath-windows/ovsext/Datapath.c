/*
 * Copyright (c) 2014 VMware, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * XXX: OVS_USE_NL_INTERFACE is being used to keep the legacy DPIF interface
 * alive while we transition over to the netlink based interface.
 * OVS_USE_NL_INTERFACE = 0 => legacy inteface to use with dpif-windows.c
 * OVS_USE_NL_INTERFACE = 1 => netlink inteface to use with ported dpif-linux.c
 */

#include "precomp.h"
#include "Switch.h"
#include "User.h"
#include "Datapath.h"
#include "Jhash.h"
#include "Switch.h"
#include "Vport.h"
#include "Event.h"
#include "User.h"
#include "PacketIO.h"
#include "NetProto.h"
#include "Flow.h"
#include "User.h"
#include "Vxlan.h"

#ifdef OVS_DBG_MOD
#undef OVS_DBG_MOD
#endif
#define OVS_DBG_MOD OVS_DBG_DATAPATH
#include "Debug.h"

#define NETLINK_FAMILY_NAME_LEN 48


/*
 * Netlink messages are grouped by family (aka type), and each family supports
 * a set of commands, and can be passed both from kernel -> userspace or
 * vice-versa. To call into the kernel, userspace uses a device operation which
 * is outside of a netlink message.
 *
 * Each command results in the invocation of a handler function to implement the
 * request functionality.
 *
 * Expectedly, only certain combinations of (device operation, netlink family,
 * command) are valid.
 *
 * Here, we implement the basic infrastructure to perform validation on the
 * incoming message, version checking, and also to invoke the corresponding
 * handler to do the heavy-lifting.
 */

/*
 * Handler for a given netlink command. Not all the parameters are used by all
 * the handlers.
 */
typedef NTSTATUS(NetlinkCmdHandler)(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                                    UINT32 *replyLen);

typedef struct _NETLINK_CMD {
    UINT16 cmd;
    NetlinkCmdHandler *handler;
    UINT32 supportedDevOp;      /* Supported device operations. */
    BOOLEAN validateDpIndex;    /* Does command require a valid DP argument. */
} NETLINK_CMD, *PNETLINK_CMD;

/* A netlink family is a group of commands. */
typedef struct _NETLINK_FAMILY {
    CHAR *name;
    UINT32 id;
    UINT8 version;
    UINT8 pad;
    UINT16 maxAttr;
    NETLINK_CMD *cmds;          /* Array of netlink commands and handlers. */
    UINT16 opsCount;
} NETLINK_FAMILY, *PNETLINK_FAMILY;

/* Handlers for the various netlink commands. */
static NetlinkCmdHandler OvsGetPidCmdHandler,
                         OvsPendEventCmdHandler,
                         OvsPendPacketCmdHandler,
                         OvsSubscribeEventCmdHandler,
                         OvsSubscribePacketCmdHandler,
                         OvsReadEventCmdHandler,
                         OvsReadPacketCmdHandler,
                         OvsNewDpCmdHandler,
                         OvsGetDpCmdHandler,
                         OvsSetDpCmdHandler,
                         OvsGetVportCmdHandler,
                         OvsSetVportCmdHandler,
                         OvsNewVportCmdHandler,
                         OvsDeleteVportCmdHandler;

NetlinkCmdHandler        OvsGetNetdevCmdHandler;

static NTSTATUS HandleGetDpTransaction(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                                       UINT32 *replyLen);
static NTSTATUS HandleGetDpDump(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                                UINT32 *replyLen);
static NTSTATUS HandleDpTransactionCommon(
                    POVS_USER_PARAMS_CONTEXT usrParamsCtx, UINT32 *replyLen);

/*
 * The various netlink families, along with the supported commands. Most of
 * these families and commands are part of the openvswitch specification for a
 * netlink datapath. In addition, each platform can implement a few families
 * and commands as extensions.
 */

/* Netlink control family: this is a Windows specific family. */
NETLINK_CMD nlControlFamilyCmdOps[] = {
    { .cmd             = OVS_CTRL_CMD_WIN_GET_PID,
      .handler         = OvsGetPidCmdHandler,
      .supportedDevOp  = OVS_TRANSACTION_DEV_OP,
      .validateDpIndex = FALSE,
    },
    { .cmd = OVS_CTRL_CMD_WIN_PEND_REQ,
      .handler = OvsPendEventCmdHandler,
      .supportedDevOp = OVS_WRITE_DEV_OP,
      .validateDpIndex = TRUE,
    },
    { .cmd = OVS_CTRL_CMD_WIN_PEND_PACKET_REQ,
      .handler = OvsPendPacketCmdHandler,
      .supportedDevOp = OVS_WRITE_DEV_OP,
      .validateDpIndex = TRUE,
    },
    { .cmd = OVS_CTRL_CMD_MC_SUBSCRIBE_REQ,
      .handler = OvsSubscribeEventCmdHandler,
      .supportedDevOp = OVS_WRITE_DEV_OP,
      .validateDpIndex = TRUE,
    },
    { .cmd = OVS_CTRL_CMD_PACKET_SUBSCRIBE_REQ,
      .handler = OvsSubscribePacketCmdHandler,
      .supportedDevOp = OVS_WRITE_DEV_OP,
      .validateDpIndex = TRUE,
    },
    { .cmd = OVS_CTRL_CMD_EVENT_NOTIFY,
      .handler = OvsReadEventCmdHandler,
      .supportedDevOp = OVS_READ_EVENT_DEV_OP,
      .validateDpIndex = FALSE,
    },
    { .cmd = OVS_CTRL_CMD_READ_NOTIFY,
      .handler = OvsReadPacketCmdHandler,
      .supportedDevOp = OVS_READ_PACKET_DEV_OP,
      .validateDpIndex = FALSE,
    }
};

NETLINK_FAMILY nlControlFamilyOps = {
    .name     = OVS_WIN_CONTROL_FAMILY,
    .id       = OVS_WIN_NL_CTRL_FAMILY_ID,
    .version  = OVS_WIN_CONTROL_VERSION,
    .maxAttr  = OVS_WIN_CONTROL_ATTR_MAX,
    .cmds     = nlControlFamilyCmdOps,
    .opsCount = ARRAY_SIZE(nlControlFamilyCmdOps)
};

/* Netlink datapath family. */
NETLINK_CMD nlDatapathFamilyCmdOps[] = {
    { .cmd             = OVS_DP_CMD_NEW,
      .handler         = OvsNewDpCmdHandler,
      .supportedDevOp  = OVS_TRANSACTION_DEV_OP,
      .validateDpIndex = FALSE
    },
    { .cmd             = OVS_DP_CMD_GET,
      .handler         = OvsGetDpCmdHandler,
      .supportedDevOp  = OVS_WRITE_DEV_OP | OVS_READ_DEV_OP |
                         OVS_TRANSACTION_DEV_OP,
      .validateDpIndex = FALSE
    },
    { .cmd             = OVS_DP_CMD_SET,
      .handler         = OvsSetDpCmdHandler,
      .supportedDevOp  = OVS_WRITE_DEV_OP | OVS_READ_DEV_OP |
                         OVS_TRANSACTION_DEV_OP,
      .validateDpIndex = TRUE
    }
};

NETLINK_FAMILY nlDatapathFamilyOps = {
    .name     = OVS_DATAPATH_FAMILY,
    .id       = OVS_WIN_NL_DATAPATH_FAMILY_ID,
    .version  = OVS_DATAPATH_VERSION,
    .maxAttr  = OVS_DP_ATTR_MAX,
    .cmds     = nlDatapathFamilyCmdOps,
    .opsCount = ARRAY_SIZE(nlDatapathFamilyCmdOps)
};

/* Netlink packet family. */

NETLINK_CMD nlPacketFamilyCmdOps[] = {
    { .cmd             = OVS_PACKET_CMD_EXECUTE,
      .handler         = OvsNlExecuteCmdHandler,
      .supportedDevOp  = OVS_TRANSACTION_DEV_OP,
      .validateDpIndex = TRUE
    }
};

NETLINK_FAMILY nlPacketFamilyOps = {
    .name     = OVS_PACKET_FAMILY,
    .id       = OVS_WIN_NL_PACKET_FAMILY_ID,
    .version  = OVS_PACKET_VERSION,
    .maxAttr  = OVS_PACKET_ATTR_MAX,
    .cmds     = nlPacketFamilyCmdOps,
    .opsCount = ARRAY_SIZE(nlPacketFamilyCmdOps)
};

/* Netlink vport family. */
NETLINK_CMD nlVportFamilyCmdOps[] = {
    { .cmd = OVS_VPORT_CMD_GET,
      .handler = OvsGetVportCmdHandler,
      .supportedDevOp = OVS_WRITE_DEV_OP | OVS_READ_DEV_OP |
                        OVS_TRANSACTION_DEV_OP,
      .validateDpIndex = TRUE
    },
    { .cmd = OVS_VPORT_CMD_NEW,
      .handler = OvsNewVportCmdHandler,
      .supportedDevOp = OVS_TRANSACTION_DEV_OP,
      .validateDpIndex = TRUE
    },
    { .cmd = OVS_VPORT_CMD_SET,
      .handler = OvsSetVportCmdHandler,
      .supportedDevOp = OVS_TRANSACTION_DEV_OP,
      .validateDpIndex = TRUE
    },
    { .cmd = OVS_VPORT_CMD_DEL,
      .handler = OvsDeleteVportCmdHandler,
      .supportedDevOp = OVS_TRANSACTION_DEV_OP,
      .validateDpIndex = TRUE
    },
};

NETLINK_FAMILY nlVportFamilyOps = {
    .name     = OVS_VPORT_FAMILY,
    .id       = OVS_WIN_NL_VPORT_FAMILY_ID,
    .version  = OVS_VPORT_VERSION,
    .maxAttr  = OVS_VPORT_ATTR_MAX,
    .cmds     = nlVportFamilyCmdOps,
    .opsCount = ARRAY_SIZE(nlVportFamilyCmdOps)
};

/* Netlink flow family. */

NETLINK_CMD nlFlowFamilyCmdOps[] = {
    { .cmd              = OVS_FLOW_CMD_NEW,
      .handler          = OvsFlowNlCmdHandler,
      .supportedDevOp   = OVS_TRANSACTION_DEV_OP,
      .validateDpIndex  = TRUE
    },
    { .cmd              = OVS_FLOW_CMD_SET,
      .handler          = OvsFlowNlCmdHandler,
      .supportedDevOp   = OVS_TRANSACTION_DEV_OP,
      .validateDpIndex  = TRUE
    },
    { .cmd              = OVS_FLOW_CMD_DEL,
      .handler          = OvsFlowNlCmdHandler,
      .supportedDevOp   = OVS_TRANSACTION_DEV_OP,
      .validateDpIndex  = TRUE
    },
    { .cmd              = OVS_FLOW_CMD_GET,
      .handler          = OvsFlowNlGetCmdHandler,
      .supportedDevOp   = OVS_TRANSACTION_DEV_OP |
                          OVS_WRITE_DEV_OP | OVS_READ_DEV_OP,
      .validateDpIndex  = TRUE
    },
};

NETLINK_FAMILY nlFLowFamilyOps = {
    .name     = OVS_FLOW_FAMILY,
    .id       = OVS_WIN_NL_FLOW_FAMILY_ID,
    .version  = OVS_FLOW_VERSION,
    .maxAttr  = OVS_FLOW_ATTR_MAX,
    .cmds     = nlFlowFamilyCmdOps,
    .opsCount = ARRAY_SIZE(nlFlowFamilyCmdOps)
};

/* Netlink netdev family. */
NETLINK_CMD nlNetdevFamilyCmdOps[] = {
    { .cmd = OVS_WIN_NETDEV_CMD_GET,
      .handler = OvsGetNetdevCmdHandler,
      .supportedDevOp = OVS_TRANSACTION_DEV_OP,
      .validateDpIndex = FALSE
    },
};

NETLINK_FAMILY nlNetdevFamilyOps = {
    .name     = OVS_WIN_NETDEV_FAMILY,
    .id       = OVS_WIN_NL_NETDEV_FAMILY_ID,
    .version  = OVS_WIN_NETDEV_VERSION,
    .maxAttr  = OVS_WIN_NETDEV_ATTR_MAX,
    .cmds     = nlNetdevFamilyCmdOps,
    .opsCount = ARRAY_SIZE(nlNetdevFamilyCmdOps)
};

static NTSTATUS MapIrpOutputBuffer(PIRP irp,
                                   UINT32 bufferLength,
                                   UINT32 requiredLength,
                                   PVOID *buffer);
static NTSTATUS ValidateNetlinkCmd(UINT32 devOp,
                                   POVS_OPEN_INSTANCE instance,
                                   POVS_MESSAGE ovsMsg,
                                   NETLINK_FAMILY *nlFamilyOps);
static NTSTATUS InvokeNetlinkCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                                        NETLINK_FAMILY *nlFamilyOps,
                                        UINT32 *replyLen);

/* Handles to the device object for communication with userspace. */
NDIS_HANDLE gOvsDeviceHandle;
PDEVICE_OBJECT gOvsDeviceObject;

_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLOSE)
DRIVER_DISPATCH OvsOpenCloseDevice;

_Dispatch_type_(IRP_MJ_CLEANUP)
DRIVER_DISPATCH OvsCleanupDevice;

_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH OvsDeviceControl;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, OvsCreateDeviceObject)
#pragma alloc_text(PAGE, OvsOpenCloseDevice)
#pragma alloc_text(PAGE, OvsCleanupDevice)
#pragma alloc_text(PAGE, OvsDeviceControl)
#endif // ALLOC_PRAGMA

/*
 * We might hit this limit easily since userspace opens a netlink descriptor for
 * each thread, and at least one descriptor per vport. Revisit this later.
 */
#define OVS_MAX_OPEN_INSTANCES 512
#define OVS_SYSTEM_DP_NAME     "ovs-system"

POVS_OPEN_INSTANCE ovsOpenInstanceArray[OVS_MAX_OPEN_INSTANCES];
UINT32 ovsNumberOfOpenInstances;
extern POVS_SWITCH_CONTEXT gOvsSwitchContext;

NDIS_SPIN_LOCK ovsCtrlLockObj;
PNDIS_SPIN_LOCK gOvsCtrlLock;


VOID
OvsInit()
{
    gOvsCtrlLock = &ovsCtrlLockObj;
    NdisAllocateSpinLock(gOvsCtrlLock);
    OvsInitEventQueue();
}

VOID
OvsCleanup()
{
    OvsCleanupEventQueue();
    if (gOvsCtrlLock) {
        NdisFreeSpinLock(gOvsCtrlLock);
        gOvsCtrlLock = NULL;
    }
}

VOID
OvsAcquireCtrlLock()
{
    NdisAcquireSpinLock(gOvsCtrlLock);
}

VOID
OvsReleaseCtrlLock()
{
    NdisReleaseSpinLock(gOvsCtrlLock);
}


/*
 * --------------------------------------------------------------------------
 * Creates the communication device between user and kernel, and also
 * initializes the data associated data structures.
 * --------------------------------------------------------------------------
 */
NDIS_STATUS
OvsCreateDeviceObject(NDIS_HANDLE ovsExtDriverHandle)
{
    NDIS_STATUS status = NDIS_STATUS_SUCCESS;
    UNICODE_STRING deviceName;
    UNICODE_STRING symbolicDeviceName;
    PDRIVER_DISPATCH dispatchTable[IRP_MJ_MAXIMUM_FUNCTION+1];
    NDIS_DEVICE_OBJECT_ATTRIBUTES deviceAttributes;
    OVS_LOG_TRACE("ovsExtDriverHandle: %p", ovsExtDriverHandle);

    RtlZeroMemory(dispatchTable,
                  (IRP_MJ_MAXIMUM_FUNCTION + 1) * sizeof (PDRIVER_DISPATCH));
    dispatchTable[IRP_MJ_CREATE] = OvsOpenCloseDevice;
    dispatchTable[IRP_MJ_CLOSE] = OvsOpenCloseDevice;
    dispatchTable[IRP_MJ_CLEANUP] = OvsCleanupDevice;
    dispatchTable[IRP_MJ_DEVICE_CONTROL] = OvsDeviceControl;

    NdisInitUnicodeString(&deviceName, OVS_DEVICE_NAME_NT);
    NdisInitUnicodeString(&symbolicDeviceName, OVS_DEVICE_NAME_DOS);

    RtlZeroMemory(&deviceAttributes, sizeof (NDIS_DEVICE_OBJECT_ATTRIBUTES));

    OVS_INIT_OBJECT_HEADER(&deviceAttributes.Header,
                           NDIS_OBJECT_TYPE_DEVICE_OBJECT_ATTRIBUTES,
                           NDIS_DEVICE_OBJECT_ATTRIBUTES_REVISION_1,
                           sizeof (NDIS_DEVICE_OBJECT_ATTRIBUTES));

    deviceAttributes.DeviceName = &deviceName;
    deviceAttributes.SymbolicName = &symbolicDeviceName;
    deviceAttributes.MajorFunctions = dispatchTable;
    deviceAttributes.ExtensionSize = sizeof (OVS_DEVICE_EXTENSION);

    status = NdisRegisterDeviceEx(ovsExtDriverHandle,
                                  &deviceAttributes,
                                  &gOvsDeviceObject,
                                  &gOvsDeviceHandle);
    if (status != NDIS_STATUS_SUCCESS) {
        POVS_DEVICE_EXTENSION ovsExt =
            (POVS_DEVICE_EXTENSION)NdisGetDeviceReservedExtension(gOvsDeviceObject);
        ASSERT(gOvsDeviceObject != NULL);
        ASSERT(gOvsDeviceHandle != NULL);

        if (ovsExt) {
            ovsExt->numberOpenInstance = 0;
        }
    } else {
        /* Initialize the associated data structures. */
        OvsInit();
    }
    OVS_LOG_TRACE("DeviceObject: %p", gOvsDeviceObject);
    return status;
}


VOID
OvsDeleteDeviceObject()
{
    if (gOvsDeviceHandle) {
#ifdef DBG
        POVS_DEVICE_EXTENSION ovsExt = (POVS_DEVICE_EXTENSION)
                    NdisGetDeviceReservedExtension(gOvsDeviceObject);
        if (ovsExt) {
            ASSERT(ovsExt->numberOpenInstance == 0);
        }
#endif

        ASSERT(gOvsDeviceObject);
        NdisDeregisterDeviceEx(gOvsDeviceHandle);
        gOvsDeviceHandle = NULL;
        gOvsDeviceObject = NULL;
    }
    OvsCleanup();
}

POVS_OPEN_INSTANCE
OvsGetOpenInstance(PFILE_OBJECT fileObject,
                   UINT32 dpNo)
{
    POVS_OPEN_INSTANCE instance = (POVS_OPEN_INSTANCE)fileObject->FsContext;
    ASSERT(instance);
    ASSERT(instance->fileObject == fileObject);
    if (gOvsSwitchContext->dpNo != dpNo) {
        return NULL;
    }
    return instance;
}


POVS_OPEN_INSTANCE
OvsFindOpenInstance(PFILE_OBJECT fileObject)
{
    UINT32 i, j;
    for (i = 0, j = 0; i < OVS_MAX_OPEN_INSTANCES &&
                       j < ovsNumberOfOpenInstances; i++) {
        if (ovsOpenInstanceArray[i]) {
            if (ovsOpenInstanceArray[i]->fileObject == fileObject) {
                return ovsOpenInstanceArray[i];
            }
            j++;
        }
    }
    return NULL;
}

NTSTATUS
OvsAddOpenInstance(POVS_DEVICE_EXTENSION ovsExt,
                   PFILE_OBJECT fileObject)
{
    POVS_OPEN_INSTANCE instance =
        (POVS_OPEN_INSTANCE) OvsAllocateMemory(sizeof (OVS_OPEN_INSTANCE));
    UINT32 i;

    if (instance == NULL) {
        return STATUS_NO_MEMORY;
    }
    OvsAcquireCtrlLock();
    ASSERT(OvsFindOpenInstance(fileObject) == NULL);

    if (ovsNumberOfOpenInstances >= OVS_MAX_OPEN_INSTANCES) {
        OvsReleaseCtrlLock();
        OvsFreeMemory(instance);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(instance, sizeof (OVS_OPEN_INSTANCE));

    for (i = 0; i < OVS_MAX_OPEN_INSTANCES; i++) {
        if (ovsOpenInstanceArray[i] == NULL) {
            ovsOpenInstanceArray[i] = instance;
            ovsNumberOfOpenInstances++;
            instance->cookie = i;
            break;
        }
    }
    ASSERT(i < OVS_MAX_OPEN_INSTANCES);
    instance->fileObject = fileObject;
    ASSERT(fileObject->FsContext == NULL);
    instance->pid = (UINT32)InterlockedIncrement((LONG volatile *)&ovsExt->pidCount);
    if (instance->pid == 0) {
        /* XXX: check for rollover. */
    }
    fileObject->FsContext = instance;
    OvsReleaseCtrlLock();
    return STATUS_SUCCESS;
}

static VOID
OvsCleanupOpenInstance(PFILE_OBJECT fileObject)
{
    POVS_OPEN_INSTANCE instance = (POVS_OPEN_INSTANCE)fileObject->FsContext;
    ASSERT(instance);
    ASSERT(fileObject == instance->fileObject);
    OvsCleanupEvent(instance);
    OvsCleanupPacketQueue(instance);
}

VOID
OvsRemoveOpenInstance(PFILE_OBJECT fileObject)
{
    POVS_OPEN_INSTANCE instance;
    ASSERT(fileObject->FsContext);
    instance = (POVS_OPEN_INSTANCE)fileObject->FsContext;
    ASSERT(instance->cookie < OVS_MAX_OPEN_INSTANCES);

    OvsAcquireCtrlLock();
    fileObject->FsContext = NULL;
    ASSERT(ovsOpenInstanceArray[instance->cookie] == instance);
    ovsOpenInstanceArray[instance->cookie] = NULL;
    ovsNumberOfOpenInstances--;
    OvsReleaseCtrlLock();
    ASSERT(instance->eventQueue == NULL);
    ASSERT (instance->packetQueue == NULL);
    OvsFreeMemory(instance);
}

NTSTATUS
OvsCompleteIrpRequest(PIRP irp,
                      ULONG_PTR infoPtr,
                      NTSTATUS status)
{
    irp->IoStatus.Information = infoPtr;
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}


NTSTATUS
OvsOpenCloseDevice(PDEVICE_OBJECT deviceObject,
                   PIRP irp)
{
    PIO_STACK_LOCATION irpSp;
    NTSTATUS status = STATUS_SUCCESS;
    PFILE_OBJECT fileObject;
    POVS_DEVICE_EXTENSION ovsExt =
        (POVS_DEVICE_EXTENSION)NdisGetDeviceReservedExtension(deviceObject);

    ASSERT(deviceObject == gOvsDeviceObject);
    ASSERT(ovsExt != NULL);

    irpSp = IoGetCurrentIrpStackLocation(irp);
    fileObject = irpSp->FileObject;
    OVS_LOG_TRACE("DeviceObject: %p, fileObject:%p, instance: %u",
                  deviceObject, fileObject,
                  ovsExt->numberOpenInstance);

    switch (irpSp->MajorFunction) {
    case IRP_MJ_CREATE:
        status = OvsAddOpenInstance(ovsExt, fileObject);
        if (STATUS_SUCCESS == status) {
            InterlockedIncrement((LONG volatile *)&ovsExt->numberOpenInstance);
        }
        break;
    case IRP_MJ_CLOSE:
        ASSERT(ovsExt->numberOpenInstance > 0);
        OvsRemoveOpenInstance(fileObject);
        InterlockedDecrement((LONG volatile *)&ovsExt->numberOpenInstance);
        break;
    default:
        ASSERT(0);
    }
    return OvsCompleteIrpRequest(irp, (ULONG_PTR)0, status);
}

_Use_decl_annotations_
NTSTATUS
OvsCleanupDevice(PDEVICE_OBJECT deviceObject,
                 PIRP irp)
{

    PIO_STACK_LOCATION irpSp;
    PFILE_OBJECT fileObject;

    NTSTATUS status = STATUS_SUCCESS;
#ifdef DBG
    POVS_DEVICE_EXTENSION ovsExt =
        (POVS_DEVICE_EXTENSION)NdisGetDeviceReservedExtension(deviceObject);
    if (ovsExt) {
        ASSERT(ovsExt->numberOpenInstance > 0);
    }
#else
    UNREFERENCED_PARAMETER(deviceObject);
#endif
    ASSERT(deviceObject == gOvsDeviceObject);
    irpSp = IoGetCurrentIrpStackLocation(irp);
    fileObject = irpSp->FileObject;

    ASSERT(irpSp->MajorFunction == IRP_MJ_CLEANUP);

    OvsCleanupOpenInstance(fileObject);

    return OvsCompleteIrpRequest(irp, (ULONG_PTR)0, status);
}


/*
 * --------------------------------------------------------------------------
 * IOCTL function handler for the device.
 * --------------------------------------------------------------------------
 */
NTSTATUS
OvsDeviceControl(PDEVICE_OBJECT deviceObject,
                 PIRP irp)
{
    PIO_STACK_LOCATION irpSp;
    NTSTATUS status = STATUS_SUCCESS;
    PFILE_OBJECT fileObject;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    UINT32 inputBufferLen, outputBufferLen;
    UINT32 code, replyLen = 0;
    POVS_OPEN_INSTANCE instance;
    UINT32 devOp;
    OVS_MESSAGE ovsMsgReadOp;
    POVS_MESSAGE ovsMsg;
    NETLINK_FAMILY *nlFamilyOps;
    OVS_USER_PARAMS_CONTEXT usrParamsCtx;

#ifdef DBG
    POVS_DEVICE_EXTENSION ovsExt =
        (POVS_DEVICE_EXTENSION)NdisGetDeviceReservedExtension(deviceObject);
    ASSERT(deviceObject == gOvsDeviceObject);
    ASSERT(ovsExt);
    ASSERT(ovsExt->numberOpenInstance > 0);
#else
    UNREFERENCED_PARAMETER(deviceObject);
#endif

    irpSp = IoGetCurrentIrpStackLocation(irp);

    ASSERT(irpSp->MajorFunction == IRP_MJ_DEVICE_CONTROL);
    ASSERT(irpSp->FileObject != NULL);

    fileObject = irpSp->FileObject;
    instance = (POVS_OPEN_INSTANCE)fileObject->FsContext;
    code = irpSp->Parameters.DeviceIoControl.IoControlCode;
    inputBufferLen = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    outputBufferLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    inputBuffer = irp->AssociatedIrp.SystemBuffer;

    /* Check if the extension is enabled. */
    if (NULL == gOvsSwitchContext) {
        status = STATUS_DEVICE_NOT_READY;
        goto done;
    }

    /* Concurrent netlink operations are not supported. */
    if (InterlockedCompareExchange((LONG volatile *)&instance->inUse, 1, 0)) {
        status = STATUS_RESOURCE_IN_USE;
        goto done;
    }

    /*
     * Validate the input/output buffer arguments depending on the type of the
     * operation.
     */
    switch (code) {
    case OVS_IOCTL_TRANSACT:
        /* Both input buffer and output buffer are mandatory. */
        if (outputBufferLen != 0) {
            status = MapIrpOutputBuffer(irp, outputBufferLen,
                                        sizeof *ovsMsg, &outputBuffer);
            if (status != STATUS_SUCCESS) {
                goto done;
            }
            ASSERT(outputBuffer);
        } else {
            status = STATUS_NDIS_INVALID_LENGTH;
            goto done;
        }

        if (inputBufferLen < sizeof (*ovsMsg)) {
            status = STATUS_NDIS_INVALID_LENGTH;
            goto done;
        }

        ovsMsg = inputBuffer;
        devOp = OVS_TRANSACTION_DEV_OP;
        break;

    case OVS_IOCTL_READ_EVENT:
    case OVS_IOCTL_READ_PACKET:
        /*
         * Output buffer is mandatory. These IOCTLs are used to read events and
         * packets respectively. It is convenient to have separate ioctls.
         */
        if (outputBufferLen != 0) {
            status = MapIrpOutputBuffer(irp, outputBufferLen,
                                        sizeof *ovsMsg, &outputBuffer);
            if (status != STATUS_SUCCESS) {
                goto done;
            }
            ASSERT(outputBuffer);
        } else {
            status = STATUS_NDIS_INVALID_LENGTH;
            goto done;
        }
        inputBuffer = NULL;
        inputBufferLen = 0;

        ovsMsg = &ovsMsgReadOp;
        ovsMsg->nlMsg.nlmsgType = OVS_WIN_NL_CTRL_FAMILY_ID;
        ovsMsg->nlMsg.nlmsgPid = instance->pid;
        /* An "artificial" command so we can use NL family function table*/
        ovsMsg->genlMsg.cmd = (code == OVS_IOCTL_READ_EVENT) ?
                              OVS_CTRL_CMD_EVENT_NOTIFY :
                              OVS_CTRL_CMD_READ_NOTIFY;
        devOp = OVS_READ_DEV_OP;
        break;

    case OVS_IOCTL_READ:
        /* Output buffer is mandatory. */
        if (outputBufferLen != 0) {
            status = MapIrpOutputBuffer(irp, outputBufferLen,
                                        sizeof *ovsMsg, &outputBuffer);
            if (status != STATUS_SUCCESS) {
                goto done;
            }
            ASSERT(outputBuffer);
        } else {
            status = STATUS_NDIS_INVALID_LENGTH;
            goto done;
        }

        /*
         * Operate in the mode that read ioctl is similar to ReadFile(). This
         * might change as the userspace code gets implemented.
         */
        inputBuffer = NULL;
        inputBufferLen = 0;

        /*
         * For implementing read (ioctl or otherwise), we need to store some
         * state in the instance to indicate the command that started the dump
         * operation. The state can setup 'ovsMsgReadOp' appropriately. Note
         * that 'ovsMsgReadOp' is needed only in this function to call into the
         * appropraite handler. The handler itself can access the state in the
         * instance.
         *
         * In the absence of a dump start, return 0 bytes.
         */
        if (instance->dumpState.ovsMsg == NULL) {
            replyLen = 0;
            status = STATUS_SUCCESS;
            goto done;
        }
        RtlCopyMemory(&ovsMsgReadOp, instance->dumpState.ovsMsg,
                      sizeof (ovsMsgReadOp));

        /* Create an NL message for consumption. */
        ovsMsg = &ovsMsgReadOp;
        devOp = OVS_READ_DEV_OP;

        break;

    case OVS_IOCTL_WRITE:
        /* Input buffer is mandatory. */
        if (inputBufferLen < sizeof (*ovsMsg)) {
            status = STATUS_NDIS_INVALID_LENGTH;
            goto done;
        }

        ovsMsg = inputBuffer;
        devOp = OVS_WRITE_DEV_OP;
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        goto done;
    }

    ASSERT(ovsMsg);
    switch (ovsMsg->nlMsg.nlmsgType) {
    case OVS_WIN_NL_CTRL_FAMILY_ID:
        nlFamilyOps = &nlControlFamilyOps;
        break;
    case OVS_WIN_NL_DATAPATH_FAMILY_ID:
        nlFamilyOps = &nlDatapathFamilyOps;
        break;
    case OVS_WIN_NL_FLOW_FAMILY_ID:
         nlFamilyOps = &nlFLowFamilyOps;
         break;
    case OVS_WIN_NL_PACKET_FAMILY_ID:
         nlFamilyOps = &nlPacketFamilyOps;
         break;
    case OVS_WIN_NL_VPORT_FAMILY_ID:
        nlFamilyOps = &nlVportFamilyOps;
        break;
    case OVS_WIN_NL_NETDEV_FAMILY_ID:
        nlFamilyOps = &nlNetdevFamilyOps;
        break;
    default:
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }

    /*
     * For read operation, the netlink command has already been validated
     * previously.
     */
    if (devOp != OVS_READ_DEV_OP) {
        status = ValidateNetlinkCmd(devOp, instance, ovsMsg, nlFamilyOps);
        if (status != STATUS_SUCCESS) {
            goto done;
        }
    }

    InitUserParamsCtx(irp, instance, devOp, ovsMsg,
                      inputBuffer, inputBufferLen,
                      outputBuffer, outputBufferLen,
                      &usrParamsCtx);

    status = InvokeNetlinkCmdHandler(&usrParamsCtx, nlFamilyOps, &replyLen);

done:
    KeMemoryBarrier();
    instance->inUse = 0;
    return OvsCompleteIrpRequest(irp, (ULONG_PTR)replyLen, status);
}


/*
 * --------------------------------------------------------------------------
 * Function to validate a netlink command. Only certain combinations of
 * (device operation, netlink family, command) are valid.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
ValidateNetlinkCmd(UINT32 devOp,
                   POVS_OPEN_INSTANCE instance,
                   POVS_MESSAGE ovsMsg,
                   NETLINK_FAMILY *nlFamilyOps)
{
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    UINT16 i;

    for (i = 0; i < nlFamilyOps->opsCount; i++) {
        if (nlFamilyOps->cmds[i].cmd == ovsMsg->genlMsg.cmd) {
            /* Validate if the command is valid for the device operation. */
            if ((devOp & nlFamilyOps->cmds[i].supportedDevOp) == 0) {
                status = STATUS_INVALID_PARAMETER;
                goto done;
            }

            /* Validate the version. */
            if (nlFamilyOps->version > ovsMsg->genlMsg.version) {
                status = STATUS_INVALID_PARAMETER;
                goto done;
            }

            /* Validate the DP for commands that require a DP. */
            if (nlFamilyOps->cmds[i].validateDpIndex == TRUE) {
                OvsAcquireCtrlLock();
                if (ovsMsg->ovsHdr.dp_ifindex !=
                                          (INT)gOvsSwitchContext->dpNo) {
                    status = STATUS_INVALID_PARAMETER;
                    OvsReleaseCtrlLock();
                    goto done;
                }
                OvsReleaseCtrlLock();
            }

            /* Validate the PID. */
            if (ovsMsg->genlMsg.cmd != OVS_CTRL_CMD_WIN_GET_PID) {
                if (ovsMsg->nlMsg.nlmsgPid != instance->pid) {
                    status = STATUS_INVALID_PARAMETER;
                    goto done;
                }
            }

            status = STATUS_SUCCESS;
            break;
        }
    }

done:
    return status;
}

/*
 * --------------------------------------------------------------------------
 * Function to invoke the netlink command handler.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
InvokeNetlinkCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                        NETLINK_FAMILY *nlFamilyOps,
                        UINT32 *replyLen)
{
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    UINT16 i;

    for (i = 0; i < nlFamilyOps->opsCount; i++) {
        if (nlFamilyOps->cmds[i].cmd == usrParamsCtx->ovsMsg->genlMsg.cmd) {
            NetlinkCmdHandler *handler = nlFamilyOps->cmds[i].handler;
            ASSERT(handler);
            if (handler) {
                status = handler(usrParamsCtx, replyLen);
            }
            break;
        }
    }

    return status;
}

/*
 * --------------------------------------------------------------------------
 *  Command Handler for 'OVS_CTRL_CMD_WIN_GET_PID'.
 *
 *  Each handle on the device is assigned a unique PID when the handle is
 *  created. On platforms that support netlink natively, the PID is available
 *  to userspace when the netlink socket is created. However, without native
 *  netlink support on Windows, OVS datapath generates the PID and lets the
 *  userspace query it.
 *
 *  This function implements the query.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
OvsGetPidCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                    UINT32 *replyLen)
{
    POVS_MESSAGE msgIn = (POVS_MESSAGE)usrParamsCtx->inputBuffer;
    POVS_MESSAGE msgOut = (POVS_MESSAGE)usrParamsCtx->outputBuffer;

    if (usrParamsCtx->outputLength >= sizeof *msgOut) {
        POVS_OPEN_INSTANCE instance =
            (POVS_OPEN_INSTANCE)usrParamsCtx->ovsInstance;

        RtlZeroMemory(msgOut, sizeof *msgOut);
        msgOut->nlMsg.nlmsgSeq = msgIn->nlMsg.nlmsgSeq;
        msgOut->nlMsg.nlmsgPid = instance->pid;
        *replyLen = sizeof *msgOut;
        /* XXX: We might need to return the DP index as well. */
    } else {
        return STATUS_NDIS_INVALID_LENGTH;
    }

    return STATUS_SUCCESS;
}

/*
 * --------------------------------------------------------------------------
 * Utility function to fill up information about the datapath in a reply to
 * userspace.
 * Assumes that 'gOvsCtrlLock' lock is acquired.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
OvsDpFillInfo(POVS_SWITCH_CONTEXT ovsSwitchContext,
              POVS_MESSAGE msgIn,
              PNL_BUFFER nlBuf)
{
    BOOLEAN writeOk;
    OVS_MESSAGE msgOutTmp;
    OVS_DATAPATH *datapath = &ovsSwitchContext->datapath;
    PNL_MSG_HDR nlMsg;

    ASSERT(NlBufAt(nlBuf, 0, 0) != 0 && NlBufRemLen(nlBuf) >= sizeof *msgIn);

    msgOutTmp.nlMsg.nlmsgType = OVS_WIN_NL_DATAPATH_FAMILY_ID;
    msgOutTmp.nlMsg.nlmsgFlags = 0;  /* XXX: ? */
    msgOutTmp.nlMsg.nlmsgSeq = msgIn->nlMsg.nlmsgSeq;
    msgOutTmp.nlMsg.nlmsgPid = msgIn->nlMsg.nlmsgPid;

    msgOutTmp.genlMsg.cmd = OVS_DP_CMD_GET;
    msgOutTmp.genlMsg.version = nlDatapathFamilyOps.version;
    msgOutTmp.genlMsg.reserved = 0;

    msgOutTmp.ovsHdr.dp_ifindex = ovsSwitchContext->dpNo;

    writeOk = NlMsgPutHead(nlBuf, (PCHAR)&msgOutTmp, sizeof msgOutTmp);
    if (writeOk) {
        writeOk = NlMsgPutTailString(nlBuf, OVS_DP_ATTR_NAME,
                                     OVS_SYSTEM_DP_NAME);
    }
    if (writeOk) {
        OVS_DP_STATS dpStats;

        dpStats.n_hit = datapath->hits;
        dpStats.n_missed = datapath->misses;
        dpStats.n_lost = datapath->lost;
        dpStats.n_flows = datapath->nFlows;
        writeOk = NlMsgPutTailUnspec(nlBuf, OVS_DP_ATTR_STATS,
                                     (PCHAR)&dpStats, sizeof dpStats);
    }
    nlMsg = (PNL_MSG_HDR)NlBufAt(nlBuf, 0, 0);
    nlMsg->nlmsgLen = NlBufSize(nlBuf);

    return writeOk ? STATUS_SUCCESS : STATUS_INVALID_BUFFER_SIZE;
}

/*
 * --------------------------------------------------------------------------
 * Handler for queueing an IRP used for event notification. The IRP is
 * completed when a port state changes. STATUS_PENDING is returned on
 * success. User mode keep a pending IRP at all times.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
OvsPendEventCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                       UINT32 *replyLen)
{
    NDIS_STATUS status;

    UNREFERENCED_PARAMETER(replyLen);

    POVS_OPEN_INSTANCE instance =
        (POVS_OPEN_INSTANCE)usrParamsCtx->ovsInstance;
    POVS_MESSAGE msgIn = (POVS_MESSAGE)usrParamsCtx->inputBuffer;
    OVS_EVENT_POLL poll;

    poll.dpNo = msgIn->ovsHdr.dp_ifindex;
    status = OvsWaitEventIoctl(usrParamsCtx->irp, instance->fileObject,
                               &poll, sizeof poll);
    return status;
}

/*
 * --------------------------------------------------------------------------
 *  Handler for the subscription for the event queue
 * --------------------------------------------------------------------------
 */
static NTSTATUS
OvsSubscribeEventCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                            UINT32 *replyLen)
{
    NDIS_STATUS status;
    OVS_EVENT_SUBSCRIBE request;
    BOOLEAN rc;
    UINT8 join;
    PNL_ATTR attrs[2];
    const NL_POLICY policy[] =  {
        [OVS_NL_ATTR_MCAST_GRP] = {.type = NL_A_U32 },
        [OVS_NL_ATTR_MCAST_JOIN] = {.type = NL_A_U8 },
        };

    UNREFERENCED_PARAMETER(replyLen);

    POVS_OPEN_INSTANCE instance =
        (POVS_OPEN_INSTANCE)usrParamsCtx->ovsInstance;
    POVS_MESSAGE msgIn = (POVS_MESSAGE)usrParamsCtx->inputBuffer;

    rc = NlAttrParse(&msgIn->nlMsg, sizeof (*msgIn),
         NlMsgAttrsLen((PNL_MSG_HDR)msgIn), policy, attrs, ARRAY_SIZE(attrs));
    if (!rc) {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }

    /* XXX Ignore the MC group for now */
    join = NlAttrGetU8(attrs[OVS_NL_ATTR_MCAST_JOIN]);
    request.dpNo = msgIn->ovsHdr.dp_ifindex;
    request.subscribe = join;
    request.mask = OVS_EVENT_MASK_ALL;

    status = OvsSubscribeEventIoctl(instance->fileObject, &request,
                                    sizeof request);
done:
    return status;
}

/*
 * --------------------------------------------------------------------------
 *  Command Handler for 'OVS_DP_CMD_NEW'.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
OvsNewDpCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                   UINT32 *replyLen)
{
    return HandleDpTransactionCommon(usrParamsCtx, replyLen);
}

/*
 * --------------------------------------------------------------------------
 *  Command Handler for 'OVS_DP_CMD_GET'.
 *
 *  The function handles both the dump based as well as the transaction based
 *  'OVS_DP_CMD_GET' command. In the dump command, it handles the initial
 *  call to setup dump state, as well as subsequent calls to continue dumping
 *  data.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
OvsGetDpCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                   UINT32 *replyLen)
{
    if (usrParamsCtx->devOp == OVS_TRANSACTION_DEV_OP) {
        return HandleDpTransactionCommon(usrParamsCtx, replyLen);
    } else {
        return HandleGetDpDump(usrParamsCtx, replyLen);
    }
}

/*
 * --------------------------------------------------------------------------
 *  Function for handling the transaction based 'OVS_DP_CMD_GET' command.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
HandleGetDpTransaction(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                       UINT32 *replyLen)
{
    return HandleDpTransactionCommon(usrParamsCtx, replyLen);
}


/*
 * --------------------------------------------------------------------------
 *  Function for handling the dump-based 'OVS_DP_CMD_GET' command.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
HandleGetDpDump(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                UINT32 *replyLen)
{
    POVS_MESSAGE msgOut = (POVS_MESSAGE)usrParamsCtx->outputBuffer;
    POVS_OPEN_INSTANCE instance =
        (POVS_OPEN_INSTANCE)usrParamsCtx->ovsInstance;

    if (usrParamsCtx->devOp == OVS_WRITE_DEV_OP) {
        *replyLen = 0;
        OvsSetupDumpStart(usrParamsCtx);
    } else {
        NL_BUFFER nlBuf;
        NTSTATUS status;
        POVS_MESSAGE msgIn = instance->dumpState.ovsMsg;

        ASSERT(usrParamsCtx->devOp == OVS_READ_DEV_OP);

        if (instance->dumpState.ovsMsg == NULL) {
            ASSERT(FALSE);
            return STATUS_INVALID_DEVICE_STATE;
        }

        /* Dump state must have been deleted after previous dump operation. */
        ASSERT(instance->dumpState.index[0] == 0);

        /* Output buffer has been validated while validating read dev op. */
        ASSERT(msgOut != NULL && usrParamsCtx->outputLength >= sizeof *msgOut);

        NlBufInit(&nlBuf, usrParamsCtx->outputBuffer,
                  usrParamsCtx->outputLength);

        OvsAcquireCtrlLock();
        status = OvsDpFillInfo(gOvsSwitchContext, msgIn, &nlBuf);
        OvsReleaseCtrlLock();

        if (status != STATUS_SUCCESS) {
            *replyLen = 0;
            FreeUserDumpState(instance);
            return status;
        }

        /* Increment the dump index. */
        instance->dumpState.index[0] = 1;
        *replyLen = msgOut->nlMsg.nlmsgLen;

        /* Free up the dump state, since there's no more data to continue. */
        FreeUserDumpState(instance);
    }

    return STATUS_SUCCESS;
}


/*
 * --------------------------------------------------------------------------
 *  Command Handler for 'OVS_DP_CMD_SET'.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
OvsSetDpCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                   UINT32 *replyLen)
{
    return HandleDpTransactionCommon(usrParamsCtx, replyLen);
}

/*
 * --------------------------------------------------------------------------
 *  Function for handling transaction based 'OVS_DP_CMD_NEW', 'OVS_DP_CMD_GET'
 *  and 'OVS_DP_CMD_SET' commands.
 *
 * 'OVS_DP_CMD_NEW' is implemented to keep userspace code happy. Creation of a
 * new datapath is not supported currently.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
HandleDpTransactionCommon(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                          UINT32 *replyLen)
{
    POVS_MESSAGE msgIn = (POVS_MESSAGE)usrParamsCtx->inputBuffer;
    POVS_MESSAGE msgOut = (POVS_MESSAGE)usrParamsCtx->outputBuffer;
    NTSTATUS status = STATUS_SUCCESS;
    NL_BUFFER nlBuf;
    NL_ERROR nlError = NL_ERROR_SUCCESS;
    static const NL_POLICY ovsDatapathSetPolicy[] = {
        [OVS_DP_ATTR_NAME] = { .type = NL_A_STRING, .maxLen = IFNAMSIZ },
        [OVS_DP_ATTR_UPCALL_PID] = { .type = NL_A_U32, .optional = TRUE },
        [OVS_DP_ATTR_USER_FEATURES] = { .type = NL_A_U32, .optional = TRUE },
    };
    PNL_ATTR dpAttrs[ARRAY_SIZE(ovsDatapathSetPolicy)];

    /* input buffer has been validated while validating write dev op. */
    ASSERT(msgIn != NULL && usrParamsCtx->inputLength >= sizeof *msgIn);

    /* Parse any attributes in the request. */
    if (usrParamsCtx->ovsMsg->genlMsg.cmd == OVS_DP_CMD_SET ||
        usrParamsCtx->ovsMsg->genlMsg.cmd == OVS_DP_CMD_NEW) {
        if (!NlAttrParse((PNL_MSG_HDR)msgIn,
                        NLMSG_HDRLEN + GENL_HDRLEN + OVS_HDRLEN,
                        NlMsgAttrsLen((PNL_MSG_HDR)msgIn),
                        ovsDatapathSetPolicy, dpAttrs, ARRAY_SIZE(dpAttrs))) {
            return STATUS_INVALID_PARAMETER;
        }

        /*
        * XXX: Not clear at this stage if there's any role for the
        * OVS_DP_ATTR_UPCALL_PID and OVS_DP_ATTR_USER_FEATURES attributes passed
        * from userspace.
        */

    } else {
        RtlZeroMemory(dpAttrs, sizeof dpAttrs);
    }

    /* Output buffer has been validated while validating transact dev op. */
    ASSERT(msgOut != NULL && usrParamsCtx->outputLength >= sizeof *msgOut);

    NlBufInit(&nlBuf, usrParamsCtx->outputBuffer, usrParamsCtx->outputLength);

    OvsAcquireCtrlLock();
    if (dpAttrs[OVS_DP_ATTR_NAME] != NULL) {
        if (!OvsCompareString(NlAttrGet(dpAttrs[OVS_DP_ATTR_NAME]),
                              OVS_SYSTEM_DP_NAME)) {
            OvsReleaseCtrlLock();

            /* Creation of new datapaths is not supported. */
            if (usrParamsCtx->ovsMsg->genlMsg.cmd == OVS_DP_CMD_SET) {
                nlError = NL_ERROR_NOTSUPP;
                goto cleanup;
            }

            nlError = NL_ERROR_NODEV;
            goto cleanup;
        }
    } else if ((UINT32)msgIn->ovsHdr.dp_ifindex != gOvsSwitchContext->dpNo) {
        OvsReleaseCtrlLock();
        nlError = NL_ERROR_NODEV;
        goto cleanup;
    }

    if (usrParamsCtx->ovsMsg->genlMsg.cmd == OVS_DP_CMD_NEW) {
        OvsReleaseCtrlLock();
        nlError = NL_ERROR_EXIST;
        goto cleanup;
    }

    status = OvsDpFillInfo(gOvsSwitchContext, msgIn, &nlBuf);
    OvsReleaseCtrlLock();

    *replyLen = NlBufSize(&nlBuf);

cleanup:
    if (nlError != NL_ERROR_SUCCESS) {
        POVS_MESSAGE_ERROR msgError = (POVS_MESSAGE_ERROR)
            usrParamsCtx->outputBuffer;

        BuildErrorMsg(msgIn, msgError, nlError);
        *replyLen = msgError->nlMsg.nlmsgLen;
    }

    return STATUS_SUCCESS;
}


NTSTATUS
OvsSetupDumpStart(POVS_USER_PARAMS_CONTEXT usrParamsCtx)
{
    POVS_MESSAGE msgIn = (POVS_MESSAGE)usrParamsCtx->inputBuffer;
    POVS_OPEN_INSTANCE instance =
        (POVS_OPEN_INSTANCE)usrParamsCtx->ovsInstance;

    /* input buffer has been validated while validating write dev op. */
    ASSERT(msgIn != NULL && usrParamsCtx->inputLength >= sizeof *msgIn);

    /* A write operation that does not indicate dump start is invalid. */
    if ((msgIn->nlMsg.nlmsgFlags & NLM_F_DUMP) != NLM_F_DUMP) {
        return STATUS_INVALID_PARAMETER;
    }
    /* XXX: Handle other NLM_F_* flags in the future. */

    /*
     * This operation should be setting up the dump state. If there's any
     * previous state, clear it up so as to set it up afresh.
     */
    if (instance->dumpState.ovsMsg != NULL) {
        FreeUserDumpState(instance);
    }

    return InitUserDumpState(instance, msgIn);
}

static VOID
BuildMsgOut(POVS_MESSAGE msgIn, POVS_MESSAGE msgOut, UINT16 type,
            UINT32 length, UINT16 flags)
{
    msgOut->nlMsg.nlmsgType = type;
    msgOut->nlMsg.nlmsgFlags = flags;
    msgOut->nlMsg.nlmsgSeq = msgIn->nlMsg.nlmsgSeq;
    msgOut->nlMsg.nlmsgPid = msgIn->nlMsg.nlmsgPid;
    msgOut->nlMsg.nlmsgLen = length;

    msgOut->genlMsg.cmd = msgIn->genlMsg.cmd;
    msgOut->genlMsg.version = msgIn->genlMsg.version;
    msgOut->genlMsg.reserved = 0;
}

/*
 * XXX: should move out these functions to a Netlink.c or to a OvsMessage.c
 * or even make them inlined functions in Datapath.h. Can be done after the
 * first sprint once we have more code to refactor.
 */
VOID
BuildReplyMsgFromMsgIn(POVS_MESSAGE msgIn, POVS_MESSAGE msgOut, UINT16 flags)
{
    BuildMsgOut(msgIn, msgOut, msgIn->nlMsg.nlmsgType, sizeof(OVS_MESSAGE),
                flags);
}

VOID
BuildErrorMsg(POVS_MESSAGE msgIn, POVS_MESSAGE_ERROR msgOut, UINT errorCode)
{
    BuildMsgOut(msgIn, (POVS_MESSAGE)msgOut, NLMSG_ERROR,
                sizeof(OVS_MESSAGE_ERROR), 0);

    msgOut->errorMsg.error = errorCode;
    msgOut->errorMsg.nlMsg = msgIn->nlMsg;
}

static NTSTATUS
OvsCreateMsgFromVport(POVS_VPORT_ENTRY vport,
                      POVS_MESSAGE msgIn,
                      PVOID outBuffer,
                      UINT32 outBufLen,
                      int dpIfIndex)
{
    NL_BUFFER nlBuffer;
    OVS_VPORT_FULL_STATS vportStats;
    BOOLEAN ok;
    OVS_MESSAGE msgOut;
    PNL_MSG_HDR nlMsg;

    NlBufInit(&nlBuffer, outBuffer, outBufLen);

    BuildReplyMsgFromMsgIn(msgIn, &msgOut, NLM_F_MULTI);
    msgOut.ovsHdr.dp_ifindex = dpIfIndex;

    ok = NlMsgPutHead(&nlBuffer, (PCHAR)&msgOut, sizeof msgOut);
    if (!ok) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ok = NlMsgPutTailU32(&nlBuffer, OVS_VPORT_ATTR_PORT_NO, vport->portNo);
    if (!ok) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ok = NlMsgPutTailU32(&nlBuffer, OVS_VPORT_ATTR_TYPE, vport->ovsType);
    if (!ok) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ok = NlMsgPutTailString(&nlBuffer, OVS_VPORT_ATTR_NAME, vport->ovsName);
    if (!ok) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /*
     * XXX: when we implement OVS_DP_ATTR_USER_FEATURES in datapath,
     * we'll need to check the OVS_DP_F_VPORT_PIDS flag: if it is set,
     * it means we have an array of pids, instead of a single pid.
     * ATM we assume we have one pid only.
    */

    ok = NlMsgPutTailU32(&nlBuffer, OVS_VPORT_ATTR_UPCALL_PID,
                         vport->upcallPid);
    if (!ok) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /*stats*/
    vportStats.rxPackets = vport->stats.rxPackets;
    vportStats.rxBytes = vport->stats.rxBytes;
    vportStats.txPackets = vport->stats.txPackets;
    vportStats.txBytes = vport->stats.txBytes;
    vportStats.rxErrors = vport->errStats.rxErrors;
    vportStats.txErrors = vport->errStats.txErrors;
    vportStats.rxDropped = vport->errStats.rxDropped;
    vportStats.txDropped = vport->errStats.txDropped;

    ok = NlMsgPutTailUnspec(&nlBuffer, OVS_VPORT_ATTR_STATS,
                            (PCHAR)&vportStats,
                            sizeof(OVS_VPORT_FULL_STATS));
    if (!ok) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /*
     * XXX: when vxlan udp dest port becomes configurable, we will also need
     * to add vport options
    */

    nlMsg = (PNL_MSG_HDR)NlBufAt(&nlBuffer, 0, 0);
    nlMsg->nlmsgLen = NlBufSize(&nlBuffer);

    return STATUS_SUCCESS;
}

static NTSTATUS
OvsGetVportDumpNext(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                    UINT32 *replyLen)
{
    POVS_MESSAGE msgIn;
    POVS_OPEN_INSTANCE instance =
        (POVS_OPEN_INSTANCE)usrParamsCtx->ovsInstance;
    LOCK_STATE_EX lockState;
    UINT32 i = OVS_MAX_VPORT_ARRAY_SIZE;

    /*
     * XXX: this function shares some code with other dump command(s).
     * In the future, we will need to refactor the dump functions
    */

    ASSERT(usrParamsCtx->devOp == OVS_READ_DEV_OP);

    if (instance->dumpState.ovsMsg == NULL) {
        ASSERT(FALSE);
        return STATUS_INVALID_DEVICE_STATE;
    }

    /* Output buffer has been validated while validating read dev op. */
    ASSERT(usrParamsCtx->outputBuffer != NULL);

    msgIn = instance->dumpState.ovsMsg;

    OvsAcquireCtrlLock();

    /*
     * XXX: when we implement OVS_DP_ATTR_USER_FEATURES in datapath,
     * we'll need to check the OVS_DP_F_VPORT_PIDS flag: if it is set,
     * it means we have an array of pids, instead of a single pid.
     * ATM we assume we have one pid only.
    */
    ASSERT(KeGetCurrentIrql() == DISPATCH_LEVEL);
    NdisAcquireRWLockRead(gOvsSwitchContext->dispatchLock, &lockState,
                          NDIS_RWL_AT_DISPATCH_LEVEL);

    if (gOvsSwitchContext->numHvVports > 0 ||
            gOvsSwitchContext->numNonHvVports > 0) {
        /* inBucket: the bucket, used for lookup */
        UINT32 inBucket = instance->dumpState.index[0];
        /* inIndex: index within the given bucket, used for lookup */
        UINT32 inIndex = instance->dumpState.index[1];
        /* the bucket to be used for the next dump operation */
        UINT32 outBucket = 0;
        /* the index within the outBucket to be used for the next dump */
        UINT32 outIndex = 0;

        for (i = inBucket; i < OVS_MAX_VPORT_ARRAY_SIZE; i++) {
            PLIST_ENTRY head, link;
            head = &(gOvsSwitchContext->portNoHashArray[i]);
            POVS_VPORT_ENTRY vport = NULL;

            outIndex = 0;
            LIST_FORALL(head, link) {

                /*
                 * if one or more dumps were previously done on this same bucket,
                 * inIndex will be > 0, so we'll need to reply with the
                 * inIndex + 1 vport from the bucket.
                */
                if (outIndex >= inIndex) {
                    vport = CONTAINING_RECORD(link, OVS_VPORT_ENTRY, portNoLink);

                    ASSERT(vport->portNo != OVS_DPPORT_NUMBER_INVALID);
                    OvsCreateMsgFromVport(vport, msgIn,
                                          usrParamsCtx->outputBuffer,
                                          usrParamsCtx->outputLength,
                                          gOvsSwitchContext->dpNo);
                    ++outIndex;
                    break;
                }

                ++outIndex;
            }

            if (vport) {
                break;
            }

            /*
             * if no vport was found above, check the next bucket, beginning
             * with the first (i.e. index 0) elem from within that bucket
            */
            inIndex = 0;
        }

        outBucket = i;

        /* XXX: what about NLMSG_DONE (as msg type)? */
        instance->dumpState.index[0] = outBucket;
        instance->dumpState.index[1] = outIndex;
    }

    NdisReleaseRWLock(gOvsSwitchContext->dispatchLock, &lockState);

    OvsReleaseCtrlLock();

    /* if i < OVS_MAX_VPORT_ARRAY_SIZE => vport was found */
    if (i < OVS_MAX_VPORT_ARRAY_SIZE) {
        POVS_MESSAGE msgOut = (POVS_MESSAGE)usrParamsCtx->outputBuffer;
        *replyLen = msgOut->nlMsg.nlmsgLen;
    } else {
        /*
         * if i >= OVS_MAX_VPORT_ARRAY_SIZE => vport was not found =>
         * it's dump done
         */
        *replyLen = 0;
        /* Free up the dump state, since there's no more data to continue. */
        FreeUserDumpState(instance);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
OvsGetVport(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
            UINT32 *replyLen)
{
    NTSTATUS status = STATUS_SUCCESS;
    LOCK_STATE_EX lockState;

    POVS_MESSAGE msgIn = (POVS_MESSAGE)usrParamsCtx->inputBuffer;
    POVS_MESSAGE msgOut = (POVS_MESSAGE)usrParamsCtx->outputBuffer;
    POVS_VPORT_ENTRY vport = NULL;
    NL_ERROR nlError = NL_ERROR_SUCCESS;
    PCHAR portName = NULL;
    UINT32 portNameLen = 0;
    UINT32 portNumber = OVS_DPPORT_NUMBER_INVALID;

    static const NL_POLICY ovsVportPolicy[] = {
        [OVS_VPORT_ATTR_PORT_NO] = { .type = NL_A_U32, .optional = TRUE },
        [OVS_VPORT_ATTR_NAME] = { .type = NL_A_STRING,
                                  .minLen = 2,
                                  .maxLen = IFNAMSIZ,
                                  .optional = TRUE},
    };
    PNL_ATTR vportAttrs[ARRAY_SIZE(ovsVportPolicy)];

    /* input buffer has been validated while validating write dev op. */
    ASSERT(usrParamsCtx->inputBuffer != NULL);

    if (!NlAttrParse((PNL_MSG_HDR)msgIn,
        NLMSG_HDRLEN + GENL_HDRLEN + OVS_HDRLEN,
        NlMsgAttrsLen((PNL_MSG_HDR)msgIn),
        ovsVportPolicy, vportAttrs, ARRAY_SIZE(vportAttrs))) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Output buffer has been validated while validating transact dev op. */
    ASSERT(msgOut != NULL && usrParamsCtx->outputLength >= sizeof *msgOut);

    NdisAcquireRWLockRead(gOvsSwitchContext->dispatchLock, &lockState, 0);
    if (vportAttrs[OVS_VPORT_ATTR_NAME] != NULL) {
        portName = NlAttrGet(vportAttrs[OVS_VPORT_ATTR_NAME]);
        portNameLen = NlAttrGetSize(vportAttrs[OVS_VPORT_ATTR_NAME]);

        /* the port name is expected to be null-terminated */
        ASSERT(portName[portNameLen - 1] == '\0');

        vport = OvsFindVportByOvsName(gOvsSwitchContext, portName);
    } else if (vportAttrs[OVS_VPORT_ATTR_PORT_NO] != NULL) {
        portNumber = NlAttrGetU32(vportAttrs[OVS_VPORT_ATTR_PORT_NO]);

        vport = OvsFindVportByPortNo(gOvsSwitchContext, portNumber);
    } else {
        nlError = NL_ERROR_INVAL;
        NdisReleaseRWLock(gOvsSwitchContext->dispatchLock, &lockState);
        goto Cleanup;
    }

    if (!vport) {
        nlError = NL_ERROR_NODEV;
        NdisReleaseRWLock(gOvsSwitchContext->dispatchLock, &lockState);
        goto Cleanup;
    }

    status = OvsCreateMsgFromVport(vport, msgIn, usrParamsCtx->outputBuffer,
                                   usrParamsCtx->outputLength,
                                   gOvsSwitchContext->dpNo);
    NdisReleaseRWLock(gOvsSwitchContext->dispatchLock, &lockState);

    *replyLen = msgOut->nlMsg.nlmsgLen;

Cleanup:
    if (nlError != NL_ERROR_SUCCESS) {
        POVS_MESSAGE_ERROR msgError = (POVS_MESSAGE_ERROR)
            usrParamsCtx->outputBuffer;

        BuildErrorMsg(msgIn, msgError, nlError);
        *replyLen = msgError->nlMsg.nlmsgLen;
    }

    return STATUS_SUCCESS;
}

/*
 * --------------------------------------------------------------------------
 *  Handler for the get vport command. The function handles the initial call to
 *  setup the dump state, as well as subsequent calls to continue dumping data.
 * --------------------------------------------------------------------------
*/
static NTSTATUS
OvsGetVportCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                      UINT32 *replyLen)
{
    *replyLen = 0;

    switch (usrParamsCtx->devOp)
    {
    case OVS_WRITE_DEV_OP:
        return OvsSetupDumpStart(usrParamsCtx);

    case OVS_READ_DEV_OP:
        return OvsGetVportDumpNext(usrParamsCtx, replyLen);

    case OVS_TRANSACTION_DEV_OP:
        return OvsGetVport(usrParamsCtx, replyLen);

    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }

}



static UINT32
OvsComputeVportNo(POVS_SWITCH_CONTEXT switchContext)
{
    /* we are not allowed to create the port OVS_DPPORT_NUMBER_LOCAL */
    for (ULONG i = OVS_DPPORT_NUMBER_LOCAL + 1; i < MAXUINT16; ++i) {
        POVS_VPORT_ENTRY vport;

        vport = OvsFindVportByPortNo(switchContext, i);
        if (!vport) {
            return i;
        }
    }

    return OVS_DPPORT_NUMBER_INVALID;
}

/*
 * --------------------------------------------------------------------------
 *  Command Handler for 'OVS_VPORT_CMD_NEW'.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
OvsNewVportCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                      UINT32 *replyLen)
{
    NDIS_STATUS status = STATUS_SUCCESS;
    LOCK_STATE_EX lockState;

    NL_ERROR nlError = NL_ERROR_SUCCESS;
    POVS_MESSAGE msgIn = (POVS_MESSAGE)usrParamsCtx->inputBuffer;
    POVS_MESSAGE msgOut = (POVS_MESSAGE)usrParamsCtx->outputBuffer;
    POVS_VPORT_ENTRY vport = NULL;
    PCHAR portName;
    ULONG portNameLen;
    UINT32 portType;
    BOOLEAN isBridgeInternal = FALSE;
    BOOLEAN vportAllocated = FALSE, vportInitialized = FALSE;
    BOOLEAN addInternalPortAsNetdev = FALSE;

    static const NL_POLICY ovsVportPolicy[] = {
        [OVS_VPORT_ATTR_PORT_NO] = { .type = NL_A_U32, .optional = TRUE },
        [OVS_VPORT_ATTR_TYPE] = { .type = NL_A_U32, .optional = FALSE },
        [OVS_VPORT_ATTR_NAME] = { .type = NL_A_STRING, .maxLen = IFNAMSIZ,
                                  .optional = FALSE},
        [OVS_VPORT_ATTR_UPCALL_PID] = { .type = NL_A_UNSPEC,
                                        .optional = FALSE },
        [OVS_VPORT_ATTR_OPTIONS] = { .type = NL_A_NESTED, .optional = TRUE },
    };

    PNL_ATTR vportAttrs[ARRAY_SIZE(ovsVportPolicy)];

    /* input buffer has been validated while validating write dev op. */
    ASSERT(usrParamsCtx->inputBuffer != NULL);

    /* Output buffer has been validated while validating transact dev op. */
    ASSERT(msgOut != NULL && usrParamsCtx->outputLength >= sizeof *msgOut);

    if (!NlAttrParse((PNL_MSG_HDR)msgIn,
        NLMSG_HDRLEN + GENL_HDRLEN + OVS_HDRLEN,
        NlMsgAttrsLen((PNL_MSG_HDR)msgIn),
        ovsVportPolicy, vportAttrs, ARRAY_SIZE(vportAttrs))) {
        return STATUS_INVALID_PARAMETER;
    }

    portName = NlAttrGet(vportAttrs[OVS_VPORT_ATTR_NAME]);
    portNameLen = NlAttrGetSize(vportAttrs[OVS_VPORT_ATTR_NAME]);
    portType = NlAttrGetU32(vportAttrs[OVS_VPORT_ATTR_TYPE]);

    /* we are expecting null terminated strings to be passed */
    ASSERT(portName[portNameLen - 1] == '\0');

    NdisAcquireRWLockWrite(gOvsSwitchContext->dispatchLock, &lockState, 0);

    vport = OvsFindVportByOvsName(gOvsSwitchContext, portName);
    if (vport) {
        nlError = NL_ERROR_EXIST;
        goto Cleanup;
    }

    if (portName && portType == OVS_VPORT_TYPE_NETDEV &&
        !strcmp(OVS_DPPORT_INTERNAL_NAME_A, portName)) {
        addInternalPortAsNetdev = TRUE;
    }

    if (portName && portType == OVS_VPORT_TYPE_INTERNAL &&
        strcmp(OVS_DPPORT_INTERNAL_NAME_A, portName)) {
        isBridgeInternal = TRUE;
    }

    if (portType == OVS_VPORT_TYPE_INTERNAL && !isBridgeInternal) {
        vport = gOvsSwitchContext->internalVport;
    } else if (portType == OVS_VPORT_TYPE_NETDEV) {
        /* External ports can also be looked up like VIF ports. */
        vport = OvsFindVportByHvNameA(gOvsSwitchContext, portName);
    } else {
        ASSERT(OvsIsTunnelVportType(portType) ||
               (portType == OVS_VPORT_TYPE_INTERNAL && isBridgeInternal));
        ASSERT(OvsGetTunnelVport(gOvsSwitchContext, portType) == NULL ||
               !OvsIsTunnelVportType(portType));

        vport = (POVS_VPORT_ENTRY)OvsAllocateVport();
        if (vport == NULL) {
            nlError = NL_ERROR_NOMEM;
            goto Cleanup;
        }
        vportAllocated = TRUE;

        if (OvsIsTunnelVportType(portType)) {
            status = OvsInitTunnelVport(vport, portType, VXLAN_UDP_PORT);
            nlError = NlMapStatusToNlErr(status);
        } else {
            OvsInitBridgeInternalVport(vport);
        }
        vportInitialized = TRUE;

        if (nlError == NL_ERROR_SUCCESS) {
            vport->ovsState = OVS_STATE_CONNECTED;
            vport->nicState = NdisSwitchNicStateConnected;

            /*
             * Allow the vport to be deleted, because there is no
             * corresponding hyper-v switch part.
             */
            vport->hvDeleted = TRUE;
        }
    }

    if (!vport) {
        nlError = NL_ERROR_INVAL;
        goto Cleanup;
    }
    if (vport->portNo != OVS_DPPORT_NUMBER_INVALID) {
        nlError = NL_ERROR_EXIST;
        goto Cleanup;
    }

    /* Initialize the vport with OVS specific properties. */
    if (addInternalPortAsNetdev != TRUE) {
        vport->ovsType = portType;
    }
    if (vportAttrs[OVS_VPORT_ATTR_PORT_NO] != NULL) {
        /*
         * XXX: when we implement the limit for ovs port number to be
         * MAXUINT16, we'll need to check the port number received from the
         * userspace.
         */
        vport->portNo = NlAttrGetU32(vportAttrs[OVS_VPORT_ATTR_PORT_NO]);
    } else {
        vport->portNo = OvsComputeVportNo(gOvsSwitchContext);
        if (vport->portNo == OVS_DPPORT_NUMBER_INVALID) {
            nlError = NL_ERROR_NOMEM;
            goto Cleanup;
        }
    }

    /* The ovs port name must be uninitialized. */
    ASSERT(vport->ovsName[0] == '\0');
    ASSERT(portNameLen <= OVS_MAX_PORT_NAME_LENGTH);

    RtlCopyMemory(vport->ovsName, portName, portNameLen);
    /* if we don't have options, then vport->portOptions will be NULL */
    vport->portOptions = vportAttrs[OVS_VPORT_ATTR_OPTIONS];

    /*
     * XXX: when we implement OVS_DP_ATTR_USER_FEATURES in datapath,
     * we'll need to check the OVS_DP_F_VPORT_PIDS flag: if it is set,
     * it means we have an array of pids, instead of a single pid.
     * ATM we assume we have one pid only.
     */
    vport->upcallPid = NlAttrGetU32(vportAttrs[OVS_VPORT_ATTR_UPCALL_PID]);

    status = InitOvsVportCommon(gOvsSwitchContext, vport);
    ASSERT(status == STATUS_SUCCESS);

    status = OvsCreateMsgFromVport(vport, msgIn, usrParamsCtx->outputBuffer,
                                   usrParamsCtx->outputLength,
                                   gOvsSwitchContext->dpNo);

    *replyLen = msgOut->nlMsg.nlmsgLen;

Cleanup:
    NdisReleaseRWLock(gOvsSwitchContext->dispatchLock, &lockState);

    if (nlError != NL_ERROR_SUCCESS) {
        POVS_MESSAGE_ERROR msgError = (POVS_MESSAGE_ERROR)
            usrParamsCtx->outputBuffer;

        if (vport && vportAllocated == TRUE) {
            if (vportInitialized == TRUE) {
                if (OvsIsTunnelVportType(portType)) {
                    OvsCleanupVxlanTunnel(vport);
                }
            }
            OvsFreeMemory(vport);
        }

        BuildErrorMsg(msgIn, msgError, nlError);
        *replyLen = msgError->nlMsg.nlmsgLen;
    }

    return STATUS_SUCCESS;
}


/*
 * --------------------------------------------------------------------------
 *  Command Handler for 'OVS_VPORT_CMD_SET'.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
OvsSetVportCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                      UINT32 *replyLen)
{
    NDIS_STATUS status = STATUS_SUCCESS;
    LOCK_STATE_EX lockState;

    POVS_MESSAGE msgIn = (POVS_MESSAGE)usrParamsCtx->inputBuffer;
    POVS_MESSAGE msgOut = (POVS_MESSAGE)usrParamsCtx->outputBuffer;
    POVS_VPORT_ENTRY vport = NULL;
    NL_ERROR nlError = NL_ERROR_SUCCESS;

    static const NL_POLICY ovsVportPolicy[] = {
        [OVS_VPORT_ATTR_PORT_NO] = { .type = NL_A_U32, .optional = TRUE },
        [OVS_VPORT_ATTR_TYPE] = { .type = NL_A_U32, .optional = TRUE },
        [OVS_VPORT_ATTR_NAME] = { .type = NL_A_STRING, .maxLen = IFNAMSIZ,
                                  .optional = TRUE },
        [OVS_VPORT_ATTR_UPCALL_PID] = { .type = NL_A_UNSPEC,
                                        .optional = TRUE },
        [OVS_VPORT_ATTR_STATS] = { .type = NL_A_UNSPEC,
                                   .minLen = sizeof(OVS_VPORT_FULL_STATS),
                                   .maxLen = sizeof(OVS_VPORT_FULL_STATS),
                                   .optional = TRUE },
        [OVS_VPORT_ATTR_OPTIONS] = { .type = NL_A_NESTED, .optional = TRUE },
    };
    PNL_ATTR vportAttrs[ARRAY_SIZE(ovsVportPolicy)];

    ASSERT(usrParamsCtx->inputBuffer != NULL);

    if (!NlAttrParse((PNL_MSG_HDR)msgIn,
        NLMSG_HDRLEN + GENL_HDRLEN + OVS_HDRLEN,
        NlMsgAttrsLen((PNL_MSG_HDR)msgIn),
        ovsVportPolicy, vportAttrs, ARRAY_SIZE(vportAttrs))) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Output buffer has been validated while validating transact dev op. */
    ASSERT(msgOut != NULL && usrParamsCtx->outputLength >= sizeof *msgOut);

    OvsAcquireCtrlLock();

    NdisAcquireRWLockWrite(gOvsSwitchContext->dispatchLock, &lockState, 0);
    if (vportAttrs[OVS_VPORT_ATTR_NAME] != NULL) {
        PSTR portName = NlAttrGet(vportAttrs[OVS_VPORT_ATTR_NAME]);
#ifdef DBG
        UINT32 portNameLen = NlAttrGetSize(vportAttrs[OVS_VPORT_ATTR_NAME]);
#endif
        /* the port name is expected to be null-terminated */
        ASSERT(portName[portNameLen - 1] == '\0');

        vport = OvsFindVportByOvsName(gOvsSwitchContext, portName);
    } else if (vportAttrs[OVS_VPORT_ATTR_PORT_NO] != NULL) {
        vport = OvsFindVportByPortNo(gOvsSwitchContext,
                    NlAttrGetU32(vportAttrs[OVS_VPORT_ATTR_PORT_NO]));
    }

    if (!vport) {
        nlError = NL_ERROR_NODEV;
        goto Cleanup;
    }

    /*
     * XXX: when we implement OVS_DP_ATTR_USER_FEATURES in datapath,
     * we'll need to check the OVS_DP_F_VPORT_PIDS flag: if it is set,
     * it means we have an array of pids, instead of a single pid.
     * Currently, we support only one pid.
     */
    if (vportAttrs[OVS_VPORT_ATTR_UPCALL_PID]) {
        vport->upcallPid = NlAttrGetU32(vportAttrs[OVS_VPORT_ATTR_UPCALL_PID]);
    }

    if (vportAttrs[OVS_VPORT_ATTR_TYPE]) {
        OVS_VPORT_TYPE type = NlAttrGetU32(vportAttrs[OVS_VPORT_ATTR_TYPE]);
        if (type != vport->ovsType) {
            nlError = NL_ERROR_INVAL;
            goto Cleanup;
        }
    }

    if (vportAttrs[OVS_VPORT_ATTR_OPTIONS]) {
        OVS_LOG_ERROR("Vport options not supported");
        nlError = NL_ERROR_NOTSUPP;
        goto Cleanup;
    }

    status = OvsCreateMsgFromVport(vport, msgIn, usrParamsCtx->outputBuffer,
                                   usrParamsCtx->outputLength,
                                   gOvsSwitchContext->dpNo);

    *replyLen = msgOut->nlMsg.nlmsgLen;

Cleanup:
    NdisReleaseRWLock(gOvsSwitchContext->dispatchLock, &lockState);
    OvsReleaseCtrlLock();

    if (nlError != NL_ERROR_SUCCESS) {
        POVS_MESSAGE_ERROR msgError = (POVS_MESSAGE_ERROR)
            usrParamsCtx->outputBuffer;

        BuildErrorMsg(msgIn, msgError, nlError);
        *replyLen = msgError->nlMsg.nlmsgLen;
    }

    return STATUS_SUCCESS;
}

/*
 * --------------------------------------------------------------------------
 *  Command Handler for 'OVS_VPORT_CMD_DEL'.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
OvsDeleteVportCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                         UINT32 *replyLen)
{
    NDIS_STATUS status = STATUS_SUCCESS;
    LOCK_STATE_EX lockState;

    POVS_MESSAGE msgIn = (POVS_MESSAGE)usrParamsCtx->inputBuffer;
    POVS_MESSAGE msgOut = (POVS_MESSAGE)usrParamsCtx->outputBuffer;
    POVS_VPORT_ENTRY vport = NULL;
    NL_ERROR nlError = NL_ERROR_SUCCESS;
    PSTR portName = NULL;
    UINT32 portNameLen = 0;

    static const NL_POLICY ovsVportPolicy[] = {
        [OVS_VPORT_ATTR_PORT_NO] = { .type = NL_A_U32, .optional = TRUE },
        [OVS_VPORT_ATTR_NAME] = { .type = NL_A_STRING, .maxLen = IFNAMSIZ,
                                  .optional = TRUE },
    };
    PNL_ATTR vportAttrs[ARRAY_SIZE(ovsVportPolicy)];

    ASSERT(usrParamsCtx->inputBuffer != NULL);

    if (!NlAttrParse((PNL_MSG_HDR)msgIn,
        NLMSG_HDRLEN + GENL_HDRLEN + OVS_HDRLEN,
        NlMsgAttrsLen((PNL_MSG_HDR)msgIn),
        ovsVportPolicy, vportAttrs, ARRAY_SIZE(vportAttrs))) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Output buffer has been validated while validating transact dev op. */
    ASSERT(msgOut != NULL && usrParamsCtx->outputLength >= sizeof *msgOut);

    NdisAcquireRWLockWrite(gOvsSwitchContext->dispatchLock, &lockState, 0);
    if (vportAttrs[OVS_VPORT_ATTR_NAME] != NULL) {
        portName = NlAttrGet(vportAttrs[OVS_VPORT_ATTR_NAME]);
        portNameLen = NlAttrGetSize(vportAttrs[OVS_VPORT_ATTR_NAME]);

        /* the port name is expected to be null-terminated */
        ASSERT(portName[portNameLen - 1] == '\0');

        vport = OvsFindVportByOvsName(gOvsSwitchContext, portName);
    }
    else if (vportAttrs[OVS_VPORT_ATTR_PORT_NO] != NULL) {
        vport = OvsFindVportByPortNo(gOvsSwitchContext,
            NlAttrGetU32(vportAttrs[OVS_VPORT_ATTR_PORT_NO]));
    }

    if (!vport) {
        nlError = NL_ERROR_NODEV;
        goto Cleanup;
    }

    status = OvsCreateMsgFromVport(vport, msgIn, usrParamsCtx->outputBuffer,
                                   usrParamsCtx->outputLength,
                                   gOvsSwitchContext->dpNo);

    /*
     * Mark the port as deleted from OVS userspace. If the port does not exist
     * on the Hyper-V switch, it gets deallocated. Otherwise, it stays.
     */
    OvsRemoveAndDeleteVport(gOvsSwitchContext, vport, FALSE, TRUE, NULL);

    *replyLen = msgOut->nlMsg.nlmsgLen;

Cleanup:
    NdisReleaseRWLock(gOvsSwitchContext->dispatchLock, &lockState);

    if (nlError != NL_ERROR_SUCCESS) {
        POVS_MESSAGE_ERROR msgError = (POVS_MESSAGE_ERROR)
            usrParamsCtx->outputBuffer;

        BuildErrorMsg(msgIn, msgError, nlError);
        *replyLen = msgError->nlMsg.nlmsgLen;
    }

    return STATUS_SUCCESS;
}


/*
 * --------------------------------------------------------------------------
 *  Utility function to map the output buffer in an IRP. The buffer is assumed
 *  to have been passed down using METHOD_OUT_DIRECT (Direct I/O).
 * --------------------------------------------------------------------------
 */
static NTSTATUS
MapIrpOutputBuffer(PIRP irp,
                   UINT32 bufferLength,
                   UINT32 requiredLength,
                   PVOID *buffer)
{
    ASSERT(irp);
    ASSERT(buffer);
    ASSERT(bufferLength);
    ASSERT(requiredLength);
    if (!buffer || !irp || bufferLength == 0 || requiredLength == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (bufferLength < requiredLength) {
        return STATUS_NDIS_INVALID_LENGTH;
    }
    if (irp->MdlAddress == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *buffer = MmGetSystemAddressForMdlSafe(irp->MdlAddress,
                                           NormalPagePriority);
    if (*buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
}

/*
 * --------------------------------------------------------------------------
 * Utility function to fill up information about the state of a port in a reply
 * to* userspace.
 * Assumes that 'gOvsCtrlLock' lock is acquired.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
OvsPortFillInfo(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                POVS_EVENT_ENTRY eventEntry,
                PNL_BUFFER nlBuf)
{
    NTSTATUS status;
    BOOLEAN rc;
    OVS_MESSAGE msgOutTmp;
    PNL_MSG_HDR nlMsg;
    POVS_VPORT_ENTRY vport;

    ASSERT(NlBufAt(nlBuf, 0, 0) != 0 && nlBuf->bufRemLen >= sizeof msgOutTmp);

    msgOutTmp.nlMsg.nlmsgType = OVS_WIN_NL_VPORT_FAMILY_ID;
    msgOutTmp.nlMsg.nlmsgFlags = 0;  /* XXX: ? */

    /* driver intiated messages should have zerp seq number*/
    msgOutTmp.nlMsg.nlmsgSeq = 0;
    msgOutTmp.nlMsg.nlmsgPid = usrParamsCtx->ovsInstance->pid;

    msgOutTmp.genlMsg.version = nlVportFamilyOps.version;
    msgOutTmp.genlMsg.reserved = 0;

    /* we don't have netdev yet, treat link up/down a adding/removing a port*/
    if (eventEntry->status & (OVS_EVENT_LINK_UP | OVS_EVENT_CONNECT)) {
        msgOutTmp.genlMsg.cmd = OVS_VPORT_CMD_NEW;
    } else if (eventEntry->status &
             (OVS_EVENT_LINK_DOWN | OVS_EVENT_DISCONNECT)) {
        msgOutTmp.genlMsg.cmd = OVS_VPORT_CMD_DEL;
    } else {
        ASSERT(FALSE);
        return STATUS_UNSUCCESSFUL;
    }
    msgOutTmp.ovsHdr.dp_ifindex = gOvsSwitchContext->dpNo;

    rc = NlMsgPutHead(nlBuf, (PCHAR)&msgOutTmp, sizeof msgOutTmp);
    if (!rc) {
        status = STATUS_INVALID_BUFFER_SIZE;
        goto cleanup;
    }

    vport = OvsFindVportByPortNo(gOvsSwitchContext, eventEntry->portNo);
    if (!vport) {
        status = STATUS_DEVICE_DOES_NOT_EXIST;
        goto cleanup;
    }

    rc = NlMsgPutTailU32(nlBuf, OVS_VPORT_ATTR_PORT_NO, eventEntry->portNo) ||
         NlMsgPutTailU32(nlBuf, OVS_VPORT_ATTR_TYPE, vport->ovsType) ||
         NlMsgPutTailString(nlBuf, OVS_VPORT_ATTR_NAME, vport->ovsName);
    if (!rc) {
        status = STATUS_INVALID_BUFFER_SIZE;
        goto cleanup;
    }

    /* XXXX Should we add the port stats attributes?*/
    nlMsg = (PNL_MSG_HDR)NlBufAt(nlBuf, 0, 0);
    nlMsg->nlmsgLen = NlBufSize(nlBuf);
    status = STATUS_SUCCESS;

cleanup:
    return status;
}


/*
 * --------------------------------------------------------------------------
 * Handler for reading events from the driver event queue. This handler is
 * executed when user modes issues a socket receive on a socket assocaited
 * with the MC group for events.
 * XXX user mode should read multiple events in one system call
 * --------------------------------------------------------------------------
 */
static NTSTATUS
OvsReadEventCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                       UINT32 *replyLen)
{
#ifdef DBG
    POVS_MESSAGE msgOut = (POVS_MESSAGE)usrParamsCtx->outputBuffer;
    POVS_OPEN_INSTANCE instance =
        (POVS_OPEN_INSTANCE)usrParamsCtx->ovsInstance;
#endif
    NL_BUFFER nlBuf;
    NTSTATUS status;
    OVS_EVENT_ENTRY eventEntry;

    ASSERT(usrParamsCtx->devOp == OVS_READ_DEV_OP);

    /* Should never read events with a dump socket */
    ASSERT(instance->dumpState.ovsMsg == NULL);

    /* Must have an event queue */
    ASSERT(instance->eventQueue != NULL);

    /* Output buffer has been validated while validating read dev op. */
    ASSERT(msgOut != NULL && usrParamsCtx->outputLength >= sizeof *msgOut);

    NlBufInit(&nlBuf, usrParamsCtx->outputBuffer, usrParamsCtx->outputLength);

    OvsAcquireCtrlLock();

    /* remove an event entry from the event queue */
    status = OvsRemoveEventEntry(usrParamsCtx->ovsInstance, &eventEntry);
    if (status != STATUS_SUCCESS) {
        /* If there were not elements, read should return no data. */
        status = STATUS_SUCCESS;
        *replyLen = 0;
        goto cleanup;
    }

    status = OvsPortFillInfo(usrParamsCtx, &eventEntry, &nlBuf);
    if (status == NDIS_STATUS_SUCCESS) {
        *replyLen = NlBufSize(&nlBuf);
    }

cleanup:
    OvsReleaseCtrlLock();
    return status;
}

/*
 * --------------------------------------------------------------------------
 * Handler for reading missed pacckets from the driver event queue. This
 * handler is executed when user modes issues a socket receive on a socket
 * --------------------------------------------------------------------------
 */
static NTSTATUS
OvsReadPacketCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                       UINT32 *replyLen)
{
#ifdef DBG
    POVS_MESSAGE msgOut = (POVS_MESSAGE)usrParamsCtx->outputBuffer;
#endif
    POVS_OPEN_INSTANCE instance =
        (POVS_OPEN_INSTANCE)usrParamsCtx->ovsInstance;
    NTSTATUS status;

    ASSERT(usrParamsCtx->devOp == OVS_READ_DEV_OP);

    /* Should never read events with a dump socket */
    ASSERT(instance->dumpState.ovsMsg == NULL);

    /* Must have an packet queue */
    ASSERT(instance->packetQueue != NULL);

    /* Output buffer has been validated while validating read dev op. */
    ASSERT(msgOut != NULL && usrParamsCtx->outputLength >= sizeof *msgOut);

    /* Read a packet from the instance queue */
    status = OvsReadDpIoctl(instance->fileObject, usrParamsCtx->outputBuffer,
                            usrParamsCtx->outputLength, replyLen);
    return status;
}

/*
 * --------------------------------------------------------------------------
 *  Handler for the subscription for a packet queue
 * --------------------------------------------------------------------------
 */
static NTSTATUS
OvsSubscribePacketCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                            UINT32 *replyLen)
{
    NDIS_STATUS status;
    BOOLEAN rc;
    UINT8 join;
    UINT32 pid;
    const NL_POLICY policy[] =  {
        [OVS_NL_ATTR_PACKET_PID] = {.type = NL_A_U32 },
        [OVS_NL_ATTR_PACKET_SUBSCRIBE] = {.type = NL_A_U8 }
        };
    PNL_ATTR attrs[ARRAY_SIZE(policy)];

    UNREFERENCED_PARAMETER(replyLen);

    POVS_OPEN_INSTANCE instance =
        (POVS_OPEN_INSTANCE)usrParamsCtx->ovsInstance;
    POVS_MESSAGE msgIn = (POVS_MESSAGE)usrParamsCtx->inputBuffer;

    rc = NlAttrParse(&msgIn->nlMsg, sizeof (*msgIn),
         NlMsgAttrsLen((PNL_MSG_HDR)msgIn), policy, attrs, ARRAY_SIZE(attrs));
    if (!rc) {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }

    join = NlAttrGetU8(attrs[OVS_NL_ATTR_PACKET_PID]);
    pid = NlAttrGetU32(attrs[OVS_NL_ATTR_PACKET_PID]);

    /* The socket subscribed with must be the same socket we perform receive*/
    ASSERT(pid == instance->pid);

    status = OvsSubscribeDpIoctl(instance, pid, join);

    /*
     * XXX Need to add this instance to a global data structure
     * which hold all packet based instances. The data structure (hash)
     * should be searched through the pid field of the instance for
     * placing the missed packet into the correct queue
     */
done:
    return status;
}

/*
 * --------------------------------------------------------------------------
 * Handler for queueing an IRP used for missed packet notification. The IRP is
 * completed when a packet received and mismatched. STATUS_PENDING is returned
 * on success. User mode keep a pending IRP at all times.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
OvsPendPacketCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                       UINT32 *replyLen)
{
    UNREFERENCED_PARAMETER(replyLen);

    POVS_OPEN_INSTANCE instance =
        (POVS_OPEN_INSTANCE)usrParamsCtx->ovsInstance;

    /*
     * XXX access to packet queue must be through acquiring a lock as user mode
     * could unsubscribe and the instnace will be freed.
     */
    return OvsWaitDpIoctl(usrParamsCtx->irp, instance->fileObject);
}
