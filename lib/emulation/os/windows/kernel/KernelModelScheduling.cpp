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

#include "KernelAPINames.h"
#include "KernelModel.h"
#include "KernelWaits.h"
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

llvm::Expected<uint64_t> KernelModel::currentThreadObject() {
  if (!CurrentExecution || !CurrentThreadKey)
    return schedulingError("current thread requires an active execution");
  for (const auto &[Object, Thread] : SystemThreads)
    if (Thread.CallbackID == CurrentThreadKey) {
      if (Thread.Exited)
        return schedulingError("current thread has already exited");
      return Object;
    }
  if (const auto Found = CurrentThreadObjects.find(CurrentThreadKey);
      Found != CurrentThreadObjects.end())
    return Found->second;
  auto Object = allocate(profile::ProcessTokenSize);
  if (!Object)
    return Object.takeError();
  CurrentThreadObjects.emplace(CurrentThreadKey, *Object);
  return *Object;
}

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
  if (!A[0] || !A[5])
    return windows::StatusInvalidParameter;
  if (A[3] || A[4])
    return schedulingError("non-system process handles and client IDs are "
                           "unsupported for system threads");
  if (uint32_t(A[1]) & ~windows::ThreadAllAccess)
    return schedulingError("unsupported system-thread access mask");
  uint32_t Attributes = 0;
  if (A[2]) {
    if (auto E =
            validateGuestAccess(A[2], windows::ObjectAttributesSize, false))
      return E;
    auto Size = Memory.readInteger(A[2], 4);
    if (!Size)
      return Size.takeError();
    auto Root = Memory.readInteger(A[2] + windows::ObjectRootOffset, 8);
    if (!Root)
      return Root.takeError();
    auto Name = Memory.readInteger(A[2] + windows::ObjectNameOffset, 8);
    if (!Name)
      return Name.takeError();
    auto Flags = Memory.readInteger(A[2] + windows::ObjectFlagsOffset, 4);
    if (!Flags)
      return Flags.takeError();
    auto Security = Memory.readInteger(A[2] + windows::ObjectSecurityOffset, 8);
    if (!Security)
      return Security.takeError();
    auto Quality = Memory.readInteger(A[2] + windows::ObjectQualityOffset, 8);
    if (!Quality)
      return Quality.takeError();
    if (*Size != windows::ObjectAttributesSize)
      return windows::StatusInvalidParameter;
    if (*Root || *Name || *Security || *Quality ||
        (*Flags & ~windows::ObjectKernelHandle))
      return schedulingError("unsupported system-thread object attributes");
    Attributes = uint32_t(*Flags);
  }
  const bool CallerInUserProcess =
      (CurrentExecution == profile::StackBase && UserRequestContext) ||
      (hasProcessAttachment(CurrentExecution));
  if (CallerInUserProcess && !(Attributes & windows::ObjectKernelHandle))
    return schedulingError("system-thread creation outside the system process "
                           "requires OBJ_KERNEL_HANDLE");
  if (auto E = validateGuestAccess(A[0], 8, true))
    return E;
  if (SystemThreads.size() >= profile::MaxConcurrentCallbacks ||
      NextThreadHandle > profile::SystemThreadHandleLimit)
    return windows::StatusInsufficientResources;
  const uint64_t Aligned = (NextAllocation + windows::PoolAlignment - 1) &
                           ~uint64_t(windows::PoolAlignment - 1);
  if (Aligned > AllocationEnd ||
      profile::ProcessTokenSize > AllocationEnd - Aligned)
    return windows::StatusInsufficientResources;
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
  NextThreadHandle += profile::SystemThreadHandleStride;
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
  auto Handle = ThreadHandles.find(A[0]);
  if (Handle == ThreadHandles.end()) {
    if (Registry.ownsHandle(A[0]))
      return schedulingError("registry-key object references are unsupported");
    return windows::StatusInvalidHandle;
  }
  auto Thread = SystemThreads.find(Handle->second);
  if (Thread == SystemThreads.end())
    return schedulingError("thread handle lost its object");
  // Only the modeled thread object type and kernel caller mode are available.
  if (!A[4])
    return windows::StatusInvalidParameter;
  if (A[2] || uint8_t(A[3]) != windows::KernelMode || A[5])
    return schedulingError(
        "unsupported object type, access mode or handle-information output");
  if (uint32_t(A[1]) & ~windows::ThreadAllAccess)
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
      return Registry.call(*this, kernel_api::ZwClose, {Handle});
    return windows::StatusInvalidHandle;
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
    Scheduler.forgetThreadPriority(Thread->second.CallbackID);
    SystemThreads.erase(Thread);
  }
}

