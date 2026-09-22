//===- driver_wdm_stack.c - Genuine WDK layered IRP fixture ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original WDM driver compiled with unmodified Microsoft WDK headers and
/// linked to the genuine ntoskrnl import library. Two DEVICE_OBJECTs belong to
/// one DRIVER_OBJECT; the named lower device has an unnamed upper attachment.
/// S skips synchronously; C copies with successful completion; E recovers a
/// lower error; R preserves a warning selected by InvokeOnError; P propagates
/// pending worker completion; W waits for a worker with MORE_PROCESSING_REQUIRED;
/// D does that with direct buffers; N completes recursively inside completion;
/// I completes from a timer DPC. B and F deliberately corrupt cursor/control.
/// File lifecycle and buffered READ/WRITE always copy the complete WDK prefix.
///
//===----------------------------------------------------------------------===//

#include <ntddk.h>

#define ABI_OFFSET(Type, Member, Offset)                                      \
  _Static_assert(__builtin_offsetof(Type, Member) == Offset,                  \
                 #Type "." #Member " x64 ABI")

_Static_assert(sizeof(void *) == 8, "x64 fixture");
_Static_assert(sizeof(IRP) == 0xd0, "IRP ABI");
_Static_assert(sizeof(IO_STACK_LOCATION) == 0x48, "I/O stack ABI");
ABI_OFFSET(IRP, PendingReturned, 0x41);
ABI_OFFSET(IRP, StackCount, 0x42);
ABI_OFFSET(IRP, CurrentLocation, 0x43);
ABI_OFFSET(IRP, Tail.Overlay.CurrentStackLocation, 0xb8);
ABI_OFFSET(IO_STACK_LOCATION, Control, 3);
ABI_OFFSET(IO_STACK_LOCATION, DeviceObject, 0x28);
ABI_OFFSET(IO_STACK_LOCATION, FileObject, 0x30);
ABI_OFFSET(IO_STACK_LOCATION, CompletionRoutine, 0x38);
ABI_OFFSET(IO_STACK_LOCATION, Context, 0x40);
ABI_OFFSET(DEVICE_OBJECT, NextDevice, 0x10);
ABI_OFFSET(DEVICE_OBJECT, AttachedDevice, 0x18);
ABI_OFFSET(DEVICE_OBJECT, StackSize, 0x4c);
_Static_assert(SL_PENDING_RETURNED == 1 && SL_INVOKE_ON_CANCEL == 0x20 &&
                   SL_INVOKE_ON_SUCCESS == 0x40 && SL_INVOKE_ON_ERROR == 0x80,
               "completion control ABI");
_Static_assert((ULONG)STATUS_MORE_PROCESSING_REQUIRED == 0xc0000016,
               "completion continuation ABI");

#define IOCTL_STACK_BUFFERED                                                 \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_STACK_DIRECT                                                   \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)

typedef struct {
  BOOLEAN Upper;
  PDEVICE_OBJECT Lower;
  PIRP PendingIrp;
  PIO_WORKITEM Work;
  KEVENT Event;
  KTIMER Timer;
  KDPC Dpc;
  PIO_STACK_LOCATION UpperStack;
  PMDL SavedMdl;
  PVOID SavedBuffer;
  NTSTATUS CompletedStatus;
  ULONG CompletionCount;
} STACK_EXTENSION;

static PDEVICE_OBJECT LowerDevice;
static PDEVICE_OBJECT UpperDevice;
static UCHAR Mode;
static const WCHAR DeviceName[] = L"\\Device\\NeverDWdmStack";
static const WCHAR LinkName[] = L"\\DosDevices\\NeverDWdmStack";

static BOOLEAN Check(BOOLEAN Condition, ULONG Code) {
  if (!Condition)
    DbgPrint("WDM stack: failure %lu\n", Code);
  return Condition;
}

static BOOLEAN HeldCompletion(void) { return Mode == 'W' || Mode == 'D'; }

static PUCHAR TransferBuffer(PIRP Irp) {
  return Irp->MdlAddress
             ? MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority)
             : Irp->AssociatedIrp.SystemBuffer;
}

static void PrepareTransfer(PIRP Irp) {
  PUCHAR Buffer = TransferBuffer(Irp);
  if (!Check(Buffer != NULL, 1)) {
    Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
    Irp->IoStatus.Information = 0;
    return;
  }
  Buffer[0] = 'S';
  Buffer[1] = Mode;
  Buffer[2] = 'D';
  Buffer[3] = 'M';
  Irp->IoStatus.Status = Mode == 'E'   ? STATUS_ACCESS_DENIED
                         : Mode == 'R' ? STATUS_BUFFER_OVERFLOW
                                       : STATUS_SUCCESS;
  Irp->IoStatus.Information = Mode == 'S' ? 4 : 2;
}

