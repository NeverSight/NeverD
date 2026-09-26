//===- driver_dispatcher.c - Original WDM dispatcher fixture --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute timer, DPC, worker and wait contracts through compiled x64 code.
///
//===----------------------------------------------------------------------===//

#define DriverEntry BufferedDriverEntry
#include "driver_io.c"
#undef DriverEntry

#define NEVERD_DISPATCHER_CASE(Name, Code, Pending, Dpcs, Workers, Waits,      \
                               Proof)                                          \
  enum { Name = Code };
#define NEVERD_DISPATCHER_FAILURE(Name, Code, Diagnostic) enum { Name = Code };
#include "DriverDispatcherCases.def"
#undef NEVERD_DISPATCHER_FAILURE
#undef NEVERD_DISPATCHER_CASE
#define NEVERD_WDM_VALUE(Name, Value) enum { Name = Value };
#include "../../../lib/emulation/windows/KernelValues.def"
#undef NEVERD_WDM_VALUE
#define NEVERD_KERNEL_DISPATCHER_VALUE(Name, Value)                            \
  enum { Dispatcher##Name = Value };
#include "../../../lib/emulation/windows/KernelDispatcherValues.def"
#undef NEVERD_KERNEL_DISPATCHER_VALUE

// Public x64 WDM declarations, independently checked against Microsoft's
// wdkmetadata wdm.h at fbb3a13785073a707c6171bffbd5522438031732.
// Storage is opaque to the driver; only initialization/service APIs access it.
typedef long long I64;
typedef union {
  I64 QuadPart;
  struct {
    U32 LowPart;
    int HighPart;
  } Parts;
} LARGE_INTEGER;
typedef struct {
  U32 Control;
  int SignalState;
  void *WaitList[2];
} DISPATCHER_HEADER;
typedef struct {
  DISPATCHER_HEADER Header;
} KEVENT;
typedef struct KDPC KDPC;
typedef void (*DPC_ROUTINE)(KDPC *, void *, void *, void *);
struct KDPC {
  U32 TargetInfo;
  void *DpcListEntry;
  U64 ProcessorHistory;
  DPC_ROUTINE DeferredRoutine;
  void *DeferredContext;
  void *SystemArgument1;
  void *SystemArgument2;
  void *DpcData;
};
typedef struct {
  DISPATCHER_HEADER Header;
  U64 DueTime;
  void *TimerList[2];
  KDPC *Dpc;
  U16 Processor;
  U16 TimerType;
  U32 Period;
} KTIMER;
_Static_assert(sizeof(LARGE_INTEGER) == 8, "x64 LARGE_INTEGER");
_Static_assert(sizeof(KEVENT) == DispatcherEventSize, "x64 KEVENT");
_Static_assert(sizeof(KDPC) == DispatcherDpcSize, "x64 KDPC");
_Static_assert(_Alignof(KDPC) == DispatcherObjectAlignment,
               "x64 KDPC alignment");
_Static_assert(__builtin_offsetof(KDPC, DeferredRoutine) == 0x18,
               "x64 KDPC routine");
_Static_assert(__builtin_offsetof(KDPC, SystemArgument2) == 0x30,
               "x64 KDPC fourth argument");
_Static_assert(sizeof(KTIMER) == DispatcherTimerSize, "x64 KTIMER");
_Static_assert(__builtin_offsetof(KTIMER, Dpc) == 0x30, "x64 KTIMER DPC");
_Static_assert(__builtin_offsetof(KTIMER, Period) == 0x3c, "x64 KTIMER period");

enum { HighImportance = 2, DelayedQueue = 1, PoolTag = 0x44535054 };

__declspec(dllimport) void KeInitializeDpc(KDPC *, DPC_ROUTINE, void *);
__declspec(dllimport) U8 KeInsertQueueDpc(KDPC *, void *, void *);
__declspec(dllimport) U8 KeRemoveQueueDpc(KDPC *);
__declspec(dllimport) void KeSetImportanceDpc(KDPC *, U32);
__declspec(dllimport) void KeSetTargetProcessorDpc(KDPC *, signed char);
__declspec(dllimport) void KeInitializeTimer(KTIMER *);
__declspec(dllimport) void KeInitializeTimerEx(KTIMER *, U32);
__declspec(dllimport) U8 KeSetTimer(KTIMER *, LARGE_INTEGER, KDPC *);
__declspec(dllimport) U8 KeSetTimerEx(KTIMER *, LARGE_INTEGER, int, KDPC *);
__declspec(dllimport) U8 KeCancelTimer(KTIMER *);
__declspec(dllimport) U8 KeReadStateTimer(KTIMER *);
__declspec(dllimport) void KeInitializeEvent(KEVENT *, U32, U8);
__declspec(dllimport) int KeSetEvent(KEVENT *, int, U8);
__declspec(dllimport) int KeResetEvent(KEVENT *);
__declspec(dllimport) void KeClearEvent(KEVENT *);
__declspec(dllimport) int KeReadStateEvent(KEVENT *);
__declspec(dllimport) NTSTATUS KeWaitForSingleObject(void *, U32, signed char,
                                                     U8, LARGE_INTEGER *);
__declspec(dllimport) NTSTATUS KeDelayExecutionThread(signed char, U8,
                                                      LARGE_INTEGER *);
__declspec(dllimport) void *IoAllocateWorkItem(void *);
__declspec(dllimport) void IoQueueWorkItem(void *, void (*)(void *, void *),
                                           U32, void *);
__declspec(dllimport) void IoFreeWorkItem(void *);
__declspec(dllimport) void IoMarkIrpPending(IRP *);
__declspec(dllimport) U8 KeGetCurrentIrql(void);
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);

