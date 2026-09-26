//===- driver_wdm_resources.c - Genuine WDK register-bank fixture -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original WDM resource driver using packed WDK lists, actual scalar and REP
/// register access, aliases, and STOP/restart persistence. Negative modes are
/// deliberately invalid operations used to test explicit model boundaries.
//===----------------------------------------------------------------------===//

#include <intrin.h>
#include <ntddk.h>

#define OFFSET(Type, Field, Value)                                             \
  _Static_assert(__builtin_offsetof(Type, Field) == Value, #Type "." #Field)
_Static_assert(sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR) == 20 &&
                   _Alignof(CM_PARTIAL_RESOURCE_DESCRIPTOR) == 4 &&
                   sizeof(CM_PARTIAL_RESOURCE_LIST) == 28 &&
                   sizeof(CM_FULL_RESOURCE_DESCRIPTOR) == 36 &&
                   sizeof(CM_RESOURCE_LIST) == 40,
               "genuine x64 packed resource ABI");
OFFSET(CM_PARTIAL_RESOURCE_DESCRIPTOR, u.Memory.Start, 4);
OFFSET(CM_PARTIAL_RESOURCE_DESCRIPTOR, u.Memory.Length, 12);
OFFSET(CM_RESOURCE_LIST, List[0].PartialResourceList.PartialDescriptors, 20);
OFFSET(IO_STACK_LOCATION, Parameters.StartDevice.AllocatedResources, 8);
OFFSET(IO_STACK_LOCATION, Parameters.StartDevice.AllocatedResourcesTranslated,
       16);
_Static_assert(CmResourceTypeMemory == 3 &&
                   CmResourceShareDeviceExclusive == 1 &&
                   CM_RESOURCE_MEMORY_READ_WRITE == 0 && Internal == 0 &&
                   MmNonCached == 0 && PAGE_NOCACHE == 0x200,
               "explicit register-bank resource facts");

typedef struct {
  PDEVICE_OBJECT Self;
  PDEVICE_OBJECT PDO;
  PDEVICE_OBJECT Lower;
  KEVENT Event;
  volatile UCHAR *Base;
  PHYSICAL_ADDRESS Physical;
  ULONG Length;
  ULONG Unit;
  ULONG Starts;
  BOOLEAN Started;
  BOOLEAN RemovePending;
  BOOLEAN Surprise;
} RESOURCE_EXTENSION;

static UCHAR Mode;
static ULONG Units;
static ULONG Live;

static BOOLEAN Check(BOOLEAN Condition, ULONG Code) {
  if (!Condition)
    DbgPrint("WDM resources: failure %lu\n", Code);
  return Condition;
}

