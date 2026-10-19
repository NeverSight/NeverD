//===- KernelModelWaits.cpp - Windows dispatcher wait registrations -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// The model owns thread references, opaque caller wait blocks and deadlines.
/// KernelDispatcher owns the atomic acquisition of every selected object.

#include "KernelModel.h"
#include "KernelWaits.h"
#include "WindowsKernelLayout.h"

#include <bit>
#include <set>
#include <utility>

namespace neverd::emulation {
namespace {
llvm::Error waitError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

std::optional<KernelModel::Wait> KernelModel::takeWait() {
  return std::exchange(PendingWait, std::nullopt);
}

llvm::Expected<uint64_t> KernelModel::beginWait(llvm::ArrayRef<uint64_t> A,
                                                bool Delay) {
  const uint8_t Mode = A[Delay ? 0 : 2];
  const uint8_t Alertable = A[Delay ? 1 : 3];
  if (Mode != windows::KernelMode || Alertable)
    return waitError(kernel_wait::WaitMode);
  if (!Delay && uint32_t(A[1]) != windows::ExecutiveWaitReason)
    return waitError(kernel_wait::WaitReason);
  const uint64_t Timeout = A[Delay ? 2 : 4];
  if (Delay && !Timeout)
    return waitError(kernel_wait::DelayInterval);
  Wait Pending;
  Pending.Type = Delay                          ? Wait::Kind::Delay
                 : ThreadObjects.contains(A[0]) ? Wait::Kind::Thread
                                                : Wait::Kind::Dispatcher;
  Pending.Object = Delay ? 0 : A[0];
  if (!Delay)
    Pending.Objects.push_back(A[0]);
  return beginObjectWait(std::move(Pending), Timeout);
}

llvm::Expected<uint64_t>
KernelModel::beginMultipleWait(llvm::ArrayRef<uint64_t> A) {
  const uint32_t Count = A[0];
  if (!Count || Count > kernel_wait::MaximumObjects)
    return waitError(kernel_wait::InvalidCount);
  const uint32_t Type = A[2];
  if (Type != kernel_wait::All && Type != kernel_wait::Any)
    return waitError(kernel_wait::InvalidType);
  if (uint32_t(A[3]) != windows::ExecutiveWaitReason)
    return waitError(kernel_wait::WaitReason);
  if (uint8_t(A[4]) != windows::KernelMode || uint8_t(A[5]))
    return waitError(kernel_wait::WaitMode);
  const auto ValidateBuffer = [&](uint64_t Address, uint32_t Size, bool IsWrite,
                                  const char *Message) -> llvm::Error {
    if (Address < profile::UserProbeLimit)
      return waitError(Message);
    if (auto E = validateDispatcherStorage(Address, Size, IsWrite))
      return E;
    if (auto E = validateGuestAccess(Address, Size, IsWrite))
      return E;
    auto Accessible =
        Memory.canAccess(Address, Size, IsWrite ? Read | Write : Read);
    if (!Accessible)
      return Accessible.takeError();
    return *Accessible ? llvm::Error::success() : waitError(Message);
  };
  const uint32_t ArraySize = Count * kernel_wait::PointerSize;
  if (auto E =
          ValidateBuffer(A[1], ArraySize, false, kernel_wait::ArrayStorage))
    return E;
  if (!A[7] && Count > kernel_wait::BuiltinObjects)
    return waitError(kernel_wait::BlocksRequired);
  Wait Pending;
  Pending.Type = Wait::Kind::Multiple;
  Pending.All = Type == kernel_wait::All;
  if (A[7]) {
    const uint32_t Size = Count * kernel_wait::BlockSize;
    if (A[7] % dispatcher::ObjectAlignment)
      return waitError(kernel_wait::BlocksStorage);
    if (auto E = ValidateBuffer(A[7], Size, true, kernel_wait::BlocksStorage))
      return E;
    if (A[1] < A[7] + Size && A[7] < A[1] + ArraySize)
      return waitError(kernel_wait::BufferOverlap);
    Pending.WaitBlockArray = A[7];
    Pending.WaitBlockSize = Size;
  }
  std::set<uint64_t> Seen;
  for (uint32_t I = 0; I < Count; ++I) {
    auto Object = Memory.readInteger(A[1] + I * kernel_wait::PointerSize,
                                     kernel_wait::PointerSize);
    if (!Object)
      return Object.takeError();
    if (!Seen.insert(*Object).second)
      return waitError(kernel_wait::DuplicateObject);
    // A caller block may not overwrite opaque dispatcher/thread storage.
    if (Pending.WaitBlockArray && *Object >= Pending.WaitBlockArray &&
        *Object - Pending.WaitBlockArray < Pending.WaitBlockSize)
      return waitError(kernel_wait::BufferOverlap);
    Pending.Objects.push_back(*Object);
  }
  return beginObjectWait(std::move(Pending), A[6]);
}

llvm::Expected<uint64_t> KernelModel::beginObjectWait(Wait Pending,
                                                      uint64_t Timeout) {
  if (PendingWait)
    return waitError(kernel_wait::PreviousWait);
  Pending.Execution = CurrentExecution;
  Pending.Thread = CurrentThreadKey;
  Pending.IRQL = CurrentIRQL;
  bool PollOnly = false;
  if (Timeout) {
    if (auto E = validateGuestAccess(Timeout, sizeof(int64_t), false))
      return E;
    auto Raw = Memory.readInteger(Timeout, sizeof(int64_t));
    if (!Raw)
      return Raw.takeError();
    auto Deadline = Scheduler.computeDeadline(std::bit_cast<int64_t>(*Raw));
    if (!Deadline)
      return Deadline.takeError();
    Pending.Deadline = *Deadline;
    if (Pending.Type == Wait::Kind::Delay)
      APICallDetail =
          "interval_100ns=" + std::to_string(std::bit_cast<int64_t>(*Raw)) +
          "; deadline_100ns=" + std::to_string(*Deadline);
    PollOnly = !*Raw;
  }
  if (CurrentIRQL > ((Pending.Type != Wait::Kind::Delay && PollOnly)
                         ? scheduler::DispatchLevel
                         : windows::APCLevel))
    return waitError(kernel_wait::BlockingIRQL);
  for (uint64_t Object : Pending.Objects)
    if (const auto Thread = ThreadObjects.find(Object);
        Thread != ThreadObjects.end() && !ReferencedObjects.at(Object).Pointers)
      return waitError(kernel_wait::ThreadReference);
  auto Acquired = acquireWaitObjects(Pending);
  if (!Acquired)
    return Acquired.takeError();
  if (*Acquired)
    return uint64_t(**Acquired);
  if (Pending.Deadline && *Pending.Deadline <= Scheduler.now100ns())
    return Pending.Type == Wait::Kind::Delay ? windows::StatusSuccess
                                             : windows::StatusTimeout;
  if (!NextWaitRegistration)
    return waitError(kernel_wait::RegistrationExhausted);
  Pending.Registration = NextWaitRegistration++;
  WaitRegistrations.emplace(Pending.Registration, Pending);
  for (uint64_t Object : Pending.Objects)
    ++WaitReferences[Object];
  if (Pending.WaitBlockArray)
    ++WaitBlockReferences[{Pending.WaitBlockArray, Pending.WaitBlockSize}];
  PendingWait = std::move(Pending);
  // This placeholder never becomes an API result: DriverSession retains the
  // actual guest frame and records only the final pollWait result.
  return uint64_t(0);
}

llvm::Expected<std::optional<uint32_t>>
KernelModel::acquireWaitObjects(const Wait &Pending) {
  if (Pending.Type == Wait::Kind::Delay)
    return std::optional<uint32_t>{};
  const auto Objects = Pending.Objects.empty() && Pending.Object
                           ? llvm::ArrayRef<uint64_t>(Pending.Object)
                           : llvm::ArrayRef<uint64_t>(Pending.Objects);
  std::vector<KernelDispatcher::WaitCondition> Conditions;
  for (uint64_t Object : Objects) {
    using Condition = KernelDispatcher::WaitCondition;
    if (const auto Thread = ThreadObjects.find(Object);
        Thread != ThreadObjects.end()) {
      if (!Thread->second.Exited && Thread->second.CallbackID == Pending.Thread)
        return waitError(kernel_wait::SelfWait);
      Conditions.push_back({Thread->second.Exited ? Condition::Kind::Ready
                                                  : Condition::Kind::Pending});
    } else {
      if (Pending.Type == Wait::Kind::Thread)
        return waitError(kernel_wait::ThreadLost);
      Conditions.push_back({Condition::Kind::Dispatcher, Object});
    }
  }
  return Dispatcher.tryAcquireSet(Conditions, Pending.All, Pending.Thread,
                                  Pending.IRQL);
}

llvm::Error KernelModel::releaseWaitReferences(const Wait &Pending) {
  const auto Objects = Pending.Objects.empty() && Pending.Object
                           ? llvm::ArrayRef<uint64_t>(Pending.Object)
                           : llvm::ArrayRef<uint64_t>(Pending.Objects);
  for (uint64_t Object : Objects) {
    auto Reference = WaitReferences.find(Object);
    if (Reference == WaitReferences.end() || !Reference->second)
      return waitError(kernel_wait::WaitLost);
  }
  if (Pending.WaitBlockArray) {
    auto Reference = WaitBlockReferences.find(
        {Pending.WaitBlockArray, Pending.WaitBlockSize});
    if (Reference == WaitBlockReferences.end() || !Reference->second)
      return waitError(kernel_wait::BlockLost);
    if (!--Reference->second)
      WaitBlockReferences.erase(Reference);
  }
  for (uint64_t Object : Objects)
    if (!--WaitReferences.at(Object))
      WaitReferences.erase(Object);
  for (uint64_t Object : Objects)
    retireObjectIfUnreferenced(Object);
  WaitRegistrations.erase(Pending.Registration);
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint32_t>>
KernelModel::pollWait(const Wait &Pending) {
  if (Pending.Type == Wait::Kind::FileOperation) {
    auto Ready = fileOperationReady(Pending.Object);
    if (!Ready)
      return Ready.takeError();
    return *Ready ? std::optional<uint32_t>{windows::StatusSuccess}
                  : std::optional<uint32_t>{};
  }
  if (Pending.Type == Wait::Kind::FileRequest) {
    auto Ready = fileRequestReady(Pending.Object);
    if (!Ready)
      return Ready.takeError();
    return *Ready ? std::optional<uint32_t>{windows::StatusSuccess}
                  : std::optional<uint32_t>{};
  }
  if (Pending.Type == Wait::Kind::FileHandleClose) {
    const auto Operation = FileHandleCloses.find(Pending.Object);
    if (Operation == FileHandleCloses.end())
      return waitError("file handle wait lost its continuation");
    return FinalizedRequests.contains(Operation->second.IRP)
               ? std::optional<uint32_t>{windows::StatusSuccess}
               : std::optional<uint32_t>{};
  }
  if (Pending.Type == Wait::Kind::FileClose) {
    if (InternalFileCloses.contains(Pending.Object))
      return std::optional<uint32_t>{};
    if (!FinalizedRequests.contains(Pending.Object))
      return waitError("file CLOSE wait lost its retained request");
    return std::optional<uint32_t>{windows::StatusSuccess};
  }
  if (Pending.Registration) {
    const auto Registration = WaitRegistrations.find(Pending.Registration);
    if (Registration == WaitRegistrations.end() ||
        Registration->second != Pending)
      return waitError(kernel_wait::RegistrationLost);
  } else if (Pending.Type == Wait::Kind::Dispatcher ||
             Pending.Type == Wait::Kind::Thread ||
             Pending.Type == Wait::Kind::Multiple ||
             Pending.Type == Wait::Kind::Delay) {
    return waitError(kernel_wait::RegistrationLost);
  }
  if (Pending.Type == Wait::Kind::PoFxActive ||
      Pending.Type == Wait::Kind::PoFxIdle) {
    auto Operation = BlockingPoFx.find(Pending.Thread);
    if (Operation == BlockingPoFx.end() ||
        Operation->second.Handle != Pending.Object)
      return waitError(kernel_wait::PoFxWaitLost);
    auto Ready =
        PoFx.conditionReached(Pending.Object, Operation->second.Component,
                              Pending.Type == Wait::Kind::PoFxActive);
    if (!Ready)
      return Ready.takeError();
    if (!Operation->second.Completed && !*Ready)
      return std::optional<uint32_t>{};
    BlockingPoFx.erase(Operation);
    return std::optional<uint32_t>{windows::StatusSuccess};
  }
  if (Pending.Type == Wait::Kind::FrameworkWaitLock)
    return pollFrameworkWaitLock(Pending);
  if (Pending.Type == Wait::Kind::ExecutiveCallbackUnregister)
    return pollExecutiveUnregister(Pending);
  if (Pending.Type == Wait::Kind::ProcessNotifyRemove)
    return pollProcessNotifyRemove(Pending);
  if (Pending.Type == Wait::Kind::ThreadNotifyRemove)
    return pollThreadNotifyRemove(Pending);
  if (Pending.Type == Wait::Kind::RegistryCallbackRemove)
    return pollRegistryCallbackRemove(Pending);
  if (Pending.Type == Wait::Kind::FrameworkCallback ||
      Pending.Type == Wait::Kind::FrameworkCallbackLock)
    return pollFrameworkCallback(Pending);
  if (Pending.Type == Wait::Kind::FrameworkInterruptLock) {
    auto Acquired =
        Interrupts.tryAcquirePassive(Pending.Object, Pending.Execution);
    if (!Acquired)
      return Acquired.takeError();
    if (!*Acquired)
      return std::optional<uint32_t>{};
    FrameworkInterruptLocks.emplace(
        std::make_pair(Pending.Execution, Pending.Object),
        scheduler::PassiveLevel);
    return std::optional<uint32_t>{windows::StatusSuccess};
  }
  if (Pending.Type == Wait::Kind::InterruptSynchronization) {
    auto Ready = Interrupts.reserveSynchronization(Pending.Object);
    if (!Ready)
      return Ready.takeError();
    return *Ready ? std::optional<uint32_t>{windows::StatusSuccess}
                  : std::optional<uint32_t>{};
  }
  if (Pending.Type == Wait::Kind::FrameworkIdle) {
    if (!Framework)
      return waitError(kernel_wait::FrameworkIdleLost);
    return Framework->powerPolicyWait(Pending.Object);
  }
  if (Pending.Type == Wait::Kind::FrameworkFileSend) {
    if (!Framework)
      return waitError(kernel_wait::FrameworkFileLost);
    auto Waiting = Framework->synchronousFileSendPending(Pending.Object);
    if (!Waiting)
      return waitError(kernel_wait::FrameworkRequestLost);
    return *Waiting ? std::optional<uint32_t>{} : std::optional<uint32_t>{1};
  }
  if (Pending.Type == Wait::Kind::FrameworkQueueStop ||
      Pending.Type == Wait::Kind::FrameworkQueueEmpty) {
    if (!Framework)
      return waitError(kernel_wait::FrameworkQueueLost);
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
      return waitError(kernel_wait::RemoveLockLost);
    if (!--Reference->second)
      RemoveLockWaitReferences.erase(Reference);
    return std::optional<uint32_t>{windows::StatusSuccess};
  }
  const auto Objects = Pending.Objects.empty() && Pending.Object
                           ? llvm::ArrayRef<uint64_t>(Pending.Object)
                           : llvm::ArrayRef<uint64_t>(Pending.Objects);
  // Validate every retained reference before acquisition can consume a signal.
  for (uint64_t Object : Objects) {
    const auto Reference = WaitReferences.find(Object);
    if (Reference == WaitReferences.end() || !Reference->second)
      return waitError(kernel_wait::WaitLost);
  }
  if (Pending.WaitBlockArray &&
      !WaitBlockReferences.contains(
          {Pending.WaitBlockArray, Pending.WaitBlockSize}))
    return waitError(kernel_wait::BlockLost);
  auto Acquired = acquireWaitObjects(Pending);
  if (!Acquired)
    return Acquired.takeError();
  const bool Expired =
      Pending.Deadline && *Pending.Deadline <= Scheduler.now100ns();
  if (!*Acquired && !Expired)
    return std::optional<uint32_t>{};
  const auto Status =
      *Acquired ? *Acquired
                : std::optional<uint32_t>{Pending.Type == Wait::Kind::Delay
                                              ? windows::StatusSuccess
                                              : windows::StatusTimeout};
  if (auto E = releaseWaitReferences(Pending))
    return E;
  return Status;
}

} // namespace neverd::emulation
