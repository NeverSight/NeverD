//===- KernelScheduler.h - Deterministic Windows callback scheduler -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest callback ordering and virtual time, independent of guest structures.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWS_KERNELSCHEDULER_H
#define NEVERD_EMULATION_WINDOWS_KERNELSCHEDULER_H

#include "KernelProcessorTopology.h"
#include "KernelThreadPriorities.h"

#include "neverd/emulation/DriverInterrupts.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <vector>

namespace neverd::emulation {
static_assert(
    processor::ActiveCount == 1 && processor::GroupCount == 1,
    "the deterministic scheduler requires the single-processor profile");

namespace scheduler {
#define NEVERD_KERNEL_SCHEDULER_VALUE(Name, Value)                             \
  constexpr uint64_t Name = Value;
#include "KernelSchedulerValues.def"
#undef NEVERD_KERNEL_SCHEDULER_VALUE
} // namespace scheduler

/// A concrete single-processor schedule, not an assertion of Windows timing or
/// interleaving equivalence. Interrupts precede DPCs, DMA callbacks,
/// request cancellation, provider completions and workers at dispatch
/// boundaries. Interrupts use descending assigned priority and FIFO ties;
/// high-importance DPCs insert at the head. No callback executes here: next()
/// returns guest metadata.
/// The session may advance the execution clock and preempt between
/// instructions. Guest API wrappers own object initialization, thread
/// identities and memory.
class KernelScheduler {
public:
  enum class CallbackKind {
    WorkItem,
    SystemThread,
    DPC,
    FrameworkCancel,
    WDMCancel,
    WDMDispatch,
    WDMProviderDispatch,
    WDMCompletion,
    FrameworkCompletion,
    FrameworkDeferred,
    PoFx,
    UsbIdle,
    FrameworkUsbIdle,
    FrameworkInterruptDPC,
    FrameworkInterruptWorkItem,
    Interrupt,
    DMAListControl,
    DMAAdapterControl
  };
  static bool isDMACallbackKind(CallbackKind Kind);
  static bool isFrameworkInterruptCallbackKind(CallbackKind Kind);
  enum class DpcImportance { Low = 0, Medium = 1, High = 2, MediumHigh = 3 };

  struct Limits {
    uint64_t MaxPendingCallbacks = scheduler::DefaultMaxPendingCallbacks;
    uint64_t MaxDispatches = scheduler::DefaultMaxDispatches;
    uint64_t MaxTimers = scheduler::DefaultMaxTimers;
    uint64_t MaxTimerExpirations = scheduler::DefaultMaxTimerExpirations;
    uint64_t InitialTime100ns = 0;
    uint64_t MaxTime100ns = std::numeric_limits<uint64_t>::max();
    std::optional<uint32_t> SystemClockIncrement100ns;
  };

  struct Callback {
    uint64_t Object = 0;
    uint64_t Owner = 0;
    uint64_t Thread = 0;
    uint64_t PC = 0;
    std::vector<uint64_t> Arguments;
    uint64_t SynchronizationObject = 0;
  };

  struct DpcCallback : Callback {
    DpcImportance Importance = DpcImportance::Medium;
  };

  struct InterruptCallback : Callback {
    uint8_t IRQL = 0;     // SynchronizeIrql at callback entry.
    uint8_t Priority = 0; // Assigned interrupt level for ready ordering.
  };

  struct Invocation : Callback {
    uint64_t ID = 0;
    CallbackKind Kind = CallbackKind::WorkItem;
    uint8_t IRQL = scheduler::PassiveLevel;
    uint8_t InterruptPriority = 0;
    uint64_t DueTime100ns = 0;
    uint64_t SourceTimer = 0;
    bool SourceTimerPeriodic = false;
    /// A native USB task keeps its IRP identity while executing the real
    /// framework callback that replaced it in this slot.
    bool FrameworkUsbIdleContinuation = false;
    uint64_t ReadyOrder = 0;
  };

  KernelScheduler() = default;
  explicit KernelScheduler(Limits Bounds, bool OrderedPassive = false)
      : Bounds(Bounds), Now(Bounds.InitialTime100ns),
        OrderedPassive(OrderedPassive) {}

