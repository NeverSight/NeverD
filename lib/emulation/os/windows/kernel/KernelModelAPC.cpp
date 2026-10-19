//===- KernelModelAPC.cpp - Guest thread APC state ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Critical and guarded regions share the current guest thread's APC state.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error apcError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

llvm::Expected<KernelModel::ApcState *> KernelModel::apcStateForCall() {
  if (!CurrentExecution || !CurrentThreadKey)
    return apcError("APC state requires an active guest thread");
  return &ApcStates[CurrentThreadKey];
}

llvm::Expected<uint64_t> KernelModel::apcsDisabled() {
  auto State = apcStateForCall();
  if (!State)
    return State.takeError();
  const bool PassiveInterrupt = std::any_of(
      PassiveInterruptThreads.begin(), PassiveInterruptThreads.end(),
      [&](const auto &Entry) { return Entry.second == CurrentThreadKey; });
  return uint64_t((*State)->CriticalDepth || (*State)->GuardedDepth ||
                  Dispatcher.ownsMutex(CurrentThreadKey) || PassiveInterrupt);
}

llvm::Expected<uint64_t> KernelModel::allApcsDisabled() {
  auto State = apcStateForCall();
  if (!State)
    return State.takeError();
  return uint64_t((*State)->GuardedDepth || CurrentIRQL >= windows::APCLevel);
}

llvm::Expected<uint64_t> KernelModel::enterApcRegion(ApcRegionKind Kind) {
  auto State = apcStateForCall();
  if (!State)
    return State.takeError();
  const bool Critical = Kind == ApcRegionKind::Critical;
  auto &Depth = Critical ? (*State)->CriticalDepth : (*State)->GuardedDepth;
  if (Depth == profile::MaxAPCRegionNesting)
    return apcError(Critical ? "critical-region nesting limit exceeded"
                             : "guarded-region nesting limit exceeded");
  ++Depth;
  return 0;
}

llvm::Expected<uint64_t> KernelModel::leaveApcRegion(ApcRegionKind Kind) {
  auto State = apcStateForCall();
  if (!State)
    return State.takeError();
  const bool Critical = Kind == ApcRegionKind::Critical;
  auto &Depth = Critical ? (*State)->CriticalDepth : (*State)->GuardedDepth;
  if (Critical)
    for (const auto &[ID, N] : ThreadNotifications)
      if (N.Active && N.Thread == CurrentThreadKey &&
          Depth <= N.Baseline.CriticalDepth + 1)
        return apcError("thread notify callback cannot consume its implicit "
                        "critical region");
  if (Critical && ProcessNotificationState &&
      ProcessNotificationState->Active &&
      ProcessNotificationState->Thread == CurrentThreadKey &&
      Depth <= ProcessNotificationState->Baseline.CriticalDepth + 1)
    return apcError(
        "process notify callback cannot consume its implicit critical region");
  if (!Depth)
    return apcError(Critical ? "KeLeaveCriticalRegion has no matching entry"
                             : "KeLeaveGuardedRegion has no matching entry");
  if (auto E = validateFastMutexContext(
          CurrentIRQL, (*State)->CriticalDepth - (Critical ? 1 : 0),
          (*State)->GuardedDepth - (Critical ? 0 : 1)))
    return E;
  if (auto E = validateExecutiveResourceContext(
          CurrentIRQL, (*State)->CriticalDepth - (Critical ? 1 : 0),
          (*State)->GuardedDepth - (Critical ? 0 : 1)))
    return E;
  --Depth;
  return 0;
}
} // namespace neverd::emulation
