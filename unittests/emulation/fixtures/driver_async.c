//===- driver_async.c - Original asynchronous WDM fixture -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Extend the software lifecycle fixture with deferred IOCTL completion.
///
//===----------------------------------------------------------------------===//

#define DriverEntry BufferedDriverEntry
#include "driver_io.c"
#undef DriverEntry

#define NEVERD_ASYNC_CASE(Name, Code) enum { Name = Code };
#include "DriverAsyncCases.def"
#undef NEVERD_ASYNC_CASE
#define NEVERD_WDM_VALUE(Name, Value) enum { Name = Value };
#include "../../../lib/emulation/windows/KernelValues.def"
#include "../../../lib/emulation/windows/WindowsKernelLayout.def"
#undef NEVERD_WDM_VALUE

__declspec(dllimport) void *IoAllocateWorkItem(void *);
__declspec(dllimport) void IoQueueWorkItem(void *, void (*)(void *, void *),
                                           U32, void *);
__declspec(dllimport) void IoFreeWorkItem(void *);
__declspec(dllimport) void IoMarkIrpPending(IRP *);
__declspec(dllimport) U8 KeGetCurrentIrql(void);
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) void IoAcquireCancelSpinLock(U8 *);
__declspec(dllimport) void IoReleaseCancelSpinLock(U8);

static void *WorkItem;
static U32 Mode, Runs;
enum { BatchPoolTag = 0x42415443 };

typedef struct {
  IRP *Request;
  void *Work;
  U32 Slot;
  U8 Cancelable;
  U8 Cancelled;
} BATCH_STATE;
static BATCH_STATE *BatchStates[64];

static void BatchCancel(void *Target, IRP *Request) {
  BATCH_STATE *State = 0;
  for (U32 I = 0; I < 64; ++I)
    if (BatchStates[I] && BatchStates[I]->Request == Request) {
      State = BatchStates[I];
      break;
    }
  U8 Valid = Target == Device && State && KeGetCurrentIrql() == 2 &&
             *((U8 *)Request + IRPCancelOffset);
  if (State)
    State->Cancelled = 1;
  IoReleaseCancelSpinLock(*((U8 *)Request + IRPCancelIROffset));
  Request->Status = Valid ? (NTSTATUS)0xc0000120U : (NTSTATUS)0xc000000dU;
  Request->Information = 0;
  IofCompleteRequest(Request, 0);
}

static void BatchWorker(void *Target, void *Context) {
  BATCH_STATE *State = Context;
  IRP *Request = State->Request;
  void *Work = State->Work;
  if (State->Cancelled) {
    IoFreeWorkItem(Work);
    BatchStates[State->Slot] = 0;
    ExFreePoolWithTag(State, BatchPoolTag);
    return;
  }
  if (State->Cancelable) {
    U8 OldIRQL;
    IoAcquireCancelSpinLock(&OldIRQL);
    *(void **)((U8 *)Request + IRPCancelRoutineOffset) = 0;
    IoReleaseCancelSpinLock(OldIRQL);
  }
  U32 Length = Request->Stack->InputLength;
  NTSTATUS Status = 0;
  if (Target != Device || KeGetCurrentIrql() != 0 ||
      Request->Stack->OutputLength < Length)
    Status = (NTSTATUS)0xc000000dU;
  else
    for (U32 I = 0; I < Length; ++I)
      Request->SystemBuffer[I] ^= 0x5a;
  IoFreeWorkItem(Work);
  BatchStates[State->Slot] = 0;
  ExFreePoolWithTag(State, BatchPoolTag);
  Request->Status = Status;
  Request->Information = Status ? 0 : Length;
  IofCompleteRequest(Request, 0);
}