  /// One order shared by fresh passive callbacks and execution continuations.
  /// Allocate only when an owner becomes ready or exhausts its time slice.
  llvm::Expected<uint64_t> issueReadyOrder();
  std::optional<uint64_t> nextPassiveReadyOrder() const;
  /// Transient selection metadata; priority is always read from canonical
  /// state, never retained in a queued invocation or CPU context.
  struct ReadyThread {
    int32_t Priority;
    uint64_t ReadyOrder;
    bool precedes(const ReadyThread &Other) const {
      return Priority > Other.Priority ||
             (Priority == Other.Priority && ReadyOrder < Other.ReadyOrder);
    }
  };
  ReadyThread readyThread(uint64_t Key, uint64_t Order) const {
    return {threadPriority(Key), Order};
  }
  std::optional<ReadyThread> nextPassiveThread() const;
  int32_t threadPriority(uint64_t Key) const;
  llvm::Expected<int32_t> setThreadPriority(uint64_t Key, int32_t Priority);
  void forgetThreadPriority(uint64_t Key) { ThreadPriorities.erase(Key); }

  /// Already-queued work items are a driver error. Once next() dequeues an
  /// item, that object may be freed or queued again by its callback.
  llvm::Expected<uint64_t> enqueueWorkItem(Callback Work);
  llvm::Error canEnqueueSystemThread(const Callback &Thread) const;
  llvm::Expected<uint64_t> enqueueSystemThread(Callback Thread);
  // A created thread owns scheduler capacity but is not runnable until its
  // creator has completed the synchronous thread-notification traversal.
  llvm::Expected<uint64_t> reserveSystemThread(Callback Thread);
  llvm::Error readySystemThread(uint64_t ID);
  llvm::Expected<uint64_t> enqueuePoFx(Callback Call);
  /// USB permission callbacks have their own identity namespace and carry
  /// exactly one opaque context argument. Admission of a composite group is
  /// pure; the bridge publishes the complete preflighted batch serially.
  llvm::Error canEnqueueUsbIdleCallbacks(llvm::ArrayRef<Callback> Calls) const;
  llvm::Expected<uint64_t> enqueueUsbIdleCallback(Callback Call);
  struct UsbIdleCallback {
    Callback Call;
    CallbackKind Kind = CallbackKind::UsbIdle;
  };
  /// Atomically preflight mixed guest callbacks and native model tasks. A
  /// native task has PC zero and arguments {PDO, IRP, owner callback token}.
  llvm::Error
  canEnqueueUsbIdleBatch(llvm::ArrayRef<UsbIdleCallback> Calls) const;
  llvm::Expected<uint64_t> enqueueFrameworkUsbIdleCallback(uint64_t IRP,
                                                           uint64_t PDO,
                                                           uint64_t Thread,
                                                           uint64_t Token);
  /// Continue a selected native task with a real framework guest callback,
  /// preserving its occupied slot, ID, time and dispatch count.
  llvm::Expected<Invocation> beginFrameworkUsbIdleCallback(uint64_t ID,
                                                           Callback Call);
  llvm::Error canWithdrawUsbIdleCallback(uint64_t ID) const;
  llvm::Error withdrawUsbIdleCallback(uint64_t ID);
  bool cancelWorkItem(uint64_t Object);
  bool isWorkItemQueued(uint64_t Object) const;

  /// Reserve nothing: validate capacity and identity space for one power
  /// dispatch before the model allocates its real IRP. Both kinds use the
  /// PASSIVE worker FIFO and are independent of driver work-item removal.
  llvm::Error canEnqueueWDMDispatch() const;
  llvm::Expected<uint64_t> enqueueWDMDispatch(Callback Dispatch);
  /// A provider-only dispatch is a model task, with no guest executable PC.
  /// The model consumes it at selection before returning a guest invocation.
  llvm::Expected<uint64_t>
  enqueueWDMProviderDispatch(uint64_t IRP, uint64_t PDO, uint64_t Thread);
  /// Replace only the active provider task with its real completion callback.
  /// Preserve its ID, time, owner, thread and occupied capacity. No new
  /// callback identity or dispatch budget is consumed by this transition.
  llvm::Expected<Invocation> beginWDMProviderCompletion(uint64_t ID,
                                                        Callback Completion);