static KDPC Dpc;
static KTIMER Timer;
static KEVENT Event;
static void *WaitWorkItem, *SignalWorkItem;
static IRP *ActiveRequest;
static U32 Mode, Failures, DpcRuns, WorkerRuns, Waits, StackProof,
    DpcPriorityFlag;
static U64 ArgumentOne, ArgumentTwo;

static void Check(int Condition) {
  if (!Condition)
    ++Failures;
}

static NTSTATUS Complete(IRP *Request) {
  NTSTATUS Status = Failures ? (NTSTATUS)0xc000000dU : StatusSuccess;
  Request->Status = Status;
  Request->Information = Status == StatusSuccess ? 8 : 0;
  if (Status == StatusSuccess) {
    Request->SystemBuffer[0] = (U8)DpcRuns;
    Request->SystemBuffer[1] = (U8)WorkerRuns;
    Request->SystemBuffer[2] = (U8)Waits;
    Request->SystemBuffer[3] = (U8)StackProof;
    Request->SystemBuffer[4] = 'W';
    Request->SystemBuffer[5] = 'D';
    Request->SystemBuffer[6] = 'M';
    Request->SystemBuffer[7] = '!';
  }
  IofCompleteRequest(Request, 0);
  return Status;
}

static void ObserveWait(void *Object, LARGE_INTEGER *Timeout,
                        NTSTATUS Expected) {
  Check(KeWaitForSingleObject(Object, ExecutiveWaitReason, KernelMode, 0,
                              Timeout) == Expected);
  ++Waits;
}

// Volatile locals must stay on the suspended guest stack, including when a
// worker resumes after another worker has used its own stack.
static void WaitWithStack(void) {
  volatile U64 Local[4];
  Local[0] = 0x1122334455667788ULL;
  Local[1] = 0x8877665544332211ULL;
  Local[2] = (U64)&Local[0];
  Local[3] = Local[0] ^ Local[1];
  ObserveWait(&Event, 0, StatusSuccess);
  Check(Local[0] == 0x1122334455667788ULL);
  Check(Local[1] == 0x8877665544332211ULL);
  Check(Local[2] == (U64)&Local[0]);
  Check(Local[3] == (Local[0] ^ Local[1]));
  Check(KeGetCurrentIrql() == 0);
  StackProof = 0xa5;
}

