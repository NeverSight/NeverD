//===- RegistrationStateResult.cpp - x86 EH proof publication -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <utility>

namespace neverd::registration_state {

RegistrationStateAnalysis RegistrationStateSolver::finish() {
  if (Exhausted) {
    Result.Diagnostics.push_back(
        "registration-state propagation budget exhausted");
    return std::move(Result);
  }
  if (!ProvenInstallation) {
    Result.Diagnostics.push_back(
        "registration installation was not proven by the lifted code");
    return std::move(Result);
  }

  Result.Complete = Result.RegistrationLifetimeComplete = true;
  for (const auto &[Identity, Continuation] : CxxContinuations) {
    Result.CxxContinuations.push_back(Continuation);
    CompleteCxxContinuations &= Entries.count(Continuation.TargetVA) != 0;
  }
  Result.CxxContinuationsComplete = CompleteCxxContinuations;
  for (size_t I = 0; I < Function.Blocks.size(); ++I) {
    const LowBlock &Block = Function.Blocks[I];
    const Domain &State = Incoming[I];
    const bool CallbackOnly = State.Callback && !State.Parent;
    const bool Unknown =
        State.Reached && (State.Unknown || Facts[I].Invalid ||
                          (State.Callback && State.Parent) ||
                          (State.Uninstalled && State.Installed));
    const std::set<int32_t> &Levels = Unknown ? AllLevels : State.Levels;
    auto Searches = cxxSearches(I, State);
    if (Exhausted || !charge(Levels.size() + State.CxxCatchStacks.size() + 1)) {
      Result.Complete = false;
      Result.Blocks.clear();
      Result.Diagnostics.push_back(
          "registration-state output budget exhausted");
      return std::move(Result);
    }
    Result.Blocks.push_back({Block.Id,
                             {Block.StartAddr, Block.EndAddr},
                             {Levels.begin(), Levels.end()},
                             Unknown,
                             CallbackOnly,
                             State.CanDispatch && State.Installed,
                             EH.Cxx ? cxxMinimumTryLevel(State, *EH.Cxx) : 0,
                             State.Reached,
                             std::move(Searches)});
    if (CallbackOnly && Unknown)
      Result.CallbackStatesComplete = false;
    if (!CallbackOnly && Unknown)
      Result.Complete = false;
    if (State.Parent && State.Reached &&
        (Unknown || (Block.Succs.empty() && Facts[I].InstalledAtExit &&
                     !Facts[I].NoReturnAtExit)))
      Result.RegistrationLifetimeComplete = false;
    if (!State.CxxCatchStacks.empty() && State.Reached && Block.Succs.empty() &&
        !Facts[I].CxxContinuationAtExit && !Facts[I].NoReturnAtExit)
      Result.CxxContinuationsComplete = false;
  }
  Result.RegistrationLifetimeComplete &= Result.CxxContinuationsComplete;
  if (!Result.CxxContinuationsComplete) {
    Result.Complete = Result.CallbackStatesComplete = false;
    Result.Diagnostics.push_back(
        "C++ catch continuation or restored stack is not proven");
  }
  if (!Result.Complete)
    Result.Diagnostics.push_back(
        "registration state has an unproven transition");
  Result.ChainOperationsComplete = CompleteChainOperations && Result.Complete &&
                                   Result.CallbackStatesComplete &&
                                   Result.RegistrationLifetimeComplete;
  if (Result.ChainOperationsComplete)
    for (const auto &[Identity, Access] : ChainAccesses)
      Result.ChainAccesses.push_back(Access);
  Result.IncomingFrameAccessesComplete = ConsistentIncomingAccesses;
  Result.CallFrameEffectsComplete = CompleteCalls && Result.Complete &&
                                    Result.CallbackStatesComplete &&
                                    Result.RegistrationLifetimeComplete;
  for (const auto &[Identity, Call] : CallEffects)
    Result.CallFrameEffects.push_back(Call);
  if (CheckCalls && !Result.CallFrameEffectsComplete)
    Result.Diagnostics.push_back(
        "registration call stack or initialized object borrow is not proven");
  Result.CxxCatchObjectsComplete = CompleteCatchObjects && Result.Complete &&
                                   Result.CallbackStatesComplete &&
                                   Result.RegistrationLifetimeComplete &&
                                   !Exhausted;
  if (Result.CxxCatchObjectsComplete)
    for (const auto &[Identity, Object] : CatchObjects)
      Result.CxxCatchObjects.push_back(Object);
  Result.RuntimeObjectAccessesComplete = Result.CxxCatchObjectsComplete &&
                                         CompleteRuntimeObjects &&
                                         Result.CallFrameEffectsComplete;
  if (Result.RuntimeObjectAccessesComplete)
    for (const auto &[Identity, Access] : RuntimeAccesses)
      Result.RuntimeObjectAccesses.push_back(Access);
  if (CheckRuntimeObjects && !Result.CxxCatchObjectsComplete)
    Result.Diagnostics.push_back("C++ runtime catch object is not proven");
  if (CheckRuntimeObjects && !Result.RuntimeObjectAccessesComplete)
    Result.Diagnostics.push_back(
        "C++ runtime exception object access is not proven");
  Result.CleanupFrameEffectsComplete =
      CompleteCleanups && Result.Complete && Result.CallbackStatesComplete &&
      Result.RegistrationLifetimeComplete && !Exhausted;
  if (Result.CleanupFrameEffectsComplete)
    for (const auto &[Identity, Effect] : CleanupEffects)
      Result.CleanupFrameEffects.push_back(Effect);
  if (CheckCleanups && !Result.CleanupFrameEffectsComplete)
    Result.Diagnostics.push_back(
        "registration cleanup initialized object borrow is not proven");
  Result.ImageReadsComplete = Result.Complete &&
                              Result.CallbackStatesComplete &&
                              CompleteImageReads && !Exhausted;
  Result.SecurityCookiesComplete =
      CompleteCookies && Result.Complete && Result.CallbackStatesComplete &&
      Result.RegistrationLifetimeComplete && Result.ChainOperationsComplete &&
      CookieChecks.size() == CookieCheckOccurrences.size() &&
      (!ReadsGSCookie || !CookieChecks.empty());
  if (Result.SecurityCookiesComplete) {
    Result.SecurityCookieVA = SecurityCookieVA;
    Result.CookieCheckVA = CookieCheckVA;
    for (const auto &[Identity, Check] : CookieChecks)
      Result.CookieChecks.push_back(Check);
  }
  if (Result.ImageReadsComplete)
    for (const auto &[Begin, End] : ImageReads)
      Result.ImageReads.push_back({Begin, End});
  if (Result.Complete && Result.CallbackStatesComplete &&
      ConsistentIncomingAccesses)
    for (const auto &[Identity, Access] : IncomingAccesses)
      if (Access)
        Result.IncomingFrameAccesses.push_back(*Access);
  if (!ConsistentIncomingAccesses)
    Result.Diagnostics.push_back(
        "incoming caller frame projection is not exact");
  if (Result.Complete && Result.CallbackStatesComplete) {
    if (!charge(FrameValues.size())) {
      Result.Complete = Result.ChainOperationsComplete =
          Result.ImageReadsComplete = Result.SecurityCookiesComplete =
              Result.CallFrameEffectsComplete =
                  Result.CleanupFrameEffectsComplete =
                      Result.CxxCatchObjectsComplete =
                          Result.RuntimeObjectAccessesComplete = false;
      Result.CallFrameEffects.clear();
      Result.CleanupFrameEffects.clear();
      Result.CxxCatchObjects.clear();
      Result.RuntimeObjectAccesses.clear();
      Result.SecurityCookieVA = 0;
      Result.CookieCheckVA = 0;
      Result.CookieChecks.clear();
      Result.ChainAccesses.clear();
      Result.Diagnostics.push_back(
          "registration-frame output budget exhausted");
      return std::move(Result);
    }
    for (const auto &[Identity, Value] : FrameValues)
      Result.FrameValues.push_back(
          {Identity.first, Identity.second, Value.Offset});
  }
  return std::move(Result);
}

} // namespace neverd::registration_state
