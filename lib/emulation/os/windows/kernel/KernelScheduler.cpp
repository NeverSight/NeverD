//===- KernelScheduler.cpp - Deterministic Windows callback scheduler -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded, serial guest scheduling. The Windows API contracts are described
/// by Microsoft's System Worker Threads, KeInsertQueueDpc, KeSetImportanceDpc,
/// KeSetTimerEx and KeCancelTimer documentation. This implementation chooses
/// a reproducible schedule within those object-lifetime contracts; it does not
/// supply processor preemption, timer coalescing, or the Windows thread pool.
/// The driver session owns preemption and consumes this owner's priority
/// policy.
///
//===----------------------------------------------------------------------===//

#include "KernelScheduler.h"

#include "KernelWaits.h"

#include "neverd/emulation/DriverClock.h"
#include "neverd/emulation/DriverScheduling.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

namespace neverd::emulation {
namespace {

llvm::Error schedulerError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

bool containsObject(const std::deque<KernelScheduler::Invocation> &Queue,
                    uint64_t Object,
                    std::optional<KernelScheduler::CallbackKind> Kind = {}) {
  return llvm::any_of(Queue, [Object, Kind](const auto &Call) {
    return Call.Object == Object && (!Kind || Call.Kind == *Kind);
  });
}

bool removeObject(std::deque<KernelScheduler::Invocation> &Queue,
                  uint64_t Object,
                  std::optional<KernelScheduler::CallbackKind> Kind = {}) {
  auto I = llvm::find_if(Queue, [Object, Kind](const auto &Call) {
    return Call.Object == Object && (!Kind || Call.Kind == *Kind);
  });
  if (I == Queue.end())
    return false;
  Queue.erase(I);
  return true;
}

} // namespace

llvm::Expected<uint64_t> KernelScheduler::queryTimeIncrement() const {
  if (auto E =
          validateDriverSystemClockIncrement(Bounds.SystemClockIncrement100ns))
    return E;
  if (!Bounds.SystemClockIncrement100ns)
    return schedulerError(
        "KeQueryTimeIncrement requires system_clock_increment_100ns");
  return uint64_t(*Bounds.SystemClockIncrement100ns);
}

llvm::Expected<uint64_t> KernelScheduler::queryTickCount() const {
  auto Increment = queryTimeIncrement();
  if (!Increment)
    return Increment.takeError();
  return Now / *Increment;
}

bool KernelScheduler::isDMACallbackKind(CallbackKind Kind) {
  return Kind == CallbackKind::DMAListControl ||
         Kind == CallbackKind::DMAAdapterControl;
}

bool KernelScheduler::isFrameworkInterruptCallbackKind(CallbackKind Kind) {
  return Kind == CallbackKind::FrameworkInterruptDPC ||
         Kind == CallbackKind::FrameworkInterruptWorkItem;
}

llvm::Expected<uint64_t> KernelScheduler::issueReadyOrder() {
  if (NextReadyOrder == UINT64_MAX)
    return schedulerError(driver_scheduling::ReadyOverflow);
  return NextReadyOrder++;
}

std::optional<uint64_t> KernelScheduler::nextPassiveReadyOrder() const {
  if (auto Next = nextPassiveThread())
    return Next->ReadyOrder;
  return std::nullopt;
}

int32_t KernelScheduler::threadPriority(uint64_t Key) const {
  const auto Found = ThreadPriorities.find(Key);
  return Found == ThreadPriorities.end() ? thread_priority::Default
                                         : Found->second;
}

llvm::Expected<int32_t> KernelScheduler::setThreadPriority(uint64_t Key,
                                                           int32_t Priority) {
  if (Priority < thread_priority::Minimum ||
      Priority > thread_priority::Maximum)
    return schedulerError(thread_priority::InvalidPriority);
  const int32_t Previous = threadPriority(Key);
  ThreadPriorities.insert_or_assign(Key, Priority);
  return Previous;
}

const KernelScheduler::Invocation *
KernelScheduler::nextPassiveInvocation() const {
  if (!OrderedPassive)
    return !Workers.empty()         ? &Workers.front()
           : !SystemThreads.empty() ? &SystemThreads.front()
                                    : nullptr;
  const Invocation *Best = nullptr;
  for (const auto *Queue : {&Workers, &SystemThreads})
    for (const auto &Call : *Queue)
      if (!Best || readyThread(Call.ID, Call.ReadyOrder)
                       .precedes(readyThread(Best->ID, Best->ReadyOrder)))
        Best = &Call;
  return Best;
}

std::optional<KernelScheduler::ReadyThread>
KernelScheduler::nextPassiveThread() const {
  if (const auto *Call = nextPassiveInvocation())
    return readyThread(Call->ID, Call->ReadyOrder);
  return std::nullopt;
}

llvm::Error KernelScheduler::validateTime() const {
  if (Now > Bounds.MaxTime100ns)
    return schedulerError("scheduler initial time exceeds virtual time limit");
  return llvm::Error::success();
}

llvm::Error KernelScheduler::validateCallback(const Callback &Work) const {
  if (auto E = validateTime())
    return E;
  if (!Work.Object || !Work.Owner || !Work.Thread || !Work.PC)
    return schedulerError("callback requires nonzero object, owner, thread, "
                          "and guest PC identities");
  if (Work.Arguments.size() > scheduler::MaxArguments)
    return schedulerError("callback argument count exceeds scheduler limit");
  return llvm::Error::success();
}

llvm::Error KernelScheduler::validateDPC(const DpcCallback &DPC) const {
  if (auto E = validateCallback(DPC))
    return E;
  switch (DPC.Importance) {
  case DpcImportance::Low:
  case DpcImportance::Medium:
  case DpcImportance::High:
  case DpcImportance::MediumHigh:
    return llvm::Error::success();
  }
  return schedulerError("invalid DPC importance");
}

llvm::Error
KernelScheduler::validateInterrupt(const InterruptCallback &Interrupt) const {
  if (auto E = validateCallback(Interrupt))
    return E;
  const bool Passive = Interrupt.IRQL == scheduler::PassiveLevel;
  if ((!Passive && Interrupt.Priority < scheduler::MinDeviceIRQL) ||
      (Passive && Interrupt.Priority &&
       Interrupt.Priority < scheduler::MinDeviceIRQL) ||
      Interrupt.Priority > scheduler::MaxDeviceIRQL ||
      (!Passive && Interrupt.IRQL < Interrupt.Priority) ||
      Interrupt.IRQL > scheduler::MaxDeviceIRQL)
    return schedulerError("interrupt requires a device priority and an equal "
                          "or higher synchronization IRQL");
  return llvm::Error::success();
}

llvm::Error KernelScheduler::checkCapacity(uint64_t Additional) const {
  const uint64_t Outstanding = queuedCallbackCount() + Suspended.size() +
                               bool(Active) + WaitingDMA.size() +
                               WaitingSystemThreads.size() + InlineDMA.size();
  if (Outstanding > Bounds.MaxPendingCallbacks ||
      Additional > Bounds.MaxPendingCallbacks - Outstanding)
    return schedulerError("scheduler pending callback limit exhausted");
  if (Additional > UINT64_MAX - NextID)
    return schedulerError("scheduler callback identity overflow");
  if (Additional > UINT64_MAX - NextReadyOrder)
    return schedulerError(driver_scheduling::ReadyOverflow);
  return llvm::Error::success();
}

KernelScheduler::Invocation KernelScheduler::makeInvocation(Callback Work,
                                                            CallbackKind Kind,
                                                            uint64_t DueTime) {
  Invocation Call;
  static_cast<Callback &>(Call) = std::move(Work);
  Call.ID = NextID++;
  Call.ReadyOrder = NextReadyOrder++;
  Call.Kind = Kind;
  Call.IRQL = Kind == CallbackKind::DPC ||
                      Kind == CallbackKind::FrameworkInterruptDPC ||
                      Kind == CallbackKind::WDMCancel || isDMACallbackKind(Kind)
                  ? scheduler::DispatchLevel
                  : scheduler::PassiveLevel;
  Call.DueTime100ns = DueTime;
  return Call;
}

llvm::Expected<uint64_t> KernelScheduler::enqueueWorkItem(Callback Work) {
  if (auto E = validateCallback(Work))
    return E;
  if (isWorkItemQueued(Work.Object))
    return schedulerError("work item is already queued");
  if (auto E = checkCapacity(1))
    return E;
  Workers.push_back(
      makeInvocation(std::move(Work), CallbackKind::WorkItem, Now));
  return Workers.back().ID;
}

llvm::Error
KernelScheduler::canEnqueueSystemThread(const Callback &Thread) const {
  if (auto E = validateCallback(Thread))
    return E;
  return checkCapacity(1);
}

llvm::Expected<uint64_t> KernelScheduler::enqueueSystemThread(Callback Thread) {
  if (auto E = canEnqueueSystemThread(Thread))
    return E;
  SystemThreads.push_back(
      makeInvocation(std::move(Thread), CallbackKind::SystemThread, Now));
  return SystemThreads.back().ID;
}

llvm::Expected<uint64_t> KernelScheduler::reserveSystemThread(Callback Thread) {
  if (auto E = canEnqueueSystemThread(Thread))
    return E;
  auto Call =
      makeInvocation(std::move(Thread), CallbackKind::SystemThread, Now);
  const auto ID = Call.ID;
  WaitingSystemThreads.emplace(ID, std::move(Call));
  return ID;
}
llvm::Error KernelScheduler::readySystemThread(uint64_t ID) {
  auto It = WaitingSystemThreads.find(ID);
  if (It == WaitingSystemThreads.end())
    return schedulerError("system thread readiness lost its reservation");
  auto Order = issueReadyOrder();
  if (!Order)
    return Order.takeError();
  It->second.ReadyOrder = *Order;
  It->second.DueTime100ns = Now;
  SystemThreads.push_back(std::move(It->second));
  WaitingSystemThreads.erase(It);
  return llvm::Error::success();
}
bool KernelScheduler::cancelWorkItem(uint64_t Object) {
  return removeObject(Workers, Object, CallbackKind::WorkItem);
}

bool KernelScheduler::isWorkItemQueued(uint64_t Object) const {
  return containsObject(Workers, Object, CallbackKind::WorkItem);
}

llvm::Error KernelScheduler::canEnqueueWDMDispatch() const {
  if (auto E = validateTime())
    return E;
  return checkCapacity(1);
}

bool KernelScheduler::hasWDMDispatch(uint64_t Object) const {
  auto Matches = [Object](const Invocation &Call) {
    return Call.Object == Object &&
           (Call.Kind == CallbackKind::WDMDispatch ||
            Call.Kind == CallbackKind::WDMProviderDispatch);
  };
  return (Active && Matches(*Active)) || llvm::any_of(Workers, Matches) ||
         llvm::any_of(Suspended,
                      [&](const auto &Entry) { return Matches(Entry.second); });
}

llvm::Expected<uint64_t>
KernelScheduler::enqueueWDMDispatch(Callback Dispatch) {
  if (auto E = validateCallback(Dispatch))
    return E;
  if (Dispatch.Arguments.size() != 2 ||
      Dispatch.Arguments[0] != Dispatch.Owner ||
      Dispatch.Arguments[1] != Dispatch.Object ||
      Dispatch.SynchronizationObject)
    return schedulerError("WDM dispatch requires its device and IRP arguments");
  if (hasWDMDispatch(Dispatch.Object))
    return schedulerError("WDM dispatch is already outstanding for this IRP");
  if (auto E = canEnqueueWDMDispatch())
    return E;
  Workers.push_back(
      makeInvocation(std::move(Dispatch), CallbackKind::WDMDispatch, Now));
  return Workers.back().ID;
}

llvm::Expected<uint64_t>
KernelScheduler::enqueueWDMProviderDispatch(uint64_t IRP, uint64_t PDO,
                                            uint64_t Thread) {
  if (!IRP || !PDO || !Thread)
    return schedulerError(
        "provider dispatch requires nonzero IRP, PDO and thread identities");
  if (hasWDMDispatch(IRP))
    return schedulerError("WDM dispatch is already outstanding for this IRP");
  if (auto E = canEnqueueWDMDispatch())
    return E;
  Workers.push_back(makeInvocation({IRP, PDO, Thread, 0, {PDO, IRP}},
                                   CallbackKind::WDMProviderDispatch, Now));
  return Workers.back().ID;
}

llvm::Expected<KernelScheduler::Invocation>
KernelScheduler::beginWDMProviderCompletion(uint64_t ID, Callback Completion) {
  if (auto E = canFinish(ID))
    return E;
  if (Active->Kind != CallbackKind::WDMProviderDispatch || Active->PC ||
      Active->IRQL != scheduler::PassiveLevel)
    return schedulerError("completion requires its active provider dispatch");
  if (Completion.Object != Active->Object ||
      Completion.Owner != Active->Owner ||
      Completion.Thread != Active->Thread || Completion.SynchronizationObject)
    return schedulerError("provider completion changed its dispatch identity");
  if (auto E = validateCallback(Completion))
    return E;
  if (containsObject(Completions, Completion.Object))
    return schedulerError("request completion is already queued");
  static_cast<Callback &>(*Active) = std::move(Completion);
  Active->Kind = CallbackKind::WDMCompletion;
  return *Active;
}

llvm::Expected<uint64_t>
KernelScheduler::enqueueFrameworkCancel(Callback Cancellation) {
  if (auto E = validateCallback(Cancellation))
    return E;
  if (containsObject(Cancellations, Cancellation.Object))
    return schedulerError("framework cancellation is already queued");
  if (auto E = checkCapacity(1))
    return E;
  Cancellations.push_back(makeInvocation(std::move(Cancellation),
                                         CallbackKind::FrameworkCancel, Now));
  return Cancellations.back().ID;
}

llvm::Expected<uint64_t>
KernelScheduler::enqueueFrameworkDeferred(Callback Call, uint8_t IRQL) {
  if (auto E = validateCallback(Call))
    return E;
  if (IRQL != scheduler::PassiveLevel && IRQL != scheduler::DispatchLevel)
    return schedulerError(
        "framework callback requires PASSIVE or DISPATCH IRQL");
  auto Matches = [&](const Invocation &Existing) {
    return Existing.Kind == CallbackKind::FrameworkDeferred &&
           Existing.Object == Call.Object;
  };
  if (llvm::any_of(Workers, Matches) || llvm::any_of(DPCs, Matches) ||
      (Active && Matches(*Active) && Active->IRQL == IRQL) ||
      llvm::any_of(Suspended,
                   [&](const auto &Entry) { return Matches(Entry.second); }))
    return schedulerError("framework continuation is already outstanding");
  if (auto E = checkCapacity(1))
    return E;
  // An active continuation can hand its next callback to another IRQL FIFO;
  // the current invocation retires before that successor can be selected.
  auto Invocation =
      makeInvocation(std::move(Call), CallbackKind::FrameworkDeferred, Now);
  Invocation.IRQL = IRQL;
  auto &Queue = IRQL == scheduler::DispatchLevel ? DPCs : Workers;
  Queue.push_back(std::move(Invocation));
  return Queue.back().ID;
}

llvm::Expected<uint64_t> KernelScheduler::enqueuePoFx(Callback Call) {
  if (auto E = validateCallback(Call))
    return E;
  const auto Matches = [&](const Invocation &Existing) {
    return Existing.Kind == CallbackKind::PoFx &&
           Existing.Object == Call.Object;
  };
  if (llvm::any_of(Workers, Matches) || (Active && Matches(*Active)) ||
      llvm::any_of(Suspended,
                   [&](const auto &Entry) { return Matches(Entry.second); }))
    return schedulerError("PoFx callback is already outstanding");
  if (auto E = checkCapacity(1))
    return E;
  Workers.push_back(makeInvocation(std::move(Call), CallbackKind::PoFx, Now));
  return Workers.back().ID;
}

llvm::Error KernelScheduler::canEnqueueUsbIdleCallbacks(
    llvm::ArrayRef<Callback> Calls) const {
  std::vector<UsbIdleCallback> Batch;
  Batch.reserve(Calls.size());
  for (const auto &Call : Calls)
    Batch.push_back({Call, CallbackKind::UsbIdle});
  return canEnqueueUsbIdleBatch(Batch);
}

llvm::Error KernelScheduler::canEnqueueUsbIdleBatch(
    llvm::ArrayRef<UsbIdleCallback> Calls) const {
  if (auto E = validateTime())
    return E;
  std::set<uint64_t> Objects;
  for (const auto &Entry : Calls) {
    const auto &Call = Entry.Call;
    if (Entry.Kind == CallbackKind::UsbIdle) {
      if (auto E = validateCallback(Call))
        return E;
      if (Call.Arguments.size() != 1 || Call.SynchronizationObject)
        return schedulerError("USB idle callback requires one context argument "
                              "without a synchronization object");
    } else if (Entry.Kind == CallbackKind::FrameworkUsbIdle) {
      if (!Call.Object || !Call.Owner || !Call.Thread || Call.PC ||
          Call.SynchronizationObject || Call.Arguments.size() != 3 ||
          Call.Arguments[0] != Call.Owner || Call.Arguments[1] != Call.Object ||
          !Call.Arguments[2])
        return schedulerError("framework USB task requires its typed provider, "
                              "IRP and callback token without a guest PC");
    } else {
      return schedulerError("USB batch has a foreign callback kind");
    }
    const auto Matches = [&](const Invocation &Existing) {
      return (Existing.Kind == CallbackKind::UsbIdle ||
              Existing.Kind == CallbackKind::FrameworkUsbIdle ||
              Existing.FrameworkUsbIdleContinuation) &&
             Existing.Object == Call.Object;
    };
    if (!Objects.insert(Call.Object).second || llvm::any_of(Workers, Matches) ||
        (Active && Matches(*Active)) ||
        llvm::any_of(Suspended,
                     [&](const auto &Entry) { return Matches(Entry.second); }))
      return schedulerError("USB idle callback is already outstanding");
  }
  return checkCapacity(Calls.size());
}

llvm::Expected<uint64_t>
KernelScheduler::enqueueUsbIdleCallback(Callback Call) {
  if (auto E = canEnqueueUsbIdleCallbacks({Call}))
    return E;
  Workers.push_back(
      makeInvocation(std::move(Call), CallbackKind::UsbIdle, Now));
  return Workers.back().ID;
}

llvm::Expected<uint64_t> KernelScheduler::enqueueFrameworkUsbIdleCallback(
    uint64_t IRP, uint64_t PDO, uint64_t Thread, uint64_t Token) {
  Callback Call{IRP, PDO, Thread, 0, {PDO, IRP, Token}};
  if (auto E = canEnqueueUsbIdleBatch({{Call, CallbackKind::FrameworkUsbIdle}}))
    return E;
  Workers.push_back(
      makeInvocation(std::move(Call), CallbackKind::FrameworkUsbIdle, Now));
  return Workers.back().ID;
}

llvm::Expected<KernelScheduler::Invocation>
KernelScheduler::beginFrameworkUsbIdleCallback(uint64_t ID, Callback Call) {
  if (auto E = canFinish(ID))
    return E;
  if (Active->Kind != CallbackKind::FrameworkUsbIdle || Active->PC ||
      Active->IRQL != scheduler::PassiveLevel)
    return schedulerError(
        "framework USB continuation requires its active task");
  if (Call.Object != Active->Object || Call.Owner != Active->Owner ||
      Call.Thread != Active->Thread)
    return schedulerError(
        "framework USB continuation changed its task identity");
  if (auto E = validateCallback(Call))
    return E;
  static_cast<Callback &>(*Active) = std::move(Call);
  Active->Kind = CallbackKind::FrameworkDeferred;
  Active->FrameworkUsbIdleContinuation = true;
  return *Active;
}

llvm::Error KernelScheduler::canWithdrawUsbIdleCallback(uint64_t ID) const {
  if (llvm::none_of(Workers, [ID](const auto &Call) {
        return Call.ID == ID && (Call.Kind == CallbackKind::UsbIdle ||
                                 Call.Kind == CallbackKind::FrameworkUsbIdle);
      }))
    return schedulerError("USB idle withdrawal requires its exact queued ID");
  return llvm::Error::success();
}

llvm::Error KernelScheduler::withdrawUsbIdleCallback(uint64_t ID) {
  if (auto E = canWithdrawUsbIdleCallback(ID))
    return E;
  Workers.erase(llvm::find_if(Workers, [ID](const auto &Call) {
    return Call.ID == ID && (Call.Kind == CallbackKind::UsbIdle ||
                             Call.Kind == CallbackKind::FrameworkUsbIdle);
  }));
  return llvm::Error::success();
}

llvm::Error KernelScheduler::canEnqueueWDMCancellations(
    llvm::ArrayRef<Callback> Batch) const {
  if (auto E = validateTime())
    return E;
  std::set<uint64_t> Objects;
  for (const auto &Cancellation : Batch) {
    if (auto E = validateCallback(Cancellation))
      return E;
    if (containsObject(Cancellations, Cancellation.Object) ||
        !Objects.insert(Cancellation.Object).second)
      return schedulerError("request cancellation is already queued");
  }
  return checkCapacity(Batch.size());
}

llvm::Expected<uint64_t>
KernelScheduler::enqueueWDMCancellation(Callback Cancellation) {
  if (auto E = canEnqueueWDMCancellations({Cancellation}))
    return E;
  Cancellations.push_back(
      makeInvocation(std::move(Cancellation), CallbackKind::WDMCancel, Now));
  return Cancellations.back().ID;
}

bool KernelScheduler::hasQueuedFrameworkCancel() const {
  return llvm::any_of(Cancellations, [](const auto &Call) {
    return Call.Kind == CallbackKind::FrameworkCancel;
  });
}

llvm::Expected<bool> KernelScheduler::queueDPC(DpcCallback DPC) {
  if (auto E = validateDPC(DPC))
    return E;
  if (isDPCQueued(DPC.Object))
    return false;
  if (auto E = checkCapacity(1))
    return E;
  const auto Importance = DPC.Importance;
  auto Call = makeInvocation(std::move(DPC), CallbackKind::DPC, Now);
  if (Importance == DpcImportance::High)
    DPCs.push_front(std::move(Call));
  else
    DPCs.push_back(std::move(Call));
  return true;
}

llvm::Expected<std::optional<uint64_t>>
KernelScheduler::queueFrameworkInterrupt(Callback Call, bool WorkItem) {
  if (auto E = validateCallback(Call))
    return E;
  const auto Kind = WorkItem ? CallbackKind::FrameworkInterruptWorkItem
                             : CallbackKind::FrameworkInterruptDPC;
  auto &Queue = WorkItem ? Workers : DPCs;
  if (containsObject(Queue, Call.Object, Kind))
    return std::optional<uint64_t>{};
  if (auto E = checkCapacity(1))
    return E;
  Queue.push_back(makeInvocation(std::move(Call), Kind, Now));
  return std::optional<uint64_t>{Queue.back().ID};
}

bool KernelScheduler::hasFrameworkInterrupt(uint64_t Object) const {
  auto Matches = [Object](const Invocation &Call) {
    return Call.Object == Object && isFrameworkInterruptCallbackKind(Call.Kind);
  };
  return (Active && Matches(*Active)) || llvm::any_of(Workers, Matches) ||
         llvm::any_of(DPCs, Matches) ||
         llvm::any_of(Suspended,
                      [&](const auto &Entry) { return Matches(Entry.second); });
}

llvm::Error
KernelScheduler::canEnqueueCompletions(llvm::ArrayRef<Callback> Batch) const {
  if (auto E = validateTime())
    return E;
  std::set<uint64_t> Objects;
  for (const auto &Completion : Batch) {
    if (auto E = validateCallback(Completion))
      return E;
    if (containsObject(Completions, Completion.Object) ||
        !Objects.insert(Completion.Object).second)
      return schedulerError("request completion is already queued");
  }
  return checkCapacity(Batch.size());
}

llvm::Expected<uint64_t>
KernelScheduler::enqueueWDMCompletion(Callback Completion) {
  if (auto E = canEnqueueCompletions({Completion}))
    return E;
  Completions.push_back(
      makeInvocation(std::move(Completion), CallbackKind::WDMCompletion, Now));
  return Completions.back().ID;
}

llvm::Expected<uint64_t>
KernelScheduler::enqueueFrameworkCompletion(Callback Completion) {
  if (auto E = canEnqueueCompletions({Completion}))
    return E;
  Completions.push_back(makeInvocation(std::move(Completion),
                                       CallbackKind::FrameworkCompletion, Now));
  return Completions.back().ID;
}

llvm::Error KernelScheduler::canEnqueueInterrupts(
    llvm::ArrayRef<InterruptCallback> Batch) const {
  if (auto E = validateTime())
    return E;
  std::set<uint64_t> Objects;
  for (const auto &Interrupt : Batch) {
    if (auto E = validateInterrupt(Interrupt))
      return E;
    if (containsObject(Interrupts, Interrupt.Object) ||
        !Objects.insert(Interrupt.Object).second)
      return schedulerError("interrupt event is already queued");
  }
  return checkCapacity(Batch.size());
}

llvm::Expected<uint64_t>
KernelScheduler::enqueueInterrupt(InterruptCallback Interrupt) {
  if (auto E = canEnqueueInterrupts({Interrupt}))
    return E;
  const uint8_t IRQL = Interrupt.IRQL;
  const uint8_t Priority = Interrupt.Priority;
  auto Call =
      makeInvocation(std::move(Interrupt), CallbackKind::Interrupt, Now);
  Call.IRQL = IRQL;
  Call.InterruptPriority = Priority;
  const uint64_t ID = Call.ID;
  auto Position =
      llvm::find_if(Interrupts, [IRQL, Priority](const auto &Queued) {
        if (bool(IRQL) != bool(Queued.IRQL))
          return bool(IRQL);
        return Queued.InterruptPriority < Priority;
      });
  Interrupts.insert(Position, std::move(Call));
  return ID;
}

bool KernelScheduler::removeDPC(uint64_t Object) {
  return removeObject(DPCs, Object, CallbackKind::DPC);
}

llvm::Error KernelScheduler::canReserveDMACallback(const Callback &Call,
                                                   CallbackKind Kind) const {
  if (!isDMACallbackKind(Kind))
    return schedulerError("callback kind is not a DMA callback");
  if (auto E = validateCallback(Call))
    return E;
  return checkCapacity(1);
}

llvm::Expected<uint64_t>
KernelScheduler::reserveDMACallback(Callback Call, CallbackKind Kind) {
  if (auto E = canReserveDMACallback(Call, Kind))
    return E;
  auto Reserved = makeInvocation(std::move(Call), Kind, Now);
  const uint64_t ID = Reserved.ID;
  WaitingDMA.emplace(ID, std::move(Reserved));
  return ID;
}

llvm::Error
KernelScheduler::validateDMAAdmission(llvm::ArrayRef<uint64_t> IDs,
                                      std::optional<CallbackKind> Kind) const {
  if (auto E = validateTime())
    return E;
  std::set<uint64_t> Seen;
  for (uint64_t ID : IDs) {
    const auto Found = WaitingDMA.find(ID);
    if (Found == WaitingDMA.end())
      return schedulerError("DMA callback is not waiting for resources");
    if (Kind && Found->second.Kind != *Kind)
      return schedulerError("DMA callback kind mismatch");
    if (!Seen.insert(ID).second)
      return schedulerError("duplicate DMA callback in admission batch");
  }
  return llvm::Error::success();
}

llvm::Error
KernelScheduler::canReadyDMACallbacks(llvm::ArrayRef<uint64_t> IDs) const {
  return validateDMAAdmission(IDs, std::nullopt);
}

llvm::Error KernelScheduler::readyDMACallbacks(llvm::ArrayRef<uint64_t> IDs) {
  if (auto E = canReadyDMACallbacks(IDs))
    return E;
  for (uint64_t ID : IDs) {
    auto Reserved = WaitingDMA.extract(ID);
    Reserved.mapped().DueTime100ns = Now;
    ReadyDMA.push_back(std::move(Reserved.mapped()));
  }
  return llvm::Error::success();
}

llvm::Error KernelScheduler::canDispatchInlineDMACallback() const {
  if (auto E = validateTime())
    return E;
  if (Dispatches >= Bounds.MaxDispatches)
    return schedulerError("scheduler callback dispatch limit exhausted");
  return llvm::Error::success();
}

llvm::Error
KernelScheduler::canBeginInlineDMACallback(uint64_t ID,
                                           CallbackKind Kind) const {
  if (!isDMACallbackKind(Kind))
    return schedulerError("callback kind is not a DMA callback");
  if (auto E = canDispatchInlineDMACallback())
    return E;
  const auto Found = WaitingDMA.find(ID);
  if (Found == WaitingDMA.end())
    return schedulerError("DMA callback is not waiting for inline delivery");
  if (Found->second.Kind != Kind)
    return schedulerError("DMA callback kind mismatch");
  return llvm::Error::success();
}

llvm::Error KernelScheduler::beginInlineDMACallback(uint64_t ID,
                                                    CallbackKind Kind) {
  if (auto E = canBeginInlineDMACallback(ID, Kind))
    return E;
  auto Reserved = WaitingDMA.extract(ID);
  Reserved.mapped().DueTime100ns = Now;
  InlineDMA.push_back(std::move(Reserved.mapped()));
  ++Dispatches;
  return llvm::Error::success();
}

llvm::Error
KernelScheduler::canFinishInlineDMACallback(uint64_t ID,
                                            CallbackKind Kind) const {
  if (!isDMACallbackKind(Kind))
    return schedulerError("callback kind is not a DMA callback");
  if (InlineDMA.empty() || InlineDMA.back().ID != ID)
    return schedulerError("inline DMA callback return identity mismatch");
  if (InlineDMA.back().Kind != Kind)
    return schedulerError("DMA callback kind mismatch");
  return llvm::Error::success();
}

llvm::Error KernelScheduler::finishInlineDMACallback(uint64_t ID,
                                                     CallbackKind Kind) {
  if (auto E = canFinishInlineDMACallback(ID, Kind))
    return E;
  InlineDMA.pop_back();
  return llvm::Error::success();
}

llvm::Error
KernelScheduler::canReserveDMAListControl(const Callback &Call) const {
  return canReserveDMACallback(Call, CallbackKind::DMAListControl);
}

llvm::Expected<uint64_t> KernelScheduler::reserveDMAListControl(Callback Call) {
  return reserveDMACallback(std::move(Call), CallbackKind::DMAListControl);
}

llvm::Error
KernelScheduler::canReadyDMAListControls(llvm::ArrayRef<uint64_t> IDs) const {
  return validateDMAAdmission(IDs, CallbackKind::DMAListControl);
}

llvm::Error
KernelScheduler::readyDMAListControls(llvm::ArrayRef<uint64_t> IDs) {
  if (auto E = canReadyDMAListControls(IDs))
    return E;
  return readyDMACallbacks(IDs);
}

bool KernelScheduler::hasQueuedDMAListControl() const {
  return llvm::any_of(ReadyDMA, [](const auto &Call) {
    return Call.Kind == CallbackKind::DMAListControl;
  });
}

llvm::Error KernelScheduler::canDispatchInlineDMAListControl() const {
  return canDispatchInlineDMACallback();
}

llvm::Error KernelScheduler::canBeginInlineDMAListControl(uint64_t ID) const {
  return canBeginInlineDMACallback(ID, CallbackKind::DMAListControl);
}

llvm::Error KernelScheduler::beginInlineDMAListControl(uint64_t ID) {
  return beginInlineDMACallback(ID, CallbackKind::DMAListControl);
}

llvm::Error KernelScheduler::canFinishInlineDMAListControl(uint64_t ID) const {
  return canFinishInlineDMACallback(ID, CallbackKind::DMAListControl);
}

llvm::Error KernelScheduler::finishInlineDMAListControl(uint64_t ID) {
  return finishInlineDMACallback(ID, CallbackKind::DMAListControl);
}

bool KernelScheduler::isDPCQueued(uint64_t Object) const {
  return containsObject(DPCs, Object, CallbackKind::DPC);
}

llvm::Expected<uint64_t>
KernelScheduler::computeDeadline(int64_t Time100ns) const {
  if (auto E = validateTime())
    return E;
  uint64_t Due = Time100ns;
  if (Time100ns < 0) {
    const uint64_t Interval = uint64_t(-(Time100ns + 1)) + 1;
    if (Interval > UINT64_MAX - Now)
      return schedulerError("relative deadline overflows virtual time");
    Due = Now + Interval;
  }
  if (Due > Bounds.MaxTime100ns)
    return schedulerError("deadline exceeds virtual time limit");
  return Due;
}

llvm::Expected<bool> KernelScheduler::setTimer(uint64_t Timer, uint64_t Owner,
                                               int64_t DueTime100ns,
                                               uint32_t PeriodMilliseconds,
                                               std::optional<DpcCallback> DPC) {
  if (auto E = validateTime())
    return E;
  if (!Timer || !Owner)
    return schedulerError("timer requires nonzero object and owner identities");
  if (PeriodMilliseconds > INT32_MAX)
    return schedulerError(
        "timer period does not fit a nonnegative Windows LONG");
  if (DPC) {
    if (auto E = validateDPC(*DPC))
      return E;
    if (DPC->Owner != Owner)
      return schedulerError("timer and DPC must retain the same owner");
  }
  auto I = Timers.find(Timer);
  if (I == Timers.end() && Timers.size() >= Bounds.MaxTimers)
    return schedulerError("scheduler timer object limit exhausted");
  if (I != Timers.end() && I->second.Owner != Owner)
    return schedulerError(
        "timer owner cannot change before its object is freed");
  if (NextTimerSequence == UINT64_MAX)
    return schedulerError("scheduler timer sequence overflow");
  auto Due = computeDeadline(DueTime100ns);
  if (!Due)
    return Due.takeError();
  const bool WasArmed = I != Timers.end() && I->second.Armed;
  TimerState State;
  State.Owner = Owner;
  State.DueTime100ns = *Due;
  State.Period100ns =
      uint64_t(PeriodMilliseconds) * scheduler::TicksPerMillisecond;
  State.Sequence = NextTimerSequence++;
  State.Armed = true;
  State.DPC = std::move(DPC);
  Timers.insert_or_assign(Timer, std::move(State));
  return WasArmed;
}

bool KernelScheduler::cancelTimer(uint64_t Timer) {
  auto I = Timers.find(Timer);
  if (I == Timers.end() || !I->second.Armed)
    return false;
  // Cancellation removes the future timer insertion. A DPC from an earlier
  // expiration has an independent queue/running lifetime.
  I->second.Armed = false;
  return true;
}

bool KernelScheduler::isTimerArmed(uint64_t Timer) const {
  const auto I = Timers.find(Timer);
  return I != Timers.end() && I->second.Armed;
}

llvm::Expected<bool> KernelScheduler::timerSignaled(uint64_t Timer) const {
  const auto I = Timers.find(Timer);
  if (I == Timers.end())
    return schedulerError("unknown scheduler timer");
  return I->second.Signaled;
}

llvm::Expected<bool> KernelScheduler::consumeTimerSignal(uint64_t Timer) {
  auto I = Timers.find(Timer);
  if (I == Timers.end())
    return schedulerError("unknown scheduler timer");
  const bool WasSignaled = I->second.Signaled;
  I->second.Signaled = false;
  return WasSignaled;
}

llvm::Error
KernelScheduler::consumeTimerSignals(llvm::ArrayRef<uint64_t> TimerObjects) {
  for (uint64_t Timer : TimerObjects) {
    auto I = Timers.find(Timer);
    if (I == Timers.end() || !I->second.Signaled)
      return schedulerError(kernel_wait::TimerLost);
  }
  for (uint64_t Timer : TimerObjects)
    Timers.at(Timer).Signaled = false;
  return llvm::Error::success();
}

llvm::Error KernelScheduler::canForgetTimer(uint64_t Timer) const {
  auto I = Timers.find(Timer);
  if (I == Timers.end())
    return schedulerError("unknown scheduler timer");
  if (I->second.Armed ||
      (Active && Active->SourceTimer == Timer && Active->SourceTimerPeriodic) ||
      llvm::any_of(Suspended,
                   [Timer](const auto &Item) {
                     return Item.second.SourceTimer == Timer;
                   }) ||
      llvm::any_of(DPCs, [Timer](const auto &Call) {
        return Call.SourceTimer == Timer;
      }))
    return schedulerError("timer still has an armed expiry or outstanding DPC");
  return llvm::Error::success();
}

llvm::Error KernelScheduler::forgetTimer(uint64_t Timer) {
  if (auto E = canForgetTimer(Timer))
    return E;
  Timers.erase(Timer);
  return llvm::Error::success();
}

llvm::Error
KernelScheduler::validateTimerExpirations(uint64_t Time,
                                          uint64_t AdditionalCallbacks) const {
  if (auto E = validateTime())
    return E;
  if (Time < Now || Time > Bounds.MaxTime100ns)
    return schedulerError("timer boundary exceeds the virtual time range");
  // Validate the entire expiration batch before advancing time, signaling a
  // timer, or enqueuing callbacks. A resource failure must not drop callbacks
  // or partially consume deadlines. Duplicate DPC insertion is the documented
  // coalescing behavior, not an exhausted-budget fallback.
  uint64_t Expirations = 0;
  std::set<uint64_t> NewDPCs;
  for (const auto &[Object, Timer] : Timers) {
    if (!Timer.Armed || Timer.DueTime100ns > Time)
      continue;
    ++Expirations;
    if (Timer.Period100ns && (Timer.Period100ns > UINT64_MAX - Time ||
                              Timer.Period100ns > Bounds.MaxTime100ns - Time))
      return schedulerError(
          "periodic timer deadline overflows virtual time limit");
    if (Timer.DPC && !isDPCQueued(Timer.DPC->Object))
      NewDPCs.insert(Timer.DPC->Object);
  }
  if (TimerExpirations > Bounds.MaxTimerExpirations ||
      Expirations > Bounds.MaxTimerExpirations - TimerExpirations)
    return schedulerError("scheduler timer expiration limit exhausted");
  if (AdditionalCallbacks > UINT64_MAX - NewDPCs.size())
    return schedulerError("scheduler pending callback count overflow");
  return checkCapacity(NewDPCs.size() + AdditionalCallbacks);
}

llvm::Error KernelScheduler::expireTimers(uint64_t Time) {
  if (auto E = validateTimerExpirations(Time, 0))
    return E;
  std::vector<std::pair<uint64_t, TimerState *>> Due;
  for (auto &[Object, Timer] : Timers)
    if (Timer.Armed && Timer.DueTime100ns <= Time)
      Due.emplace_back(Object, &Timer);
  llvm::sort(Due, [](const auto &A, const auto &B) {
    return std::tie(A.second->DueTime100ns, A.second->Sequence) <
           std::tie(B.second->DueTime100ns, B.second->Sequence);
  });

  Now = Time;
  TimerExpirations += Due.size();
  for (auto &[Object, Timer] : Due) {
    Timer->Signaled = true;
    const uint64_t ExpiredTime = Timer->DueTime100ns;
    if (Timer->Period100ns)
      Timer->DueTime100ns = Time + Timer->Period100ns;
    else
      Timer->Armed = false;
    if (!Timer->DPC || isDPCQueued(Timer->DPC->Object))
      continue;
    auto Call = makeInvocation(*Timer->DPC, CallbackKind::DPC, ExpiredTime);
    Call.SourceTimer = Object;
    Call.SourceTimerPeriodic = Timer->Period100ns != 0;
    if (Timer->DPC->Importance == DpcImportance::High)
      DPCs.push_front(std::move(Call));
    else
      DPCs.push_back(std::move(Call));
  }
  return llvm::Error::success();
}

llvm::Expected<std::optional<KernelScheduler::Invocation>>
KernelScheduler::next(bool AdvanceTime,
                      std::optional<uint64_t> MaxAdvanceTime) {
  if (auto E = validateTime())
    return E;
  if (Active || !InlineDMA.empty())
    return schedulerError("cannot dispatch with an unfinished callback");
  if (MaxAdvanceTime && *MaxAdvanceTime > Bounds.MaxTime100ns)
    return schedulerError("wait deadline exceeds virtual time limit");
  bool Advanced = false;
  for (;;) {
    const uint64_t PreviousExpirations = TimerExpirations;
    if (auto E = expireTimers(Now))
      return E;
    if (queuedCallbackCount()) {
      if (Dispatches >= Bounds.MaxDispatches)
        return schedulerError("scheduler callback dispatch limit exhausted");
      const auto *Passive = nextPassiveInvocation();
      auto &Queue = !Interrupts.empty() && Interrupts.front().IRQL ? Interrupts
                    : !DPCs.empty()                                ? DPCs
                    : !ReadyDMA.empty()                            ? ReadyDMA
                    : !Cancellations.empty() ? Cancellations
                    : !Completions.empty()   ? Completions
                    : !Interrupts.empty()    ? Interrupts
                    : Passive->Kind == CallbackKind::SystemThread
                        ? SystemThreads
                        : Workers;
      auto Position = (&Queue == &Workers || &Queue == &SystemThreads)
                          ? llvm::find_if(Queue,
                                          [Passive](const auto &Call) {
                                            return Call.ID == Passive->ID;
                                          })
                          : Queue.begin();
      Active = std::move(*Position);
      Queue.erase(Position);
      ++Dispatches;
      return Active;
    }
    if (!AdvanceTime ||
        (MaxAdvanceTime && (Advanced || Now >= *MaxAdvanceTime ||
                            TimerExpirations != PreviousExpirations)))
      return std::optional<Invocation>();
    auto Earliest = nextEventTime100ns();
    if (MaxAdvanceTime && (!Earliest || *MaxAdvanceTime < *Earliest))
      Earliest = MaxAdvanceTime;
    if (!Earliest)
      return std::optional<Invocation>();
    if (auto E = expireTimers(*Earliest))
      return E;
    Advanced = true;
  }
}

std::optional<uint64_t> KernelScheduler::nextEventTime100ns() const {
  std::optional<uint64_t> Earliest;
  for (const auto &[Object, Timer] : Timers)
    if (Timer.Armed && (!Earliest || Timer.DueTime100ns < *Earliest))
      Earliest = Timer.DueTime100ns;
  if (Earliest)
    return std::max(Now, *Earliest);
  return std::nullopt;
}

llvm::Error KernelScheduler::processDueTimers() {
  if (auto E = validateTime())
    return E;
  return expireTimers(Now);
}

llvm::Error
KernelScheduler::canAdvanceTo100ns(uint64_t Time,
                                   uint64_t AdditionalCallbacks) const {
  if (Time > Now) {
    if (Active || !InlineDMA.empty() || queuedCallbackCount())
      return schedulerError("cannot advance virtual time with runnable work");
  }
  return canAdvanceExecutionTo100ns(Time, AdditionalCallbacks);
}

llvm::Error KernelScheduler::canAdvanceExecutionTo100ns(
    uint64_t Time, uint64_t AdditionalCallbacks) const {
  if (Time > Now) {
    auto Earliest = nextEventTime100ns();
    if (Earliest && Time > *Earliest)
      return schedulerError("cannot advance past an earlier timer boundary");
  }
  return validateTimerExpirations(Time, AdditionalCallbacks);
}

llvm::Error KernelScheduler::advanceExecutionTo100ns(uint64_t Time) {
  if (auto E = canAdvanceExecutionTo100ns(Time))
    return E;
  return expireTimers(Time);
}

llvm::Error KernelScheduler::advanceTo100ns(uint64_t Time) {
  if (auto E = canAdvanceTo100ns(Time))
    return E;
  return expireTimers(Time);
}

llvm::Error KernelScheduler::canFinish(uint64_t ID) const {
  if (!Active || Active->ID != ID)
    return schedulerError("scheduler callback completion identity mismatch");
  if (!InlineDMA.empty())
    return schedulerError("callback still owns an inline DMA invocation");
  return llvm::Error::success();
}

llvm::Error KernelScheduler::finish(uint64_t ID) {
  if (auto E = canFinish(ID))
    return E;
  Active.reset();
  return llvm::Error::success();
}

llvm::Error KernelScheduler::suspend(uint64_t ID) {
  if (!Active || Active->ID != ID)
    return schedulerError("scheduler callback suspension identity mismatch");
  if (!InlineDMA.empty())
    return schedulerError("callback still owns an inline DMA invocation");
  if (Suspended.count(ID))
    return schedulerError("scheduler callback is already suspended");
  Suspended.emplace(ID, std::move(*Active));
  Active.reset();
  return llvm::Error::success();
}

llvm::Error KernelScheduler::resume(uint64_t ID) {
  if (Active || !InlineDMA.empty())
    return schedulerError("cannot resume with an unfinished callback");
  auto I = Suspended.find(ID);
  if (I == Suspended.end())
    return schedulerError("scheduler callback resume identity mismatch");
  Active = std::move(I->second);
  Suspended.erase(I);
  return llvm::Error::success();
}

const KernelScheduler::Invocation *
KernelScheduler::suspended(uint64_t ID) const {
  auto I = Suspended.find(ID);
  return I == Suspended.end() ? nullptr : &I->second;
}

bool KernelScheduler::hasOutstanding(uint64_t Owner) const {
  if (Active && Active->Owner == Owner)
    return true;
  auto Matches = [Owner](const auto &Call) { return Call.Owner == Owner; };
  if (llvm::any_of(Workers, Matches) || llvm::any_of(SystemThreads, Matches) ||
      llvm::any_of(DPCs, Matches) || llvm::any_of(Cancellations, Matches) ||
      llvm::any_of(Completions, Matches) || llvm::any_of(Interrupts, Matches) ||
      llvm::any_of(ReadyDMA, Matches) || llvm::any_of(InlineDMA, Matches))
    return true;
  auto HeldMatches = [Owner](const auto &Item) {
    return Item.second.Owner == Owner;
  };
  if (llvm::any_of(Suspended, HeldMatches) ||
      llvm::any_of(WaitingDMA, HeldMatches) ||
      llvm::any_of(WaitingSystemThreads, HeldMatches))
    return true;
  return llvm::any_of(Timers, [Owner](const auto &Item) {
    return Item.second.Armed && Item.second.Owner == Owner;
  });
}

bool KernelScheduler::hasForegroundBlockingCallbacks() const {
  if (Active || !SystemThreads.empty() || !Workers.empty() || !DPCs.empty() ||
      !Cancellations.empty() || !Completions.empty() || !Interrupts.empty() ||
      !ReadyDMA.empty() || !WaitingSystemThreads.empty())
    return true;
  return std::any_of(Suspended.begin(), Suspended.end(), [](const auto &Entry) {
    return Entry.second.Kind != CallbackKind::SystemThread;
  });
}

bool KernelScheduler::isSuspendedSystemThread(uint64_t ID) const {
  const auto Found = Suspended.find(ID);
  return Found != Suspended.end() &&
         Found->second.Kind == CallbackKind::SystemThread;
}

bool KernelScheduler::hasPending() const {
  if (Active || !Suspended.empty() || queuedCallbackCount() ||
      !WaitingDMA.empty() || !InlineDMA.empty() ||
      !WaitingSystemThreads.empty())
    return true;
  return llvm::any_of(Timers,
                      [](const auto &Item) { return Item.second.Armed; });
}

} // namespace neverd::emulation
