//===- KernelModelProcessNotify.cpp - Typed process callback delivery
//------===//
// NeverD Decompiler
// WDK PsSetCreateProcessNotifyRoutine / PCREATE_PROCESS_NOTIFY_ROUTINE.
// Native 10.0.19041 PspSetCreateProcessNotifyRoutine uses 64 indexed slots,
// clears a removed slot before rundown, and permits legacy duplicate entries.
///
/// \file
/// Typed process callback delivery.
///

#include "KernelModel.h"
#include "KernelProcessCreate.h"
#include "KernelReferenceImage.h"
#include "WindowsKernelLayout.h"

#include <algorithm>
namespace neverd::emulation {
namespace {
llvm::Error processError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "process notify: " + Text);
}
constexpr uint64_t ProcedureNotFound = 0xc000007a, AccessDenied = 0xc0000022;
} // namespace
llvm::Error KernelModel::initializeProcessEvents(const DriverOptions &Options) {
  for (const auto &E : Options.ProcessEvents)
    if (E.CreateInfo &&
        (!Options.Version || Options.Version->Major != 10 ||
         Options.Version->Minor || Options.Version->Build != 19041))
      return processError("create_info requires Windows 10.0.19041");
  if (auto E = validateDriverProcessInventory(Options))
    return E;
  const auto Initial = initialDriverProcessIDs(
      Options.ProcessEvents, Options.Requests, Options.ProcessInventory);
  if (auto E = validateDriverProcessEvents(Options.ProcessEvents,
                                           Options.Requests, Initial))
    return E;
  LifecycleLiveProcesses.insert(Initial.begin(), Initial.end());
  LifecycleSeenProcesses.insert(Initial.begin(), Initial.end());
  ProcessInventory = Options.ProcessInventory;
  if (Options.Version && Options.Version->Major == 10 &&
      Options.Version->Minor == 0 && Options.Version->Build == 19041 &&
      (!DebuggerReference || !DebuggerReference->debugger()))
    ProcessObjectSize = std::max<uint32_t>(
        ProcessObjectSize, windows::ProcessExitStatus19041Offset + 4);
  if (ProcessInventory &&
      (!DebuggerReference || !DebuggerReference->debugger()))
    ProcessObjectSize = std::max<uint32_t>(
        ProcessObjectSize, windows::ProcessImageFileName19041Offset +
                               DriverProcessImageFileNameSize);
  if (ProcessInventory)
    for (const auto &P : *ProcessInventory)
      if (LifecycleLiveProcesses.contains(P.ProcessID))
        LifecycleParents.emplace(P.ProcessID, P.ParentID);
  return llvm::Error::success();
}
llvm::Expected<uint64_t>
KernelModel::setProcessNotify(llvm::ArrayRef<uint64_t> A, bool Extended) {
  if (uint8_t(A[1])) {
    if (ProcessNotificationState &&
        ProcessNotificationState->Thread == CurrentThreadKey)
      return processError(
          "removal from a process notification callback is prohibited");
    for (auto &Slot : ProcessNotifySlots) {
      if (!Slot || ProcessNotifiers.at(*Slot).Routine != A[0] ||
          ProcessNotifiers.at(*Slot).Extended != Extended)
        continue;
      const uint64_t ID = *Slot;
      auto &R = ProcessNotifiers.at(ID);
      if (R.Busy && PendingWait)
        return processError("removal cannot replace a pending wait");
      Slot.reset();
      if (!R.Busy) {
        ProcessNotifiers.erase(ID);
      } else {
        R.Removing = true;
        R.RemovalThread = CurrentThreadKey;
        Wait W;
        W.Type = Wait::Kind::ProcessNotifyRemove;
        W.Object = ID;
        W.Execution = CurrentExecution;
        W.Thread = CurrentThreadKey;
        W.IRQL = CurrentIRQL;
        PendingWait = W;
      }
      return windows::StatusSuccess;
    }
    return ProcedureNotFound;
  }
  auto Executable = Memory.canAccess(A[0], 1, Execute);
  if (!Executable)
    return Executable.takeError();
  const bool InImage =
      std::any_of(LoadedModules.begin(), LoadedModules.end(),
                  [&](const auto &M) {
                    return A[0] >= M.Base && A[0] - M.Base < M.Size;
                  }) ||
      (DebuggerReference && DebuggerReference->contains(A[0]));
  // MmVerifyCallbackFunction(Routine, 0) still requires an image owner.
  // Executable pool memory alone does not satisfy the native legacy check.
  if (!A[0] || !*Executable || !InImage)
    return AccessDenied;
  if (Extended) {
    const auto Owner = std::find_if(
        LoadedModules.begin(), LoadedModules.end(), [&](const auto &M) {
          return M.Guest && A[0] >= M.Base && A[0] - M.Base < M.Size;
        });
    if (Owner == LoadedModules.end())
      return processError("Ex callback integrity is only modeled for the "
                          "loaded guest driver");
    if (!(Owner->DLLCharacteristics & 0x80))
      return AccessDenied;
  }
  const bool Native19041 = Version && Version->Major == 10 &&
                           Version->Minor == 0 && Version->Build == 19041;
  if (!Native19041)
    for (const auto &Slot : ProcessNotifySlots)
      if (Slot && ProcessNotifiers.at(*Slot).Routine == A[0] &&
          ProcessNotifiers.at(*Slot).Extended == Extended)
        return windows::StatusInvalidParameter;
  auto Slot = std::find(ProcessNotifySlots.begin(), ProcessNotifySlots.end(),
                        std::nullopt);
  if (Slot == ProcessNotifySlots.end())
    return windows::StatusInvalidParameter;
  if (NextProcessNotifier == UINT64_MAX)
    return processError("registration generation limit exceeded");
  *Slot = NextProcessNotifier++;
  ProcessNotifier Registration{A[0]};
  Registration.Extended = Extended;
  ProcessNotifiers.emplace(**Slot, Registration);
  return windows::StatusSuccess;
}
llvm::Expected<std::optional<uint32_t>>
KernelModel::pollProcessNotifyRemove(const Wait &W) {
  auto It = ProcessNotifiers.find(W.Object);
  if (It == ProcessNotifiers.end() || !It->second.Removing ||
      It->second.RemovalThread != W.Thread)
    return processError(
        "removal wait lost its registration generation or thread");
  if (It->second.Busy)
    return std::optional<uint32_t>{};
  ProcessNotifiers.erase(It);
  return std::optional<uint32_t>{0};
}
llvm::Error KernelModel::prepareProcessNotify() {
  auto &N = *ProcessNotificationState;
  const auto &Event = Result.ProcessEvents.at(N.ReportIndex).Event;
  while (N.Cursor < ProcessNotifySlots.size()) {
    auto Slot = ProcessNotifySlots[N.Cursor++];
    if (!Slot)
      continue;
    auto &R = ProcessNotifiers.at(*Slot);
    std::vector<uint64_t> Arguments;
    if (R.Extended) {
      if (Event.Type == DriverProcessEvent::Kind::Create && !N.CreateInfo)
        return processError("Ex process creation requires an explicit "
                            "PS_CREATE_NOTIFY_INFO provider");
      auto Object = processObject(Event.ProcessID);
      if (!Object)
        return Object.takeError();
      Arguments = {*Object, Event.ProcessID, N.CreateInfo};
    } else {
      Arguments = {Event.ParentID, Event.ProcessID,
                   Event.Type == DriverProcessEvent::Kind::Create ? 1u : 0u};
    }
    ++R.Busy;
    N.Registration = *Slot;
    PendingProcessNotify =
        KernelGuestCall{{GuestCallOwner::ProcessNotify, N.ID},
                        R.Routine,
                        std::move(Arguments),
                        0};
    return llvm::Error::success();
  }
  N.Registration = 0;
  return llvm::Error::success();
}
llvm::Expected<std::optional<KernelGuestCall>>
KernelModel::beginProcessEvent(const DriverProcessEvent &Event,
                               size_t SourceIndex) {
  if (ProcessNotificationState || hasPendingModelGuestCall() || PendingWait ||
      CurrentIRQL || CurrentThreadKey != profile::StackBase ||
      Scheduler.active() || hasProcessAttachment(CurrentExecution))
    return processError(
        "lifecycle event requires an idle foreground thread at PASSIVE_LEVEL");
  if (!Event.ProcessID || Event.ProcessID == 4 || !Event.ParentID ||
      Event.ProcessID == Event.ParentID)
    return processError("invalid target or parent process identity");
  const bool Creating = Event.Type == DriverProcessEvent::Kind::Create;
  if (Creating && Event.ExitStatus)
    return processError("exit_status is exit-only");
  if (!Creating && Event.Type != DriverProcessEvent::Kind::Exit)
    return processError("unknown lifecycle event kind");
  if (Creating) {
    for (const auto &Slot : ProcessNotifySlots)
      if (Slot && ProcessNotifiers.at(*Slot).Extended && !Event.CreateInfo)
        return processError("Ex process creation requires an explicit "
                            "PS_CREATE_NOTIFY_INFO provider");
    if (LifecycleSeenProcesses.contains(Event.ProcessID) ||
        ProcessObjects.contains(Event.ProcessID) ||
        !LifecycleLiveProcesses.contains(Event.ParentID) ||
        !LifecycleLiveProcesses.contains(Event.CreatorProcessID))
      return processError(
          "create requires a fresh PID and live parent and creator");
  } else if (Event.CreatorProcessID != 4 ||
             !LifecycleLiveProcesses.contains(Event.ProcessID) ||
             (LifecycleParents.contains(Event.ProcessID) &&
              LifecycleParents.at(Event.ProcessID) != Event.ParentID)) {
    return processError("exit requires a live PID and its original parent");
  }
  const uint32_t Actor = Creating ? Event.CreatorProcessID : Event.ProcessID;
  for (auto PID : {Actor, Event.ProcessID}) {
    auto Object = processObject(PID);
    if (!Object)
      return Object.takeError();
  }
  if (NextProcessNotification == UINT64_MAX)
    return processError("notification generation limit exceeded");
  if (!Creating)
    if (auto E = observeProcessExitStatus(Event.ProcessID, Event.ExitStatus))
      return E;
  if (Creating) {
    if (auto E = inheritSectionViews(Event.ParentID, Event.ProcessID))
      return E;
    LifecycleLiveProcesses.insert(Event.ProcessID);
    LifecycleSeenProcesses.insert(Event.ProcessID);
    LifecycleParents.emplace(Event.ProcessID, Event.ParentID);
    if (auto E = synchronizeProcessList())
      return E;
  }
  const size_t Report = Result.ProcessEvents.size();
  Result.ProcessEvents.push_back({Event, SourceIndex, Actor});
  ProcessNotificationState = ProcessNotification{
      NextProcessNotification++,  CurrentThreadKey, 0, 0, Report,
      ApcStates[CurrentThreadKey]};
  if (Creating && Event.CreateInfo)
    if (auto E = prepareProcessCreateInfo(Event))
      return E;
  if (auto E = prepareProcessNotify())
    return E;
  return std::exchange(PendingProcessNotify, std::nullopt);
}
llvm::Error KernelModel::beginProcessNotify(uint64_t ID) {
  if (!ProcessNotificationState || ProcessNotificationState->ID != ID ||
      ProcessNotificationState->Active ||
      !ProcessNotificationState->Registration ||
      ProcessNotificationState->Thread != CurrentThreadKey || CurrentIRQL)
    return processError(
        "callback entry lost its event, thread or PASSIVE_LEVEL");
  auto &State = ApcStates[CurrentThreadKey];
  const auto &Baseline = ProcessNotificationState->Baseline;
  if (State.CriticalDepth != Baseline.CriticalDepth ||
      State.GuardedDepth != Baseline.GuardedDepth ||
      State.CriticalDepth >= profile::MaxAPCRegionNesting)
    return processError("callback entry has unexpected APC-disable state");
  ++State.CriticalDepth;
  ProcessNotificationState->Active = true;
  return llvm::Error::success();
}
llvm::Expected<std::optional<uint64_t>>
KernelModel::finishProcessNotify(uint64_t ID) {
  if (!ProcessNotificationState || ProcessNotificationState->ID != ID ||
      !ProcessNotificationState->Active ||
      ProcessNotificationState->Thread != CurrentThreadKey || CurrentIRQL)
    return processError(
        "callback return lost its event, thread or PASSIVE_LEVEL");
  auto &N = *ProcessNotificationState;
  auto &APC = ApcStates[CurrentThreadKey];
  if (APC.CriticalDepth != N.Baseline.CriticalDepth + 1 ||
      APC.GuardedDepth != N.Baseline.GuardedDepth)
    return processError(
        "callback returned with an unbalanced APC-disable region");
  --APC.CriticalDepth;
  auto &R = ProcessNotifiers.at(N.Registration);
  --R.Busy;
  Result.ProcessEvents.at(N.ReportIndex).DeliveredRoutines.push_back(R.Routine);
  N.Active = false;
  if (N.CreateInfo) {
    auto Status = Memory.readInteger(N.CreateInfo + process_create::Status, 4);
    if (!Status)
      return Status.takeError();
    Result.ProcessEvents.at(N.ReportIndex).CreationStatus = uint32_t(*Status);
    if (*Status & profile::NTStatusFailureMask) {
      // Native PspCallProcessNotifyRoutines stops the slot traversal on veto.
      N.Registration = 0;
      N.Cursor = ProcessNotifySlots.size();
      return std::optional<uint64_t>{0};
    }
  }
  if (auto E = prepareProcessNotify())
    return E;
  if (PendingProcessNotify)
    return std::optional<uint64_t>{};
  return std::optional<uint64_t>{
      0}; // VOID callback result is deliberately ignored.
}
llvm::Error KernelModel::completeProcessEvent() {
  if (!ProcessNotificationState || ProcessNotificationState->Active ||
      ProcessNotificationState->Registration || PendingProcessNotify)
    return processError("cannot commit an incomplete notification traversal");
  auto &Report = Result.ProcessEvents.at(ProcessNotificationState->ReportIndex);
  if (Report.Event.Type == DriverProcessEvent::Kind::Exit ||
      (Report.CreationStatus &&
       (*Report.CreationStatus & profile::NTStatusFailureMask))) {
    if (Report.CreationStatus)
      if (auto E = observeProcessExitStatus(Report.Event.ProcessID,
                                            Report.CreationStatus))
        return E;
    if (auto E = retireUserProcess(Report.Event.ProcessID))
      return E;
    LifecycleLiveProcesses.erase(Report.Event.ProcessID);
  }
  for (const auto &[Address, Size] : ProcessNotificationState->Storage) {
    if (auto E = prepareReleaseRange(Address, Size))
      return E;
    FreedRanges.emplace(Address, Size);
  }
  Report.Completed = true;
  ProcessNotificationState.reset();
  return llvm::Error::success();
}
llvm::Error KernelModel::validateProcessNotifyUnload() const {
  if (!ProcessNotifiers.empty() || ProcessNotificationState ||
      PendingProcessNotify)
    return processError("unload returned with registered, retiring or "
                        "executing process callbacks");
  return llvm::Error::success();
}
llvm::Error KernelModel::canReleaseProcessNotifyStorage(uint64_t Base,
                                                        uint64_t Size) const {
  for (const auto &[ID, R] : ProcessNotifiers)
    if (R.Routine >= Base && R.Routine - Base < Size)
      return processError(
          "cannot release registered or retiring process callback code");
  return llvm::Error::success();
}
} // namespace neverd::emulation
