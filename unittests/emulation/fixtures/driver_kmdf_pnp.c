//===- driver_kmdf_pnp.c - Genuine WDK PnP framework fixture ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A resource-free KMDF FDO and explicit non-power-managed I/O queue, linked
/// through the genuine WDK entry library. The service suffix F returns an
/// AddDevice failure after creating the FDO to exercise framework cleanup.
///
//===----------------------------------------------------------------------===//

#include <ntifs.h>
#include <wdf.h>

#define ABI_SLOT(Name, Index)                                                  \
  _Static_assert(Name##TableIndex == Index, #Name " table slot")

#define IOCTL_NEVERD_PNP                                                       \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

enum { ResponseSize = 4 };

ABI_SLOT(WdfDeviceWdmGetPhysicalDevice, 33);
ABI_SLOT(WdfDeviceCreate, 75);
ABI_SLOT(WdfDriverCreate, 116);
ABI_SLOT(WdfFdoInitWdmGetPhysicalDevice, 124);
ABI_SLOT(WdfIoQueueCreate, 152);
ABI_SLOT(WdfRequestCompleteWithInformation, 265);
ABI_SLOT(WdfRequestRetrieveInputBuffer, 269);
ABI_SLOT(WdfRequestRetrieveOutputBuffer, 270);

static WCHAR ServiceMode;

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

static VOID IoControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength,
                      size_t InputLength, ULONG Code) {
  UCHAR *Input;
  UCHAR *Output;
  UCHAR Value;
  NTSTATUS Status;
  UNREFERENCED_PARAMETER(Queue);
  if (Code != IOCTL_NEVERD_PNP || InputLength != 1 ||
      OutputLength < ResponseSize) {
    WdfRequestCompleteWithInformation(Request, STATUS_INVALID_PARAMETER, 0);
    return;
  }
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
  WDFDEVICE Device = NULL;
  WDFQUEUE Queue = NULL;
  PDEVICE_OBJECT PDO;
  NTSTATUS Status;
  UNREFERENCED_PARAMETER(Driver);
  PDO = WdfFdoInitWdmGetPhysicalDevice(Init);
  if (PDO == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
    return STATUS_INVALID_DEVICE_STATE;
  WdfDeviceInitSetIoType(Init, WdfDeviceIoBuffered);
  WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
  Attributes.ExecutionLevel = WdfExecutionLevelPassive;
  Attributes.SynchronizationScope = WdfSynchronizationScopeNone;
  Attributes.EvtCleanupCallback = DeviceCleanup;
  Attributes.EvtDestroyCallback = DeviceDestroy;
  Status = WdfDeviceCreate(&Init, &Attributes, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  if (WdfDeviceWdmGetPhysicalDevice(Device) != PDO)
    return STATUS_INVALID_DEVICE_STATE;
  if (ServiceMode == L'F') {
    DbgPrint("KMDF PnP: failing AddDevice\n");
    return STATUS_UNSUCCESSFUL;
  }
  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig,
                                         WdfIoQueueDispatchSequential);
  QueueConfig.PowerManaged = WdfFalse;
  QueueConfig.EvtIoDeviceControl = IoControl;
  WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
  Attributes.ExecutionLevel = WdfExecutionLevelPassive;
  Attributes.SynchronizationScope = WdfSynchronizationScopeNone;
  Status = WdfIoQueueCreate(Device, &QueueConfig, &Attributes, &Queue);
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
  WDF_DRIVER_CONFIG_INIT(&Config, DeviceAdd);
  if (ServiceMode != L'N')
    Config.EvtDriverUnload = DriverUnload;
  return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                         &Config, WDF_NO_HANDLE);
}