  /// Framework cancellation has its own object namespace and PASSIVE_LEVEL
  /// FIFO, independent of work-item removal. The framework owns cancellation
  /// eligibility and the request lifetime; the scheduler only orders delivery.
  /// An already-queued cancellation object is an explicit error.
  llvm::Expected<uint64_t> enqueueFrameworkCancel(Callback Cancellation);
  /// A deferred framework continuation uses the DPC or worker FIFO selected by
  /// its effective execution level. Its token has an independent namespace.
  llvm::Expected<uint64_t> enqueueFrameworkDeferred(Callback Call,
                                                    uint8_t IRQL);
  llvm::Error
  canEnqueueWDMCancellations(llvm::ArrayRef<Callback> Cancellations) const;
  llvm::Expected<uint64_t> enqueueWDMCancellation(Callback Cancellation);
  bool hasQueuedCancellation() const { return !Cancellations.empty(); }
  bool hasQueuedFrameworkCancel() const;

  /// Preflight a whole provider-completion batch without reserving identities
  /// or mutating queues. The model commits without intervening guest execution.
  llvm::Error canEnqueueCompletions(llvm::ArrayRef<Callback> Completions) const;
  llvm::Expected<uint64_t> enqueueWDMCompletion(Callback Completion);
  llvm::Expected<uint64_t> enqueueFrameworkCompletion(Callback Completion);
  bool hasQueuedCompletion() const { return !Completions.empty(); }

  /// Object identifies an explicit event, not a connection: separate pulses
  /// on the same interrupt are never coalesced by DPC/work-item identity rules.
  llvm::Error
  canEnqueueInterrupts(llvm::ArrayRef<InterruptCallback> Interrupts) const;
  llvm::Expected<uint64_t> enqueueInterrupt(InterruptCallback Interrupt);
  bool hasQueuedInterrupt() const { return !Interrupts.empty(); }

  /// Successful DMA admission reserves callback capacity and identity even
  /// while waiting for map registers. Each reservation is independent; Object
  /// never coalesces requests. Waiting reservations do not create deadlines or
  /// become runnable until explicitly promoted by the owning DMA model.
  llvm::Error canReserveDMACallback(const Callback &Call,
                                    CallbackKind Kind) const;
  llvm::Expected<uint64_t> reserveDMACallback(Callback Call, CallbackKind Kind);
  /// Validate the complete ordered batch before moving any reservation to the
  /// DISPATCH_LEVEL FIFO. Promotion consumes no new identity or capacity.
  llvm::Error canReadyDMACallbacks(llvm::ArrayRef<uint64_t> IDs) const;
  llvm::Error readyDMACallbacks(llvm::ArrayRef<uint64_t> IDs);
  bool hasQueuedDMACallback() const { return !ReadyDMA.empty(); }

  /// Immediate guest delivery retains the scheduled parent (if any) as Active.
  /// Nested inline callbacks count toward the same capacity/dispatch limits
  /// and must return in order; their guest return registers are not
  /// interpreted. Validate dispatch budget before reserving an immediate
  /// callback identity.
  llvm::Error canDispatchInlineDMACallback() const;
  llvm::Error canBeginInlineDMACallback(uint64_t ID, CallbackKind Kind) const;
  llvm::Error beginInlineDMACallback(uint64_t ID, CallbackKind Kind);
  llvm::Error canFinishInlineDMACallback(uint64_t ID, CallbackKind Kind) const;
  llvm::Error finishInlineDMACallback(uint64_t ID, CallbackKind Kind);

  /// Typed compatibility entry points use the same reservation, FIFO and
  /// inline stack, and cannot admit or finish an AdapterControl callback.
  llvm::Error canReserveDMAListControl(const Callback &Call) const;
  llvm::Expected<uint64_t> reserveDMAListControl(Callback Call);
  llvm::Error canReadyDMAListControls(llvm::ArrayRef<uint64_t> IDs) const;
  llvm::Error readyDMAListControls(llvm::ArrayRef<uint64_t> IDs);
  bool hasQueuedDMAListControl() const;
  llvm::Error canDispatchInlineDMAListControl() const;
  llvm::Error canBeginInlineDMAListControl(uint64_t ID) const;
  llvm::Error beginInlineDMAListControl(uint64_t ID);
  llvm::Error canFinishInlineDMAListControl(uint64_t ID) const;
  llvm::Error finishInlineDMAListControl(uint64_t ID);

