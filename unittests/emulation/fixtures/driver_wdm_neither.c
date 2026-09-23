//===- driver_wdm_neither.c - Genuine WDK neither-I/O fixture -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original x64 WDM driver exercising user probes, actual C handlers and
/// driver-created locked user MDLs with genuine WDK declarations.
//===----------------------------------------------------------------------===//

#include <ntifs.h>

#define IO_NORMAL 0x222003u
#define IO_LATE_FAULT 0x222007u
#define IO_PROBE_ERRORS 0x22200bu
#define IO_WRITE_PROBE 0x22200fu
#define IO_LOCKED 0x222013u
#define IO_HELPER_FAULT 0x222017u
#define IO_WORKER_LOCKED 0x22201bu
#define IO_WORKER_RAW 0x22201fu
#define IO_CANCEL 0x222023u
#define IO_CANCEL_BAD_RELEASE 0x222027u
#define IO_SELF_CANCEL 0x22202bu
#define IO_SELF_CANCEL_NO_ROUTINE 0x22202fu

static PDEVICE_OBJECT Device;
static volatile ULONG Stage;

typedef struct _WORKER_STATE {
  PIRP Irp;
  PIO_WORKITEM Work;
  PMDL InMdl;
  PMDL OutMdl;
  PUCHAR InAlias;
  PUCHAR OutAlias;
  volatile UCHAR *RawInput;
  BOOLEAN Raw;
  BOOLEAN Cancelable;
  BOOLEAN Cancelled;
  BOOLEAN BadRelease;
  ULONG RequestorPID;
} WORKER_STATE;

static WORKER_STATE WorkerState;

