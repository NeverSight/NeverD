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

llvm::Expected<uint64_t>
KernelModel::createSystemThread(llvm::ArrayRef<uint64_t> A) {
  constexpr uint32_t ThreadAllAccess = 0x001fffff;
  constexpr uint32_t StatusInvalidParameter = 0xc000000d;
  constexpr uint32_t StatusInsufficientResources = 0xc000009a;
  if (!A[0] || !A[5])
    return StatusInvalidParameter;
  if (A[3] || A[4])
    return schedulingError("non-system process handles and client IDs are "
                           "unsupported for system threads");
  if (uint32_t(A[1]) & ~ThreadAllAccess)
    return schedulingError("unsupported system-thread access mask");
  uint32_t Attributes = 0;
  if (A[2]) {
    if (auto E = validateGuestAccess(A[2], 48, false))
      return E;
    auto Size = Memory.readInteger(A[2], 4);
    if (!Size)
      return Size.takeError();
    auto Root = Memory.readInteger(A[2] + 8, 8);
    if (!Root)
      return Root.takeError();
    auto Name = Memory.readInteger(A[2] + 16, 8);
    if (!Name)
      return Name.takeError();
    auto Flags = Memory.readInteger(A[2] + 24, 4);
    if (!Flags)
      return Flags.takeError();
    auto Security = Memory.readInteger(A[2] + 32, 8);
    if (!Security)
      return Security.takeError();
    auto Quality = Memory.readInteger(A[2] + 40, 8);
    if (!Quality)
      return Quality.takeError();
    if (*Size != 48)
      return StatusInvalidParameter;
    if (*Root || *Name || *Security || *Quality || (*Flags & ~0x200u))
      return schedulingError("unsupported system-thread object attributes");
    Attributes = uint32_t(*Flags);
  }
  const bool CallerInUserProcess =
      (CurrentExecution == profile::StackBase && UserRequestContext) ||
      (!ProcessAttachments.empty() &&
       ProcessAttachments.back().Execution == CurrentExecution);
  if (CallerInUserProcess && !(Attributes & 0x200u))
    return schedulingError("system-thread creation outside the system process "
                           "requires OBJ_KERNEL_HANDLE");
  if (auto E = validateGuestAccess(A[0], 8, true))
    return E;
  if (SystemThreads.size() >= profile::MaxConcurrentCallbacks ||
      NextThreadHandle > 0x6ffffffc)
    return StatusInsufficientResources;
  const uint64_t Aligned = (NextAllocation + 15) & ~uint64_t(15);
  if (Aligned > AllocationEnd ||
      profile::ProcessTokenSize > AllocationEnd - Aligned)
    return StatusInsufficientResources;
  KernelScheduler::Callback Callback;
  Callback.Object = 1;
  Callback.Owner = DriverObject;
  Callback.Thread = 1;
  Callback.PC = A[5];
  Callback.Arguments = {A[6]};
  if (auto E = Scheduler.canEnqueueSystemThread(Callback))
    return E;
  // The arena and scheduler are preflighted, so a faulting output cannot
  // reserve a thread object or callback identity.
  if (auto E = Memory.writeInteger(A[0], NextThreadHandle, 8))
    return E;
  auto Object = allocate(profile::ProcessTokenSize);
  if (!Object)
    return Object.takeError();
  Callback.Object = *Object;
  Callback.Thread = *Object;
  auto ID = Scheduler.enqueueSystemThread(std::move(Callback));
  if (!ID)
    return ID.takeError();
  ThreadHandles.emplace(NextThreadHandle, *Object);
  SystemThreads.emplace(*Object, SystemThread{NextThreadHandle, *ID});
  NextThreadHandle += 4;
  return uint64_t(0);
}

llvm::Expected<uint64_t> KernelModel::terminateSystemThread(uint32_t Status) {
  const auto &Active = Scheduler.active();
  if (!Active || Active->Kind != KernelScheduler::CallbackKind::SystemThread ||
      CurrentExecution == profile::StackBase || PendingThreadTermination)
    return schedulingError(
        "PsTerminateSystemThread requires the running system thread");
  auto Thread = SystemThreads.find(Active->Object);
  if (Thread == SystemThreads.end() ||
      Thread->second.CallbackID != Active->ID || Thread->second.Exited ||
      Thread->second.Terminating)
    return schedulingError("system thread termination lost its live object");
  Thread->second.ExitStatus = Status;
  Thread->second.Terminating = true;
  PendingThreadTermination = Status;
  return uint64_t(0);
}

