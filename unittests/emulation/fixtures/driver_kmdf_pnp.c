//===- driver_kmdf_pnp.c - Genuine WDK PnP framework fixture ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A KMDF FDO and default I/O queue, linked through the genuine
/// WDK entry library. Service suffixes M and T use default and explicit power
/// management; C completes a held request from EvtIoStop, A requeues it,
/// V resumes it, G purges it on surprise removal, B/D complete it from a
/// worker without/with EvtIoStop, E/Z leave one without a completion producer
/// with/without EvtIoStop, R consumes one assigned memory resource, L leaves
/// its mapping live, W attempts an invalid descriptor write, and F fails
/// AddDevice.
///
//===----------------------------------------------------------------------===//

#include <ntifs.h>
#include <wdf.h>

#define ABI_SLOT(Name, Index)                                                  \
  _Static_assert(Name##TableIndex == Index, #Name " table slot")

#define IOCTL_NEVERD_PNP                                                       \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

enum { ResponseSize = 4 };
static const ULONGLONG RawMemoryStart = 0x200000000ULL;
static const ULONGLONG TranslatedMemoryStart = 0x300000000ULL;
static const ULONG MemoryLength = 0x1000;
static const ULONG InitialRegisterValue = 0x12345678;

ABI_SLOT(WdfDeviceWdmGetPhysicalDevice, 33);
ABI_SLOT(WdfDeviceWdmGetAttachedDevice, 32);
ABI_SLOT(WdfWdmDeviceGetWdfDeviceHandle, 30);
ABI_SLOT(WdfDeviceGetDriver, 39);
ABI_SLOT(WdfDeviceCreate, 75);
ABI_SLOT(WdfDeviceInitSetPnpPowerEventCallbacks, 55);
ABI_SLOT(WdfCmResourceListGetCount, 304);
ABI_SLOT(WdfCmResourceListGetDescriptor, 305);
ABI_SLOT(WdfDriverCreate, 116);
ABI_SLOT(WdfFdoInitWdmGetPhysicalDevice, 124);
ABI_SLOT(WdfIoQueueCreate, 152);
ABI_SLOT(WdfIoQueueGetState, 153);
ABI_SLOT(WdfRequestCompleteWithInformation, 265);
ABI_SLOT(WdfRequestStopAcknowledge, 284);
ABI_SLOT(WdfRequestRetrieveInputBuffer, 269);
ABI_SLOT(WdfRequestRetrieveOutputBuffer, 270);

static WCHAR ServiceMode;
static WDFQUEUE PowerQueue;
static PVOID MappedResource;
static ULONG DeliveryCount;
static PIO_WORKITEM PendingWorkItem;

static BOOLEAN UsesPowerQueue(VOID) {
  return ServiceMode == L'M' || ServiceMode == L'T' || ServiceMode == L'C' ||
         ServiceMode == L'A' || ServiceMode == L'V' || ServiceMode == L'G' ||
         ServiceMode == L'B' || ServiceMode == L'D' || ServiceMode == L'E' ||
         ServiceMode == L'Z';
}

static BOOLEAN UsesAssignedMemory(VOID) {
  return ServiceMode == L'R' || ServiceMode == L'L' || ServiceMode == L'W';
}

static BOOLEAN QueueIsPowerHeld(BOOLEAN Expected) {
  WDF_IO_QUEUE_STATE State;
  if (PowerQueue == NULL)
    return FALSE;
  State = WdfIoQueueGetState(PowerQueue, NULL, NULL);
  return !!(State & WdfIoQueuePnpHeld) == !!Expected;
}

_Static_assert(sizeof(WDF_PNPPOWER_EVENT_CALLBACKS) == 144,
               "KMDF 1.33 PnP callback layout");

static NTSTATUS DeviceD0Entry(WDFDEVICE Device,
                              WDF_POWER_DEVICE_STATE PreviousState) {
  UNREFERENCED_PARAMETER(Device);
  if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
      PreviousState != WdfPowerDeviceD3Final ||
      (UsesPowerQueue() && !QueueIsPowerHeld(TRUE)))
    return STATUS_INVALID_DEVICE_STATE;
  DbgPrint("KMDF PnP: D0 entry\n");
  return ServiceMode == L'Q' || ServiceMode == L'J' ? STATUS_UNSUCCESSFUL
                                                    : STATUS_SUCCESS;
}

