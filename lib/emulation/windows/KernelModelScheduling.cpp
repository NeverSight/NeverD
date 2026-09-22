//===- KernelModelScheduling.cpp - Guest work-item ownership --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Connect Windows work-item lifetime to scheduled guest callbacks.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace neverd::emulation {
namespace {
llvm::Error schedulingError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

llvm::Expected<uint64_t> KernelModel::allocateWorkItem(uint64_t Device) {
  if (CurrentIRQL > scheduler::DispatchLevel)
    return schedulingError(
        "IoAllocateWorkItem requires IRQL <= DISPATCH_LEVEL");
  if (!Devices.count(Device))
    return schedulingError("IoAllocateWorkItem requires a live device");
  const uint64_t Aligned = (NextAllocation + profile::WorkItemTokenSize - 1) &
                           ~(profile::WorkItemTokenSize - 1);
  if (Aligned > AllocationEnd ||
      profile::WorkItemTokenSize > AllocationEnd - Aligned)
    return 0;
  auto Address = allocate(profile::WorkItemTokenSize);
  if (!Address)
    return Address.takeError();
  WorkItems.emplace(*Address, Device);
  return *Address;
}

llvm::Error KernelModel::queueWorkItem(llvm::ArrayRef<uint64_t> Arguments) {
  if (CurrentIRQL > scheduler::DispatchLevel)
    return schedulingError("IoQueueWorkItem requires IRQL <= DISPATCH_LEVEL");
  auto Item = WorkItems.find(Arguments[0]);
  if (Item == WorkItems.end() || !Devices.count(Item->second))
    return schedulingError(
        "IoQueueWorkItem requires a live work item and device");
  if (static_cast<uint32_t>(Arguments[2]) != profile::DelayedWorkQueue)
    return schedulingError("IoQueueWorkItem requires DelayedWorkQueue");
  KernelScheduler::Callback Callback;
  Callback.Object = Item->first;
  Callback.Owner = Item->second;
  Callback.Thread = profile::WorkerThreadIdentity;
  Callback.PC = Arguments[1];
  Callback.Arguments = {Item->second, Arguments[3]};
  auto ID = Scheduler.enqueueWorkItem(std::move(Callback));
  if (!ID)
    return ID.takeError();
  ++WorkReferences[Item->second];
  return updateDeviceReferences(Item->second);
}

llvm::Error KernelModel::freeWorkItem(uint64_t Address) {
  if (CurrentIRQL > scheduler::DispatchLevel)
    return schedulingError("IoFreeWorkItem requires IRQL <= DISPATCH_LEVEL");
  auto Item = WorkItems.find(Address);
  if (Item == WorkItems.end())
    return schedulingError(
        "IoFreeWorkItem requires a live allocated work item");
  if (Scheduler.isWorkItemQueued(Address))
    return schedulingError("IoFreeWorkItem cannot free a queued work item");
  // Windows dequeues before invoking the callback; it may free its own item.
  // The scheduler retains the device reference until that invocation returns.
  WorkItems.erase(Item);
  FreedRanges.emplace(Address, profile::WorkItemTokenSize);
  return llvm::Error::success();
}

llvm::Error KernelModel::updateDeviceReferences(uint64_t Device) {
  if (!Devices.count(Device))
    return schedulingError("cannot update references of an unknown device");
  const uint64_t OpenCount =
      std::count_if(Files.begin(), Files.end(), [&](const auto &Entry) {
        return Entry.second.Device == Device &&
               Entry.second.State == FileState::Open;
      });
  // The public DEVICE_OBJECT field counts open handles. Scheduler ownership
  // and retained IRP routes protect lifetime without becoming open handles.
  return Memory.writeInteger(Device + windows::DeviceReferenceCount, OpenCount,
                             4);
}

llvm::Expected<std::optional<KernelScheduler::Invocation>>
KernelModel::nextScheduled(bool AdvanceTime, std::optional<uint64_t> Deadline) {
  if (Scheduler.active())
    return schedulingError("cannot dispatch with an unfinished callback");
  auto ProcessBoundary = [&](uint64_t Time) -> llvm::Error {
    auto Additional = preflightScheduledBoundary(Time);
    if (!Additional)
      return Additional.takeError();
    if (auto E = Scheduler.canAdvanceTo100ns(Time, *Additional))
      return E;
    if (auto E = Scheduler.advanceTo100ns(Time))
      return E;
    // Hardware publication precedes interrupt eligibility. An actual bus
    // transition may make a due pulse ineligible; preserve that ordered fact.
    if (auto E = processProviderCompletions())
      return E;
    if (auto E = processRequestCancellations())
      return E;
    if (auto E = DMA.processEvents(Scheduler.now100ns()))
      return E;
    return processInterruptEvents();
  };
  if (auto E = ProcessBoundary(Scheduler.now100ns()))
    return E;
  // Admission and time advancement never dequeue. In particular, an ISR due
  // with a timer must be present before selecting that timer's lower-IRQL DPC.
  auto Next = Scheduler.next(false, Deadline);
  if (!Next)
    return Next.takeError();
  if (!*Next && AdvanceTime) {
    auto Boundary = nextEventTime();
    if (Deadline && (!Boundary || *Deadline < *Boundary))
      Boundary = Deadline;
    if (Boundary && *Boundary > Scheduler.now100ns()) {
      if (auto E = ProcessBoundary(*Boundary))
        return E;
      Next = Scheduler.next(false, Deadline);
      if (!Next)
        return Next.takeError();
    }
  }
  if (*Next) {
    if ((**Next).Kind == KernelScheduler::CallbackKind::FrameworkCancel) {
      auto Token = ScheduledModelContinuations.find((**Next).ID);
      if (!Framework || Token == ScheduledModelContinuations.end() ||
          Token->second.Owner != GuestCallOwner::Framework)
        return schedulingError("cancel callback lost its framework identity");
      if (auto E = Framework->beginCancelCallback(Token->second.ID))
        return E;
    }
    if ((**Next).Kind == KernelScheduler::CallbackKind::Interrupt) {
      auto Token = ScheduledModelContinuations.find((**Next).ID);
      if (Token == ScheduledModelContinuations.end() ||
          Token->second.Owner != GuestCallOwner::Interrupt)
        return schedulingError("interrupt lost its model continuation");
      if (auto E = beginGuestCall(Token->second))
        return E;
      if (CurrentIRQL != (**Next).IRQL)
        return schedulingError("interrupt entry disagrees with its IRQL");
    } else {
      CurrentIRQL = (**Next).IRQL;
      if (KernelScheduler::isDMACallbackKind((**Next).Kind)) {
        auto Token = ScheduledModelContinuations.find((**Next).ID);
        if (Token == ScheduledModelContinuations.end() ||
            Token->second.Owner != GuestCallOwner::DMA)
          return schedulingError("DMA callback lost its model continuation");
        if (auto E = beginGuestCall(Token->second))
          return E;
      }
    }
  }
  return std::move(*Next);
}

llvm::Error KernelModel::finishScheduled(uint64_t ID) {
  if (!Scheduler.active() || Scheduler.active()->ID != ID)
    return schedulingError("callback completion does not match active task");
  if (ScheduledModelContinuations.count(ID))
    return schedulingError(
        "scheduled callback still owns a model continuation");
  const auto Invocation = *Scheduler.active();
  if (auto E = Scheduler.finish(ID))
    return E;
  CurrentIRQL = scheduler::PassiveLevel;
  if (Invocation.Kind == KernelScheduler::CallbackKind::WorkItem) {
    auto Reference = WorkReferences.find(Invocation.Owner);
    if (Reference == WorkReferences.end() || !Reference->second)
      return schedulingError("work item lost its device reference");
    --Reference->second;
    if (auto E = updateDeviceReferences(Invocation.Owner))
      return E;
    return retireDeviceIfUnreferenced(Invocation.Owner);
  }
  if (Invocation.Kind == KernelScheduler::CallbackKind::FrameworkCancel ||
      Invocation.Kind == KernelScheduler::CallbackKind::WDMCompletion ||
      Invocation.Kind == KernelScheduler::CallbackKind::Interrupt ||
      KernelScheduler::isDMACallbackKind(Invocation.Kind))
    return retireDeviceIfUnreferenced(Invocation.Owner);
  return llvm::Error::success();
}

llvm::Error KernelModel::suspendScheduled(uint64_t ID) {
  if (auto E = Scheduler.suspend(ID))
    return E;
  CurrentIRQL = scheduler::PassiveLevel;
  return llvm::Error::success();
}

llvm::Error KernelModel::resumeScheduled(uint64_t ID) {
  if (auto E = Scheduler.resume(ID))
    return E;
  CurrentIRQL = Scheduler.active()->IRQL;
  return llvm::Error::success();
}

std::optional<uint64_t> KernelModel::nextEventTime() const {
  auto Deadline = Scheduler.nextEventTime100ns();
  for (const auto &[IRP, Request] : Requests)
    if (!Request.Completed && Request.CancelDeadline &&
        (!Deadline || *Request.CancelDeadline < *Deadline))
      Deadline = std::max(*Request.CancelDeadline, Scheduler.now100ns());
  for (const auto &[IRP, Completion] : ProviderCompletions)
    if (!Deadline || Completion.Deadline < *Deadline)
      Deadline = std::max(Completion.Deadline, Scheduler.now100ns());
  auto Interrupt = nextInterruptEventTime();
  if (Interrupt && (!Deadline || *Interrupt < *Deadline))
    Deadline = std::max(*Interrupt, Scheduler.now100ns());
  auto Transfer = DMA.nextEventTime();
  if (Transfer && (!Deadline || *Transfer < *Deadline))
    Deadline = std::max(*Transfer, Scheduler.now100ns());
  return Deadline;
}

std::optional<KernelModel::Wait> KernelModel::takeWait() {
  return std::exchange(PendingWait, std::nullopt);
}

llvm::Expected<uint64_t> KernelModel::beginWait(llvm::ArrayRef<uint64_t> A,
                                                bool Delay) {
  const uint8_t Mode = A[Delay ? 0 : 2];
  const uint8_t Alertable = A[Delay ? 1 : 3];
  if (Mode != windows::KernelMode || Alertable)
    return schedulingError(
        "wait requires KernelMode and nonalertable execution");
  if (!Delay && A[1] != windows::ExecutiveWaitReason)
    return schedulingError("only Executive wait reason is currently modeled");
  const uint64_t TimeoutAddress = A[Delay ? 2 : 4];
  if (Delay && !TimeoutAddress)
    return schedulingError("KeDelayExecutionThread requires an interval");
  Wait Pending;
  Pending.Type = Delay ? Wait::Kind::Delay : Wait::Kind::Dispatcher;
  Pending.Object = Delay ? 0 : A[0];
  bool PollOnly = false;
  if (TimeoutAddress) {
    if (auto E = validateGuestAccess(TimeoutAddress, sizeof(int64_t), false))
      return E;
    auto Raw = Memory.readInteger(TimeoutAddress, sizeof(int64_t));
    if (!Raw)
      return Raw.takeError();
    auto Deadline = Scheduler.computeDeadline(std::bit_cast<int64_t>(*Raw));
    if (!Deadline)
      return Deadline.takeError();
    Pending.Deadline = *Deadline;
    PollOnly = !*Raw;
  }
  if (CurrentIRQL >
      ((!Delay && PollOnly) ? scheduler::DispatchLevel : windows::APCLevel))
    return schedulingError("blocking wait requires IRQL <= APC_LEVEL");
  if (!Delay) {
    auto Acquired = Dispatcher.tryAcquire(Pending.Object);
    if (!Acquired)
      return Acquired.takeError();
    if (*Acquired)
      return windows::StatusSuccess;
  }
  if (Pending.Deadline && *Pending.Deadline <= Scheduler.now100ns())
    return Delay ? windows::StatusSuccess : windows::StatusTimeout;
  if (PendingWait)
    return schedulingError("previous deferred wait was not consumed");
  if (Pending.Object)
    ++WaitReferences[Pending.Object];
  PendingWait = Pending;
  // The session does not expose this placeholder as a guest return or event
  // result. It saves the complete call frame until pollWait returns a status.
  return 0;
}

llvm::Expected<std::optional<uint32_t>>
KernelModel::pollWait(const Wait &Pending) {
  if (Pending.Type == Wait::Kind::RemoveLock) {
    auto Drained = RemoveLocks.drained(Pending.Object);
    if (!Drained)
      return Drained.takeError();
    if (!*Drained)
      return std::optional<uint32_t>{};
    auto Reference = RemoveLockWaitReferences.find(Pending.Object);
    if (Reference == RemoveLockWaitReferences.end() || !Reference->second)
      return schedulingError("remove-lock wait lost its storage reference");
    if (!--Reference->second)
      RemoveLockWaitReferences.erase(Reference);
    return std::optional<uint32_t>{windows::StatusSuccess};
  }
  bool Signaled = false;
  if (Pending.Object) {
    auto Acquired = Dispatcher.tryAcquire(Pending.Object);
    if (!Acquired)
      return Acquired.takeError();
    Signaled = *Acquired;
  }
  const bool Expired =
      Pending.Deadline && *Pending.Deadline <= Scheduler.now100ns();
  if (!Signaled && !Expired)
    return std::optional<uint32_t>{};
  if (Pending.Object) {
    auto Reference = WaitReferences.find(Pending.Object);
    if (Reference == WaitReferences.end() || !Reference->second)
      return schedulingError("wait lost its dispatcher object reference");
    if (!--Reference->second)
      WaitReferences.erase(Reference);
  }
  return std::optional<uint32_t>{Signaled || !Pending.Object
                                     ? windows::StatusSuccess
                                     : windows::StatusTimeout};
}

llvm::Error KernelModel::prepareReleaseRange(uint64_t Base, uint64_t Size,
                                             uint64_t IgnoredDMAPin) {
  return prepareReleaseRanges({{Base, Size}}, IgnoredDMAPin);
}

llvm::Error KernelModel::canReleaseRange(uint64_t Base, uint64_t Size,
                                         uint64_t IgnoredDMAPin) const {
  if (Size > UINT64_MAX - Base)
    return schedulingError("overflowing object storage range");
  if (Size)
    if (auto E = Physical.canReleaseRange(Base, Size, IgnoredDMAPin))
      return E;
  return canRevokeVirtualRange(Base, Size);
}

llvm::Error KernelModel::canRevokeVirtualRange(uint64_t Base,
                                               uint64_t Size) const {
  if (Size > UINT64_MAX - Base)
    return schedulingError("overflowing virtual storage range");
  if (auto E = DMA.canReleaseRange(Base, Size))
    return E;
  if (auto E = Interrupts.canReleaseRange(Base, Size))
    return E;
  for (const auto &[Object, References] : WaitReferences)
    if (References && Object >= Base && Object < Base + Size)
      return schedulingError("cannot release storage with outstanding waits");
  if (auto E = canReleaseRemoveLockStorage(Base, Size))
    return E;
  return Dispatcher.canReleaseRange(Base, Size);
}

llvm::Error KernelModel::prepareRevokeVirtualRange(uint64_t Base,
                                                   uint64_t Size) {
  if (auto E = canRevokeVirtualRange(Base, Size))
    return E;
  if (auto E = Dispatcher.prepareReleaseRange(Base, Size))
    return E;
  return RemoveLocks.forgetRange(Base, Size);
}

llvm::Error KernelModel::prepareReleaseRanges(
    llvm::ArrayRef<std::pair<uint64_t, uint64_t>> Ranges,
    uint64_t IgnoredDMAPin) {
  for (const auto &[Base, Size] : Ranges)
    if (auto E = canReleaseRange(Base, Size, IgnoredDMAPin))
      return E;
  for (const auto &[Base, Size] : Ranges) {
    if (auto E = Dispatcher.prepareReleaseRange(Base, Size))
      return E;
    if (auto E = RemoveLocks.forgetRange(Base, Size))
      return E;
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::validateDispatcherStorage(uint64_t Address,
                                                   uint32_t Size,
                                                   bool IsWrite) const {
  if (auto E = validateGuestAccessImpl(Address, Size, IsWrite, false))
    return E;
  for (const auto &[Base, Allocation] : Allocations)
    if (Address < Base + Allocation.Size && Base < Address + Size &&
        !Allocation.NonPaged)
      return schedulingError("dispatcher objects require nonpaged storage");
  return llvm::Error::success();
}

llvm::Error KernelModel::activateStack(uint64_t Base, uint64_t Size) {
  auto Old = FreedRanges.find(Base);
  if (Old != FreedRanges.end()) {
    if (Old->second != Size)
      return schedulingError(
          "reused thread stack changed its allocation extent");
    FreedRanges.erase(Old);
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::retireStack(uint64_t Base, uint64_t Size) {
  if (auto E = prepareReleaseRange(Base, Size))
    return E;
  FreedRanges.emplace(Base, Size);
  return llvm::Error::success();
}
} // namespace neverd::emulation