std::optional<uint32_t> KernelModel::takeThreadTermination() {
  return std::exchange(PendingThreadTermination, std::nullopt);
}

llvm::Expected<uint64_t>
KernelModel::referenceThreadByHandle(llvm::ArrayRef<uint64_t> A) {
  constexpr uint32_t StatusInvalidHandle = 0xc0000008;
  constexpr uint32_t StatusInvalidParameter = 0xc000000d;
  auto Handle = ThreadHandles.find(A[0]);
  if (Handle == ThreadHandles.end()) {
    if (Registry.ownsHandle(A[0]))
      return schedulingError("registry-key object references are unsupported");
    return StatusInvalidHandle;
  }
  auto Thread = SystemThreads.find(Handle->second);
  if (Thread == SystemThreads.end())
    return schedulingError("thread handle lost its object");
  // Only the modeled thread object type and kernel caller mode are available.
  if (!A[4])
    return StatusInvalidParameter;
  if (A[2] || A[3] != windows::KernelMode || A[5])
    return schedulingError(
        "unsupported object type, access mode or handle-information output");
  if (uint32_t(A[1]) & ~0x001fffffu)
    return schedulingError("unsupported system-thread reference access mask");
  if (auto E = validateGuestAccess(A[4], 8, true))
    return E;
  if (auto E = Memory.writeInteger(A[4], Handle->second, 8))
    return E;
  ++Thread->second.PointerReferences;
  return uint64_t(0);
}

llvm::Expected<uint64_t> KernelModel::dereferenceThread(uint64_t Object) {
  auto Thread = SystemThreads.find(Object);
  if (Thread == SystemThreads.end() || !Thread->second.PointerReferences)
    return schedulingError(
        "ObfDereferenceObject requires a referenced thread object");
  --Thread->second.PointerReferences;
  retireThreadIfUnreferenced(Object);
  // The macro's return value is reserved; driver code must treat it as void.
  return uint64_t(0);
}

llvm::Expected<uint64_t> KernelModel::closeHandle(uint64_t Handle) {
  auto It = ThreadHandles.find(Handle);
  if (It == ThreadHandles.end()) {
    if (Registry.ownsHandle(Handle))
      return Registry.call(*this, "ZwClose", {Handle});
    return uint64_t(0xc0000008);
  }
  const uint64_t Object = It->second;
  auto Thread = SystemThreads.find(Object);
  if (Thread == SystemThreads.end() || !Thread->second.HandleOpen)
    return schedulingError("thread handle lost its live object");
  Thread->second.HandleOpen = false;
  ThreadHandles.erase(It);
  retireThreadIfUnreferenced(Object);
  return uint64_t(0);
}

void KernelModel::retireThreadIfUnreferenced(uint64_t Object) {
  auto Thread = SystemThreads.find(Object);
  if (Thread != SystemThreads.end() && Thread->second.Exited &&
      !Thread->second.HandleOpen && !Thread->second.PointerReferences &&
      !WaitReferences.contains(Object)) {
    FreedRanges.emplace(Object, profile::ProcessTokenSize);
    SystemThreads.erase(Thread);
  }
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
      if (Framework->isCancelCallback(Token->second.ID))
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
      if ((**Next).Kind == KernelScheduler::CallbackKind::WDMCancel) {
        if (CancelLock.Held || CancelLock.Callback)
          return schedulingError(
              "WDM cancel callback encountered an owned cancel spin lock");
        CancelLock.Held = true;
        CancelLock.Callback = true;
        CancelLock.Owner = 0;
        CancelLock.IRP = (**Next).Object;
        CancelLock.OldIRQL = scheduler::PassiveLevel;
      }
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
  if (Invocation.Kind == KernelScheduler::CallbackKind::SystemThread) {
    auto Thread = SystemThreads.find(Invocation.Object);
    if (Thread == SystemThreads.end() || Thread->second.CallbackID != ID ||
        !Thread->second.Terminating)
      return schedulingError(
          "system thread returned without PsTerminateSystemThread");
  }
  if (Invocation.Kind == KernelScheduler::CallbackKind::WDMCancel) {
    if (!CancelLock.Callback || CancelLock.Held ||
        CancelLock.IRP != Invocation.Object ||
        CurrentIRQL != CancelLock.OldIRQL)
      return schedulingError(
          "WDM cancel callback did not release its cancel spin lock");
    CancelLock = {};
  }
  if (auto E = Scheduler.finish(ID))
    return E;
  CurrentIRQL = scheduler::PassiveLevel;
  ApcStates.erase(ID);
  if (Invocation.Kind == KernelScheduler::CallbackKind::SystemThread) {
    SystemThreads.at(Invocation.Object).Exited = true;
    SystemThreads.at(Invocation.Object).Terminating = false;
    retireThreadIfUnreferenced(Invocation.Object);
    return llvm::Error::success();
  }
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
      Invocation.Kind == KernelScheduler::CallbackKind::WDMCancel ||
      Invocation.Kind == KernelScheduler::CallbackKind::WDMCompletion ||
      Invocation.Kind == KernelScheduler::CallbackKind::Interrupt ||
      KernelScheduler::isDMACallbackKind(Invocation.Kind))
    return retireDeviceIfUnreferenced(Invocation.Owner);
  return llvm::Error::success();
}