static NTSTATUS DeviceD0Exit(WDFDEVICE Device,
                             WDF_POWER_DEVICE_STATE TargetState) {
  UNREFERENCED_PARAMETER(Device);
  if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
      TargetState != WdfPowerDeviceD3Final ||
      (UsesPowerQueue() && !QueueIsPowerHeld(TRUE)))
    return STATUS_INVALID_DEVICE_STATE;
  DbgPrint("KMDF PnP: D0 exit\n");
  return STATUS_SUCCESS;
}

static NTSTATUS DevicePrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw,
                                      WDFCMRESLIST Translated) {
  UNREFERENCED_PARAMETER(Device);
  if (UsesAssignedMemory()) {
    PCM_PARTIAL_RESOURCE_DESCRIPTOR RawDescriptor;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR TranslatedDescriptor;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || Raw == NULL ||
        Translated == NULL || Raw == Translated || MappedResource != NULL ||
        WdfCmResourceListGetCount(Raw) != 1 ||
        WdfCmResourceListGetCount(Translated) != 1)
      return STATUS_INVALID_DEVICE_STATE;
    RawDescriptor = WdfCmResourceListGetDescriptor(Raw, 0);
    TranslatedDescriptor = WdfCmResourceListGetDescriptor(Translated, 0);
    if (RawDescriptor == NULL || TranslatedDescriptor == NULL ||
        WdfCmResourceListGetDescriptor(Raw, 1) != NULL ||
        WdfCmResourceListGetDescriptor(Translated, 1) != NULL ||
        RawDescriptor->Type != CmResourceTypeMemory ||
        TranslatedDescriptor->Type != CmResourceTypeMemory ||
        RawDescriptor->u.Memory.Start.QuadPart != RawMemoryStart ||
        TranslatedDescriptor->u.Memory.Start.QuadPart !=
            TranslatedMemoryStart ||
        RawDescriptor->u.Memory.Length != MemoryLength ||
        TranslatedDescriptor->u.Memory.Length != MemoryLength)
      return STATUS_INVALID_DEVICE_STATE;
    if (ServiceMode == L'W') {
      *((volatile UCHAR *)&TranslatedDescriptor->Type) = CmResourceTypePort;
      return STATUS_SUCCESS;
    }
    MappedResource =
        MmMapIoSpace(TranslatedDescriptor->u.Memory.Start,
                     TranslatedDescriptor->u.Memory.Length, MmNonCached);
    if (MappedResource == NULL ||
        READ_REGISTER_ULONG((volatile ULONG *)MappedResource) !=
            InitialRegisterValue)
      return STATUS_INVALID_DEVICE_STATE;
    DbgPrint("KMDF PnP: mapped hardware\n");
    return STATUS_SUCCESS;
  }
  if (KeGetCurrentIrql() != PASSIVE_LEVEL || Raw == NULL ||
      Translated == NULL || Raw == Translated ||
      WdfCmResourceListGetCount(Raw) != 0 ||
      WdfCmResourceListGetCount(Translated) != 0 ||
      WdfCmResourceListGetDescriptor(Raw, 0) != NULL ||
      WdfCmResourceListGetDescriptor(Translated, 0) != NULL)
    return STATUS_INVALID_DEVICE_STATE;
  DbgPrint("KMDF PnP: prepare hardware\n");
  return ServiceMode == L'I' ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

static NTSTATUS DeviceReleaseHardware(WDFDEVICE Device,
                                      WDFCMRESLIST Translated) {
  UNREFERENCED_PARAMETER(Device);
  if (ServiceMode == L'R' || ServiceMode == L'L') {
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || Translated == NULL ||
        MappedResource == NULL || WdfCmResourceListGetCount(Translated) != 1)
      return STATUS_INVALID_DEVICE_STATE;
    Descriptor = WdfCmResourceListGetDescriptor(Translated, 0);
    if (Descriptor == NULL || Descriptor->Type != CmResourceTypeMemory ||
        Descriptor->u.Memory.Length != MemoryLength)
      return STATUS_INVALID_DEVICE_STATE;
    if (ServiceMode == L'L')
      return STATUS_SUCCESS;
    MmUnmapIoSpace(MappedResource, Descriptor->u.Memory.Length);
    MappedResource = NULL;
    DbgPrint("KMDF PnP: unmapped hardware\n");
    return STATUS_SUCCESS;
  }
  if (KeGetCurrentIrql() != PASSIVE_LEVEL || Translated == NULL ||
      WdfCmResourceListGetCount(Translated) != 0 ||
      WdfCmResourceListGetDescriptor(Translated, 0) != NULL)
    return STATUS_INVALID_DEVICE_STATE;
  DbgPrint("KMDF PnP: release hardware\n");
  return STATUS_SUCCESS;
}