  /// An already-queued DPC is unchanged and returns false, as KeInsertQueueDpc.
  /// Removing a DPC does not cancel any timer that may queue it again.
  llvm::Expected<bool> queueDPC(DpcCallback DPC);
  bool removeDPC(uint64_t Object);
  bool isDPCQueued(uint64_t Object) const;
  bool hasQueuedDPC() const { return !DPCs.empty(); }

  /// Interrupt-owned deferred callbacks share the DPC/worker FIFOs while
  /// retaining their own identity namespace and coalescing contract. A queued
  /// invocation coalesces; a running invocation may queue its next delivery.
  llvm::Expected<std::optional<uint64_t>>
  queueFrameworkInterrupt(Callback Call, bool WorkItem);
  bool hasFrameworkInterrupt(uint64_t Object) const;

  /// Returns one callback, retaining its ownership until finish(). Calling
  /// next() with an active callback is an error. With AdvanceTime false,
  /// already-due timers are still processed but future deadlines are untouched.
  /// Timer-only expirations are bounded even when they produce no callback.
  /// If MaxAdvanceTime is supplied, yield after the first timer expiry
  /// even if no callback became ready, so callers can recheck wait predicates.
  /// Otherwise idle time can advance to MaxAdvanceTime, but never beyond it.
  /// A deadline at or before now does not move time backwards.
  llvm::Expected<std::optional<Invocation>>
  next(bool AdvanceTime = true,
       std::optional<uint64_t> MaxAdvanceTime = std::nullopt);
  llvm::Error canFinish(uint64_t ID) const;
  llvm::Error finish(uint64_t ID);
  /// A blocked callback retains its identity, device ownership and capacity.
  /// While it is suspended, other callbacks may run. Only the caller knows
  /// which wait predicate makes a suspended callback ready to resume.
  llvm::Error suspend(uint64_t ID);
  llvm::Error resume(uint64_t ID);
  const Invocation *suspended(uint64_t ID) const;

  /// Nonnegative deadlines are absolute; negative intervals are relative to
  /// the current virtual clock. Includes overflow and configured-limit checks.
  llvm::Expected<uint64_t> computeDeadline(int64_t Time100ns) const;
  /// Earliest armed timer boundary, clamped to now if it is already due.
  std::optional<uint64_t> nextEventTime100ns() const;
  /// Expire timers at the current clock, including during an active callback.
  /// Does not advance time or dequeue callbacks; budget failures are atomic.
  llvm::Error processDueTimers();

  /// Split idle time advancement from callback selection, allowing all external
  /// producers at this boundary to be admitted before choosing the next ISR or
  /// DPC. AdditionalCallbacks reserves nothing: preflight and commit must have
  /// no intervening mutation. The preflight counts due timer DPCs together with
  /// that exact external count and checks time, expiration and identity bounds.
  /// Future time cannot skip an earlier timer or advance with ready/active
  /// work.
  llvm::Error canAdvanceTo100ns(uint64_t Time,
                                uint64_t AdditionalCallbacks = 0) const;
  llvm::Error advanceTo100ns(uint64_t Time);
  /// Running time may advance with active/ready work. Each caller must visit
  /// every earlier timer boundary and admit external producers at that same
  /// boundary before selecting callbacks. Idle callers retain stricter rules.
  llvm::Error
  canAdvanceExecutionTo100ns(uint64_t Time,
                             uint64_t AdditionalCallbacks = 0) const;
  llvm::Error advanceExecutionTo100ns(uint64_t Time);