static void CompleteWorker(void *Target, void *Context) {
  ++WorkerRuns;
  Check(Target == Device && KeGetCurrentIrql() == 0);
  if (Mode == WorkerEventWait || Mode == WorkerEventSetReset ||
      Mode == WorkerTimerWait || Mode == DpcBeforeResumedWorker)
    WaitWithStack();
  if (Mode == DpcBeforeResumedWorker)
    Check(DpcPriorityFlag == 1);
  IoFreeWorkItem(WaitWorkItem);
  Complete((IRP *)Context);
}

static void SignalWorker(void *Target, void *Context) {
  volatile U64 Local[4];
  (void)Context;
  Local[0] = 0xfedcba9876543210ULL;
  Local[1] = Local[0] + 1;
  Local[2] = Local[1] + 1;
  Local[3] = Local[2] + 1;
  ++WorkerRuns;
  Check(Target == Device && KeGetCurrentIrql() == 0);
  if (Mode == DpcBeforeResumedWorker)
    Check(KeInsertQueueDpc(&Dpc, &ArgumentOne, &ArgumentTwo) == 1);
  Check(KeSetEvent(&Event, 0, 0) == 0);
  if (Mode == WorkerEventSetReset)
    Check(KeResetEvent(&Event) == 1);
  Check(Local[3] == 0xfedcba9876543213ULL);
  IoFreeWorkItem(SignalWorkItem);
}

static void Deferred(KDPC *Object, void *Context, void *First, void *Second) {
  ++DpcRuns;
  Check(Object == &Dpc && KeGetCurrentIrql() == DispatcherDispatchLevel);
  Check(Context == ActiveRequest);
  if (Mode == DpcBeforeResumedWorker) {
    DpcPriorityFlag = 1;
    return;
  }
  if (Mode == DpcArguments || Mode == DpcRemoval) {
    Check(First == &ArgumentOne && Second == &ArgumentTwo);
    Check(*(U64 *)First == 0x0123456789abcdefULL);
    Check(*(U64 *)Second == 0xfedcba9876543210ULL);
  }
  // Timer DPC SystemArgument1/2 are reserved; their values are not inspected.
  if (Mode == InvalidDpcWait) {
    ObserveWait(&Event, 0, StatusSuccess);
    return;
  }
  if (Mode == DpcArguments) {
    LARGE_INTEGER Poll = {0};
    ObserveWait(&Event, &Poll, StatusTimeout); // Polling at DISPATCH is legal.
  }
  if (Mode == DpcToWorker) {
    IoQueueWorkItem(WaitWorkItem, CompleteWorker, DelayedQueue, Context);
    return;
  }
  if (Mode == WorkerTimerWait) {
    Check(KeSetEvent(&Event, 0, 0) == 0);
    return;
  }
  if (Mode == PeriodicCancel) {
    if (DpcRuns != 3)
      return;
    Check(KeCancelTimer(&Timer) == 1);
    Check(KeCancelTimer(&Timer) == 0);
  }
  Complete((IRP *)Context);
}

static void RearmWaitedTimer(KDPC *Object, void *Context, void *First,
                             void *Second) {
  LARGE_INTEGER Later = {-1000};
  // This must be the first imported call: no earlier API may accidentally
  // latch the expired timer signal on behalf of the scheduler.
  U8 WasArmed = KeSetTimer(&Timer, Later, 0);
  U8 WasCanceled = KeCancelTimer(&Timer);
  (void)First;
  (void)Second;
  ++DpcRuns;
  Check(WasArmed == 0 && WasCanceled == 1);
  Check(Object == &Dpc && Context == ActiveRequest);
  Check(KeGetCurrentIrql() == DispatcherDispatchLevel);
  Check(KeReadStateTimer(&Timer) == 0);
}