static NTSTATUS Complete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information) {
  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = Information;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

static NTSTATUS Completion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context) {
  RESOURCE_EXTENSION *Extension = Context;
  Check(Device == Extension->Self && Irp->IoStatus.Information == 0 &&
            KeGetCurrentIrql() == PASSIVE_LEVEL,
        1);
  DbgPrint("WDM resources: lower completed unit=%lu status=0x%08lx\n",
           Extension->Unit, (ULONG)Irp->IoStatus.Status);
  KeSetEvent(&Extension->Event, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS ForwardWait(RESOURCE_EXTENSION *Extension, PIRP Irp) {
  KeClearEvent(&Extension->Event);
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, Completion, Extension, TRUE, TRUE, TRUE);
  const NTSTATUS Status = IoCallDriver(Extension->Lower, Irp);
  if (Status == STATUS_PENDING)
    Check(KeWaitForSingleObject(&Extension->Event, Executive, KernelMode, FALSE,
                                NULL) == STATUS_SUCCESS,
          2);
  return Irp->IoStatus.Status;
}

static void Snapshot(RESOURCE_EXTENSION *Extension, PULONG Values) {
  volatile UCHAR *Base = Extension->Base;
  Values[0] = READ_REGISTER_UCHAR(Base);
  Values[1] = READ_REGISTER_USHORT((volatile USHORT *)(Base + 2));
  Values[2] = READ_REGISTER_ULONG((volatile ULONG *)(Base + 4));
  Values[3] = READ_REGISTER_ULONG((volatile ULONG *)(Base + 8));
  READ_REGISTER_BUFFER_ULONG((volatile ULONG *)(Base + 0x10), Values + 4, 4);
}

static void Unmap(RESOURCE_EXTENSION *Extension) {
  if (Extension->Base) {
    MmUnmapIoSpace((PVOID)Extension->Base, Extension->Length);
    Extension->Base = NULL;
    DbgPrint("WDM resources: unmapped unit=%lu\n", Extension->Unit);
  }
  Extension->Started = FALSE;
}

static NTSTATUS MapAndExercise(RESOURCE_EXTENSION *Extension,
                               PCM_RESOURCE_LIST Raw,
                               PCM_RESOURCE_LIST Translated) {
  if (!Check(Raw && Translated && Raw != Translated && Raw->Count == 1 &&
                 Translated->Count == 1,
             10))
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  PCM_FULL_RESOURCE_DESCRIPTOR RawFull = &Raw->List[0];
  PCM_FULL_RESOURCE_DESCRIPTOR Full = &Translated->List[0];
  if (!Check(RawFull->InterfaceType == Internal &&
                 Full->InterfaceType == Internal && RawFull->BusNumber == 0 &&
                 Full->BusNumber == 0 &&
                 RawFull->PartialResourceList.Version == 1 &&
                 RawFull->PartialResourceList.Revision == 1 &&
                 Full->PartialResourceList.Version == 1 &&
                 Full->PartialResourceList.Revision == 1 &&
                 RawFull->PartialResourceList.Count == 1 &&
                 Full->PartialResourceList.Count == 1,
             11))
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR R =
      RawFull->PartialResourceList.PartialDescriptors;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR T =
      Full->PartialResourceList.PartialDescriptors;
  if (!Check(R->Type == CmResourceTypeMemory &&
                 T->Type == CmResourceTypeMemory &&
                 R->ShareDisposition == CmResourceShareDeviceExclusive &&
                 T->ShareDisposition == CmResourceShareDeviceExclusive &&
                 R->Flags == CM_RESOURCE_MEMORY_READ_WRITE && T->Flags == 0 &&
                 R->u.Memory.Start.QuadPart != T->u.Memory.Start.QuadPart &&
                 R->u.Memory.Length == 0x1000 && T->u.Memory.Length == 0x1000 &&
                 Extension->Base == NULL,
             12))
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  Extension->Physical = T->u.Memory.Start;
  Extension->Length = T->u.Memory.Length;
  PHYSICAL_ADDRESS Physical = Extension->Physical;
  if (Mode == 'P') {
    Physical.QuadPart += Extension->Length;
    Extension->Base = MmMapIoSpace(Physical, Extension->Length, MmNonCached);
  } else if (Mode == 'B') {
    Extension->Base =
        MmMapIoSpace(R->u.Memory.Start, Extension->Length, MmNonCached);
  } else if (Mode == 'K') {
    Extension->Base = MmMapIoSpace(Physical, Extension->Length, MmCached);
  } else if (Mode == 'X') {
    Extension->Base =
        MmMapIoSpaceEx(Physical, Extension->Length, PAGE_READWRITE);
  } else {
    Extension->Base = MmMapIoSpace(Physical, Extension->Length, MmNonCached);
  }
  if (!Extension->Base)
    return STATUS_INSUFFICIENT_RESOURCES;
  volatile UCHAR *Base = Extension->Base;
  if (Mode == 'Q') {
    volatile ULONG64 Value = READ_REGISTER_ULONG64((volatile ULONG64 *)Base);
    (void)Value;
  } else if (Mode == 'U') {
    volatile USHORT Value = READ_REGISTER_USHORT((volatile USHORT *)(Base + 1));
    (void)Value;
  } else if (Mode == 'G') {
    volatile ULONG Value = READ_REGISTER_ULONG((volatile ULONG *)(Base + 0xc));
    (void)Value;
  } else if (Mode == 'H') {
    MmUnmapIoSpace((PVOID)Base, Extension->Length - 1);
  } else if (Mode == 'T') {
    MmUnmapIoSpace((PVOID)Base, Extension->Length);
    volatile UCHAR Value = READ_REGISTER_UCHAR(Base);
    (void)Value;
  } else if (Mode == 'O') {
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + 8), 0);
  } else if (Mode == 'R') {
    volatile UCHAR *ReadOnly = MmMapIoSpaceEx(Physical, Extension->Length,
                                              PAGE_READONLY | PAGE_NOCACHE);
    Check(ReadOnly != NULL, 13);
    WRITE_REGISTER_UCHAR(ReadOnly, 0);
  }
  ULONG Values[8];
  Snapshot(Extension, Values);
  Check(Values[0] == 0x12 + Extension->Starts &&
            Values[1] == 0x3456 + Extension->Starts &&
            Values[2] == 0x789abcde + Extension->Starts &&
            Values[3] == 0x10203040,
        14);
  for (ULONG Index = 0; Index != 4; ++Index)
    Check(Values[Index + 4] == Index + 1 + 10 * Extension->Starts, 15);
  ++Extension->Starts;
  WRITE_REGISTER_UCHAR(Base, (UCHAR)(0x12 + Extension->Starts));
  WRITE_REGISTER_USHORT((volatile USHORT *)(Base + 2),
                        (USHORT)(0x3456 + Extension->Starts));
  WRITE_REGISTER_ULONG((volatile ULONG *)(Base + 4),
                       0x789abcde + Extension->Starts);
  for (ULONG Index = 0; Index != 4; ++Index)
    Values[Index] = Index + 1 + 10 * Extension->Starts;
  WRITE_REGISTER_BUFFER_ULONG((volatile ULONG *)(Base + 0x10), Values, 4);

  Physical.QuadPart += 0x10;
  volatile ULONG *Alias =
      MmMapIoSpaceEx(Physical, 16, PAGE_READWRITE | PAGE_NOCACHE);
  Check(Alias != NULL, 16);
  READ_REGISTER_BUFFER_ULONG(Alias, Values, 4);
  Check(Values[0] == 1 + 10 * Extension->Starts &&
            Values[3] == 4 + 10 * Extension->Starts,
        17);
  WRITE_REGISTER_ULONG(Alias, Values[0] + 100);
  Check(READ_REGISTER_ULONG((volatile ULONG *)(Base + 0x10)) == Values[0] + 100,
        18);
  WRITE_REGISTER_ULONG((volatile ULONG *)(Base + 0x10), Values[0]);
  MmUnmapIoSpace((PVOID)Alias, 16);
  Check(READ_REGISTER_ULONG((volatile ULONG *)(Base + 0x10)) == Values[0], 19);

  Physical = Extension->Physical;
  Physical.QuadPart += 0xffc;
  volatile ULONG *Tail =
      MmMapIoSpaceEx(Physical, 4, PAGE_READONLY | PAGE_NOCACHE);
  Check(Tail != NULL && READ_REGISTER_ULONG(Tail) == 5, 20);
  MmUnmapIoSpace((PVOID)Tail, 4);
  Extension->Started = TRUE;
  DbgPrint("WDM resources: mapped unit=%lu start=%lu\n", Extension->Unit,
           Extension->Starts);
  return STATUS_SUCCESS;
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT Device, PIRP Irp) {
  RESOURCE_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  UCHAR Minor = Stack->MinorFunction;
  Check(Stack->FileObject == NULL && KeGetCurrentIrql() == PASSIVE_LEVEL, 30);
  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  if (Minor == IRP_MN_REMOVE_DEVICE) {
    PDEVICE_OBJECT Lower = Extension->Lower;
    Unmap(Extension);
    IoSkipCurrentIrpStackLocation(Irp);
    NTSTATUS Status = IoCallDriver(Lower, Irp);
    IoDetachDevice(Lower);
    --Live;
    IoDeleteDevice(Device);
    DbgPrint("WDM resources: removed\n");
    return Status;
  }
  if (Minor == IRP_MN_STOP_DEVICE || Minor == IRP_MN_SURPRISE_REMOVAL) {
    if (Minor == IRP_MN_SURPRISE_REMOVAL)
      Extension->Surprise = TRUE;
    Unmap(Extension);
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Extension->Lower, Irp);
  }
  if (Minor != IRP_MN_START_DEVICE && Minor != IRP_MN_QUERY_STOP_DEVICE &&
      Minor != IRP_MN_CANCEL_STOP_DEVICE &&
      Minor != IRP_MN_QUERY_REMOVE_DEVICE &&
      Minor != IRP_MN_CANCEL_REMOVE_DEVICE)
    return Complete(Irp, STATUS_NOT_SUPPORTED, 0);
  NTSTATUS Status = ForwardWait(Extension, Irp);
  if (Minor == IRP_MN_START_DEVICE) {
    if (NT_SUCCESS(Status))
      Status = MapAndExercise(
          Extension, Stack->Parameters.StartDevice.AllocatedResources,
          Stack->Parameters.StartDevice.AllocatedResourcesTranslated);
    else
      DbgPrint("WDM resources: failed lower START did not map\n");
  } else if (Minor == IRP_MN_QUERY_REMOVE_DEVICE) {
    Extension->RemovePending = NT_SUCCESS(Status);
  } else if (Minor == IRP_MN_CANCEL_REMOVE_DEVICE) {
    Extension->RemovePending = FALSE;
  }
  return Complete(Irp, Status, 0);
}

