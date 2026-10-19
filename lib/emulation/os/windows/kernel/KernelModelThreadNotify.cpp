//===- KernelModelThreadNotify.cpp - Thread creation and exit callbacks
//----===//
// NeverD Decompiler
///
/// \file
/// Thread creation and exit callbacks.
///

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>
namespace neverd::emulation {
namespace {
llvm::Error notifyError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "thread notify: " + Text);
}
} // namespace
llvm::Expected<uint64_t> KernelModel::setThreadNotify(uint64_t Routine,
                                                      bool Remove) {
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return notifyError("requires Windows 10.0.19041");
  if (Remove) {
    for (const auto &[ID, N] : ThreadNotifications)
      if (N.Active && N.Thread == CurrentThreadKey)
        return notifyError(
            "removal from a thread notification callback is prohibited");
    for (auto &Slot : ThreadNotifySlots) {
      if (!Slot || ThreadNotifiers.at(*Slot).Routine != Routine)
        continue;
      const auto ID = *Slot;
      auto &R = ThreadNotifiers.at(ID);
      if (R.Busy && PendingWait)
        return notifyError("removal cannot replace a pending wait");
      Slot.reset();
      if (!R.Busy)
        ThreadNotifiers.erase(ID);
      else {
        R.Removing = true;
        R.RemovalThread = CurrentThreadKey;
        Wait W;
        W.Type = Wait::Kind::ThreadNotifyRemove;
        W.Object = ID;
        W.Execution = CurrentExecution;
        W.Thread = CurrentThreadKey;
        W.IRQL = CurrentIRQL;
        PendingWait = W;
      }
      return windows::StatusSuccess;
    }
    return uint64_t(0xc000007a);
  }
  auto Executable = Memory.canAccess(Routine, 1, Execute);
  if (!Executable)
    return Executable.takeError();
  const bool GuestImage = std::any_of(
      LoadedModules.begin(), LoadedModules.end(), [&](const auto &M) {
        return M.Guest && Routine >= M.Base && Routine - M.Base < M.Size;
      });
  // The native legacy allocator does not validate an image as the process
  // callback API does. Unsupported callback targets are model errors, not
  // invented native STATUS_ACCESS_DENIED results.
  const bool OwnedPool =
      std::any_of(Allocations.begin(), Allocations.end(), [&](const auto &A) {
        return Routine >= A.first && Routine - A.first < A.second.Size;
      });
  if (!Routine || !*Executable || (!GuestImage && !OwnedPool))
    return notifyError(
        "callback requires executable owned guest driver or pool code");
  auto Slot = std::find(ThreadNotifySlots.begin(), ThreadNotifySlots.end(),
                        std::nullopt);
  if (Slot == ThreadNotifySlots.end())
    return windows::StatusInsufficientResources;
  if (NextThreadNotifier == UINT64_MAX)
    return notifyError("registration generation limit exceeded");
  *Slot = NextThreadNotifier++;
  ThreadNotifiers.emplace(**Slot, ThreadNotifier{Routine});
  return windows::StatusSuccess;
}
llvm::Error KernelModel::canStartThreadNotification() const {
  if (NextThreadNotification == UINT64_MAX ||
      ThreadNotifications.size() >= 64 || PendingThreadNotify)
    return notifyError("notification traversal capacity exceeded or pending "
                       "callback not consumed");
  if (!ThreadNotifiers.empty()) {
    if (!CurrentThreadKey || !executionProcessContext() ||
        CurrentIRQL > windows::APCLevel)
      return notifyError(
          "notification requires a modeled actor thread at IRQL <= APC_LEVEL");
    if (auto It = ApcStates.find(CurrentThreadKey);
        It != ApcStates.end() &&
        It->second.CriticalDepth == profile::MaxAPCRegionNesting)
      return notifyError("implicit critical region exceeds nesting limit");
  }
  return llvm::Error::success();
}
llvm::Error KernelModel::startThreadNotification(
    uint64_t Object, bool Create, std::optional<ThreadCreationOutput> Output) {
  if (!Version && ThreadNotifiers.empty() && !Output) {
    if (!Create)
      PendingThreadTerminations.emplace(CurrentThreadKey,
                                        ThreadObjects.at(Object).ExitStatus);
    return llvm::Error::success();
  }
  const auto T = ThreadIdentities.find(Object);
  if (T == ThreadIdentities.end() || !T->second.CreatingProcessID)
    return notifyError("lifecycle event lost its canonical thread identity");
  const auto Actor = executionProcessContext();
  DriverThreadNotificationResult R;
  R.ProcessID = *T->second.CreatingProcessID;
  R.ThreadID = T->second.ID;
  R.ThreadObject = Object;
  R.Create = Create;
  R.ActorProcessID =
      Actor ? Actor->ProcessID : uint32_t(windows::SystemProcessID);
  R.ActorThreadKey = CurrentThreadKey;
  const auto Report = Result.ThreadNotifications.size();
  Result.ThreadNotifications.push_back(std::move(R));
  const auto ID = NextThreadNotification++;
  ThreadNotification N;
  N.Object = Object;
  N.Thread = CurrentThreadKey;
  N.Report = Report;
  N.IRQL = CurrentIRQL;
  if (auto It = ApcStates.find(CurrentThreadKey); It != ApcStates.end())
    N.Baseline = It->second;
  N.Output = std::move(Output);
  ThreadNotifications.emplace(ID, std::move(N));
  if (auto E = prepareThreadNotify(ID))
    return E;
  if (!ThreadNotifications.at(ID).Registration)
    return completeThreadNotification(ID);
  return llvm::Error::success();
}
llvm::Error KernelModel::prepareThreadNotify(uint64_t ID) {
  auto &N = ThreadNotifications.at(ID);
  while (N.Cursor < ThreadNotifySlots.size()) {
    const auto Slot = ThreadNotifySlots[N.Cursor++];
    if (!Slot)
      continue;
    auto &R = ThreadNotifiers.at(*Slot);
    ++R.Busy;
    N.Registration = *Slot;
    const auto &Event = Result.ThreadNotifications.at(N.Report);
    PendingThreadNotify = KernelGuestCall{
        {GuestCallOwner::ThreadNotify, ID},
        R.Routine,
        {Event.ProcessID, Event.ThreadID, Event.Create ? 1u : 0u},
        0};
    return llvm::Error::success();
  }
  N.Registration = 0;
  return llvm::Error::success();
}
llvm::Error KernelModel::beginThreadNotify(uint64_t ID) {
  auto It = ThreadNotifications.find(ID);
  if (It == ThreadNotifications.end() || It->second.Active ||
      !It->second.Registration || It->second.Thread != CurrentThreadKey ||
      It->second.IRQL != CurrentIRQL)
    return notifyError("callback entry lost its event, actor or IRQL");
  auto &N = It->second;
  auto &APC = ApcStates[CurrentThreadKey];
  if (APC.CriticalDepth != N.Baseline.CriticalDepth ||
      APC.GuardedDepth != N.Baseline.GuardedDepth)
    return notifyError("callback entry changed its actor APC state");
  ++APC.CriticalDepth;
  N.Active = true;
  return llvm::Error::success();
}
llvm::Expected<std::optional<uint64_t>>
KernelModel::finishThreadNotify(uint64_t ID) {
  auto It = ThreadNotifications.find(ID);
  if (It == ThreadNotifications.end() || !It->second.Active ||
      It->second.Thread != CurrentThreadKey || It->second.IRQL != CurrentIRQL)
    return notifyError("callback return lost its event, actor or IRQL");
  auto &N = It->second;
  auto &APC = ApcStates[CurrentThreadKey];
  if (APC.CriticalDepth != N.Baseline.CriticalDepth + 1 ||
      APC.GuardedDepth != N.Baseline.GuardedDepth)
    return notifyError(
        "callback returned with an unbalanced APC-disable region");
  --APC.CriticalDepth;
  auto &R = ThreadNotifiers.at(N.Registration);
  --R.Busy;
  N.Active = false;
  Result.ThreadNotifications.at(N.Report).DeliveredRoutines.push_back(
      R.Routine);
  if (auto E = prepareThreadNotify(ID))
    return std::move(E);
  if (N.Registration)
    return std::optional<uint64_t>{};
  if (auto E = completeThreadNotification(ID))
    return std::move(E);
  return std::optional<uint64_t>{0};
}
llvm::Error KernelModel::completeThreadNotification(uint64_t ID) {
  const auto &N = ThreadNotifications.at(ID);
  if (N.Active || N.Registration)
    return notifyError("cannot commit an active traversal");
  auto &R = Result.ThreadNotifications.at(N.Report);
  if (N.Output) {
    const auto &O = *N.Output;
    if (auto E = objectBuffer(O.Output, 8, true))
      return E;
    if (O.ClientID)
      if (auto E = objectBuffer(O.ClientID, 16, true))
        return E;
    if (auto E = Memory.writeInteger(O.Output, O.Value, 8))
      return E;
    if (O.ClientID) {
      if (auto E = Memory.writeInteger(O.ClientID, R.ProcessID, 8))
        return E;
      if (auto E = Memory.writeInteger(O.ClientID + 8, R.ThreadID, 8))
        return E;
    }
    retainReservedObjectHandle(O.Value, O.Handle.Object, O.Handle.GrantedAccess,
                               O.Handle.Attributes, O.Handle.ProcessID);
    if (O.Reserved)
      if (auto E = Scheduler.readySystemThread(
              ThreadObjects.at(N.Object).CallbackID))
        return E;
  } else if (!R.Create) {
    const auto &T = ThreadObjects.at(N.Object);
    if (!T.Terminating || T.Exited)
      return notifyError("exit completion lost its terminating thread");
    PendingThreadTerminations.emplace(N.Thread, T.ExitStatus);
  }
  R.Completed = true;
  ThreadNotifications.erase(ID);
  return llvm::Error::success();
}
llvm::Expected<std::optional<uint32_t>>
KernelModel::pollThreadNotifyRemove(const Wait &W) {
  auto It = ThreadNotifiers.find(W.Object);
  if (It == ThreadNotifiers.end() || !It->second.Removing ||
      It->second.RemovalThread != W.Thread)
    return notifyError(
        "removal wait lost its registration generation or thread");
  if (It->second.Busy)
    return std::optional<uint32_t>{};
  ThreadNotifiers.erase(It);
  return std::optional<uint32_t>{0};
}
llvm::Error KernelModel::validateThreadNotifyUnload() const {
  if (!ThreadNotifiers.empty() || !ThreadNotifications.empty() ||
      PendingThreadNotify || !PendingThreadTerminations.empty())
    return notifyError("unload returned with registered, retiring or executing "
                       "thread callbacks");
  return llvm::Error::success();
}
llvm::Error KernelModel::canReleaseThreadNotifyStorage(uint64_t Base,
                                                       uint64_t Size) const {
  for (const auto &[ID, R] : ThreadNotifiers)
    if (R.Routine >= Base && R.Routine - Base < Size)
      return notifyError(
          "cannot release registered or retiring thread callback code");
  return llvm::Error::success();
}
} // namespace neverd::emulation
