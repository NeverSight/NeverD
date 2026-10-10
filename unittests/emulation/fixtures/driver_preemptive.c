//===- driver_preemptive.c - Original preemptible WDM guest --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#define DriverEntry BufferedDriverEntry
#include "driver_io.c"
#undef DriverEntry

#define NEVERD_PREEMPT_CASE(Name, Value) enum { Name = Value };
#define NEVERD_PREEMPT_VALUE(Name, Value) enum { Name = Value };
#include "DriverPreemptiveCases.def"
#undef NEVERD_PREEMPT_VALUE
#undef NEVERD_PREEMPT_CASE
#define NEVERD_WDM_VALUE(Name, Value) enum { Name = Value };
#include "../../../lib/emulation/os/windows/kernel/KernelValues.def"
#include "../../../lib/emulation/os/windows/kernel/WindowsKernelLayout.def"
#undef NEVERD_WDM_VALUE
#define NEVERD_KERNEL_DISPATCHER_VALUE(Name, Value) enum { Name = Value };
#include "../../../lib/emulation/os/windows/kernel/KernelDispatcherValues.def"
#undef NEVERD_KERNEL_DISPATCHER_VALUE

typedef struct {
  U32 Length;
  void *RootDirectory;
  UNICODE_STRING *ObjectName;
  U32 Attributes;
  void *SecurityDescriptor;
  void *SecurityQualityOfService;
} OBJECT_ATTRIBUTES;
typedef struct {
  U64 Data[DpcSize / sizeof(U64)];
} DPC;
typedef struct {
  U64 Data[TimerSize / sizeof(U64)];
} TIMER;
typedef struct {
  U64 Data[EventSize / sizeof(U64)];
} EVENT;
__declspec(dllimport) NTSTATUS PsCreateSystemThread(void **, U32, void *,
                                                    void *, void *,
                                                    void (*)(void *), void *);
__declspec(dllimport) void PsTerminateSystemThread(NTSTATUS);
__declspec(dllimport) NTSTATUS ZwClose(void *);
__declspec(dllimport) U8 KeGetCurrentIrql(void);
__declspec(dllimport) void *IoGetCurrentProcess(void);
U64 __readgsqword(unsigned long);
#pragma intrinsic(__readgsqword)
U64 __rdtsc(void);
#pragma intrinsic(__rdtsc)
__declspec(dllimport) void KeStackAttachProcess(void *, void *);
__declspec(dllimport) void KeUnstackDetachProcess(void *);
__declspec(dllimport) NTSTATUS KeDelayExecutionThread(U32, U8, long long *);
__declspec(dllimport) U8 KfRaiseIrql(U8);
__declspec(dllimport) void KeLowerIrql(U8);
__declspec(dllimport) void KeEnterGuardedRegion(void);
__declspec(dllimport) void KeLeaveGuardedRegion(void);
__declspec(dllimport) void KeLeaveCriticalRegion(void);
__declspec(dllimport) U8 KeAreAllApcsDisabled(void);
__declspec(dllimport) U8 KeAreApcsDisabled(void);
__declspec(dllimport) void
KeInitializeDpc(DPC *, void (*)(DPC *, void *, void *, void *), void *);
__declspec(dllimport) void KeInitializeTimer(TIMER *);
__declspec(dllimport) U8 KeSetTimerEx(TIMER *, long long, int, DPC *);
__declspec(dllimport) U8 KeCancelTimer(TIMER *);
__declspec(dllimport) void KeInitializeEvent(EVENT *, U32, U8);
__declspec(dllimport) long KeSetEvent(EVENT *, long, U8);
__declspec(dllimport) NTSTATUS KeWaitForSingleObject(void *, U32, U32, U8,
                                                     long long *);
__declspec(dllimport) void IoMarkIrpPending(IRP *);
__declspec(dllimport) void IoAcquireCancelSpinLock(U8 *);
__declspec(dllimport) void IoReleaseCancelSpinLock(U8);
__declspec(dllimport) void *IoAllocateWorkItem(void *);
__declspec(dllimport) void IoQueueWorkItem(void *, void (*)(void *, void *),
                                           U32, void *);
__declspec(dllimport) void IoFreeWorkItem(void *);

static volatile U32 Started[2], Finished[2], Ticks, Invalid;
static U32 Mode;
static TIMER Timer;
static EVENT Event;
static DPC Dpc;
int _fltused;
static void *Work, *Process;
#define NEVERD_PREEMPT_FLOAT(Name, Value) static volatile double Name = Value;
#include "DriverPreemptiveCases.def"
#undef NEVERD_PREEMPT_FLOAT