static NTSTATUS Completion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context) {
  STACK_EXTENSION *Extension = Context;
  const BOOLEAN Asynchronous =
      Mode == 'P' || Mode == 'I' || HeldCompletion();
  const KIRQL ExpectedIRQL = Mode == 'I' ? DISPATCH_LEVEL : PASSIVE_LEVEL;
  if (!Check(Device == UpperDevice &&
                 Extension == UpperDevice->DeviceExtension &&
                 KeGetCurrentIrql() == ExpectedIRQL &&
                 IoGetCurrentIrpStackLocation(Irp) == Extension->UpperStack &&
                 Irp->CurrentLocation == 2 &&
                 Irp->PendingReturned == Asynchronous &&
                 Irp->IoStatus.Information == 2,
             2))
    return STATUS_CONTINUE_COMPLETION;
  ++Extension->CompletionCount;
  Irp->IoStatus.Information = 4;
  if (Mode == 'E')
    Irp->IoStatus.Status = STATUS_SUCCESS;
  Extension->CompletedStatus = Irp->IoStatus.Status;
  DbgPrint("WDM stack: %c completion irql=%u pending=%u\n", Mode,
           (unsigned)KeGetCurrentIrql(), (unsigned)Irp->PendingReturned);
  if (HeldCompletion()) {
    Check(TransferBuffer(Irp) == Extension->SavedBuffer &&
              Irp->MdlAddress == Extension->SavedMdl,
          3);
    KeSetEvent(&Extension->Event, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
  }
  if (Irp->PendingReturned)
    IoMarkIrpPending(Irp);
  if (Mode == 'N') {
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    // The nested completion retired the IRP. Only driver context is live.
    DbgPrint("WDM stack: nested completion returned\n");
    return STATUS_MORE_PROCESSING_REQUIRED;
  }
  // The completion's control result must not overwrite IoStatus or the lower
  // dispatch return. Only MORE_PROCESSING_REQUIRED has special significance.
  return Mode == 'E' ? STATUS_UNSUCCESSFUL : STATUS_CONTINUE_COMPLETION;
}

static void Worker(PDEVICE_OBJECT Device, PVOID Context) {
  STACK_EXTENSION *Extension = Context;
  PIRP Irp = Extension->PendingIrp;
  if (!Check(Device == LowerDevice && Extension == Device->DeviceExtension &&
                 KeGetCurrentIrql() == PASSIVE_LEVEL && Irp != NULL,
             4))
    return;
  IoFreeWorkItem(Extension->Work);
  Extension->Work = NULL;
  Extension->PendingIrp = NULL;
  PrepareTransfer(Irp);
  DbgPrint("WDM stack: lower worker\n");
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  DbgPrint("WDM stack: lower worker completed\n");
}

static void TimerDpc(PKDPC Dpc, PVOID Context, PVOID First, PVOID Second) {
  STACK_EXTENSION *Extension = Context;
  PIRP Irp = Extension->PendingIrp;
  UNREFERENCED_PARAMETER(First);
  UNREFERENCED_PARAMETER(Second);
  if (!Check(Dpc == &Extension->Dpc &&
                 Extension == LowerDevice->DeviceExtension &&
                 KeGetCurrentIrql() == DISPATCH_LEVEL && Irp != NULL,
             5))
    return;
  Extension->PendingIrp = NULL;
  PrepareTransfer(Irp);
  DbgPrint("WDM stack: lower DPC\n");
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  DbgPrint("WDM stack: lower DPC completed\n");
}

static NTSTATUS Dispatch(PDEVICE_OBJECT Device, PIRP Irp) {
  STACK_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  NTSTATUS Status;
  if (!Check(Stack->DeviceObject == Device && Stack->FileObject != NULL &&
                 Stack->FileObject->DeviceObject == LowerDevice &&
                 Irp->StackCount == 2,
             6)) {
    Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_UNSUCCESSFUL;
  }
  if (Stack->MajorFunction != IRP_MJ_DEVICE_CONTROL) {
    if (Extension->Upper) {
      IoCopyCurrentIrpStackLocationToNext(Irp);
      return IoCallDriver(Extension->Lower, Irp);
    }
    ULONG_PTR Information = 0;
    Status = STATUS_SUCCESS;
    if (!Check(Device == LowerDevice && Irp->CurrentLocation == 1, 15))
      Status = STATUS_UNSUCCESSFUL;
    DbgPrint("WDM stack: lower major=%u\n", (unsigned)Stack->MajorFunction);
    if (NT_SUCCESS(Status) && Stack->MajorFunction == IRP_MJ_READ) {
      static const UCHAR Bytes[] = {'S', 'T', 'A', 'K'};
      const ULONG Length = Stack->Parameters.Read.Length;
      PUCHAR Buffer = TransferBuffer(Irp);
      if (!Check(Length == sizeof(Bytes) && Buffer != NULL &&
                     Stack->Parameters.Read.ByteOffset.QuadPart == 18,
                 16)) {
        Status = STATUS_INVALID_PARAMETER;
      } else {
        for (ULONG I = 0; I < sizeof(Bytes); ++I)
          Buffer[I] = Bytes[I];
        Information = sizeof(Bytes);
        DbgPrint("WDM stack: lower read bytes=%lu offset=%lu\n", Length,
                 (ULONG)Stack->Parameters.Read.ByteOffset.QuadPart);
      }
    } else if (NT_SUCCESS(Status) && Stack->MajorFunction == IRP_MJ_WRITE) {
      const ULONG Length = Stack->Parameters.Write.Length;
      PUCHAR Buffer = TransferBuffer(Irp);
      if (!Check(Length == 3 && Buffer != NULL && Buffer[0] == 0x11 &&
                     Buffer[1] == 0x22 && Buffer[2] == 0x33 &&
                     Stack->Parameters.Write.ByteOffset.QuadPart == 64,
                 17)) {
        Status = STATUS_INVALID_PARAMETER;
      } else {
        Information = Length;
        DbgPrint("WDM stack: lower write bytes=%lu offset=%lu sum=%lu\n",
                 Length, (ULONG)Stack->Parameters.Write.ByteOffset.QuadPart,
                 (ULONG)(Buffer[0] + Buffer[1] + Buffer[2]));
      }
    }
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
  }
  if (!Check(Stack->Parameters.DeviceIoControl.InputBufferLength == 4 &&
                 Stack->Parameters.DeviceIoControl.OutputBufferLength == 4 &&
                 Stack->Parameters.DeviceIoControl.IoControlCode ==
                     (Mode == 'D' ? IOCTL_STACK_DIRECT : IOCTL_STACK_BUFFERED),
             7)) {
    Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_PARAMETER;
  }
  if (Extension->Upper) {
    if (!Check(Device == UpperDevice && Irp->CurrentLocation == 2, 8))
      return STATUS_UNSUCCESSFUL;
    Extension->UpperStack = Stack;
    Extension->CompletionCount = 0;
    DbgPrint("WDM stack: %c upper dispatch\n", Mode);
    if (Mode == 'S') {
      IoSkipCurrentIrpStackLocation(Irp);
      return IoCallDriver(Extension->Lower, Irp);
    }
    IoCopyCurrentIrpStackLocationToNext(Irp);
    if (Mode == 'B') {
      // Deliberately inconsistent header cursor, rejected before lower entry.
      Irp->Tail.Overlay.CurrentStackLocation++;
    } else if (Mode == 'F') {
      // Deliberate verifier violation, avoiding a header debug assertion.
      PIO_STACK_LOCATION Next = IoGetNextIrpStackLocation(Irp);
      Next->CompletionRoutine = NULL;
      Next->Control = SL_INVOKE_ON_SUCCESS;
    } else {
      IoSetCompletionRoutine(Irp, Completion, Extension, Mode != 'E' && Mode != 'R',
                             TRUE, FALSE);
    }
    if (HeldCompletion()) {
      KeInitializeEvent(&Extension->Event, NotificationEvent, FALSE);
      Extension->SavedMdl = Irp->MdlAddress;
      Extension->SavedBuffer = TransferBuffer(Irp);
    }
    Status = IoCallDriver(Extension->Lower, Irp);
    DbgPrint("WDM stack: lower returned 0x%08lx\n", (ULONG)Status);
    if (HeldCompletion()) {
      if (Status == STATUS_PENDING)
        if (!Check(KeWaitForSingleObject(&Extension->Event, Executive,
                                         KernelMode, FALSE, NULL) ==
                       STATUS_SUCCESS,
                   9))
          return STATUS_UNSUCCESSFUL;
      if (!Check(Extension->CompletionCount == 1 &&
                     Irp->CurrentLocation == 2 &&
                     IoGetCurrentIrpStackLocation(Irp) == Stack &&
                     TransferBuffer(Irp) == Extension->SavedBuffer &&
                     Irp->MdlAddress == Extension->SavedMdl &&
                     ((PUCHAR)Extension->SavedBuffer)[3] == 'M',
                 10))
        return STATUS_UNSUCCESSFUL;
      Status = Irp->IoStatus.Status;
      DbgPrint("WDM stack: upper resumed with live buffer\n");
      IoCompleteRequest(Irp, IO_NO_INCREMENT);
      return Status;
    }
    if (Mode == 'P' || Mode == 'I')
      return Status;
    if (!Check(Extension->CompletionCount == 1, 11))
      return STATUS_UNSUCCESSFUL;
    return Extension->CompletedStatus;
  }
  if (!Check(Device == LowerDevice &&
                 Irp->CurrentLocation == (Mode == 'S' ? 2 : 1) &&
                 (Mode != 'S' || Stack ==
                                      ((STACK_EXTENSION *)UpperDevice
                                           ->DeviceExtension)
                                          ->UpperStack),
             12))
    return STATUS_UNSUCCESSFUL;
  DbgPrint("WDM stack: %c lower dispatch\n", Mode);
  if (Mode == 'P' || Mode == 'I' || HeldCompletion()) {
    IoMarkIrpPending(Irp);
    Extension->PendingIrp = Irp;
    if (Mode == 'I') {
      LARGE_INTEGER Due;
      Due.QuadPart = -10;
      KeInitializeDpc(&Extension->Dpc, TimerDpc, Extension);
      KeInitializeTimer(&Extension->Timer);
      KeSetTimer(&Extension->Timer, Due, &Extension->Dpc);
    } else {
      Extension->Work = IoAllocateWorkItem(Device);
      if (!Check(Extension->Work != NULL, 13))
        return STATUS_PENDING;
      IoQueueWorkItem(Extension->Work, Worker, DelayedWorkQueue, Extension);
    }
    return STATUS_PENDING;
  }
  PrepareTransfer(Irp);
  Status = Irp->IoStatus.Status;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  DbgPrint("WDM stack: lower synchronous completion returned\n");
  return Status;
}

static void Unload(PDRIVER_OBJECT Driver) {
  UNICODE_STRING Link;
  UNREFERENCED_PARAMETER(Driver);
  RtlInitUnicodeString(&Link, LinkName);
  IoDeleteSymbolicLink(&Link);
  IoDetachDevice(((STACK_EXTENSION *)UpperDevice->DeviceExtension)->Lower);
  IoDeleteDevice(UpperDevice);
  IoDeleteDevice(LowerDevice);
  DbgPrint("WDM stack: unloaded\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath) {
  UNICODE_STRING Name;
  UNICODE_STRING Link;
  NTSTATUS Status;
  Mode = 'S';
  if (RegistryPath->Length >= sizeof(WCHAR)) {
    WCHAR Last = RegistryPath->Buffer[RegistryPath->Length / sizeof(WCHAR) - 1];
    if (Last == 'S' || Last == 'C' || Last == 'E' || Last == 'R' || Last == 'P' ||
        Last == 'W' || Last == 'D' || Last == 'N' || Last == 'I' || Last == 'B' ||
        Last == 'F')
      Mode = (UCHAR)Last;
  }
  Driver->MajorFunction[IRP_MJ_CREATE] = Dispatch;
  Driver->MajorFunction[IRP_MJ_CLEANUP] = Dispatch;
  Driver->MajorFunction[IRP_MJ_CLOSE] = Dispatch;
  Driver->MajorFunction[IRP_MJ_READ] = Dispatch;
  Driver->MajorFunction[IRP_MJ_WRITE] = Dispatch;
  Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Dispatch;
  Driver->DriverUnload = Unload;
  RtlInitUnicodeString(&Name, DeviceName);
  Status = IoCreateDevice(Driver, sizeof(STACK_EXTENSION), &Name,
                          FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE,
                          &LowerDevice);
  if (!NT_SUCCESS(Status))
    return Status;
  Status = IoCreateDevice(Driver, sizeof(STACK_EXTENSION), NULL,
                          FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE,
                          &UpperDevice);
  if (!NT_SUCCESS(Status)) {
    IoDeleteDevice(LowerDevice);
    return Status;
  }
  STACK_EXTENSION *Upper = UpperDevice->DeviceExtension;
  Upper->Upper = TRUE;
  Upper->Lower = IoAttachDeviceToDeviceStack(UpperDevice, LowerDevice);
  if (!Check(Upper->Lower == LowerDevice && UpperDevice->StackSize == 2 &&
                 LowerDevice->StackSize == 1 && UpperDevice->NextDevice == LowerDevice,
             14))
    return STATUS_UNSUCCESSFUL;
  LowerDevice->Flags |= Mode == 'D' ? DO_DIRECT_IO : DO_BUFFERED_IO;
  UpperDevice->Flags |= Mode == 'D' ? DO_DIRECT_IO : DO_BUFFERED_IO;
  LowerDevice->Flags &= ~DO_DEVICE_INITIALIZING;
  UpperDevice->Flags &= ~DO_DEVICE_INITIALIZING;
  RtlInitUnicodeString(&Link, LinkName);
  Status = IoCreateSymbolicLink(&Link, &Name);
  if (!NT_SUCCESS(Status)) {
    IoDetachDevice(Upper->Lower);
    IoDeleteDevice(UpperDevice);
    IoDeleteDevice(LowerDevice);
    return Status;
  }
  DbgPrint("WDM stack: ready %c\n", Mode);
  return STATUS_SUCCESS;
}