static void Worker(void *Target, void *Context) {
  IRP *Request = Context;
  if (Mode == WorkerLoop)
    for (;;)
      __asm__ volatile("pause");
  if (Mode == CallbackFault)
    *(volatile U32 *)0x1234 = 1;
  if (Mode == RequeueCurrent && !Runs++) {
    IoQueueWorkItem(WorkItem, Worker, 1, Request);
    return;
  }
  IoFreeWorkItem(WorkItem);
  if (Mode == DeleteInWorker)
    IoDeleteDevice(Device);
  if (Mode == DoubleFree)
    IoFreeWorkItem(WorkItem);
  if (Mode == WorkerNoCompletion)
    return;
  NTSTATUS Status = 0;
  U64 Information = 0;
  if (Target != Device || KeGetCurrentIrql() != 0 || Mode == DeferredFailure)
    Status = (NTSTATUS)0xc000000dU;
  else if (Request->Stack->OutputLength < Request->Stack->InputLength)
    Status = (NTSTATUS)0xc0000023U;
  else {
    Information = Request->Stack->InputLength;
    for (U32 I = 0; I < Information; ++I)
      Request->SystemBuffer[I] ^= 0x5a;
  }
  Request->Status = Status;
  Request->Information = Information;
  IofCompleteRequest(Request, 0);
  if (Mode == CompletedAccess)
    *(volatile U32 *)&Request->Status = 0;
}

static NTSTATUS AsyncDispatch(void *Target, IRP *Request) {
  (void)Target;
  if (Request->Stack->ControlCode == BatchDeferred ||
      Request->Stack->ControlCode == BatchCancelable) {
    U32 Slot = 64;
    for (U32 I = 0; I < 64; ++I)
      if (!BatchStates[I]) {
        Slot = I;
        break;
      }
    if (Slot == 64) {
      Request->Status = (NTSTATUS)0xc000009aU;
      IofCompleteRequest(Request, 0);
      return Request->Status;
    }
    BATCH_STATE *State =
        ExAllocatePoolWithTag(0, sizeof(BATCH_STATE), BatchPoolTag);
    if (!State) {
      Request->Status = (NTSTATUS)0xc000009aU;
      IofCompleteRequest(Request, 0);
      return Request->Status;
    }
    State->Request = Request;
    State->Slot = Slot;
    State->Cancelable = Request->Stack->ControlCode == BatchCancelable;
    State->Cancelled = 0;
    State->Work = IoAllocateWorkItem(Device);
    if (!State->Work) {
      ExFreePoolWithTag(State, BatchPoolTag);
      Request->Status = (NTSTATUS)0xc000009aU;
      IofCompleteRequest(Request, 0);
      return Request->Status;
    }
    BatchStates[Slot] = State;
    if (State->Cancelable) {
      U8 OldIRQL;
      IoAcquireCancelSpinLock(&OldIRQL);
      *(void **)((U8 *)Request + IRPCancelRoutineOffset) = BatchCancel;
      IoReleaseCancelSpinLock(OldIRQL);
    }
    IoMarkIrpPending(Request);
    IoQueueWorkItem(State->Work, BatchWorker, 1, State);
    return StatusPending;
  }
  Mode = Request->Stack->ControlCode;
  Runs = 0;
  if (Mode != MissingMark) {
    if (Mode == DeferredSuccess)
      IoMarkIrpPending(Request);
    else
      Request->Stack->Control |= StackPendingReturned;
  }
  if (Mode == NoProducer)
    return StatusPending;
  if (Mode == EarlyCompletion || Mode == EarlyWrongReturn) {
    Request->Status = 0;
    Request->Information = 0;
    IofCompleteRequest(Request, 0);
    return Mode == EarlyCompletion ? StatusPending : 0;
  }
  WorkItem = IoAllocateWorkItem(Device);
  if (Mode == OpaqueWorkItem)
    *(volatile U8 *)WorkItem = 1;
  IoQueueWorkItem(WorkItem, Worker, 1, Request);
  if (Mode == DoubleQueue)
    IoQueueWorkItem(WorkItem, Worker, 1, Request);
  if (Mode == FreeQueued)
    IoFreeWorkItem(WorkItem);
  return Mode == MarkedSuccess ? 0 : StatusPending;
}

static void AsyncUnload(DRIVER_OBJECT *Driver) {
  if (Mode == DeleteInWorker)
    IoDeleteSymbolicLink(&LinkName);
  else
    Unload(Driver);
}

NTSTATUS DriverEntry(DRIVER_OBJECT *Driver, UNICODE_STRING *RegistryPath) {
  NTSTATUS Status = BufferedDriverEntry(Driver, RegistryPath);
  if (Status >= 0) {
    Driver->MajorFunction[14] = (void *)AsyncDispatch;
    Driver->DriverUnload = (void *)AsyncUnload;
  }
  return Status;
}
