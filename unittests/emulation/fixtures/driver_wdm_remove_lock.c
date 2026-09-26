//===- driver_wdm_remove_lock.c - Genuine WDK remove-lock fixture ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original WDM remove-lock driver. W (the default) drains a worker which waits
/// again after its final release; S drains immediately; C first waits for lower
/// completion; D releases from a DPC; N/R use duplicate NULL/non-NULL tags.
/// F cleans up failed AddDevice; L stalls; I/P invoke AndWait in invalid
/// contexts. Retail and DBG builds use the real WDK macros and their different
/// layouts.
///
//===----------------------------------------------------------------------===//

#include <ntddk.h>

_Static_assert(sizeof(void *) == 8 && _Alignof(IO_REMOVE_LOCK) == 8,
               "x64 remove-lock alignment");
_Static_assert(sizeof(IO_REMOVE_LOCK_COMMON_BLOCK) == 32 &&
                   sizeof(IO_REMOVE_LOCK_DBG_BLOCK) == 88,
               "genuine WDK remove-lock block ABI");
#if DBG
_Static_assert(sizeof(IO_REMOVE_LOCK) == 120, "DBG remove-lock ABI");
#else
_Static_assert(sizeof(IO_REMOVE_LOCK) == 32, "retail remove-lock ABI");
#endif
typedef VOID(NTAPI *INITIALIZE_ABI)(PIO_REMOVE_LOCK, ULONG, ULONG, ULONG,
                                    ULONG);
typedef NTSTATUS(NTAPI *ACQUIRE_ABI)(PIO_REMOVE_LOCK, PVOID, PCSTR, ULONG,
                                     ULONG);
typedef VOID(NTAPI *RELEASE_ABI)(PIO_REMOVE_LOCK, PVOID, ULONG);
_Static_assert(
    __builtin_types_compatible_p(__typeof__(&IoInitializeRemoveLockEx),
                                 INITIALIZE_ABI) &&
        __builtin_types_compatible_p(__typeof__(&IoAcquireRemoveLockEx),
                                     ACQUIRE_ABI) &&
        __builtin_types_compatible_p(__typeof__(&IoReleaseRemoveLockEx),
                                     RELEASE_ABI) &&
        __builtin_types_compatible_p(__typeof__(&IoReleaseRemoveLockAndWaitEx),
                                     RELEASE_ABI),
    "remove-lock macro export signatures");

typedef struct {
  PDEVICE_OBJECT Self;
  PDEVICE_OBJECT PDO;
  PDEVICE_OBJECT Lower;
  IO_REMOVE_LOCK RemoveLock;
  KEVENT PnpEvent;
  KEVENT WorkerEvent;
  KDPC Dpc;
  KTIMER Timer;
  PIO_WORKITEM Work;
  ULONG Canary;
  ULONG WorkTag;
  ULONG Unit;
  ULONG DpcStage;
  BOOLEAN DrainReturned;
} REMOVE_EXTENSION;

static UCHAR Mode;
static ULONG LiveDevices;
static ULONG AddCalls;

static BOOLEAN Check(BOOLEAN Condition, ULONG Code) {
  if (!Condition)
    DbgPrint("Remove lock: failure %lu\n", Code);
  return Condition;
}

static PVOID WorkTag(REMOVE_EXTENSION *Extension) {
  return Mode == 'N' ? NULL : &Extension->WorkTag;
}

static NTSTATUS Finish(REMOVE_EXTENSION *Extension, PIRP Irp, NTSTATUS Status,
                       ULONG_PTR Information) {
  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = Information;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  // Tag is an opaque identity. Completion has consumed the packet already.
  IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
  return Status;
}

