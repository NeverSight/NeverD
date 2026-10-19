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
#include "KernelModelRuntime.h"
#include "KernelObjectTypes.h"
#include "KernelReferenceImage.h"
#include "KernelReferenceThread.h"
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
  for (const auto &[Object, Thread] : ThreadObjects)
    if (Thread.System && Thread.CallbackID == CurrentThreadKey) {
      if (Thread.Exited)
        return schedulingError("current thread has already exited");
      return Object;
    }
  const auto Context = executionProcessContext();
  const uint32_t ProcessID = Context ? Context->CreatingProcessID : 0;
  if (const auto Found = CurrentThreadObjects.find(CurrentThreadKey);
      Found != CurrentThreadObjects.end()) {
    auto &Thread = ThreadObjects.at(Found->second);
    if (!Context || !Thread.ProcessID || Thread.ProcessID == ProcessID) {
      if (Context)
        Thread.ProcessID = ProcessID;
      return Found->second;
    }
    // Foreground requests can run in different process contexts. A retained
    // old thread must never acquire the next requestor's process identity.
    if (Version || ProcessNotificationState)
      CurrentThreadObjects.erase(CurrentThreadKey);
    else
      retireBorrowedThread(CurrentThreadKey);
  }
  if (Version || ProcessNotificationState)
    for (const auto &[Object, T] : ThreadObjects)
      if (!T.System && !T.Exited && T.CallbackID == CurrentThreadKey &&
          T.ProcessID == ProcessID) {
        CurrentThreadObjects[CurrentThreadKey] = Object;
        return Object;
      }
  const bool NativeHeader = Version && DebuggerReference &&
                            hasKernelThreadConstruction(*DebuggerReference);
  if (NativeHeader) {
    auto Header = allocate(0x30);
    if (!Header)
      return Header.takeError();
  }
  auto Object =
      allocate((Version || DebuggerReference) ? ThreadObjectSize
                                              : profile::ProcessTokenSize);
  if (!Object)
    return Object.takeError();
  if (NativeHeader)
    NativeInitialThreadViews.emplace(*Object,
                                     NativeInitialThreadView{0, 0, false});
  CurrentThreadObjects.emplace(CurrentThreadKey, *Object);
  ThreadObjects.emplace(*Object, ThreadObject{CurrentThreadKey, ProcessID});
  ReferencedObjects.emplace(*Object, ObjectReferences{ObjectKind::Thread});
  {
    auto ID = threadClientID(*Object);
    if (!ID)
      return ID.takeError();
    std::optional<uint64_t> Start;
    if (CurrentThreadKey == profile::StackBase &&
        ProcessID == windows::SystemProcessID)
      Start = ForegroundThreadStart;
    else if (const auto &Active = Scheduler.active();
             Active && Active->ID == CurrentThreadKey)
      Start = Active->PC;
    if (!Start && ProcessInventory)
      for (const auto &P : *ProcessInventory)
        if (P.ProcessID == ProcessID && P.BorrowedThreadStart)
          Start = P.BorrowedThreadStart;
    if (auto E = publishThreadIdentity(
            *Object, ThreadIdentity{*ID, Start,
                                    ProcessID ? std::optional(ProcessID)
                                              : std::nullopt}))
      return E;
  }
  return *Object;
}