  /// Nonnegative DueTime100ns is absolute; negative is relative. Period is
  /// milliseconds and must fit Windows LONG. Resetting an armed timer replaces
  /// its future expiry and resets its signal; already queued DPCs survive.
  /// Each timer expiry queues at most one copy of its DPC. Callback arguments
  /// are explicit guest data and never interpreted as host pointers.
  llvm::Expected<bool> setTimer(uint64_t Timer, uint64_t Owner,
                                int64_t DueTime100ns,
                                uint32_t PeriodMilliseconds,
                                std::optional<DpcCallback> DPC = std::nullopt);
  bool cancelTimer(uint64_t Timer);
  bool isTimerArmed(uint64_t Timer) const;
  llvm::Expected<bool> timerSignaled(uint64_t Timer) const;
  /// A successful synchronization-timer wait consumes its signal. Timer kind
  /// and wait eligibility are validated by the caller, not guessed here.
  llvm::Expected<bool> consumeTimerSignal(uint64_t Timer);
  /// Preflight every selected timer before consuming any synchronization
  /// signal.
  llvm::Error consumeTimerSignals(llvm::ArrayRef<uint64_t> TimerObjects);
  /// Rejects forgetting an armed timer or one with a queued callback. A
  /// nonperiodic timer can be freed inside its running callback.
  llvm::Error canForgetTimer(uint64_t Timer) const;
  llvm::Error forgetTimer(uint64_t Timer);

  bool hasOutstanding(uint64_t Owner) const;
  bool hasPending() const;
  uint64_t now100ns() const { return Now; }
  llvm::Expected<uint64_t> queryTimeIncrement() const;
  llvm::Expected<uint64_t> queryTickCount() const;
  uint64_t dispatchCount() const { return Dispatches; }
  uint64_t timerExpirationCount() const { return TimerExpirations; }
  size_t queuedCallbackCount() const {
    return Workers.size() + SystemThreads.size() + DPCs.size() +
           Cancellations.size() + Completions.size() + Interrupts.size() +
           ReadyDMA.size();
  }
  size_t suspendedCallbackCount() const { return Suspended.size(); }
  /// Active/queued callbacks and suspended non-thread callbacks block a
  /// foreground boundary. Only suspended system threads may remain parked.
  bool hasForegroundBlockingCallbacks() const;
  bool isSuspendedSystemThread(uint64_t ID) const;
  const std::optional<Invocation> &active() const { return Active; }

private:
  struct TimerState {
    uint64_t Owner = 0;
    uint64_t DueTime100ns = 0;
    uint64_t Period100ns = 0;
    uint64_t Sequence = 0;
    bool Armed = false;
    bool Signaled = false;
    std::optional<DpcCallback> DPC;
  };

  Limits Bounds;
  uint64_t Now = 0;
  uint64_t NextID = 1;
  uint64_t NextReadyOrder = 1;
  uint64_t NextTimerSequence = 1;
  uint64_t Dispatches = 0;
  uint64_t TimerExpirations = 0;
  bool OrderedPassive = false;
  std::deque<Invocation> Workers;
  std::deque<Invocation> SystemThreads;
  std::deque<Invocation> DPCs;
  std::deque<Invocation> Cancellations;
  std::deque<Invocation> Completions;
  std::deque<Invocation> Interrupts;
  std::map<uint64_t, Invocation> WaitingDMA;
  std::map<uint64_t, Invocation> WaitingSystemThreads;
  std::deque<Invocation> ReadyDMA;
  std::vector<Invocation> InlineDMA;
  std::optional<Invocation> Active;
  std::map<uint64_t, Invocation> Suspended;
  std::map<uint64_t, TimerState> Timers;
  std::map<uint64_t, int32_t> ThreadPriorities;

  const Invocation *nextPassiveInvocation() const;
  llvm::Error validateTime() const;
  llvm::Error validateCallback(const Callback &Work) const;
  llvm::Error validateDPC(const DpcCallback &DPC) const;
  llvm::Error validateInterrupt(const InterruptCallback &Interrupt) const;
  llvm::Error validateDMAAdmission(llvm::ArrayRef<uint64_t> IDs,
                                   std::optional<CallbackKind> Kind) const;
  llvm::Error checkCapacity(uint64_t Additional) const;
  bool hasWDMDispatch(uint64_t Object) const;
  Invocation makeInvocation(Callback Work, CallbackKind Kind, uint64_t DueTime);
  llvm::Error expireTimers(uint64_t Time);
  llvm::Error validateTimerExpirations(uint64_t Time,
                                       uint64_t AdditionalCallbacks) const;
};

} // namespace neverd::emulation

#endif
