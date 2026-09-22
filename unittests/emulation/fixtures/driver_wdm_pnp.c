//===- driver_wdm_pnp.c - Genuine WDK AddDevice and PnP fixture ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original resource-free WDM function driver built against unmodified WDK
/// headers and libraries. START and queries wait for lower completion with
/// MORE_PROCESSING_REQUIRED; REMOVE forwards then detaches and deletes its FDO.
/// A fails AddDevice after cleanup, L leaks its FDO, F fails START, T rejects
/// its first QUERY_STOP, M fails only the first AddDevice, and C owns an unrelated
/// control device. Every FDO has independent software state. No hardware is used.
///
//===----------------------------------------------------------------------===//

#include <ntddk.h>

enum {
#define NEVERD_DEVICE_POWER_REQUEST(Name, Value) NeverDPower##Name = Value,
#include "../../../include/neverd/emulation/DeviceLifecycle.def"
#undef NEVERD_DEVICE_POWER_REQUEST
};
_Static_assert(NeverDPowerSet == IRP_MN_SET_POWER &&
                   NeverDPowerQuery == IRP_MN_QUERY_POWER,
               "public power request codes match the WDK");

#define ABI_OFFSET(Type, Member, Offset)                                      \
  _Static_assert(__builtin_offsetof(Type, Member) == Offset,                  \
                 #Type "." #Member " x64 ABI")

_Static_assert(sizeof(void *) == 8 && sizeof(IRP) == 0xd0 &&
                   sizeof(IO_STACK_LOCATION) == 0x48,
               "WDM x64 packet ABI");
ABI_OFFSET(DRIVER_EXTENSION, AddDevice, 8);
ABI_OFFSET(IO_STACK_LOCATION, MinorFunction, 1);
ABI_OFFSET(IO_STACK_LOCATION, Parameters.StartDevice.AllocatedResources, 8);
ABI_OFFSET(IO_STACK_LOCATION, Parameters.StartDevice.AllocatedResourcesTranslated,
           16);
ABI_OFFSET(IRP, RequestorMode, 0x40);
ABI_OFFSET(IRP, Tail.Overlay.OriginalFileObject, 0xc0);
_Static_assert(IRP_MJ_PNP == 0x1b && IRP_MN_START_DEVICE == 0 &&
                   IRP_MN_QUERY_REMOVE_DEVICE == 1 &&
                   IRP_MN_REMOVE_DEVICE == 2 && IRP_MN_CANCEL_REMOVE_DEVICE == 3 &&
                   IRP_MN_STOP_DEVICE == 4 && IRP_MN_QUERY_STOP_DEVICE == 5 &&
                   IRP_MN_CANCEL_STOP_DEVICE == 6 &&
                   IRP_MN_SURPRISE_REMOVAL == 0x17,
               "PnP request codes");
_Static_assert(DO_BUS_ENUMERATED_DEVICE == 0x1000, "provider PDO flag");

typedef struct {
  PDEVICE_OBJECT Self;
  PDEVICE_OBJECT PDO;
  PDEVICE_OBJECT Lower;
  KEVENT Event;
  BOOLEAN Started;
  BOOLEAN RemovePending;
  BOOLEAN StopPending;
  BOOLEAN SurpriseRemoved;
  BOOLEAN RejectedStopOnce;
  BOOLEAN Control;
  ULONG Unit;
  ULONG Completions;
} PNP_EXTENSION;

static PDEVICE_OBJECT ControlDevice;
static ULONG AddCalls;
static ULONG LiveFdos;
static UCHAR Mode;

static BOOLEAN Check(BOOLEAN Condition, ULONG Code) {
  if (!Condition)
    DbgPrint("WDM PnP: failure %lu\n", Code);
  return Condition;
}

static NTSTATUS Completion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context) {
  PNP_EXTENSION *Extension = Context;
  if (!Check(Device == Extension->Self && Extension == Device->DeviceExtension &&
                 KeGetCurrentIrql() == PASSIVE_LEVEL &&
                 IoGetCurrentIrpStackLocation(Irp)->DeviceObject == Device &&
                 Irp->IoStatus.Information == 0,
             1))
    return STATUS_MORE_PROCESSING_REQUIRED;
  ++Extension->Completions;
  DbgPrint("WDM PnP: completion status=0x%08lx pending=%u\n",
           (ULONG)Irp->IoStatus.Status, (unsigned)Irp->PendingReturned);
  KeSetEvent(&Extension->Event, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT Device, PIRP Irp) {
  PNP_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  const UCHAR Minor = Stack->MinorFunction;
  NTSTATUS Status;
  if (!Check(!Extension->Control && Device == Extension->Self &&
                 KeGetCurrentIrql() == PASSIVE_LEVEL &&
                 Stack->MajorFunction == IRP_MJ_PNP &&
                 Stack->DeviceObject == Device && Stack->FileObject == NULL &&
                 Irp->Tail.Overlay.OriginalFileObject == NULL &&
                 Irp->RequestorMode == KernelMode && Irp->MdlAddress == NULL &&
                 Irp->AssociatedIrp.SystemBuffer == NULL &&
                 Irp->UserBuffer == NULL,
             2)) {
    Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_UNSUCCESSFUL;
  }
  DbgPrint("WDM PnP: dispatch minor=%u\n", (unsigned)Minor);
  if (Minor == IRP_MN_REMOVE_DEVICE) {
    PDEVICE_OBJECT Lower = Extension->Lower;
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(Lower, Irp);
    // Delayed lower removal may still own the IRP. Do not access it here.
    IoDetachDevice(Lower);
    --LiveFdos;
    IoDeleteDevice(Device);
    DbgPrint("WDM PnP: removed FDO lower=0x%08lx\n", (ULONG)Status);
    return Status;
  }
  if (Minor == IRP_MN_STOP_DEVICE || Minor == IRP_MN_SURPRISE_REMOVAL) {
    Extension->Started = FALSE;
    Extension->StopPending = FALSE;
    if (Minor == IRP_MN_SURPRISE_REMOVAL) {
      Extension->SurpriseRemoved = TRUE;
      DbgPrint("WDM PnP: surprise kept FDO unit=%lu\n", Extension->Unit);
    } else {
      DbgPrint("WDM PnP: stopped unit=%lu\n", Extension->Unit);
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Extension->Lower, Irp);
  }
  if (Minor != IRP_MN_START_DEVICE && Minor != IRP_MN_QUERY_REMOVE_DEVICE &&
      Minor != IRP_MN_CANCEL_REMOVE_DEVICE && Minor != IRP_MN_QUERY_STOP_DEVICE &&
      Minor != IRP_MN_CANCEL_STOP_DEVICE) {
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NOT_SUPPORTED;
  }
  if (Minor == IRP_MN_QUERY_STOP_DEVICE && Mode == 'T' &&
      !Extension->RejectedStopOnce) {
    Extension->RejectedStopOnce = TRUE;
    DbgPrint("WDM PnP: query-stop rejected before bus unit=%lu\n",
             Extension->Unit);
    Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_UNSUCCESSFUL;
  }
  if (Minor == IRP_MN_START_DEVICE &&
      !Check(Stack->Parameters.StartDevice.AllocatedResources == NULL &&
                 Stack->Parameters.StartDevice.AllocatedResourcesTranslated ==
                     NULL,
             3)) {
    Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_PARAMETER;
  }
  if (Minor == IRP_MN_QUERY_STOP_DEVICE)
    Extension->StopPending = TRUE;
  if (Minor == IRP_MN_QUERY_REMOVE_DEVICE)
    Extension->RemovePending = TRUE;
  KeClearEvent(&Extension->Event);
  Extension->Completions = 0;
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, Completion, Extension, TRUE, TRUE, TRUE);
  Status = IoCallDriver(Extension->Lower, Irp);
  DbgPrint("WDM PnP: lower returned=0x%08lx\n", (ULONG)Status);
  if (Status == STATUS_PENDING)
    if (!Check(KeWaitForSingleObject(&Extension->Event, Executive, KernelMode,
                                     FALSE, NULL) == STATUS_SUCCESS,
               4))
      return STATUS_UNSUCCESSFUL;
  if (!Check(Extension->Completions == 1 && Irp->CurrentLocation == 2 &&
                 IoGetCurrentIrpStackLocation(Irp) == Stack,
             5))
    return STATUS_UNSUCCESSFUL;
  Status = Irp->IoStatus.Status;
  if (Minor == IRP_MN_START_DEVICE) {
    if (NT_SUCCESS(Status) && Mode == 'F')
      Status = STATUS_UNSUCCESSFUL;
    Extension->Started = NT_SUCCESS(Status);
    Extension->StopPending = FALSE;
  } else if (Minor == IRP_MN_QUERY_STOP_DEVICE) {
    Extension->StopPending = NT_SUCCESS(Status);
  } else if (Minor == IRP_MN_CANCEL_STOP_DEVICE) {
    Extension->StopPending = FALSE;
  } else if (Minor == IRP_MN_QUERY_REMOVE_DEVICE) {
    Extension->RemovePending = NT_SUCCESS(Status);
  } else {
    Extension->RemovePending = FALSE;
  }
  DbgPrint("WDM PnP: final minor=%u status=0x%08lx\n", (unsigned)Minor,
           (ULONG)Status);
  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

static NTSTATUS DispatchFile(PDEVICE_OBJECT Device, PIRP Irp) {
  PNP_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  NTSTATUS Status = STATUS_SUCCESS;
  ULONG_PTR Information = 0;
  const BOOLEAN Releasing = Stack->MajorFunction == IRP_MJ_CLEANUP ||
                            Stack->MajorFunction == IRP_MJ_CLOSE;
  if (!Check(Device == Extension->Self && Stack->FileObject != NULL &&
                 Stack->FileObject->DeviceObject ==
                     (Extension->Control ? Device : Extension->PDO),
             6))
    Status = STATUS_UNSUCCESSFUL;
  else if (!Releasing && Extension->SurpriseRemoved)
    Status = STATUS_NO_SUCH_DEVICE;
  else if (Stack->MajorFunction == IRP_MJ_CREATE && Extension->RemovePending)
    Status = STATUS_DELETE_PENDING;
  else if (Stack->MajorFunction == IRP_MJ_CREATE && !Extension->Started)
    Status = STATUS_DEVICE_NOT_READY;
  else if (Stack->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
    const ULONG Code = Stack->Parameters.DeviceIoControl.IoControlCode;
    if ((Code != 0x222000 && Code != 0x222004) ||
        Stack->Parameters.DeviceIoControl.OutputBufferLength < 4 ||
        Irp->AssociatedIrp.SystemBuffer == NULL) {
      Status = STATUS_INVALID_PARAMETER;
    } else {
      PUCHAR Buffer = Irp->AssociatedIrp.SystemBuffer;
      // This operation reads only software state. Stopping hardware or asking
      // to remove the device does not make this already-open file inaccessible.
      if (Code == 0x222004) {
        *(PULONG)Buffer = Extension->Unit;
      } else {
        Buffer[0] = Extension->Control ? 'C' : 'P';
        Buffer[1] = Extension->Control ? 'T' : 'N';
        Buffer[2] = Extension->Control ? 'L' : 'P';
        Buffer[3] = '!';
      }
      Information = 4;
    }
  } else if (Stack->MajorFunction == IRP_MJ_READ ||
             Stack->MajorFunction == IRP_MJ_WRITE) {
    Status = STATUS_INVALID_DEVICE_REQUEST;
  }
  DbgPrint("WDM PnP: file major=%u\n", (unsigned)Stack->MajorFunction);
  DbgPrint("WDM PnP: unit=%lu file status=0x%08lx\n", Extension->Unit,
           (ULONG)Status);
  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = Information;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT PDO) {
  PDEVICE_OBJECT Fdo = NULL;
  NTSTATUS Status;
  const ULONG Unit = ++AddCalls;
  if (!Check(KeGetCurrentIrql() == PASSIVE_LEVEL &&
                 PDO->DriverObject != Driver && PDO->StackSize == 1 &&
                 (PDO->Flags & DO_BUS_ENUMERATED_DEVICE) != 0,
             7))
    return STATUS_UNSUCCESSFUL;
  Status = IoCreateDevice(Driver, sizeof(PNP_EXTENSION), NULL,
                          FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE,
                          &Fdo);
  if (!NT_SUCCESS(Status))
    return Status;
  PNP_EXTENSION *Extension = Fdo->DeviceExtension;
  Extension->Self = Fdo;
  Extension->PDO = PDO;
  Extension->Unit = Unit;
  Extension->Lower = IoAttachDeviceToDeviceStack(Fdo, PDO);
  if (!Extension->Lower) {
    IoDeleteDevice(Fdo);
    return STATUS_NO_SUCH_DEVICE;
  }
  ++LiveFdos;
  KeInitializeEvent(&Extension->Event, NotificationEvent, FALSE);
  Fdo->Flags |= DO_BUFFERED_IO;
  Fdo->Flags &= ~DO_DEVICE_INITIALIZING;
  DbgPrint("WDM PnP: added mode=%c\n", Mode);
  DbgPrint("WDM PnP: added unit=%lu\n", Unit);
  if (Mode == 'A' || (Mode == 'M' && Unit == 1)) {
    IoDetachDevice(Extension->Lower);
    --LiveFdos;
    IoDeleteDevice(Fdo);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  if (Mode == 'L')
    return STATUS_INSUFFICIENT_RESOURCES;
  return STATUS_SUCCESS;
}

static void Unload(PDRIVER_OBJECT Driver) {
  Check(LiveFdos == 0 && Driver->DeviceObject == ControlDevice, 8);
  if (ControlDevice) {
    PDEVICE_OBJECT Device = ControlDevice;
    ControlDevice = NULL;
    IoDeleteDevice(Device);
    DbgPrint("WDM PnP: deleted independent control device\n");
  }
  Check(Driver->DeviceObject == NULL, 9);
  DbgPrint("WDM PnP: unloaded\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath) {
  Mode = 'S';
  if (RegistryPath->Length >= sizeof(WCHAR)) {
    const WCHAR Last =
        RegistryPath->Buffer[RegistryPath->Length / sizeof(WCHAR) - 1];
    if (Last == 'A' || Last == 'L' || Last == 'F' || Last == 'T' ||
        Last == 'M' || Last == 'C')
      Mode = (UCHAR)Last;
  }
  Driver->DriverExtension->AddDevice = AddDevice;
  Driver->DriverUnload = Unload;
  Driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
  Driver->MajorFunction[IRP_MJ_CREATE] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_READ] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_WRITE] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLEANUP] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLOSE] = DispatchFile;
  if (Mode == 'C') {
    static WCHAR ControlName[] = L"\\Device\\NeverDPnpControl";
    UNICODE_STRING Name = {sizeof(ControlName) - sizeof(WCHAR),
                           sizeof(ControlName), ControlName};
    NTSTATUS Status = IoCreateDevice(Driver, sizeof(PNP_EXTENSION), &Name,
                                     FILE_DEVICE_UNKNOWN,
                                     FILE_DEVICE_SECURE_OPEN, FALSE,
                                     &ControlDevice);
    if (!NT_SUCCESS(Status))
      return Status;
    PNP_EXTENSION *Extension = ControlDevice->DeviceExtension;
    Extension->Self = ControlDevice;
    Extension->Control = TRUE;
    Extension->Started = TRUE;
    ControlDevice->Flags |= DO_BUFFERED_IO;
    ControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    DbgPrint("WDM PnP: created independent control device\n");
  }
  DbgPrint("WDM PnP: entry\n");
  return STATUS_SUCCESS;
}