static NTSTATUS Complete(PIRP Irp, NTSTATUS Status, ULONG_PTR Length) {
  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = Length;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

static VOID NeitherCancel(PDEVICE_OBJECT Object, PIRP Irp) {
  if (WorkerState.BadRelease) {
    IoReleaseCancelSpinLock((KIRQL)(Irp->CancelIrql + 1));
    return;
  }
  const BOOLEAN Valid =
      Object == Device && WorkerState.Irp == Irp && Irp->Cancel &&
      !Irp->CancelRoutine && KeGetCurrentIrql() == DISPATCH_LEVEL &&
      IoGetRequestorProcessId(Irp) == WorkerState.RequestorPID;
  WorkerState.Cancelled = TRUE;
  WorkerState.Irp = NULL;
  IoReleaseCancelSpinLock(Irp->CancelIrql);
  MmUnlockPages(WorkerState.OutMdl);
  MmUnlockPages(WorkerState.InMdl);
  IoFreeMdl(WorkerState.OutMdl);
  IoFreeMdl(WorkerState.InMdl);
  WorkerState.OutMdl = NULL;
  WorkerState.InMdl = NULL;
  WorkerState.InAlias = NULL;
  WorkerState.OutAlias = NULL;
  Complete(Irp, Valid ? STATUS_CANCELLED : STATUS_INVALID_DEVICE_STATE, 0);
}

static VOID SelfCancel(PDEVICE_OBJECT Object, PIRP Irp) {
  const BOOLEAN Valid = Object == Device && Irp->Cancel &&
                        !Irp->CancelRoutine &&
                        KeGetCurrentIrql() == DISPATCH_LEVEL;
  IoReleaseCancelSpinLock(Irp->CancelIrql);
  Complete(Irp, Valid ? STATUS_CANCELLED : STATUS_INVALID_DEVICE_STATE, 0);
}

static VOID NeitherWorker(PDEVICE_OBJECT Object, PVOID Context) {
  WORKER_STATE *State = (WORKER_STATE *)Context;
  if (State->Cancelled) {
    IoFreeWorkItem(State->Work);
    State->Work = NULL;
    State->Cancelable = FALSE;
    State->Cancelled = FALSE;
    State->BadRelease = FALSE;
    return;
  }
  PIRP Irp = State->Irp;
  if (State->Raw) {
    Stage = State->RawInput[0];
    return;
  }
  NTSTATUS Status = STATUS_SUCCESS;
  ULONG_PTR Length = 0;
  if (State->Cancelable) {
    KIRQL OldIRQL;
    IoAcquireCancelSpinLock(&OldIRQL);
    PDRIVER_CANCEL Old = IoSetCancelRoutine(Irp, NULL);
    IoReleaseCancelSpinLock(OldIRQL);
    if (Old != NeitherCancel)
      Status = STATUS_INVALID_DEVICE_STATE;
  }
  if (Object != Device || ExGetPreviousMode() != KernelMode ||
      !State->InAlias || !State->OutAlias ||
      IoGetRequestorProcessId(Irp) != State->RequestorPID ||
      (ULONG)(ULONG_PTR)PsGetCurrentProcessId() == State->RequestorPID)
    Status = STATUS_INVALID_PARAMETER;
  else if (NT_SUCCESS(Status)) {
    for (ULONG I = 0; I < 4; ++I)
      State->OutAlias[I] = State->InAlias[I] + 0x10;
    Length = 4;
  }
  MmUnlockPages(State->OutMdl);
  MmUnlockPages(State->InMdl);
  IoFreeMdl(State->OutMdl);
  IoFreeMdl(State->InMdl);
  IoFreeWorkItem(State->Work);
  State->Irp = NULL;
  State->Work = NULL;
  State->InMdl = NULL;
  State->OutMdl = NULL;
  State->InAlias = NULL;
  State->OutAlias = NULL;
  State->RawInput = NULL;
  State->Cancelable = FALSE;
  State->BadRelease = FALSE;
  Complete(Irp, Status, Length);
}

__declspec(noinline) static UCHAR FaultInHelper(VOID) {
  return *(volatile UCHAR *)0x20000000u;
}

static NTSTATUS Dispatch(PDEVICE_OBJECT Object, PIRP Irp) {
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  NTSTATUS Status = STATUS_SUCCESS;
  ULONG_PTR Length = 0;
  UNREFERENCED_PARAMETER(Object);
  if (Stack->MajorFunction == IRP_MJ_READ ||
      Stack->MajorFunction == IRP_MJ_WRITE) {
    const BOOLEAN Read = Stack->MajorFunction == IRP_MJ_READ;
    const ULONG BufferLength =
        Read ? Stack->Parameters.Read.Length : Stack->Parameters.Write.Length;
    volatile UCHAR *Buffer = (volatile UCHAR *)Irp->UserBuffer;
    if (BufferLength != 4 || !Buffer || Irp->RequestorMode != UserMode ||
        Irp->AssociatedIrp.SystemBuffer || Irp->MdlAddress)
      return Complete(Irp, STATUS_INVALID_PARAMETER, 0);
    __try {
      if (Read) {
        ProbeForWrite((PVOID)Buffer, BufferLength, 1);
        for (ULONG I = 0; I < BufferLength; ++I)
          Buffer[I] = (UCHAR)(0x70 + I);
      } else {
        ProbeForRead((PVOID)Buffer, BufferLength, 1);
        for (ULONG I = 0; I < BufferLength; ++I)
          if (Buffer[I] != I + 1)
            Status = STATUS_INVALID_PARAMETER;
      }
      if (NT_SUCCESS(Status))
        Length = BufferLength;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      Status = GetExceptionCode();
    }
    return Complete(Irp, Status, Length);
  }
  if (Stack->MajorFunction != IRP_MJ_DEVICE_CONTROL)
    return Complete(Irp, Status, 0);

  const ULONG Code = Stack->Parameters.DeviceIoControl.IoControlCode;
  const ULONG InputLength = Stack->Parameters.DeviceIoControl.InputBufferLength;
  const ULONG OutputLength = Stack->Parameters.DeviceIoControl.OutputBufferLength;
  volatile UCHAR *Input =
      (volatile UCHAR *)Stack->Parameters.DeviceIoControl.Type3InputBuffer;
  volatile UCHAR *Output = (volatile UCHAR *)Irp->UserBuffer;
  if ((Code & 3) != METHOD_NEITHER || InputLength != 4 || OutputLength != 4 ||
      !Input || !Output || Irp->RequestorMode != UserMode ||
      Irp->AssociatedIrp.SystemBuffer || Irp->MdlAddress) {
    DbgPrint("WDM neither: invalid packet\n");
    return Complete(Irp, STATUS_INVALID_PARAMETER, 0);
  }

  switch (Code) {
  case IO_SELF_CANCEL: {
    KIRQL OldIRQL;
    IoAcquireCancelSpinLock(&OldIRQL);
    if (IoSetCancelRoutine(Irp, SelfCancel)) {
      IoReleaseCancelSpinLock(OldIRQL);
      return Complete(Irp, STATUS_INVALID_DEVICE_STATE, 0);
    }
    IoReleaseCancelSpinLock(OldIRQL);
    IoMarkIrpPending(Irp);
    if (!IoCancelIrp(Irp))
      Status = STATUS_INVALID_DEVICE_STATE;
    return STATUS_PENDING;
  }
  case IO_SELF_CANCEL_NO_ROUTINE:
    if (IoCancelIrp(Irp))
      Status = STATUS_INVALID_DEVICE_STATE;
    else
      Status = STATUS_CANCELLED;
    return Complete(Irp, Status, 0);
  case IO_NORMAL:
    __try {
      ProbeForRead(Input, InputLength, 1);
      ProbeForWrite(Output, OutputLength, 1);
      for (ULONG I = 0; I < 4; ++I)
        Output[I] = Input[I] ^ 0x5a;
      Length = 4;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      Status = GetExceptionCode();
    }
    break;
  case IO_LATE_FAULT:
    Stage = 0;
    __try {
      ProbeForRead((PVOID)0x20000000u, 1, 1);
      Stage = 1;
      Output[0] = *(volatile UCHAR *)0x20000000u;
      Stage = 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (Stage != 1 || GetExceptionCode() != STATUS_ACCESS_VIOLATION)
        Status = STATUS_UNSUCCESSFUL;
      else {
        Output[0] = 0xa1;
        Length = 1;
      }
    }
    break;
  case IO_PROBE_ERRORS:
    Stage = 0;
    ProbeForRead((PVOID)0x70000001u, 0, 0);
    ProbeForWrite((PVOID)0x70000001u, 0, 0);
    __try {
      ProbeForRead((PVOID)0x20000001u, 1, 2);
      Stage = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (GetExceptionCode() == STATUS_DATATYPE_MISALIGNMENT)
        Stage = 2;
    }
    if (Stage != 2)
      Status = STATUS_UNSUCCESSFUL;
    __try {
      ProbeForRead((PVOID)0x70000000u, 1, 1);
      Stage = 3;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (GetExceptionCode() == STATUS_ACCESS_VIOLATION)
        Stage = 4;
    }
    if (Stage != 4)
      Status = STATUS_UNSUCCESSFUL;
    Output[0] = (UCHAR)Stage;
    Length = 1;
    break;
  case IO_WRITE_PROBE:
    Stage = 0;
    __try {
      ProbeForWrite((PVOID)0x20000000u, 1, 1);
      Stage = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (GetExceptionCode() == STATUS_ACCESS_VIOLATION)
        Stage = 2;
    }
    if (Stage != 2)
      Status = STATUS_UNSUCCESSFUL;
    Output[0] = (UCHAR)Stage;
    Length = 1;
    break;
  case IO_LOCKED: {
    PMDL InMdl = IoAllocateMdl((PVOID)Input, InputLength, FALSE, FALSE, NULL);
    PMDL OutMdl = IoAllocateMdl((PVOID)Output, OutputLength, FALSE, FALSE, NULL);
    BOOLEAN InLocked = FALSE, OutLocked = FALSE;
    if (!InMdl || !OutMdl) {
      Status = STATUS_INSUFFICIENT_RESOURCES;
      break;
    }
    __try {
      MmProbeAndLockPages(InMdl, UserMode, IoReadAccess);
      InLocked = TRUE;
      MmProbeAndLockPages(OutMdl, UserMode, IoWriteAccess);
      OutLocked = TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      Status = GetExceptionCode();
    }
    if (NT_SUCCESS(Status)) {
      PUCHAR InAlias = MmGetSystemAddressForMdlSafe(InMdl, NormalPagePriority);
      PUCHAR OutAlias = MmGetSystemAddressForMdlSafe(OutMdl, NormalPagePriority);
      if (!InAlias || !OutAlias)
        Status = STATUS_INSUFFICIENT_RESOURCES;
      else {
        for (ULONG I = 0; I < 4; ++I)
          OutAlias[I] = InAlias[I] + 1;
        Length = 4;
      }
    }
    if (OutLocked)
      MmUnlockPages(OutMdl);
    if (InLocked)
      MmUnlockPages(InMdl);
    IoFreeMdl(OutMdl);
    IoFreeMdl(InMdl);
    break;
  }
  case IO_HELPER_FAULT:
    Stage = 0;
    __try {
      Stage = 1;
      Output[0] = FaultInHelper();
      Stage = 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (Stage != 1 || GetExceptionCode() != STATUS_ACCESS_VIOLATION)
        Status = STATUS_UNSUCCESSFUL;
      else {
        Output[0] = 0xa6;
        Length = 1;
      }
    }
    break;
  case IO_WORKER_LOCKED:
  case IO_WORKER_RAW:
  case IO_CANCEL:
  case IO_CANCEL_BAD_RELEASE: {
    if (WorkerState.Irp)
      return Complete(Irp, STATUS_INVALID_DEVICE_STATE, 0);
    const ULONG RequestorPID = IoGetRequestorProcessId(Irp);
    if (!RequestorPID ||
        RequestorPID != (ULONG)(ULONG_PTR)PsGetCurrentProcessId())
      return Complete(Irp, STATUS_INVALID_DEVICE_STATE, 0);
    PMDL InMdl = NULL, OutMdl = NULL;
    BOOLEAN InLocked = FALSE, OutLocked = FALSE;
    PIO_WORKITEM Work = NULL;
    if (Code != IO_WORKER_RAW) {
      InMdl = IoAllocateMdl((PVOID)Input, InputLength, FALSE, FALSE, NULL);
      OutMdl = IoAllocateMdl((PVOID)Output, OutputLength, FALSE, FALSE, NULL);
      if (!InMdl || !OutMdl) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto WorkerFailure;
      }
      __try {
        MmProbeAndLockPages(InMdl, UserMode, IoReadAccess);
        InLocked = TRUE;
        MmProbeAndLockPages(OutMdl, UserMode, IoWriteAccess);
        OutLocked = TRUE;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        Status = GetExceptionCode();
      }
      if (!NT_SUCCESS(Status))
        goto WorkerFailure;
      WorkerState.InAlias =
          MmGetSystemAddressForMdlSafe(InMdl, NormalPagePriority);
      WorkerState.OutAlias =
          MmGetSystemAddressForMdlSafe(OutMdl, NormalPagePriority);
      if (!WorkerState.InAlias || !WorkerState.OutAlias) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto WorkerFailure;
      }
    }
    Work = IoAllocateWorkItem(Device);
    if (!Work) {
      Status = STATUS_INSUFFICIENT_RESOURCES;
      goto WorkerFailure;
    }
    WorkerState.Irp = Irp;
    WorkerState.Work = Work;
    WorkerState.InMdl = InMdl;
    WorkerState.OutMdl = OutMdl;
    WorkerState.RawInput = Input;
    WorkerState.Raw = Code == IO_WORKER_RAW;
    WorkerState.Cancelable = Code == IO_CANCEL || Code == IO_CANCEL_BAD_RELEASE;
    WorkerState.Cancelled = FALSE;
    WorkerState.BadRelease = Code == IO_CANCEL_BAD_RELEASE;
    WorkerState.RequestorPID = RequestorPID;
    if (WorkerState.Cancelable) {
      KIRQL OldIRQL;
      IoAcquireCancelSpinLock(&OldIRQL);
      if (IoSetCancelRoutine(Irp, NeitherCancel)) {
        IoReleaseCancelSpinLock(OldIRQL);
        Status = STATUS_INVALID_DEVICE_STATE;
        goto WorkerFailure;
      }
      IoReleaseCancelSpinLock(OldIRQL);
    }
    IoMarkIrpPending(Irp);
    IoQueueWorkItem(Work, NeitherWorker, DelayedWorkQueue, &WorkerState);
    return STATUS_PENDING;
  WorkerFailure:
    if (Work)
      IoFreeWorkItem(Work);
    if (OutLocked)
      MmUnlockPages(OutMdl);
    if (InLocked)
      MmUnlockPages(InMdl);
    if (OutMdl)
      IoFreeMdl(OutMdl);
    if (InMdl)
      IoFreeMdl(InMdl);
    WorkerState.InAlias = NULL;
    WorkerState.OutAlias = NULL;
    WorkerState.Cancelable = FALSE;
    WorkerState.Cancelled = FALSE;
    WorkerState.BadRelease = FALSE;
    break;
  }
  default:
    Status = STATUS_INVALID_DEVICE_REQUEST;
    break;
  }
  DbgPrint("WDM neither: code=%08lx status=%08lx stage=%lu\n", Code,
           (ULONG)Status, Stage);
  return Complete(Irp, Status, Length);
}

static VOID Unload(PDRIVER_OBJECT Driver) {
  UNREFERENCED_PARAMETER(Driver);
  IoDeleteDevice(Device);
  DbgPrint("WDM neither: unload\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING Path) {
  UNICODE_STRING Name;
  UNREFERENCED_PARAMETER(Path);
  RtlInitUnicodeString(&Name, L"\\Device\\NeverDNeither");
  NTSTATUS Status = IoCreateDevice(Driver, 0, &Name, FILE_DEVICE_UNKNOWN, 0,
                                   FALSE, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  Device->Flags &= ~DO_DEVICE_INITIALIZING;
  Driver->DriverUnload = Unload;
  Driver->MajorFunction[IRP_MJ_CREATE] = Dispatch;
  Driver->MajorFunction[IRP_MJ_CLEANUP] = Dispatch;
  Driver->MajorFunction[IRP_MJ_CLOSE] = Dispatch;
  Driver->MajorFunction[IRP_MJ_READ] = Dispatch;
  Driver->MajorFunction[IRP_MJ_WRITE] = Dispatch;
  Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Dispatch;
  return STATUS_SUCCESS;
}