static void RecordFailure(U32 Failed) {
  if (Failed)
    Invalid = 1;
}
static void Busy(U32 Iterations) {
  for (volatile U32 I = 0; I != Iterations; ++I) {
  }
}
static void Thread(void *Context) {
  U64 Index = (U64)Context;
  void *Identity = (void *)__readgsqword(GSCurrentThreadOffset);
  typedef double Pair __attribute__((vector_size(16)));
  Pair Live = {Index ? FirstSeed : SecondSeed, Index ? SecondSeed : FirstSeed};
  U64 ApcStorage[KAPCStateSize / sizeof(U64)];
  // The initial system-thread critical region is part of its OS context.
  RecordFailure(!KeAreApcsDisabled());
  KeLeaveCriticalRegion();
  if (Mode == AttachedContexts)
    KeStackAttachProcess(Process, ApcStorage);
  U8 OldIRQL = 0;
  if (Mode == APCContext)
    OldIRQL = KfRaiseIrql(APCLevel);
  if (Mode == GuardedContext && !Index)
    KeEnterGuardedRegion();
  Started[Index] = 1;
  if (!Index && Mode == TimerWakeThenReset) {
    KeSetTimerEx(&Timer, -TimerDelay, 0, 0);
    RecordFailure(KeWaitForSingleObject(&Timer, ExecutiveWaitReason, KernelMode,
                                        0, 0) != StatusSuccess);
  }
  if (!Index && Mode == TimeoutBeforeSignal) {
    long long Timeout = -TimerDelay;
    RecordFailure(KeWaitForSingleObject(&Event, ExecutiveWaitReason, KernelMode,
                                        0, &Timeout) != StatusTimeout);
  }
  if (!Index && Mode == TimeoutBeforeTimer) {
    // Both absolute deadlines lie inside the same coarse instruction. The
    // timeout must be committed before the later timer signal is published.
    long long Timeout = CoarseWaitDeadline;
    KeSetTimerEx(&Timer, CoarseWaitDeadline + 1, 0, 0);
    RecordFailure(KeWaitForSingleObject(&Timer, ExecutiveWaitReason, KernelMode,
                                        0, &Timeout) != StatusTimeout);
  }
  if (Index && (Mode == TimerWakeThenReset || Mode == TimeoutBeforeSignal)) {
    while (!Started[0]) {
    }
    Busy(ContextSpinIterations);
    if (Mode == TimerWakeThenReset)
      KeSetTimerEx(&Timer, -ResetTimerDelay, 0, 0);
    else
      KeSetEvent(&Event, 0, 0);
  }
  if (Mode == TimedWait && !Index) {
    long long Delay = -TimerDelay;
    RecordFailure(KeDelayExecutionThread(KernelMode, 0, &Delay) !=
                  StatusSuccess);
  }
  if ((Mode == TimedWait || Mode == TimeoutBeforeTimer) && Index)
    while (!Finished[0]) {
    }
  if (Mode != BusyThread) {
    while (!Started[1 - Index]) {
    }
    Busy(ContextSpinIterations);
  }
  if (Mode == APCContext) {
    RecordFailure(KeGetCurrentIrql() != APCLevel);
    KeLowerIrql(OldIRQL);
  }
  if (Mode == GuardedContext) {
    RecordFailure(!!KeAreAllApcsDisabled() != !Index);
    if (!Index)
      KeLeaveGuardedRegion();
  }
  RecordFailure((void *)__readgsqword(GSCurrentThreadOffset) != Identity);
  if (Mode == AttachedContexts) {
    RecordFailure(IoGetCurrentProcess() != Process);
    KeUnstackDetachProcess(ApcStorage);
  }
  if (Mode == FloatingContext)
    RecordFailure(Live[0] != (Index ? FirstSeed : SecondSeed) ||
                  Live[1] != (Index ? SecondSeed : FirstSeed));
  Finished[Index] = 1;
  PsTerminateSystemThread(StatusSuccess);
}
static void TimerDpc(DPC *Object, void *Context, void *First, void *Second) {
  (void)Object;
  (void)Context;
  (void)First;
  (void)Second;
  RecordFailure(KeGetCurrentIrql() != DispatchLevel);
  ++Ticks;
  if (Mode == PeriodicTimer && Ticks == TimerCount)
    KeCancelTimer(&Timer);
}
static void Complete(IRP *Request) {
  Request->SystemBuffer[0] = Invalid ? FailureMarker : SuccessMarker;
  Request->SystemBuffer[1] = (U8)Finished[0];
  Request->SystemBuffer[2] = (U8)Finished[1];
  Request->SystemBuffer[3] = (U8)Ticks;
  Request->Status = StatusSuccess;
  Request->Information = OutputLength;
  IofCompleteRequest(Request, 0);
}
static void Cancel(void *Target, IRP *Request) {
  (void)Target;
  RecordFailure(KeGetCurrentIrql() != DispatchLevel);
  RecordFailure(!*((U8 *)Request + IRPCancelOffset));
  IoReleaseCancelSpinLock(*((U8 *)Request + IRPCancelIROffset));
  if (Mode == CancelPeerContext) {
    Started[0] = 1;
    while (!Finished[0]) {
    }
  }
  Complete(Request);
}
static void LockedWorker(void *Target, void *Context) {
  (void)Target;
  IRP *Request = (IRP *)Context;
  U8 OldIRQL;
  IoAcquireCancelSpinLock(&OldIRQL);
  Busy(SpinIterations);
  RecordFailure(*((U8 *)Request + IRPCancelOffset) != 0);
  IoReleaseCancelSpinLock(OldIRQL);
  if (Mode == CancelPeerContext) {
    while (!Started[0]) {
    }
    U8 Original = KfRaiseIrql(APCLevel);
    IoAcquireCancelSpinLock(&OldIRQL);
    RecordFailure(OldIRQL != APCLevel);
    IoReleaseCancelSpinLock(OldIRQL);
    KeLowerIrql(Original);
    Finished[0] = 1;
  }
  IoFreeWorkItem(Work);
}
static NTSTATUS PreemptDispatch(void *Target, IRP *Request) {
  (void)Target;
  Mode = Request->Stack->ControlCode;
  Started[0] = Started[1] = Finished[0] = Finished[1] = Ticks = Invalid = 0;
  if (Mode == CancelBeforeDispatchReturn || Mode == CancelUnderSpinLock ||
      Mode == CancelPeerContext) {
    *(void **)((U8 *)Request + IRPCancelRoutineOffset) = (void *)Cancel;
    IoMarkIrpPending(Request);
    if (Mode == CancelBeforeDispatchReturn) {
      Busy(SpinIterations);
      RecordFailure(*((U8 *)Request + IRPCancelOffset) != 0);
    } else {
      Work = IoAllocateWorkItem(Device);
      IoQueueWorkItem(Work, LockedWorker, DelayedWorkQueue, Request);
    }
    return StatusPending;
  }
  if (Mode == TimerProgress || Mode == MaskedTimer || Mode == PeriodicTimer) {
    U8 OldIRQL =
        Mode == MaskedTimer ? KfRaiseIrql(DispatchLevel) : PassiveLevel;
    KeInitializeDpc(&Dpc, TimerDpc, 0);
    KeInitializeTimer(&Timer);
    KeSetTimerEx(&Timer, -TimerDelay, Mode == PeriodicTimer ? TimerPeriod : 0,
                 &Dpc);
    if (Mode == MaskedTimer) {
      Busy(SpinIterations);
      RecordFailure(Ticks != 0);
      KeLowerIrql(OldIRQL);
    }
    while (Ticks != (Mode == PeriodicTimer ? TimerCount : 1)) {
    }
  } else {
    if (Mode == TimerWakeThenReset || Mode == TimeoutBeforeTimer)
      KeInitializeTimer(&Timer);
    if (Mode == TimeoutBeforeSignal)
      KeInitializeEvent(&Event, NotificationObject, 0);
    Process = IoGetCurrentProcess();
    void *Handles[2] = {0, 0};
    OBJECT_ATTRIBUTES Attributes = {sizeof(Attributes), 0, 0,
                                    ObjectKernelHandle, 0, 0};
    U32 Count = Mode == BusyThread ? 1 : 2;
    for (U32 I = 0; I != Count; ++I)
      RecordFailure(PsCreateSystemThread(&Handles[I], 0, &Attributes, 0, 0,
                                         Thread,
                                         (void *)(U64)I) != StatusSuccess);
    while (!Finished[0] || (Count == 2 && !Finished[1])) {
      // An environment read must retain its instruction/time-slice charge.
      // Otherwise this polling thread can starve the thread that completes it.
      if (Mode == BusyThread)
        (void)__rdtsc();
    }
    for (U32 I = 0; I != Count; ++I)
      RecordFailure(ZwClose(Handles[I]) != StatusSuccess);
  }
  if (Mode == TimerWakeThenReset || Mode == TimeoutBeforeTimer)
    KeCancelTimer(&Timer);
  Complete(Request);
  return StatusSuccess;
}
NTSTATUS DriverEntry(DRIVER_OBJECT *Driver, UNICODE_STRING *Path) {
  NTSTATUS Status = BufferedDriverEntry(Driver, Path);
  if (Status == StatusSuccess)
    Driver->MajorFunction[DispatchIndex] = (void *)PreemptDispatch;
  return Status;
}