static NTSTATUS DeviceSelfManagedIoInit(WDFDEVICE Device) {
  UNREFERENCED_PARAMETER(Device);
  return STATUS_SUCCESS;
}

static VOID DeviceCleanup(WDFOBJECT Object) {
  UNREFERENCED_PARAMETER(Object);
  DbgPrint("KMDF PnP: device cleanup\n");
}

static VOID DeviceDestroy(WDFOBJECT Object) {
  UNREFERENCED_PARAMETER(Object);
  DbgPrint("KMDF PnP: device destroy\n");
}

static VOID DriverUnload(WDFDRIVER Driver) {
  UNREFERENCED_PARAMETER(Driver);
  DbgPrint("KMDF PnP: driver unload\n");
}

static VOID DeviceIoStop(WDFQUEUE Queue, WDFREQUEST Request,
                         ULONG ActionFlags) {
  if (Queue != PowerQueue || !QueueIsPowerHeld(TRUE) ||
      ActionFlags != (ServiceMode == L'G' ? WdfRequestStopActionPurge
                                          : WdfRequestStopActionSuspend))
    return;
  DbgPrint("KMDF PnP: I/O stop\n");
  if (ServiceMode == L'C' || ServiceMode == L'G')
    WdfRequestCompleteWithInformation(Request, STATUS_CANCELLED, 0);
  else if (ServiceMode == L'A' || ServiceMode == L'V')
    WdfRequestStopAcknowledge(Request, ServiceMode == L'A');
}

static VOID DeviceIoResume(WDFQUEUE Queue, WDFREQUEST Request) {
  DbgPrint("KMDF PnP: I/O resume\n");
  WdfRequestCompleteWithInformation(Request,
                                    Queue == PowerQueue && DeliveryCount == 1
                                        ? STATUS_SUCCESS
                                        : STATUS_INVALID_DEVICE_STATE,
                                    0);
}

static VOID CompletePendingIo(PDEVICE_OBJECT Device, PVOID Context) {
  WDFREQUEST Request = (WDFREQUEST)Context;
  PIO_WORKITEM Item = PendingWorkItem;
  (void)Device;
  PendingWorkItem = NULL;
  IoFreeWorkItem(Item);
  WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
}

static VOID IoControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength,
                      size_t InputLength, ULONG Code) {
  UCHAR *Input;
  UCHAR *Output;
  UCHAR Value;
  NTSTATUS Status;
  if (UsesPowerQueue() && (Queue != PowerQueue || !QueueIsPowerHeld(FALSE))) {
    WdfRequestCompleteWithInformation(Request, STATUS_INVALID_DEVICE_STATE, 0);
    return;
  }
  if (Code != IOCTL_NEVERD_PNP || InputLength != 1 ||
      OutputLength < ResponseSize) {
    WdfRequestCompleteWithInformation(Request, STATUS_INVALID_PARAMETER, 0);
    return;
  }
  if (ServiceMode == L'B' || ServiceMode == L'D') {
    PendingWorkItem = IoAllocateWorkItem(
        WdfDeviceWdmGetDeviceObject(WdfIoQueueGetDevice(Queue)));
    if (PendingWorkItem == NULL) {
      WdfRequestCompleteWithInformation(Request, STATUS_INSUFFICIENT_RESOURCES,
                                        0);
      return;
    }
    IoQueueWorkItem(PendingWorkItem, CompletePendingIo, DelayedWorkQueue,
                    Request);
    return;
  }
  if (ServiceMode == L'C' || ServiceMode == L'G' || ServiceMode == L'E' ||
      ServiceMode == L'Z')
    return;
  if ((ServiceMode == L'A' || ServiceMode == L'V') && DeliveryCount++ == 0)
    return;
  Status = WdfRequestRetrieveInputBuffer(Request, 1, (PVOID *)&Input, NULL);
  if (!NT_SUCCESS(Status)) {
    WdfRequestCompleteWithInformation(Request, Status, 0);
    return;
  }
  Status = WdfRequestRetrieveOutputBuffer(Request, ResponseSize,
                                          (PVOID *)&Output, NULL);
  if (!NT_SUCCESS(Status)) {
    WdfRequestCompleteWithInformation(Request, Status, 0);
    return;
  }
  Value = Input[0];
  Output[0] = 'P';
  Output[1] = 'N';
  Output[2] = 'P';
  Output[3] = Value;
  WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, ResponseSize);
}