void KernelModel::retireBorrowedThread(uint64_t Key) {
  Scheduler.forgetThreadPriority(Key);
  ApcStates.erase(Key);
  SystemAffinityThreads.erase(Key);
  if (const auto Object = CurrentThreadObjects.find(Key);
      Object != CurrentThreadObjects.end()) {
    FreedRanges.emplace(Object->second, profile::ProcessTokenSize);
    CurrentThreadObjects.erase(Object);
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
KernelModel::dispatchScheduledPowerProvider(
    const KernelScheduler::Invocation &Call) {
  auto Token = ScheduledModelContinuations.find(Call.ID);
  if (Token == ScheduledModelContinuations.end() ||
      Token->second.Owner != GuestCallOwner::WDM)
    return schedulingError("provider power dispatch lost its continuation");
  auto Dispatch = IRPCalls.find(Token->second.ID);
  const auto *Request = requestForIRP(Call.Object);
  if (Call.Kind != KernelScheduler::CallbackKind::WDMProviderDispatch ||
      Call.PC || Call.IRQL != scheduler::PassiveLevel ||
      Call.Arguments.size() != 2 || Call.Arguments[0] != Call.Owner ||
      Call.Arguments[1] != Call.Object || !Request || !Request->ChildPower ||
      Request->DispatchReturned || Request->Device != Call.Owner ||
      Request->PnpDevice != Call.Owner || Dispatch == IRPCalls.end() ||
      Dispatch->second.IRP != Call.Object ||
      Dispatch->second.Kind != IRPCallKind::PowerDispatch ||
      !Dispatch->second.AwaitingCallback)
    return schedulingError("provider power dispatch lost its captured request");
  CurrentIRQL = scheduler::PassiveLevel;
  auto Status = callProviderDriver(Call.Owner, Call.Object);
  if (!Status)
    return Status.takeError();
  if (auto Receipt = ProviderReceipts.find(Call.Object);
      Receipt != ProviderReceipts.end()) {
    if (!PendingWdmCall || Receipt->second.ScheduledDispatchToken)
      return schedulingError("provider receipt lost its suspended return");
    Receipt->second.ScheduledDispatchToken = Token->second.ID;
  } else {
    auto Returned = finishWdmGuestCall(Token->second.ID, *Status);
    if (!Returned)
      return Returned.takeError();
    if (!*Returned)
      return schedulingError(
          "provider power dispatch did not record its return");
  }
  if (auto Completion = takeWdmGuestCall()) {
    auto Next = Scheduler.beginWDMProviderCompletion(
        Call.ID, {Call.Object, Call.Owner, Call.Thread, Completion->PC,
                  std::move(Completion->Arguments)});
    if (!Next)
      return Next.takeError();
    Token->second = Completion->Token;
    return std::optional<KernelScheduler::Invocation>{std::move(*Next)};
  }
  ScheduledModelContinuations.erase(Token);
  if (auto E = finishScheduled(Call.ID))
    return E;
  return std::optional<KernelScheduler::Invocation>{};
}

KernelModel::ExecutionContext KernelModel::captureExecutionContext() const {
  return {CurrentExecution,
          CurrentThreadKey,
          CurrentGuestCall,
          CurrentUserProcessID,
          CurrentIRQL,
          UserRequestContext,
          FrameworkPowerManagedCallback};
}

llvm::Error
KernelModel::restoreExecutionContext(const ExecutionContext &Context) {
  const auto It = ExecutionThreadKeys.find(Context.Execution);
  if (It == ExecutionThreadKeys.end() || It->second != Context.Thread)
    return schedulingError(driver_scheduling::InvalidContext);
  if (auto E = setUserRequestContext(Context.UserMemory, Context.Process))
    return E;
  CurrentExecution = Context.Execution;
  CurrentThreadKey = Context.Thread;
  CurrentGuestCall = Context.Call;
  CurrentIRQL = Context.IRQL;
  FrameworkPowerManagedCallback = Context.PowerManaged;
  return llvm::Error::success();
}

llvm::Error KernelModel::preemptScheduled(uint64_t ID) {
  if (CurrentIRQL >= scheduler::DispatchLevel)
    return schedulingError(driver_scheduling::MaskedPreemption);
  if (PendingWait || PendingThreadTermination ||
      hasPendingIndependentGuestCall())
    return schedulingError(driver_scheduling::UnsafeBoundary);
  if (ID)
    if (auto E = Scheduler.suspend(ID))
      return E;
  CurrentIRQL = scheduler::PassiveLevel;
  return llvm::Error::success();
}

llvm::Error KernelModel::processScheduledBoundary(uint64_t Time,
                                                  bool Executing) {
  auto Additional = preflightScheduledBoundary(Time);
  if (!Additional)
    return Additional.takeError();
  if (auto E = Executing
                   ? Scheduler.canAdvanceExecutionTo100ns(Time, *Additional)
                   : Scheduler.canAdvanceTo100ns(Time, *Additional))
    return E;
  if (auto E = Executing ? Scheduler.advanceExecutionTo100ns(Time)
                         : Scheduler.advanceTo100ns(Time))
    return E;
  // Publish hardware effects before making their interrupts eligible.
  if (auto E = processProviderCompletions())
    return E;
  if (auto E = processRequestCancellations())
    return E;
  if (auto E = DMA.processEvents(Scheduler.now100ns()))
    return E;
  if (auto E = processInterruptEvents())
    return E;
  // Pageable policy work is eligible on a passive service boundary. A due
  // request remains pending while the running thread masks that service.
  if (Executing && !canServicePassiveEvents())
    return llvm::Error::success();
  if (auto E = processPowerPolicyEvents())
    return E;
  return processPoFxCallbacks();
}

llvm::Error KernelModel::advanceExecutionTo100ns(uint64_t Time) {
  return processScheduledBoundary(Time, true);
}

llvm::Expected<std::optional<KernelScheduler::Invocation>>
KernelModel::nextScheduled(bool AdvanceTime, std::optional<uint64_t> Deadline) {
  if (Scheduler.active())
    return schedulingError("cannot dispatch with an unfinished callback");
  if (auto E = processScheduledBoundary(Scheduler.now100ns(), false))
    return E;
  // Admission and time advancement never dequeue. In particular, an ISR due
  // with a timer must be present before selecting that timer's lower-IRQL DPC.
  auto SelectReady =
      [&]() -> llvm::Expected<std::optional<KernelScheduler::Invocation>> {
    // Each model-only dispatch consumes the same bounded scheduler budget as
    // guest dispatch. Only actual guest callbacks reach the execution engine.
    for (;;) {
      auto Next = Scheduler.next(false, Deadline);
      if (!Next)
        return Next.takeError();
      if (!*Next)
        return std::move(*Next);
      const auto Kind = (**Next).Kind;
      if (Kind != KernelScheduler::CallbackKind::WDMProviderDispatch &&
          Kind != KernelScheduler::CallbackKind::FrameworkUsbIdle)
        return std::move(*Next);
      auto Completion = Kind == KernelScheduler::CallbackKind::FrameworkUsbIdle
                            ? dispatchScheduledFrameworkUsbIdle(**Next)
                            : dispatchScheduledPowerProvider(**Next);
      if (!Completion)
        return Completion.takeError();
      if (*Completion)
        return std::move(*Completion);
    }
  };
  auto Next = SelectReady();
  if (!Next)
    return Next.takeError();
  if (!*Next && AdvanceTime) {
    auto Boundary = nextEventTime();
    if (Deadline && (!Boundary || *Deadline < *Boundary))
      Boundary = Deadline;
    if (Boundary && *Boundary > Scheduler.now100ns()) {
      if (auto E = processScheduledBoundary(*Boundary, false))
        return E;
      Next = SelectReady();
      if (!Next)
        return Next.takeError();
    }
  }
  if (*Next) {
    if ((**Next).Kind == KernelScheduler::CallbackKind::FrameworkCompletion) {
      auto Token = ScheduledModelContinuations.find((**Next).ID);
      if (!Framework || Token == ScheduledModelContinuations.end() ||
          Token->second.Owner != GuestCallOwner::Framework)
        return schedulingError(
            "request completion lost its framework identity");
      if (!Framework->isAutomaticFileContinuation(Token->second.ID))
        if (auto E =
                Framework->beginRequestCompletionCallback(Token->second.ID))
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
        CancelLock.CallbackIRQL = scheduler::PassiveLevel;
      }
      if ((**Next).Kind == KernelScheduler::CallbackKind::PoFx) {
        auto Token = ScheduledModelContinuations.find((**Next).ID);
        if (Token == ScheduledModelContinuations.end() ||
            Token->second.Owner != GuestCallOwner::PoFx)
          return schedulingError("PoFx callback lost its model continuation");
        if (auto E = beginGuestCall(Token->second))
          return E;
      }
      if ((**Next).Kind == KernelScheduler::CallbackKind::UsbIdle) {
        auto Token = ScheduledModelContinuations.find((**Next).ID);
        if (Token == ScheduledModelContinuations.end() ||
            Token->second.Owner != GuestCallOwner::UsbIdle)
          return schedulingError("USB idle callback lost its continuation");
        if (auto E = beginUsbIdleCallback(Token->second.ID))
          return E;
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
        CurrentIRQL != CancelLock.CallbackIRQL)
      return schedulingError(
          "WDM cancel callback did not release its cancel spin lock");
    CancelLock = {};
  }
  if (auto E = Scheduler.finish(ID))
    return E;
  CurrentIRQL = scheduler::PassiveLevel;
  if (Framework) {
    if (auto E = Framework->resumeInterruptDrain())
      return E;
    if (auto E = completeFrameworkTransitionIfReady())
      return E;
  }
  ApcStates.erase(ID);
  SystemAffinityThreads.erase(ID);
  if (Invocation.Kind == KernelScheduler::CallbackKind::SystemThread) {
    SystemThreads.at(Invocation.Object).Exited = true;
    SystemThreads.at(Invocation.Object).Terminating = false;
    retireThreadIfUnreferenced(Invocation.Object);
    return llvm::Error::success();
  }
  retireBorrowedThread(ID);
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
      Invocation.Kind == KernelScheduler::CallbackKind::FrameworkCompletion ||
      Invocation.Kind == KernelScheduler::CallbackKind::FrameworkDeferred ||
      Invocation.Kind == KernelScheduler::CallbackKind::PoFx ||
      KernelScheduler::isFrameworkInterruptCallbackKind(Invocation.Kind) ||
      Invocation.Kind == KernelScheduler::CallbackKind::WDMCancel ||
      Invocation.Kind == KernelScheduler::CallbackKind::WDMCompletion ||
      Invocation.Kind == KernelScheduler::CallbackKind::WDMDispatch ||
      Invocation.Kind == KernelScheduler::CallbackKind::WDMProviderDispatch ||
      Invocation.Kind == KernelScheduler::CallbackKind::FrameworkUsbIdle ||
      Invocation.Kind == KernelScheduler::CallbackKind::UsbIdle ||
      Invocation.Kind == KernelScheduler::CallbackKind::Interrupt ||
      KernelScheduler::isDMACallbackKind(Invocation.Kind))
    return retireDeviceIfUnreferenced(Invocation.Owner);
  return llvm::Error::success();
}

llvm::Error KernelModel::suspendScheduled(uint64_t ID) {
  if (CurrentIRQL > windows::APCLevel)
    return schedulingError("cannot suspend above APC_LEVEL");
  for (const RaisedIRQL &Raise : RaisedIRQLs)
    if (Raise.Execution == CurrentExecution &&
        Raise.NewIRQL > windows::APCLevel)
      return schedulingError("cannot suspend with a raised dispatch IRQL");
  for (const auto &[Address, Lock] : ExecutiveSpinLocks)
    if (Lock.Execution == CurrentExecution)
      return schedulingError("cannot suspend with an executive spin lock");
  if (hasProcessAttachment(CurrentExecution) && Scheduler.active() &&
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

llvm::Error KernelModel::restoreWaitIRQL(uint8_t IRQL) {
  if (IRQL > windows::APCLevel)
    return schedulingError("blocked wait cannot resume above APC_LEVEL");
  CurrentIRQL = IRQL;
  return llvm::Error::success();
}

std::optional<uint64_t> KernelModel::nextEventTime() const {
  auto Deadline = Scheduler.nextEventTime100ns();
  for (const auto &[IRP, Request] : Requests)
    if (canDeliverCancellation(Request) &&
        (!Deadline || *Request.CancelDeadline < *Deadline))
      Deadline = std::max(*Request.CancelDeadline, Scheduler.now100ns());
  for (const auto &[IRP, Completion] : ProviderCompletions)
    if (canServicePassiveEvents() &&
        (!Deadline || Completion.Deadline < *Deadline))
      Deadline = std::max(Completion.Deadline, Scheduler.now100ns());
  auto Interrupt = nextInterruptEventTime();
  if (Interrupt && (!Deadline || *Interrupt < *Deadline))
    Deadline = std::max(*Interrupt, Scheduler.now100ns());
  auto Policy = nextPowerPolicyEventTime();
  if (Policy && (!Deadline || *Policy < *Deadline))
    Deadline = std::max(*Policy, Scheduler.now100ns());
  auto ComponentPower =
      !canServicePassiveEvents() ? std::nullopt : PoFx.nextDeadline();
  if (ComponentPower && (!Deadline || *ComponentPower < *Deadline))
    Deadline = std::max(*ComponentPower, Scheduler.now100ns());
  auto Transfer = DMA.nextEventTime();
  if (Transfer && (!Deadline || *Transfer < *Deadline))
    Deadline = std::max(*Transfer, Scheduler.now100ns());
  return Deadline;
}

llvm::Error KernelModel::prepareReleaseRange(uint64_t Base, uint64_t Size,
                                             uint64_t IgnoredDMAPin) {
  return prepareReleaseRanges({{Base, Size}}, IgnoredDMAPin);
}

llvm::Error KernelModel::canReleaseRange(uint64_t Base, uint64_t Size,
                                         uint64_t IgnoredDMAPin) const {
  if (Size > UINT64_MAX - Base)
    return schedulingError("overflowing object storage range");
  if (auto E = canReleaseUserViewsForBacking(Base, Size))
    return E;
  if (Size)
    if (auto E = Physical.canReleaseRange(Base, Size, IgnoredDMAPin))
      return E;
  return canRevokeVirtualRange(Base, Size);
}

llvm::Error KernelModel::canRevokeVirtualRange(
    uint64_t Base, uint64_t Size,
    std::optional<UsbIdleKey> RetiringUsbIdle) const {
  if (Size > UINT64_MAX - Base)
    return schedulingError("overflowing virtual storage range");
  if (auto E = DMA.canReleaseRange(Base, Size))
    return E;
  if (auto E = Interrupts.canReleaseRange(Base, Size))
    return E;
  if (auto E = UsbIdle.canReleaseRange(Base, Size, RetiringUsbIdle))
    return E;
  for (const auto &[Address, Lock] : ExecutiveSpinLocks)
    if (Address < Base + Size && Base < Address + sizeof(uint64_t))
      return schedulingError("cannot release a held executive spin lock");
  for (const auto &[Object, References] : WaitReferences)
    if (References && Object >= Base && Object < Base + Size)
      return schedulingError("cannot release storage with outstanding waits");
  for (const auto &[Range, References] : WaitBlockReferences)
    if (References && Range.first < Base + Size &&
        Base < Range.first + Range.second)
      return schedulingError(kernel_wait::BufferInUse);
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
  if (Address < profile::UserProbeLimit) {
    auto Range = resolveUserMemoryRange(Address, Size, IsWrite);
    if (!Range)
      return Range.takeError();
  }
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
  // Detached callbacks use their private stack as the thread key. Retire that
  // lifetime before its stack slot is reused. A nested/SEH stack shares another
  // thread's key and must not retire its parent's priority or object.
  if (const auto Thread = ExecutionThreadKeys.find(Base);
      Thread != ExecutionThreadKeys.end() && Thread->second == Base &&
      Base != profile::StackBase) {
    retireBorrowedThread(Base);
  }
  ExecutionThreadKeys.erase(Base);
  InheritedExecutionContexts.erase(Base);
  FreedRanges.emplace(Base, Size);
  return llvm::Error::success();
}
} // namespace neverd::emulation
