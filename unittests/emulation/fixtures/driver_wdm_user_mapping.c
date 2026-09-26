//===- driver_wdm_user_mapping.c - Genuine WDK process MDL views ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "driver_user_mapping.h"

#include <ntifs.h>

static PDEVICE_OBJECT Device;

typedef struct UserMappingWork {
  PIRP Irp;
  PIO_WORKITEM Work;
  PEPROCESS Process;
  PMDL PoolMdl;
  PMDL ReportMdl;
  PUCHAR Pool;
  PUCHAR Report;
  PUCHAR UserView;
  ULONG Action;
} UserMappingWork;

static UserMappingWork Pending;

static NTSTATUS Complete(PIRP Irp, NTSTATUS Status) {
  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

static VOID ReleasePool(PMDL Mdl, PVOID Pool) {
  if (Mdl)
    IoFreeMdl(Mdl);
  ExFreePoolWithTag(Pool, UserMappingPoolTag);
}

static VOID MappingWorker(PDEVICE_OBJECT Object, PVOID Context) {
  UserMappingWork *Work = Context;
  PIRP Irp = Work->Irp;
  NTSTATUS Status = STATUS_SUCCESS;
  if (Object != Device || Work != &Pending || !Irp)
    Status = STATUS_INVALID_DEVICE_STATE;
  else if (Work->Action == UserMappingWrongProcessUnmap)
    MmUnmapLockedPages(Work->UserView, Work->PoolMdl);
  else if (Work->Action == UserMappingAttachedWorker) {
    KAPC_STATE Attachment;
    KeStackAttachProcess(Work->Process, &Attachment);
    __try {
      PUCHAR View = MmMapLockedPagesSpecifyCache(
          Work->PoolMdl, UserMode, MmCached, NULL, FALSE, NormalPagePriority);
      View[UserMappingByteOffset] = UserMappingWorkerByte;
      Work->Report[0] = Work->Pool[UserMappingByteOffset];
      Work->Report[1] = MmGetSystemAddressForMdlSafe(
                            Work->PoolMdl, NormalPagePriority) == Work->Pool;
      MmUnmapLockedPages(View, Work->PoolMdl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      Status = GetExceptionCode();
    }
    KeUnstackDetachProcess(&Attachment);
  } else {
    // Process exit removes its user views; the report's independent system
    // mapping and the nonpaged pool remain available to this worker.
    Work->Pool[UserMappingByteOffset] = UserMappingWorkerByte;
    Work->Report[0] = Work->Pool[UserMappingByteOffset];
    Work->Report[1] = MmGetSystemAddressForMdlSafe(
                          Work->PoolMdl, NormalPagePriority) == Work->Pool;
  }
  ReleasePool(Work->PoolMdl, Work->Pool);
  MmUnlockPages(Work->ReportMdl);
  IoFreeMdl(Work->ReportMdl);
  IoFreeWorkItem(Work->Work);
  RtlZeroMemory(Work, sizeof(*Work));
  Complete(Irp, Status);
}

static NTSTATUS CheckAliases(PMDL Mdl, PUCHAR Pool, volatile UCHAR *Report) {
  PUCHAR First = NULL, ReadOnly = NULL;
  PMDL Relocked = NULL;
  BOOLEAN Locked = FALSE;
  NTSTATUS Status = STATUS_SUCCESS;
  __try {
    First = MmMapLockedPagesSpecifyCache(Mdl, UserMode, MmCached, NULL, FALSE,
                                         NormalPagePriority);
    ReadOnly =
        MmMapLockedPagesSpecifyCache(Mdl, UserMode, MmCached, NULL, TRUE,
                                     NormalPagePriority | MdlMappingNoWrite);
    Pool[UserMappingByteOffset] = UserMappingFirstByte;
    Report[0] = First[UserMappingByteOffset];
    First[UserMappingByteOffset] = UserMappingSecondByte;
    Report[1] = ReadOnly[UserMappingByteOffset];
    Report[2] = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority) == Pool;
    __try {
      ((volatile UCHAR *)ReadOnly)[UserMappingByteOffset] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (GetExceptionCode() == STATUS_ACCESS_VIOLATION)
        Report[3] |= 1;
    }
    Relocked =
        IoAllocateMdl(First + UserMappingByteOffset, 1, FALSE, FALSE, NULL);
    if (!Relocked)
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    MmProbeAndLockPages(Relocked, UserMode, IoWriteAccess);
    Locked = TRUE;
    if (MmGetMdlPfnArray(Relocked)[0] == MmGetMdlPfnArray(Mdl)[0])
      Report[3] |= 2;
    MmUnmapLockedPages(First, Mdl);
    First = NULL;
    PUCHAR System = MmGetSystemAddressForMdlSafe(Relocked, NormalPagePriority);
    if (System && System[0] == UserMappingSecondByte)
      Report[3] |= 4;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Status = GetExceptionCode();
  }
  if (Locked)
    MmUnlockPages(Relocked);
  if (Relocked)
    IoFreeMdl(Relocked);
  if (ReadOnly)
    MmUnmapLockedPages(ReadOnly, Mdl);
  if (First)
    MmUnmapLockedPages(First, Mdl);
  return Status;
}

static NTSTATUS CheckPartialReuse(PMDL *OriginalMdl, PUCHAR Pool,
                                  volatile UCHAR *Report) {
  enum {
    ViewLength = 16,
    FirstOffset = PAGE_SIZE - 8,
    SecondOffset = PAGE_SIZE + 16,
    InheritedOffset = 32
  };
  PMDL Source = NULL, Partial = NULL, Leaf = NULL;
  PUCHAR UserView = NULL;
  BOOLEAN Locked = FALSE;
  NTSTATUS Status = STATUS_SUCCESS;
  __try {
    UserView = MmMapLockedPagesSpecifyCache(*OriginalMdl, UserMode, MmCached,
                                            NULL, FALSE, NormalPagePriority);
    Source = IoAllocateMdl(UserView, 2 * PAGE_SIZE, FALSE, FALSE, NULL);
    Partial = IoAllocateMdl(UserView, 2 * PAGE_SIZE, FALSE, FALSE, NULL);
    Leaf = IoAllocateMdl(UserView, 2 * PAGE_SIZE, FALSE, FALSE, NULL);
    if (!Source || !Partial || !Leaf)
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    MmProbeAndLockPages(Source, UserMode, IoWriteAccess);
    Locked = TRUE;
    Partial->Next = Leaf;
    IoBuildPartialMdl(Source, Partial, UserView + FirstOffset, ViewLength);
    if (MmGetMdlVirtualAddress(Partial) == UserView + FirstOffset &&
        MmGetMdlByteCount(Partial) == ViewLength)
      Report[2] |= PartialOriginalRange;
    if (MmGetMdlPfnArray(Partial)[0] == MmGetMdlPfnArray(Source)[0] &&
        MmGetMdlPfnArray(Partial)[1] == MmGetMdlPfnArray(Source)[1])
      Report[2] |= PartialPhysicalPages;
    PUCHAR System = MmGetSystemAddressForMdlSafe(Partial, NormalPagePriority);
    if (!System)
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    System[0] = UserMappingFirstByte;
    Report[0] = Pool[FirstOffset];
    MmPrepareMdlForReuse(Partial);
    if ((Partial->MdlFlags & MDL_PARTIAL) &&
        !(Partial->MdlFlags &
          (MDL_PARTIAL_HAS_BEEN_MAPPED | MDL_MAPPED_TO_SYSTEM_VA)) &&
        MmGetMdlByteCount(Partial) == ViewLength && Partial->Next == Leaf &&
        MmGetMdlPfnArray(Partial)[1] == MmGetMdlPfnArray(Source)[1])
      Report[2] |= PartialPreparedDescriptor;
    IoBuildPartialMdl(Source, Partial, UserView + SecondOffset, ViewLength);
    System = MmGetSystemAddressForMdlSafe(Partial, NormalPagePriority);
    if (!System)
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    System[0] = UserMappingSecondByte;
    Report[1] = Pool[SecondOffset];
    if (MmGetMdlVirtualAddress(Partial) == UserView + SecondOffset &&
        MmGetMdlPfnArray(Partial)[0] == MmGetMdlPfnArray(Source)[1] &&
        Partial->Next == Leaf)
      Report[2] |= PartialReusedRange;
    MmPrepareMdlForReuse(Partial);

    // The nonpaged source lends an existing system mapping. The genuine WDK
    // helpers must preserve it without calling MmMapLockedPages again.
    IoBuildPartialMdl(*OriginalMdl, Partial, Pool + InheritedOffset,
                      ViewLength);
    const CSHORT InheritedFlags =
        MDL_MAPPED_TO_SYSTEM_VA | MDL_PARENT_MAPPED_SYSTEM_VA;
    if ((Partial->MdlFlags & InheritedFlags) == InheritedFlags &&
        !(Partial->MdlFlags & MDL_PARTIAL_HAS_BEEN_MAPPED) &&
        MmGetSystemAddressForMdlSafe(Partial, NormalPagePriority) ==
            Pool + InheritedOffset)
      Report[3] |= PartialInheritedMapping;
    MmPrepareMdlForReuse(Partial);
    if (MmGetSystemAddressForMdlSafe(Partial, NormalPagePriority) ==
        Pool + InheritedOffset)
      Report[3] |= PartialInheritedPrepare;
    IoBuildPartialMdl(Partial, Leaf, Pool + InheritedOffset + 1,
                      ViewLength - 1);
    MmUnmapLockedPages(UserView, *OriginalMdl);
    UserView = NULL;
    MmUnlockPages(Source);
    Locked = FALSE;
    IoFreeMdl(Source);
    Source = NULL;
    IoFreeMdl(*OriginalMdl);
    *OriginalMdl = NULL;
    if (MmGetSystemAddressForMdlSafe(Partial, NormalPagePriority) ==
        Pool + InheritedOffset)
      Report[3] |= PartialSourceReleased;
    IoFreeMdl(Partial);
    Partial = NULL;
    if (MmGetSystemAddressForMdlSafe(Leaf, NormalPagePriority) ==
        Pool + InheritedOffset + 1)
      Report[3] |= PartialDescendantSurvives;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Status = GetExceptionCode();
  }
  if (Leaf)
    IoFreeMdl(Leaf);
  if (Partial)
    IoFreeMdl(Partial);
  if (Locked)
    MmUnlockPages(Source);
  if (Source)
    IoFreeMdl(Source);
  if (UserView)
    MmUnmapLockedPages(UserView, *OriginalMdl);
  return Status;
}

static NTSTATUS CheckShortage(PMDL Mdl, volatile UCHAR *Report) {
  enum { MaximumViews = 128 };
  PVOID Views[MaximumViews];
  ULONG Count = 0;
  NTSTATUS Status = STATUS_UNSUCCESSFUL;
  __try {
    while (Count < MaximumViews) {
      Views[Count] = MmMapLockedPagesSpecifyCache(Mdl, UserMode, MmCached, NULL,
                                                  FALSE, NormalPagePriority);
      ++Count;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (GetExceptionCode() == STATUS_INSUFFICIENT_RESOURCES && Count) {
      Report[0] = 1;
      Status = STATUS_SUCCESS;
    }
  }
  while (Count)
    MmUnmapLockedPages(Views[--Count], Mdl);
  return Status;
}

static NTSTATUS Dispatch(PDEVICE_OBJECT Object, PIRP Irp) {
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  UNREFERENCED_PARAMETER(Object);
  if (Stack->MajorFunction != IRP_MJ_DEVICE_CONTROL)
    return Complete(Irp, STATUS_SUCCESS);
  const ULONG Action = Stack->Parameters.DeviceIoControl.IoControlCode;
  if (Stack->Parameters.DeviceIoControl.InputBufferLength !=
      sizeof(DriverUserMappingRequest))
    return Complete(Irp, STATUS_INVALID_PARAMETER);
  DriverUserMappingRequest *Request =
      Stack->Parameters.DeviceIoControl.Type3InputBuffer;
  volatile UCHAR *Report;
  __try {
    ProbeForRead(Request, sizeof(*Request), TYPE_ALIGNMENT(PVOID));
    Report = Request->Report;
    ProbeForWrite((PVOID)Report, UserMappingReportSize, 1);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return Complete(Irp, GetExceptionCode());
  }
  const ULONG Size = Action == UserMappingShortage       ? 32 * PAGE_SIZE
                     : Action == UserMappingPartialReuse ? 2 * PAGE_SIZE
                                                         : PAGE_SIZE;
  PUCHAR Pool = ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, UserMappingPoolTag);
  if (!Pool)
    return Complete(Irp, STATUS_INSUFFICIENT_RESOURCES);
  PMDL Mdl = IoAllocateMdl(Pool, Size, FALSE, FALSE, NULL);
  if (!Mdl) {
    ExFreePoolWithTag(Pool, UserMappingPoolTag);
    return Complete(Irp, STATUS_INSUFFICIENT_RESOURCES);
  }
  MmBuildMdlForNonPagedPool(Mdl);
  NTSTATUS Status;
  if (Action == UserMappingPoolAliases)
    Status = CheckAliases(Mdl, Pool, Report);
  else if (Action == UserMappingShortage)
    Status = CheckShortage(Mdl, Report);
  else if (Action == UserMappingPartialReuse)
    Status = CheckPartialReuse(&Mdl, Pool, Report);
  else if (Action == UserMappingAttachedWorker ||
           Action == UserMappingProcessExit ||
           Action == UserMappingWrongProcessUnmap) {
    PMDL ReportMdl =
        IoAllocateMdl((PVOID)Report, UserMappingReportSize, FALSE, FALSE, NULL);
    PIO_WORKITEM Work = IoAllocateWorkItem(Device);
    if (!ReportMdl || !Work) {
      if (ReportMdl)
        IoFreeMdl(ReportMdl);
      if (Work)
        IoFreeWorkItem(Work);
      ReleasePool(Mdl, Pool);
      return Complete(Irp, STATUS_INSUFFICIENT_RESOURCES);
    }
    MmProbeAndLockPages(ReportMdl, UserMode, IoWriteAccess);
    Pending.Irp = Irp;
    Pending.Work = Work;
    Pending.Process = IoGetRequestorProcess(Irp);
    Pending.PoolMdl = Mdl;
    Pending.ReportMdl = ReportMdl;
    Pending.Pool = Pool;
    Pending.Report =
        MmGetSystemAddressForMdlSafe(ReportMdl, NormalPagePriority);
    Pending.Action = Action;
    if (Action != UserMappingAttachedWorker)
      Pending.UserView = MmMapLockedPagesSpecifyCache(
          Mdl, UserMode, MmCached, NULL, FALSE, NormalPagePriority);
    IoMarkIrpPending(Irp);
    IoQueueWorkItem(Work, MappingWorker, DelayedWorkQueue, &Pending);
    return STATUS_PENDING;
  } else
    Status = STATUS_INVALID_DEVICE_REQUEST;
  ReleasePool(Mdl, Pool);
  return Complete(Irp, Status);
}

static VOID Unload(PDRIVER_OBJECT Driver) {
  UNREFERENCED_PARAMETER(Driver);
  IoDeleteDevice(Device);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING Path) {
  UNICODE_STRING Name;
  UNREFERENCED_PARAMETER(Path);
  RtlInitUnicodeString(&Name, L"\\Device\\NeverDUserMapping");
  NTSTATUS Status =
      IoCreateDevice(Driver, 0, &Name, FILE_DEVICE_UNKNOWN, 0, FALSE, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  Device->Flags &= ~DO_DEVICE_INITIALIZING;
  Driver->DriverUnload = Unload;
  Driver->MajorFunction[IRP_MJ_CREATE] = Dispatch;
  Driver->MajorFunction[IRP_MJ_CLEANUP] = Dispatch;
  Driver->MajorFunction[IRP_MJ_CLOSE] = Dispatch;
  Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Dispatch;
  return STATUS_SUCCESS;
}