static NTSTATUS Completion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context) {
  REMOVE_EXTENSION *Extension = Context;
  Check(Device == Extension->Self && Irp->IoStatus.Information == 0 &&
            KeGetCurrentIrql() == PASSIVE_LEVEL,
        1);
  KeSetEvent(&Extension->PnpEvent, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS ForwardSynchronously(REMOVE_EXTENSION *Extension, PIRP Irp) {
  KeClearEvent(&Extension->PnpEvent);
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, Completion, Extension, TRUE, TRUE, TRUE);
  const NTSTATUS Status = IoCallDriver(Extension->Lower, Irp);
  if (Status == STATUS_PENDING)
    Check(KeWaitForSingleObject(&Extension->PnpEvent, Executive, KernelMode,
                                FALSE, NULL) == STATUS_SUCCESS,
          2);
  return Irp->IoStatus.Status;
}

static void DrainWorker(PDEVICE_OBJECT Device, PVOID Context) {
  REMOVE_EXTENSION *Extension = Context;
  volatile ULONG Canary = Extension->Canary;
  const PVOID Tag = WorkTag(Extension);
  PIO_WORKITEM Work = Extension->Work;
  LARGE_INTEGER Delay;
  Check(Device == Extension->Self && KeGetCurrentIrql() == PASSIVE_LEVEL &&
            Canary == 0xabc00000 + Extension->Unit,
        10);
  if (Mode == 'N' || Mode == 'R') {
    IoReleaseRemoveLock(&Extension->RemoveLock, Tag);
    Check(!Extension->DrainReturned, 11);
    DbgPrint("Remove lock: first duplicate release kept remover waiting\n");
  }
  Delay.QuadPart = -7;
  Check(KeDelayExecutionThread(KernelMode, FALSE, &Delay) == STATUS_SUCCESS,
        12);
  Check(Canary == Extension->Canary && !Extension->DrainReturned, 13);
  Check(IoAcquireRemoveLock(&Extension->RemoveLock, Tag) ==
            STATUS_DELETE_PENDING,
        14);
  DbgPrint("Remove lock: worker late acquire delete-pending\n");
  Extension->Work = NULL;
  IoFreeWorkItem(Work);
  DbgPrint("Remove lock: worker last release\n");
  IoReleaseRemoveLock(&Extension->RemoveLock, Tag);
  // Work-item execution retains the DEVICE_OBJECT independently of remove-lock
  // acquisition. This wait must not prevent AndWait from returning.
  DbgPrint("Remove lock: worker waits after release\n");
  Check(KeWaitForSingleObject(&Extension->WorkerEvent, Executive, KernelMode,
                              FALSE, NULL) == STATUS_SUCCESS,
        15);
  Check(Canary == Extension->Canary && Extension->DrainReturned, 16);
  DbgPrint("Remove lock: worker resumed after deletion\n");
}

static void DrainDpc(PKDPC Dpc, PVOID Context, PVOID Argument1,
                     PVOID Argument2) {
  REMOVE_EXTENSION *Extension = Context;
  const ULONG Stage = Extension->DpcStage;
  Check(Dpc == &Extension->Dpc && KeGetCurrentIrql() == DISPATCH_LEVEL &&
            Extension->Canary == 0xabc00000 + Extension->Unit,
        20);
  if (Stage == 1) {
    Check(Argument1 == Extension && Argument2 == Extension->Self, 21);
    Check(IoAcquireRemoveLock(&Extension->RemoveLock, &Extension->WorkTag) ==
              STATUS_SUCCESS,
          22);
    IoReleaseRemoveLock(&Extension->RemoveLock, &Extension->WorkTag);
    KeSetEvent(&Extension->PnpEvent, IO_NO_INCREMENT, FALSE);
    DbgPrint("Remove lock: DPC acquired and released at IRQL2\n");
    IoReleaseRemoveLock(&Extension->RemoveLock, Dpc);
    return;
  }
  Check(Stage == 2, 23);
  if (Mode == 'P') {
    IoReleaseRemoveLockAndWait(&Extension->RemoveLock, Dpc);
    DbgPrint("Remove lock: invalid DPC wait returned\n");
    return;
  }
  Check(IoAcquireRemoveLock(&Extension->RemoveLock, &Extension->WorkTag) ==
            STATUS_DELETE_PENDING,
        24);
  DbgPrint("Remove lock: DPC last release at IRQL2\n");
  IoReleaseRemoveLock(&Extension->RemoveLock, Dpc);
}

static void QueueDrain(REMOVE_EXTENSION *Extension) {
  if (Mode == 'L') {
    Check(IoAcquireRemoveLock(&Extension->RemoveLock, &Extension->WorkTag) ==
              STATUS_SUCCESS,
          30);
    return;
  }
  if (Mode == 'D' || Mode == 'P') {
    LARGE_INTEGER Due;
    Due.QuadPart = -7;
    Extension->DpcStage = 2;
    Check(IoAcquireRemoveLock(&Extension->RemoveLock, &Extension->Dpc) ==
              STATUS_SUCCESS,
          31);
    KeSetTimer(&Extension->Timer, Due, &Extension->Dpc);
    return;
  }
  if (Mode == 'S')
    return;
  Check(IoAcquireRemoveLock(&Extension->RemoveLock, WorkTag(Extension)) ==
            STATUS_SUCCESS,
        32);
  if (Mode == 'N' || Mode == 'R')
    Check(IoAcquireRemoveLock(&Extension->RemoveLock, WorkTag(Extension)) ==
              STATUS_SUCCESS,
          33);
  Extension->Work = IoAllocateWorkItem(Extension->Self);
  Check(Extension->Work != NULL, 34);
  KeClearEvent(&Extension->WorkerEvent);
  IoQueueWorkItem(Extension->Work, DrainWorker, DelayedWorkQueue, Extension);
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT Device, PIRP Irp) {
  REMOVE_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  const UCHAR Minor = Stack->MinorFunction;
  NTSTATUS Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
  if (!NT_SUCCESS(Status)) {
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
  }
  Check(KeGetCurrentIrql() == PASSIVE_LEVEL, 40);
  if (Mode == 'I' && Minor == IRP_MN_START_DEVICE) {
    IoReleaseRemoveLockAndWait(&Extension->RemoveLock, Irp);
    DbgPrint("Remove lock: invalid non-REMOVE wait returned\n");
    return STATUS_UNSUCCESSFUL;
  }
  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  if (Minor == IRP_MN_REMOVE_DEVICE) {
    PDEVICE_OBJECT Lower = Extension->Lower;
    volatile ULONG Canary = Extension->Canary;
    QueueDrain(Extension);
    if (Mode == 'C') {
      Status = ForwardSynchronously(Extension, Irp);
      Irp->IoStatus.Status = Status;
      IoCompleteRequest(Irp, IO_NO_INCREMENT);
      DbgPrint("Remove lock: explicit lower completion wait ended\n");
    } else {
      IoSkipCurrentIrpStackLocation(Irp);
      Status = IoCallDriver(Lower, Irp);
    }
    // The IRP may already be retired or still owned by the lower driver. Only
    // its opaque address is used as the matching acquisition tag from now on.
    DbgPrint("Remove lock: REMOVE forwarded status=0x%08lx\n", (ULONG)Status);
    IoReleaseRemoveLockAndWait(&Extension->RemoveLock, Irp);
    Check(Canary == Extension->Canary, 41);
    Extension->DrainReturned = TRUE;
    Check(IoAcquireRemoveLock(&Extension->RemoveLock, Irp) ==
              STATUS_DELETE_PENDING,
          42);
    DbgPrint("Remove lock: drain returned\n");
    KeSetEvent(&Extension->WorkerEvent, IO_NO_INCREMENT, FALSE);
    IoDetachDevice(Lower);
    --LiveDevices;
    IoDeleteDevice(Device);
    DbgPrint("Remove lock: device deletion requested\n");
    return Status;
  }
  if (Minor == IRP_MN_START_DEVICE && Mode == 'D') {
    Extension->DpcStage = 1;
    KeClearEvent(&Extension->PnpEvent);
    Check(IoAcquireRemoveLock(&Extension->RemoveLock, &Extension->Dpc) ==
              STATUS_SUCCESS,
          43);
    KeInsertQueueDpc(&Extension->Dpc, Extension, Device);
    Check(KeWaitForSingleObject(&Extension->PnpEvent, Executive, KernelMode,
                                FALSE, NULL) == STATUS_SUCCESS,
          44);
  }
  if (Minor == IRP_MN_START_DEVICE || Minor == IRP_MN_QUERY_REMOVE_DEVICE ||
      Minor == IRP_MN_CANCEL_REMOVE_DEVICE) {
    Status = ForwardSynchronously(Extension, Irp);
    return Finish(Extension, Irp, Status, 0);
  }
  return Finish(Extension, Irp, STATUS_NOT_SUPPORTED, 0);
}

static NTSTATUS DispatchFile(PDEVICE_OBJECT Device, PIRP Irp) {
  REMOVE_EXTENSION *Extension = Device->DeviceExtension;
  const PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  NTSTATUS Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
  if (!NT_SUCCESS(Status)) {
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
  }
  ULONG_PTR Information = 0;
  Check(Stack->FileObject && Stack->FileObject->DeviceObject == Extension->PDO,
        50);
  if (Stack->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
    if (Stack->Parameters.DeviceIoControl.IoControlCode != 0x222000 ||
        Stack->Parameters.DeviceIoControl.OutputBufferLength < 4) {
      Status = STATUS_INVALID_PARAMETER;
    } else {
      PUCHAR Buffer = Irp->AssociatedIrp.SystemBuffer;
      Buffer[0] = 'L';
      Buffer[1] = 'O';
      Buffer[2] = 'C';
      Buffer[3] = 'K';
      Information = 4;
    }
  }
  return Finish(Extension, Irp, Status, Information);
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT PDO) {
  PDEVICE_OBJECT Device = NULL;
  NTSTATUS Status = IoCreateDevice(Driver, sizeof(REMOVE_EXTENSION), NULL,
                                   FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                                   FALSE, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  REMOVE_EXTENSION *Extension = Device->DeviceExtension;
  Extension->Self = Device;
  Extension->PDO = PDO;
  Extension->Unit = ++AddCalls;
  Extension->Canary = 0xabc00000 + Extension->Unit;
  IoInitializeRemoveLock(&Extension->RemoveLock, 0x4c52444e, 0, 0);
  DbgPrint("Remove lock: initialized before attach size=%lu\n",
           (ULONG)sizeof(IO_REMOVE_LOCK));
  if (Mode == 'F') {
    IoDeleteDevice(Device);
    DbgPrint("Remove lock: unused initialized lock deleted\n");
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  Extension->Lower = IoAttachDeviceToDeviceStack(Device, PDO);
  if (!Extension->Lower) {
    IoDeleteDevice(Device);
    return STATUS_NO_SUCH_DEVICE;
  }
  ++LiveDevices;
  KeInitializeEvent(&Extension->PnpEvent, NotificationEvent, FALSE);
  KeInitializeEvent(&Extension->WorkerEvent, NotificationEvent, FALSE);
  KeInitializeDpc(&Extension->Dpc, DrainDpc, Extension);
  KeInitializeTimer(&Extension->Timer);
  Device->Flags |= DO_BUFFERED_IO;
  Device->Flags &= ~DO_DEVICE_INITIALIZING;
  DbgPrint("Remove lock: added unit=%lu mode=%c\n", Extension->Unit, Mode);
  return STATUS_SUCCESS;
}

static void Unload(PDRIVER_OBJECT Driver) {
  Check(LiveDevices == 0 && Driver->DeviceObject == NULL, 60);
  DbgPrint("Remove lock: unloaded\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath) {
  Mode = 'W';
  if (RegistryPath->Length >= sizeof(WCHAR)) {
    const WCHAR Last =
        RegistryPath->Buffer[RegistryPath->Length / sizeof(WCHAR) - 1];
    if (Last == 'S' || Last == 'C' || Last == 'D' || Last == 'N' ||
        Last == 'R' || Last == 'F' || Last == 'L' || Last == 'I' || Last == 'P')
      Mode = (UCHAR)Last;
  }
  Driver->DriverExtension->AddDevice = AddDevice;
  Driver->DriverUnload = Unload;
  Driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
  Driver->MajorFunction[IRP_MJ_CREATE] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLEANUP] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLOSE] = DispatchFile;
  DbgPrint("Remove lock: entry\n");
  return STATUS_SUCCESS;
}
