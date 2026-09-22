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

#include "llvm/Support/Error.h"

#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <vector>

namespace neverd::emulation {

namespace scheduler {
#define NEVERD_KERNEL_SCHEDULER_VALUE(Name, Value)                             \
  constexpr uint64_t Name = Value;
#include "KernelSchedulerValues.def"
#undef NEVERD_KERNEL_SCHEDULER_VALUE
} // namespace scheduler

/// A concrete single-processor schedule, not an assertion of Windows timing or
/// interleaving equivalence. DPCs run before workers at dispatch boundaries;
/// ordinary DPCs and workers are FIFO, and high-importance DPCs insert at the
/// head. No callback executes here: next() returns guest execution metadata.
/// Virtual time advances only when no callback is ready or running. Guest API
/// wrappers own object initialization, thread identities, and memory semantics.
class KernelScheduler {
public:
  enum class CallbackKind { WorkItem, DPC };
  enum class DpcImportance { Low = 0, Medium = 1, High = 2, MediumHigh = 3 };

  struct Limits {
    uint64_t MaxPendingCallbacks = scheduler::DefaultMaxPendingCallbacks;
    uint64_t MaxDispatches = scheduler::DefaultMaxDispatches;
    uint64_t MaxTimers = scheduler::DefaultMaxTimers;
    uint64_t MaxTimerExpirations = scheduler::DefaultMaxTimerExpirations;
    uint64_t InitialTime100ns = 0;
    uint64_t MaxTime100ns = std::numeric_limits<uint64_t>::max();
  };

  struct Callback {
    uint64_t Object = 0;
    uint64_t Owner = 0;
    uint64_t Thread = 0;
    uint64_t PC = 0;
    std::vector<uint64_t> Arguments;
  };

  struct DpcCallback : Callback {
    DpcImportance Importance = DpcImportance::Medium;
  };

  struct Invocation : Callback {
    uint64_t ID = 0;
    CallbackKind Kind = CallbackKind::WorkItem;
    uint8_t IRQL = scheduler::PassiveLevel;
    uint64_t DueTime100ns = 0;
    uint64_t SourceTimer = 0;
  };

  KernelScheduler() = default;
  explicit KernelScheduler(Limits Bounds)
      : Bounds(Bounds), Now(Bounds.InitialTime100ns) {}

  /// Already-queued work items are a driver error. Once next() dequeues an
  /// item, that object may be freed or queued again by its callback.
  llvm::Expected<uint64_t> enqueueWorkItem(Callback Work);
  bool cancelWorkItem(uint64_t Object);
  bool isWorkItemQueued(uint64_t Object) const;

  /// An already-queued DPC is unchanged and returns false, as KeInsertQueueDpc.
  /// Removing a DPC does not cancel any timer that may queue it again.
  llvm::Expected<bool> queueDPC(DpcCallback DPC);
  bool removeDPC(uint64_t Object);
  bool isDPCQueued(uint64_t Object) const;

  /// Returns one callback, retaining its ownership until finish(). Calling
  /// next() with an unfinished callback is an error. With AdvanceTime false,
  /// already-due timers are still processed but future deadlines are untouched.
  /// Timer-only expirations are bounded even when they produce no callback.
  llvm::Expected<std::optional<Invocation>> next(bool AdvanceTime = true);
  llvm::Error finish(uint64_t ID);

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
  /// Rejects forgetting an armed timer or one with a queued callback. A
  /// nonperiodic timer can be freed inside its running callback.
  llvm::Error forgetTimer(uint64_t Timer);

  bool hasOutstanding(uint64_t Owner) const;
  bool hasPending() const;
  uint64_t now100ns() const { return Now; }
  uint64_t dispatchCount() const { return Dispatches; }
  uint64_t timerExpirationCount() const { return TimerExpirations; }
  size_t queuedCallbackCount() const { return Workers.size() + DPCs.size(); }
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
  uint64_t NextTimerSequence = 1;
  uint64_t Dispatches = 0;
  uint64_t TimerExpirations = 0;
  std::deque<Invocation> Workers;
  std::deque<Invocation> DPCs;
  std::optional<Invocation> Active;
  std::map<uint64_t, TimerState> Timers;

  llvm::Error validateTime() const;
  llvm::Error validateCallback(const Callback &Work) const;
  llvm::Error validateDPC(const DpcCallback &DPC) const;
  llvm::Error checkCapacity(uint64_t Additional) const;
  Invocation makeInvocation(Callback Work, CallbackKind Kind, uint64_t DueTime);
  llvm::Error expireTimers(uint64_t Time);
};

} // namespace neverd::emulation

#endif