static NTSTATUS DeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init) {
  WDF_OBJECT_ATTRIBUTES Attributes;
  WDF_IO_QUEUE_CONFIG QueueConfig;
  WDF_PNPPOWER_EVENT_CALLBACKS PnpCallbacks;
  WDFDEVICE Device = NULL;
  WDFQUEUE Queue = NULL;
  PDEVICE_OBJECT PDO;
  PDEVICE_OBJECT FDO;
  NTSTATUS Status;
  PDO = WdfFdoInitWdmGetPhysicalDevice(Init);
  if (PDO == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    return STATUS_INVALID_DEVICE_STATE;
  WdfDeviceInitSetIoType(Init, WdfDeviceIoBuffered);
  if (ServiceMode == L'P' || ServiceMode == L'Q' || UsesAssignedMemory() ||
      UsesPowerQueue() || ServiceMode == L'H' || ServiceMode == L'I' ||
      ServiceMode == L'J' || ServiceMode == L'U') {
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpCallbacks);
    PnpCallbacks.EvtDeviceD0Entry = DeviceD0Entry;
    PnpCallbacks.EvtDeviceD0Exit = DeviceD0Exit;
    if (ServiceMode == L'H' || ServiceMode == L'I' || ServiceMode == L'J' ||
        UsesAssignedMemory()) {
      PnpCallbacks.EvtDevicePrepareHardware = DevicePrepareHardware;
      PnpCallbacks.EvtDeviceReleaseHardware = DeviceReleaseHardware;
    }
    if (ServiceMode == L'U')
      PnpCallbacks.EvtDeviceSelfManagedIoInit = DeviceSelfManagedIoInit;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &PnpCallbacks);
  }
  WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
  Attributes.ExecutionLevel = WdfExecutionLevelPassive;
  Attributes.SynchronizationScope = WdfSynchronizationScopeNone;
  Attributes.EvtCleanupCallback = DeviceCleanup;
  Attributes.EvtDestroyCallback = DeviceDestroy;
  Status = WdfDeviceCreate(&Init, &Attributes, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  FDO = WdfDeviceWdmGetDeviceObject(Device);
  if (WdfDeviceWdmGetPhysicalDevice(Device) != PDO ||
      WdfDeviceWdmGetAttachedDevice(Device) != PDO ||
      WdfWdmDeviceGetWdfDeviceHandle(FDO) != Device ||
      WdfWdmDeviceGetWdfDeviceHandle(PDO) != NULL ||
      WdfDeviceGetDriver(Device) != Driver)
    return STATUS_INVALID_DEVICE_STATE;
  if (ServiceMode == L'F') {
    DbgPrint("KMDF PnP: failing AddDevice\n");
    return STATUS_UNSUCCESSFUL;
  }
  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig,
                                         WdfIoQueueDispatchSequential);
  if (ServiceMode == L'T')
    QueueConfig.PowerManaged = WdfTrue;
  else if (!UsesPowerQueue())
    QueueConfig.PowerManaged = WdfFalse;
  QueueConfig.EvtIoDeviceControl = IoControl;
  if (ServiceMode == L'C' || ServiceMode == L'E' || ServiceMode == L'A' ||
      ServiceMode == L'V' || ServiceMode == L'G' || ServiceMode == L'D')
    QueueConfig.EvtIoStop = DeviceIoStop;
  if (ServiceMode == L'V')
    QueueConfig.EvtIoResume = DeviceIoResume;
  WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
  Attributes.ExecutionLevel = WdfExecutionLevelPassive;
  Attributes.SynchronizationScope = WdfSynchronizationScopeNone;
  Status = WdfIoQueueCreate(Device, &QueueConfig, &Attributes, &Queue);
  if (NT_SUCCESS(Status) && UsesPowerQueue()) {
    PowerQueue = Queue;
    if (!QueueIsPowerHeld(TRUE))
      return STATUS_INVALID_DEVICE_STATE;
  }
  if (NT_SUCCESS(Status) && Queue != NULL)
    DbgPrint("KMDF PnP: device ready\n");
  return Status;
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject,
                     PUNICODE_STRING RegistryPath) {
  WDF_DRIVER_CONFIG Config;
  ServiceMode =
      RegistryPath->Length >= sizeof(WCHAR)
          ? RegistryPath->Buffer[RegistryPath->Length / sizeof(WCHAR) - 1]
          : L'S';
  PowerQueue = NULL;
  MappedResource = NULL;
  DeliveryCount = 0;
  PendingWorkItem = NULL;
  WDF_DRIVER_CONFIG_INIT(&Config, DeviceAdd);
  if (ServiceMode != L'N')
    Config.EvtDriverUnload = DriverUnload;
  return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                         &Config, WDF_NO_HANDLE);
}