llvm::Expected<uint64_t> KernelModel::threadClientID(uint64_t Object) {
  if (!ThreadObjects.contains(Object))
    return schedulingError("thread client ID requires a known thread object");
  if (const auto Found = ThreadClientIDs.find(Object);
      Found != ThreadClientIDs.end())
    return uint64_t(Found->second);
  // Thread and process IDs share one client-ID namespace.
  auto ProcessID = [&](uint64_t ID) {
    return ID == windows::SystemProcessID ||
           ID == DriverRequest::DefaultRequestorProcessID ||
           ProcessObjects.count(uint32_t(ID)) ||
           RequestorProcessIDs.count(uint32_t(ID)) ||
           (UserRequestContext && ID == CurrentUserProcessID);
  };
  while (NextThreadClientID <= profile::ThreadIDLimit &&
         ProcessID(NextThreadClientID))
    NextThreadClientID += profile::ThreadIDStride;
  if (NextThreadClientID > profile::ThreadIDLimit)
    return schedulingError("thread client IDs are exhausted");
  const auto ID = uint32_t(NextThreadClientID);
  NextThreadClientID += profile::ThreadIDStride;
  ThreadClientIDs.emplace(Object, ID);
  IssuedThreadClientIDs.insert(ID);
  return uint64_t(ID);
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

bool KernelModel::isSystemThreadEntry(uint64_t PC) const {
  const auto &Active = Scheduler.active();
  if (!Active || Active->Kind != KernelScheduler::CallbackKind::SystemThread ||
      Active->PC != PC || Active->ID != CurrentThreadKey)
    return false;
  const auto Thread = ThreadObjects.find(Active->Object);
  return Thread != ThreadObjects.end() && !Thread->second.Exited &&
         Thread->second.CallbackID == Active->ID;
}

llvm::Expected<uint64_t>
KernelModel::createSystemThread(llvm::ArrayRef<uint64_t> A) {
  if (auto E = canStartThreadNotification())
    return E;
  if (!A[0] || !A[5])
    return windows::StatusInvalidParameter;
  if (A[3])
    return schedulingError("non-system process handles are unsupported for "
                           "system threads");
  auto MappedAccess = mapObjectAccess(ObjectKind::Thread, uint32_t(A[1]));
  if (!MappedAccess)
    return MappedAccess.takeError();
  const uint32_t Access = *MappedAccess;
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
      handleProcessID() != windows::SystemProcessID;
  if (CallerInUserProcess && !(Attributes & windows::ObjectKernelHandle))
    return schedulingError("system-thread creation outside the system process "
                           "requires OBJ_KERNEL_HANDLE");
  if (auto E = validateObjectOutput(A[0], 8))
    return E;
  if (A[4] && A[0] < A[4] + 16 && A[4] < A[0] + 8)
    return schedulingError("overlapping thread outputs are unsupported");
  if (A[4])
    if (auto E = validateObjectOutput(A[4], windows::ClientIDSize))
      return E;
  if (std::count_if(ThreadObjects.begin(), ThreadObjects.end(),
                    [](const auto &Entry) { return Entry.second.System; }) >=
          profile::MaxConcurrentCallbacks ||
      NextObjectHandle > profile::SystemThreadHandleLimit)
    return windows::StatusInsufficientResources;
  const uint64_t Aligned = (NextAllocation + windows::PoolAlignment - 1) &
                           ~uint64_t(windows::PoolAlignment - 1);
  if (Aligned > AllocationEnd || ThreadObjectSize > AllocationEnd - Aligned)
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
  if (ThreadNotifiers.empty())
    if (auto E = Memory.writeInteger(A[0], NextObjectHandle, 8))
      return E;
  auto Object =
      allocate((Version || DebuggerReference) ? ThreadObjectSize
                                              : profile::ProcessTokenSize);
  if (!Object)
    return Object.takeError();
  Callback.Object = *Object;
  Callback.Thread = *Object;
  const bool Reserved = !ThreadNotifiers.empty();
  auto ID = Reserved ? Scheduler.reserveSystemThread(std::move(Callback))
                     : Scheduler.enqueueSystemThread(std::move(Callback));
  if (!ID)
    return ID.takeError();
  ReferencedObjects.emplace(*Object, ObjectReferences{ObjectKind::Thread});
  ThreadObject Thread;
  Thread.CallbackID = *ID;
  Thread.ProcessID = windows::SystemProcessID;
  Thread.System = true;
  ThreadObjects.emplace(*Object, Thread);
  if (ThreadNotifiers.empty()) {
    retainObjectHandle(*Object, Access,
                       Attributes | uint32_t(windows::ObjectKernelHandle),
                       windows::SystemProcessID);
    if (A[4]) {
      // CLIENT_ID{UniqueProcess, UniqueThread} for the new system-process
      // thread.
      auto Thread = threadClientID(*Object);
      if (!Thread)
        return Thread.takeError();
      if (auto E = Memory.writeInteger(A[4], windows::SystemProcessID, 8))
        return E;
      if (auto E = Memory.writeInteger(A[4] + windows::ClientIDThreadOffset,
                                       *Thread, 8))
        return E;
    }
  }
  auto ClientID = threadClientID(*Object);
  if (!ClientID)
    return ClientID.takeError();
  if (auto E = publishThreadIdentity(
          *Object,
          ThreadIdentity{*ClientID, A[5], uint32_t(windows::SystemProcessID)}))
    return E;
  if (!ThreadNotifiers.empty()) {
    ThreadCreationOutput Output{
        A[0], A[4], NextObjectHandle,
        ObjectHandle{*Object, Access,
                     Attributes | uint32_t(windows::ObjectKernelHandle),
                     uint32_t(windows::SystemProcessID)},
        Reserved};
    NextObjectHandle += profile::SystemThreadHandleStride;
    if (auto E = startThreadNotification(*Object, true, std::move(Output)))
      return E;
  } else if (Version) {
    if (auto E = startThreadNotification(*Object, true))
      return E;
  }
  return uint64_t(0);
}

llvm::Expected<uint64_t> KernelModel::terminateSystemThread(uint32_t Status) {
  const auto &Active = Scheduler.active();
  if (!Active || Active->Kind != KernelScheduler::CallbackKind::SystemThread ||
      CurrentExecution == profile::StackBase ||
      PendingThreadTerminations.contains(CurrentThreadKey))
    return schedulingError(
        "PsTerminateSystemThread requires the running system thread");
  auto Thread = ThreadObjects.find(Active->Object);
  if (Thread == ThreadObjects.end() ||
      Thread->second.CallbackID != Active->ID || Thread->second.Exited ||
      Thread->second.Terminating)
    return schedulingError("system thread termination lost its live object");
  if (threadHoldsFastMutex(CurrentThreadKey))
    return schedulingError("system thread termination retains a fast mutex");
  if (threadHoldsExecutiveResource(CurrentThreadKey))
    return schedulingError(
        "system thread termination retains an executive resource");
  for (const auto &[ID, N] : ThreadNotifications)
    if (N.Active && N.Thread == CurrentThreadKey)
      return schedulingError(
          "cannot terminate from a thread notification callback");
  if (auto E = canStartThreadNotification())
    return E;
  for (const auto &[IRP, A] : DriverIRPs)
    if (A.Builder && !A.StorageReleased &&
        A.Builder->ThreadKey == CurrentThreadKey)
      return schedulingError("thread exit with outstanding synchronous IOCTL "
                             "requests requires cancellation/drain support");
  Thread->second.ExitStatus = Status;
  Thread->second.Terminating = true;
  if (auto E = startThreadNotification(Active->Object, false))
    return E;
  return uint64_t(0);
}

std::optional<uint32_t> KernelModel::takeThreadTermination() {
  auto It = PendingThreadTerminations.find(CurrentThreadKey);
  if (It == PendingThreadTerminations.end())
    return std::nullopt;
  const auto Status = It->second;
  PendingThreadTerminations.erase(It);
  return Status;
}

uint32_t KernelModel::handleProcessID() const {
  const auto Context = executionProcessContext();
  return Context ? Context->ProcessID : windows::SystemProcessID;
}

bool KernelModel::canAccessHandle(uint32_t Attributes,
                                  uint32_t ProcessID) const {
  return (Attributes & windows::ObjectKernelHandle) ||
         ProcessID == handleProcessID();
}

bool KernelModel::canAccessThreadHandle(const ThreadHandle &Handle) const {
  return canAccessHandle(Handle.Attributes, Handle.ProcessID);
}

bool KernelModel::hasThreadHandles(uint64_t Object) const {
  return std::any_of(
      ObjectHandles.begin(), ObjectHandles.end(),
      [&](const auto &Entry) { return Entry.second.Object == Object; });
}

llvm::Expected<uint64_t>
KernelModel::queryThreadInformation(llvm::ArrayRef<uint64_t> A) {
  if (uint32_t(A[1]) != windows::ThreadQuerySetWin32StartAddress)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "unmodeled thread information class: %u",
                                   uint32_t(A[1]));
  // The inspected 19041 NtQueryInformationThread class-9 branch requires an
  // exact pointer extent and leaves both outputs untouched on mismatch.
  if (uint32_t(A[3]) != profile::PointerSize)
    return windows::StatusInfoLengthMismatch;
  uint64_t Object = 0;
  if (A[0] == windows::CurrentThreadPseudoHandle) {
    auto Current = currentThreadObject();
    if (!Current)
      return Current.takeError();
    Object = *Current;
  } else {
    const auto Handle = ObjectHandles.find(A[0]);
    if (Handle == ObjectHandles.end() || !canAccessThreadHandle(Handle->second))
      return windows::StatusInvalidHandle;
    Object = Handle->second.Object;
  }
  const auto Thread = ThreadIdentities.find(Object);
  if (Thread == ThreadIdentities.end() || !Thread->second.StartAddress)
    return schedulingError(
        "queried thread has no modeled execution start address");
  auto Writable = [&](uint64_t Address, uint32_t Size) -> llvm::Error {
    if (auto E = runtime::checkBufferRange(Address, Size))
      return E;
    if (auto E = validateGuestAccess(Address, Size, true))
      return E;
    auto Allowed = Memory.canAccess(Address, Size, Write);
    if (!Allowed)
      return Allowed.takeError();
    if (!*Allowed)
      return schedulingError("thread information output is not writable");
    return llvm::Error::success();
  };
  if (auto E = Writable(A[2], profile::PointerSize))
    return E;
  if (A[4])
    if (auto E = Writable(A[4], 4))
      return E;
  if (auto E = Memory.writeInteger(A[2], *Thread->second.StartAddress, 8))
    return E;
  // This ordering also defines overlapping information and ULONG length slots.
  if (A[4])
    if (auto E = Memory.writeInteger(A[4], 8, 4))
      return E;
  return uint64_t(0);
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
  const uint64_t FileCount = std::count_if(
      FileObjects.begin(), FileObjects.end(),
      [&](const auto &Entry) { return Entry.second.Device == Device; });
  const uint32_t NativeFileCount = std::count_if(
      NativeOpenFiles.begin(), NativeOpenFiles.end(), [&](const auto &Entry) {
        return Entry.second.Device == Device &&
               Entry.second.State == NativeFileState::Open;
      });
  // Each file body retains its device until failed CREATE or final CLOSE.
  // Caller pointer references and IRP route holds are separate owners.
  return Memory.writeInteger(Device + windows::DeviceReferenceCount,
                             FileCount + NativeFileCount, 4);
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
  if (PendingWait || PendingThreadTerminations.contains(CurrentThreadKey) ||
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
  if (auto E = synchronizeSharedClock())
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
    auto Thread = ThreadObjects.find(Invocation.Object);
    if (Thread == ThreadObjects.end() || Thread->second.CallbackID != ID ||
        !Thread->second.Terminating)
      return schedulingError(
          "system thread returned without PsTerminateSystemThread");
    for (const auto &[Token, N] : ThreadNotifications)
      if (N.Object == Invocation.Object)
        return schedulingError(
            "system thread still owns a notification traversal");
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
    ThreadObjects.at(Invocation.Object).Exited = true;
    ThreadObjects.at(Invocation.Object).ExitPriority =
        Scheduler.threadPriority(ID);
    retireObjectIfUnreferenced(Invocation.Object);
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
  if (executionHoldsSpinLock(CurrentExecution))
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
                                         uint64_t IgnoredDMAPin,
                                         uint64_t RetiringLookaside) const {
  if (Size > UINT64_MAX - Base)
    return schedulingError("overflowing object storage range");
  if (auto E = canReleaseMDLStorage(Base, Size))
    return E;
  if (auto E = canReleaseUserViewsForBacking(Base, Size))
    return E;
  if (Size)
    if (auto E = Physical.canReleaseRange(Base, Size, IgnoredDMAPin))
      return E;
  return canRevokeVirtualRange(Base, Size, {}, RetiringLookaside);
}

llvm::Error
KernelModel::canRevokeVirtualRange(uint64_t Base, uint64_t Size,
                                   std::optional<UsbIdleKey> RetiringUsbIdle,
                                   uint64_t RetiringLookaside) const {
  if (Size > UINT64_MAX - Base)
    return schedulingError("overflowing virtual storage range");
  for (const auto &A : ProcessAttachments)
    if (A.ApcState < Base + Size && Base < A.ApcState + windows::KAPCStateSize)
      return schedulingError("cannot release active APC state storage");
  if (auto E = canReleaseBuiltControlStorage(Base, Size))
    return E;
  if (auto E = canReleaseBugCheckStorage(Base, Size))
    return E;
  if (auto E = canReleaseExecutiveCallbackStorage(Base, Size))
    return E;
  if (auto E = canReleaseProcessNotifyStorage(Base, Size))
    return E;
  if (auto E = canReleaseThreadNotifyStorage(Base, Size))
    return E;
  if (auto E = canReleaseRegistryCallbackStorage(Base, Size))
    return E;
  if (auto E = canReleaseAvlStorage(Base, Size))
    return E;
  if (auto E = DMA.canReleaseRange(Base, Size))
    return E;
  if (auto E = Interrupts.canReleaseRange(Base, Size))
    return E;
  if (auto E = UsbIdle.canReleaseRange(Base, Size, RetiringUsbIdle))
    return E;
  if (auto E = canReleaseLookasideRange(Base, Size, RetiringLookaside))
    return E;
  if (auto E = canReleaseExecutiveResourceRange(Base, Size))
    return E;
  for (const auto &[Address, Hold] : FastMutexHolds)
    if (Size && Address < Base + Size &&
        Base < Address + windows::FastMutexSize)
      return schedulingError("cannot release a held fast mutex");
  for (const auto &[Address, Lock] : ExecutiveSpinLocks)
    if (Address < Base + Size && Base < Address + sizeof(uint64_t))
      return schedulingError("cannot release a held executive spin lock");
  for (const auto &[Address, Lock] : QueuedSpinLocks)
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
  if (auto E = RemoveLocks.forgetRange(Base, Size))
    return E;
  noteBugCheckComponentRelease(Base, Size);
  return llvm::Error::success();
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
    retireMDLStorage(Base, Size);
  }
  // No observation is published by the const preflight checks. Every range
  // and every blocking owner must have accepted revocation first.
  for (const auto &[Base, Size] : Ranges) {
    forgetEmptyAvlStorage(Base, Size);
    noteBugCheckComponentRelease(Base, Size);
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

llvm::Expected<std::pair<uint64_t, uint64_t>>
KernelModel::currentStackBounds() const {
  const auto Stack = ExecutionStacks.find(CurrentExecution);
  const auto Thread = ExecutionThreadKeys.find(CurrentExecution);
  if (!CurrentExecution || !CurrentThreadKey ||
      Stack == ExecutionStacks.end() || Thread == ExecutionThreadKeys.end() ||
      Thread->second != CurrentThreadKey)
    return schedulingError("stack query requires a live activated execution "
                           "stack and its current thread");
  return std::pair{Stack->first, Stack->first + Stack->second};
}

llvm::Expected<uint64_t> KernelModel::queryInitialStack() const {
  auto Bounds = currentStackBounds();
  if (!Bounds)
    return Bounds.takeError();
  // The session reserves no separate kernel stack-control block. Its declared
  // initial base is the upper bound, independently of the current guest RSP.
  return Bounds->second;
}

llvm::Expected<uint64_t> KernelModel::queryStackLimits(uint64_t Low,
                                                       uint64_t High) {
  auto Bounds = currentStackBounds();
  if (!Bounds)
    return Bounds.takeError();
  if (Low > UINT64_MAX - profile::PointerSize ||
      High > UINT64_MAX - profile::PointerSize ||
      (Low < High + profile::PointerSize && High < Low + profile::PointerSize))
    return schedulingError("stack limits require disjoint, nonoverflowing "
                           "output cells");
  // The original ordinary/DPC stack paths publish High before Low. Preserve
  // the first write if the second output is inaccessible; unaligned cells
  // retain the x64 contract. Overlap can trigger an original fast-fail and is
  // outside this bounded output domain.
  for (const auto &[Address, Value] :
       {std::pair{High, Bounds->second}, std::pair{Low, Bounds->first}}) {
    if (auto E = validateGuestAccess(Address, profile::PointerSize, true))
      return E;
    if (auto E = Memory.writeInteger(Address, Value, profile::PointerSize))
      return E;
  }
  return 0;
}

llvm::Error KernelModel::activateStack(uint64_t Base, uint64_t Size) {
  if (!Base || !Size || Size > UINT64_MAX - Base)
    return schedulingError("execution stack requires a nonempty, "
                           "nonoverflowing extent");
  for (const auto &[Other, Length] : ExecutionStacks) {
    if (Other == Base && Length == Size)
      continue;
    if (Base < Other + Length && Other < Base + Size)
      return schedulingError("execution stack overlaps another live extent");
  }
  auto Old = FreedRanges.find(Base);
  if (Old != FreedRanges.end()) {
    if (Old->second != Size)
      return schedulingError(
          "reused thread stack changed its allocation extent");
    FreedRanges.erase(Old);
  }
  ExecutionStacks.emplace(Base, Size);
  return llvm::Error::success();
}

llvm::Error KernelModel::retireStack(uint64_t Base, uint64_t Size) {
  if (auto It = ExecutionStacks.find(Base);
      It != ExecutionStacks.end() && It->second != Size)
    return schedulingError("execution stack retirement changed its extent");
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
  ExecutionStacks.erase(Base);
  FreedRanges.emplace(Base, Size);
  return llvm::Error::success();
}
} // namespace neverd::emulation