static NTSTATUS DispatcherDispatch(void *Target, IRP *Request) {
  LARGE_INTEGER Due = {-100};
  LARGE_INTEGER Poll = {0};
  (void)Target;
  Mode = Request->Stack->ControlCode;
  ActiveRequest = Request;
  Failures = DpcRuns = WorkerRuns = Waits = StackProof = DpcPriorityFlag = 0;
  ArgumentOne = 0x0123456789abcdefULL;
  ArgumentTwo = 0xfedcba9876543210ULL;
  KeInitializeEvent(&Event, DispatcherNotificationObject, 0);
  KeInitializeDpc(&Dpc, Deferred, Request);
  KeInitializeTimer(&Timer);

  if (Mode == TimerExpiryLatchesBeforeDpcRearm) {
    KeInitializeDpc(&Dpc, RearmWaitedTimer, Request);
    Check(KeSetTimer(&Timer, Due, &Dpc) == 0);
    ObserveWait(&Timer, 0, StatusSuccess);
    Check(DpcRuns == 1);
    Check(KeReadStateTimer(&Timer) == 0);
    return Complete(Request);
  }
  if (Mode == CompleteIrpWithQueuedDpcStorage) {
    KDPC *BufferDpc = (KDPC *)Request->SystemBuffer;
    KeInitializeDpc(BufferDpc, Deferred, Request);
    Check(KeInsertQueueDpc(BufferDpc, &ArgumentOne, &ArgumentTwo) == 1);
    Request->Status = StatusSuccess;
    Request->Information = 0;
    IofCompleteRequest(Request, 0);
    return StatusSuccess;
  }

  if (Mode == OpaqueDpc) {
    *(volatile U8 *)&Dpc = 1;
    return Complete(Request);
  }
  if (Mode == FreePendingTimer) {
    KTIMER *Allocated = ExAllocatePoolWithTag(PoolNX, sizeof(KTIMER), PoolTag);
    KeInitializeTimer(Allocated);
    KeSetTimer(Allocated, Due, &Dpc);
    ExFreePoolWithTag(Allocated, PoolTag);
    return Complete(Request);
  }
  if (Mode == TimerRearmCancel) {
    Check(KeReadStateTimer(&Timer) == 0);
    Check(KeSetTimer(&Timer, Due, &Dpc) == 0);
    Due.QuadPart = -200;
    Check(KeSetTimerEx(&Timer, Due, 0, &Dpc) == 1);
    Check(KeCancelTimer(&Timer) == 1);
    Check(KeCancelTimer(&Timer) == 0);
    Check(KeReadStateTimer(&Timer) == 0);
    return Complete(Request);
  }
  if (Mode == NotificationEvent || Mode == SynchronizationEvent) {
    const int Synchronize = Mode == SynchronizationEvent
                                ? DispatcherSynchronizationObject
                                : DispatcherNotificationObject;
    KeInitializeEvent(&Event, Synchronize, 1);
    ObserveWait(&Event, &Poll, StatusSuccess);
    ObserveWait(&Event, &Poll, Synchronize ? StatusTimeout : StatusSuccess);
    Check(KeReadStateEvent(&Event) == !Synchronize);
    Check(KeResetEvent(&Event) == !Synchronize);
    Check(KeReadStateEvent(&Event) == 0);
    Check(KeResetEvent(&Event) == 0);
    Check(KeSetEvent(&Event, 0, 0) == 0);
    Check(KeSetEvent(&Event, 0, 0) == 1);
    if (Synchronize) {
      ObserveWait(&Event, &Poll, StatusSuccess);
      Check(KeReadStateEvent(&Event) == 0);
    }
    KeClearEvent(&Event);
    Check(KeReadStateEvent(&Event) == 0);
    return Complete(Request);
  }
  if (Mode == NotificationTimer || Mode == SynchronizationTimer ||
      Mode == TimerImmediateWait) {
    const int Synchronize = Mode == SynchronizationTimer
                                ? DispatcherSynchronizationObject
                                : DispatcherNotificationObject;
    KeInitializeTimerEx(&Timer, Synchronize);
    if (Mode == TimerImmediateWait)
      Due.QuadPart = 0;
    Check(KeSetTimer(&Timer, Due, 0) == 0);
    ObserveWait(&Timer, 0, StatusSuccess);
    Check(KeReadStateTimer(&Timer) == !Synchronize);
    ObserveWait(&Timer, &Poll, Synchronize ? StatusTimeout : StatusSuccess);
    Check(KeReadStateTimer(&Timer) == !Synchronize);
    Check(KeCancelTimer(&Timer) == 0);
    return Complete(Request);
  }
  if (Mode == FiniteTimeoutAndDelay) {
    volatile U64 Local = 0x123456789abcdef0ULL;
    LARGE_INTEGER TimerDue = {-200};
    Check(KeSetTimer(&Timer, TimerDue, 0) == 0);
    ObserveWait(&Event, &Due, StatusTimeout);
    Check(KeReadStateEvent(&Event) == 0);
    Check(KeReadStateTimer(&Timer) == 0);
    Check(KeDelayExecutionThread(KernelMode, 0, &Due) == StatusSuccess);
    ++Waits;
    Check(KeReadStateTimer(&Timer) == 1);
    TimerDue.QuadPart = -300;
    Check(KeSetTimer(&Timer, TimerDue, 0) == 0);
    Check(KeReadStateTimer(&Timer) == 0);
    Due.QuadPart = 500; // Absolute time in the deterministic guest clock.
    Check(KeDelayExecutionThread(KernelMode, 0, &Due) == StatusSuccess);
    ++Waits;
    Check(KeReadStateTimer(&Timer) == 1);
    Check(Local == 0x123456789abcdef0ULL);
    StackProof = 0xa5;
    return Complete(Request);
  }
  if (Mode == DispatchEventWait) {
    SignalWorkItem = IoAllocateWorkItem(Device);
    IoQueueWorkItem(SignalWorkItem, SignalWorker, DelayedQueue, 0);
    WaitWithStack();
    return Complete(Request);
  }

  IoMarkIrpPending(Request);
  if (Mode == DpcArguments || Mode == DpcRemoval || Mode == InvalidDpcWait) {
    KeSetImportanceDpc(&Dpc, HighImportance);
    KeSetTargetProcessorDpc(&Dpc, 0);
    Check(KeInsertQueueDpc(&Dpc, &ArgumentOne, &ArgumentTwo) == 1);
    // A duplicate insertion must retain the original four callback arguments.
    Check(KeInsertQueueDpc(&Dpc, 0, 0) == 0);
    if (Mode == DpcRemoval) {
      Check(KeRemoveQueueDpc(&Dpc) == 1);
      Check(KeRemoveQueueDpc(&Dpc) == 0);
      Check(KeInsertQueueDpc(&Dpc, &ArgumentOne, &ArgumentTwo) == 1);
    }
  } else if (Mode == WorkerEventWait || Mode == WorkerEventSetReset ||
             Mode == WorkerTimerWait || Mode == DpcBeforeResumedWorker) {
    WaitWorkItem = IoAllocateWorkItem(Device);
    IoQueueWorkItem(WaitWorkItem, CompleteWorker, DelayedQueue, Request);
    if (Mode == WorkerEventWait || Mode == WorkerEventSetReset ||
        Mode == DpcBeforeResumedWorker) {
      SignalWorkItem = IoAllocateWorkItem(Device);
      IoQueueWorkItem(SignalWorkItem, SignalWorker, DelayedQueue, 0);
    } else
      Check(KeSetTimer(&Timer, Due, &Dpc) == 0);
  } else {
    if (Mode == DpcToWorker)
      WaitWorkItem = IoAllocateWorkItem(Device);
    if (Mode == TimerAbsolute)
      Due.QuadPart = 200;
    if (Mode == PeriodicCancel)
      Check(KeSetTimerEx(&Timer, Due, 1, &Dpc) == 0);
    else
      Check(KeSetTimer(&Timer, Due, &Dpc) == 0);
  }
  return StatusPending;
}

NTSTATUS DriverEntry(DRIVER_OBJECT *Driver, UNICODE_STRING *RegistryPath) {
  NTSTATUS Status = BufferedDriverEntry(Driver, RegistryPath);
  if (Status >= 0)
    Driver->MajorFunction[14] = (void *)DispatcherDispatch;
  return Status;
}