llvm::Error KernelModel::suspendScheduled(uint64_t ID) {
  for (const RaisedIRQL &Raise : RaisedIRQLs)
    if (Raise.Execution == CurrentExecution)
      return schedulingError("cannot suspend with an unmatched IRQL raise");
  for (const auto &[Address, Lock] : ExecutiveSpinLocks)
    if (Lock.Execution == CurrentExecution)
      return schedulingError("cannot suspend with an executive spin lock");
  if (!ProcessAttachments.empty() && Scheduler.active() &&
      ProcessAttachments.back().Execution == CurrentExecution &&
      Scheduler.active()->ID == ID)
    return schedulingError("cannot suspend a process-attached work item");
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
  Pending.Type = Delay                          ? Wait::Kind::Delay
                 : SystemThreads.contains(A[0]) ? Wait::Kind::Thread
                                                : Wait::Kind::Dispatcher;
  Pending.Object = Delay ? 0 : A[0];
  Pending.Execution = CurrentExecution;
  Pending.IRQL = CurrentIRQL;
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
    if (Pending.Type == Wait::Kind::Thread) {
      const auto &Thread = SystemThreads.at(Pending.Object);
      if (!Thread.PointerReferences)
        return schedulingError(
            "thread wait requires a referenced object pointer");
      if (Scheduler.active() && Thread.CallbackID == Scheduler.active()->ID)
        return schedulingError("system thread cannot wait on itself");
      if (Thread.Exited)
        return windows::StatusSuccess;
    } else {
      auto Acquired = Dispatcher.tryAcquire(Pending.Object, Pending.Execution,
                                            Pending.IRQL);
      if (!Acquired)
        return Acquired.takeError();
      if (*Acquired)
        return windows::StatusSuccess;
    }
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
  if (Pending.Type == Wait::Kind::FrameworkQueueStop ||
      Pending.Type == Wait::Kind::FrameworkQueueEmpty) {
    if (!Framework)
      return schedulingError("framework queue wait lost its binding");
    auto Ready = Framework->queueWaitReady(
        Pending.Object, Pending.Type == Wait::Kind::FrameworkQueueEmpty);
    if (!Ready)
      return Ready.takeError();
    return *Ready ? std::optional<uint32_t>{windows::StatusSuccess}
                  : std::optional<uint32_t>{};
  }
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
    if (Pending.Type == Wait::Kind::Thread) {
      auto Thread = SystemThreads.find(Pending.Object);
      if (Thread == SystemThreads.end())
        return schedulingError("thread wait lost its object");
      Signaled = Thread->second.Exited;
    } else {
      auto Acquired = Dispatcher.tryAcquire(Pending.Object, Pending.Execution,
                                            Pending.IRQL);
      if (!Acquired)
        return Acquired.takeError();
      Signaled = *Acquired;
    }
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
    if (Pending.Type == Wait::Kind::Thread)
      retireThreadIfUnreferenced(Pending.Object);
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
  for (const auto &[Address, Lock] : ExecutiveSpinLocks)
    if (Address < Base + Size && Base < Address + sizeof(uint64_t))
      return schedulingError("cannot release a held executive spin lock");
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
  ExecutionThreadKeys.erase(Base);
  FreedRanges.emplace(Base, Size);
  return llvm::Error::success();
}
} // namespace neverd::emulation