static NTSTATUS DispatchFile(PDEVICE_OBJECT Device, PIRP Irp) {
  RESOURCE_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  NTSTATUS Status = STATUS_SUCCESS;
  ULONG_PTR Information = 0;
  Check(Stack->FileObject && Stack->FileObject->DeviceObject == Extension->PDO,
        40);
  const BOOLEAN Releasing = Stack->MajorFunction == IRP_MJ_CLEANUP ||
                            Stack->MajorFunction == IRP_MJ_CLOSE;
  if (!Releasing && Extension->Surprise)
    Status = STATUS_NO_SUCH_DEVICE;
  else if (Stack->MajorFunction == IRP_MJ_CREATE && Extension->RemovePending)
    Status = STATUS_DELETE_PENDING;
  else if (!Releasing && !Extension->Started)
    Status = STATUS_DEVICE_NOT_READY;
  else if (Stack->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
    if (Stack->Parameters.DeviceIoControl.IoControlCode != 0x222000 ||
        Stack->Parameters.DeviceIoControl.OutputBufferLength < 32)
      Status = STATUS_INVALID_PARAMETER;
    else {
      Snapshot(Extension, Irp->AssociatedIrp.SystemBuffer);
      Information = 32;
    }
  }
  return Complete(Irp, Status, Information);
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT PDO) {
  PDEVICE_OBJECT Device = NULL;
  NTSTATUS Status = IoCreateDevice(Driver, sizeof(RESOURCE_EXTENSION), NULL,
                                   FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                                   FALSE, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  RESOURCE_EXTENSION *Extension = Device->DeviceExtension;
  Extension->Self = Device;
  Extension->PDO = PDO;
  Extension->Unit = ++Units;
  KeInitializeEvent(&Extension->Event, NotificationEvent, FALSE);
  Extension->Lower = IoAttachDeviceToDeviceStack(Device, PDO);
  if (!Extension->Lower) {
    IoDeleteDevice(Device);
    return STATUS_NO_SUCH_DEVICE;
  }
  ++Live;
  Device->Flags |= DO_BUFFERED_IO;
  Device->Flags &= ~DO_DEVICE_INITIALIZING;
  return STATUS_SUCCESS;
}

static void Unload(PDRIVER_OBJECT Driver) {
  Check(Live == 0 && Driver->DeviceObject == NULL, 50);
  DbgPrint("WDM resources: unloaded\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING Path) {
  Mode = 'S';
  if (Path->Length >= sizeof(WCHAR)) {
    WCHAR Last = Path->Buffer[Path->Length / sizeof(WCHAR) - 1];
    if (Last == 'P' || Last == 'B' || Last == 'K' || Last == 'X' ||
        Last == 'Q' || Last == 'U' || Last == 'G' || Last == 'H' ||
        Last == 'T' || Last == 'O' || Last == 'R')
      Mode = (UCHAR)Last;
  }
  Driver->DriverExtension->AddDevice = AddDevice;
  Driver->DriverUnload = Unload;
  Driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
  Driver->MajorFunction[IRP_MJ_CREATE] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLEANUP] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLOSE] = DispatchFile;
  DbgPrint("WDM resources: entry\n");
  return STATUS_SUCCESS;
}
