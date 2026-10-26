//===- RegistrationStateResult.cpp - x86 EH proof publication -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Publish source effects separately from native registration call receipts.
///
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <utility>

namespace neverd::registration_state {

RegistrationStateAnalysis RegistrationStateSolver::finish() {
  if (FailedLocalUnwind) {
    Result.SEHLocalUnwindsComplete = false;
    Result.Diagnostics.push_back(
        "local-unwind frame arguments or finally effects are not proven");
  }
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
  if (Result.SEHInvocationExitsComplete)
    for (const auto &[Identity, Exit] : SEHExits)
      Result.SEHInvocationExits.push_back(Exit);
  std::shared_ptr<const std::vector<int32_t>> UnknownLevels;
  for (size_t I = 0; I < Function.Blocks.size(); ++I) {
    const LowBlock &Block = Function.Blocks[I];
    std::set<int32_t> Levels;
    std::set<RegistrationCxxSearch> Searches;
    std::set<std::vector<std::pair<uint32_t, uint32_t>>> CatchStacks;
    bool Reached = false, Parent = false, Callback = false;
    bool Unknown = false, CanDispatch = false;
    int32_t Minimum = INT32_MAX;
    for (const auto &[Activation, Node] : States[I]) {
      const Domain &State = Node.Incoming;
      for (const auto &Stack : State.CxxCatchStacks) {
        if (!charge(Stack.size() + 1)) {
          Result.Complete = Result.RegistrationLifetimeComplete = false;
          Result.Blocks.clear();
          Result.Diagnostics.push_back(
              "registration catch output budget exhausted");
          return std::move(Result);
        }
        std::vector<std::pair<uint32_t, uint32_t>> Identities;
        for (const auto &Context : Stack)
          Identities.emplace_back(Context.TryIndex, Context.CatchIndex);
        CatchStacks.insert(std::move(Identities));
      }
      const BlockFacts &Facts = Node.Facts;
      const bool Unproven =
          State.Reached &&
          (State.Unknown || Facts.Invalid || (State.Callback && State.Parent) ||
           (State.Uninstalled && State.Installed));
      auto ContextSearches = cxxSearches(State, Facts.Invalid);
      if (!charge(State.CxxCatchStacks.size() + ContextSearches.size() + 1))
        break;
      // Unknown already denotes the full domain. Keep the invocation's other
      // facts separate without materializing that same set for this join.
      if (!Unknown && !Unproven) {
        if (!charge(State.Levels.size()))
          break;
        Levels.insert(State.Levels.begin(), State.Levels.end());
      }
      Searches.insert(ContextSearches.begin(), ContextSearches.end());
      Reached |= State.Reached;
      Parent |= State.Parent;
      Callback |= State.Callback;
      Unknown |= Unproven;
      CanDispatch |= State.CanDispatch && State.Installed;
      Minimum =
          std::min(Minimum, EH.Cxx ? cxxMinimumTryLevel(State, *EH.Cxx) : 0);
      if (State.Callback && !State.Parent && Unproven)
        Result.CallbackStatesComplete = false;
      if (SEHControls && !KnownCxx && State.Callback && State.Reached &&
          Block.Succs.empty() && !Facts.CallbackReturnAtExit &&
          !Facts.InvocationAtExit && !Facts.NoReturnAtExit)
        Result.SEHInvocationExitsComplete = false;
      if (State.Parent && State.Reached &&
          (Unproven || (Block.Succs.empty() && Facts.InstalledAtExit &&
                        !Facts.NoReturnAtExit && !Facts.InvocationAtExit)))
        Result.RegistrationLifetimeComplete = false;
      if (!State.CxxCatchStacks.empty() && State.Reached &&
          Block.Succs.empty() && !Facts.CxxContinuationAtExit &&
          !Facts.NoReturnAtExit)
        Result.CxxContinuationsComplete = false;
    }
    // Context separation proves each runtime return independently. It does
    // not grant a parent/callback bridge at shared ordinary code; that still
    // needs an explicit invocation-exit contract consumed by the CFG/emitters.
    if (Parent && Callback) {
      Unknown = true;
      Result.CallbackStatesComplete = Result.RegistrationLifetimeComplete =
          false;
    }
    const bool CallbackOnly = Callback && !Parent;
    if (!CallbackOnly && Unknown)
      Result.Complete = false;
    const size_t LevelWork =
        Unknown ? (UnknownLevels ? 0 : AllLevels.size()) : Levels.size();
    if (Exhausted || !charge(LevelWork + Searches.size() + 1)) {
      Result.Complete = false;
      Result.Blocks.clear();
      Result.Diagnostics.push_back(
          "registration-state output budget exhausted");
      return std::move(Result);
    }
    std::vector<int32_t> ExplicitLevels;
    if (Unknown) {
      if (!UnknownLevels)
        UnknownLevels = std::make_shared<const std::vector<int32_t>>(
            AllLevels.begin(), AllLevels.end());
    } else
      ExplicitLevels.assign(Levels.begin(), Levels.end());
    Result.Blocks.push_back({Block.Id,
                             {Block.StartAddr, Block.EndAddr},
                             std::move(ExplicitLevels),
                             Unknown,
                             CallbackOnly,
                             CanDispatch,
                             Minimum == INT32_MAX ? 0 : Minimum,
                             Reached,
                             {Searches.begin(), Searches.end()},
                             {CatchStacks.begin(), CatchStacks.end()},
                             Unknown ? UnknownLevels : nullptr});
  }
  Result.RegistrationLifetimeComplete &= Result.CxxContinuationsComplete;
  if (!Result.SEHInvocationExitsComplete) {
    Result.SEHInvocationExits.clear();
    Result.Complete = Result.CallbackStatesComplete =
        Result.RegistrationLifetimeComplete = false;
    Result.Diagnostics.push_back(
        "SEH invocation exit or parent stack is not proven");
  }
  // Receipts gathered on an earlier visit are provisional. A later reaching
  // state with an expired binding revokes only that same source occurrence.
  for (const auto &Identity : InvalidRuntimeBindings) {
    if (!charge(1))
      break;
    if (BoundCalls.count(Identity))
      Result.SEHBoundCallsComplete = false;
    if (LocalUnwindCalls.count(Identity))
      Result.SEHLocalUnwindsComplete = false;
    if (JumpCaptureOccurrences.count(Identity))
      Result.SEHJumpCapturesComplete = false;
  }
  for (const auto &[Identity, Resume] : JumpResumptions)
    if (!charge(1) || ModifiedCallbackCaptures.count(Resume.CaptureCall))
      Result.SEHJumpResumptionsComplete = false;
  if (!Result.SEHJumpCapturesComplete) {
    Result.SEHJumpResumptionsComplete = false;
    Result.Complete = Result.CallbackStatesComplete =
        Result.RegistrationLifetimeComplete = false;
    Result.Diagnostics.push_back(
        "SEH jump capture has conflicting source facts");
  }
  const bool FailedJump = !Result.SEHJumpResumptionsComplete;
  Result.SEHBoundCallsComplete &=
      Result.Complete && Result.CallbackStatesComplete &&
      Result.RegistrationLifetimeComplete && !Exhausted;
  if (Result.SEHBoundCallsComplete && charge(BoundCalls.size()))
    for (const auto &[Identity, Call] : BoundCalls)
      Result.SEHBoundCalls.push_back(Call);
  Result.SEHLocalUnwindsComplete &=
      Result.Complete && Result.CallbackStatesComplete &&
      Result.RegistrationLifetimeComplete && !Exhausted;
  if (Result.SEHLocalUnwindsComplete && charge(LocalUnwindCalls.size()))
    for (const auto &[Identity, Call] : LocalUnwindCalls)
      Result.SEHLocalUnwinds.push_back(Call);
  Result.SEHJumpResumptionsComplete &=
      Result.Complete && Result.CallbackStatesComplete;
  if (Result.SEHJumpResumptionsComplete)
    for (const auto &[Identity, Resume] : JumpResumptions)
      Result.SEHJumpResumptions.push_back(Resume);
  else if (FailedJump || !JumpResumptions.empty()) {
    Result.Complete = Result.CallbackStatesComplete =
        Result.RegistrationLifetimeComplete = false;
    Result.Diagnostics.push_back("SEH jump buffer or resumption is not proven");
  }
  Result.SEHJumpCapturesComplete &=
      Result.Complete && Result.CallbackStatesComplete &&
      Result.SEHJumpResumptionsComplete && !Exhausted;
  if (Result.SEHJumpCapturesComplete) {
    if (charge(JumpCaptureOccurrences.size()))
      for (const auto &[Identity, Capture] : JumpCaptureOccurrences)
        Result.SEHJumpCaptures.push_back(Capture);
  }
  if (!Result.CxxContinuationsComplete) {
    Result.Complete = Result.CallbackStatesComplete = false;
    Result.Diagnostics.push_back(
        "C++ catch continuation or restored stack is not proven");
  }
  Result.SEHExceptionAccessesComplete &=
      Result.Complete && Result.CallbackStatesComplete && !Exhausted;
  if (Result.SEHExceptionAccessesComplete)
    for (const auto &[Identity, Access] : SEHExceptionAccesses) {
      if (!charge(1))
        break;
      if (Access)
        Result.SEHExceptionAccesses.push_back(*Access);
    }
  if (!Result.SEHExceptionAccessesComplete)
    Result.SEHExceptionAccesses.clear();
  if (!Result.Complete)
    Result.Diagnostics.push_back(
        "registration state has an unproven transition");
  Result.ChainOperationsComplete = CompleteChainOperations && Result.Complete &&
                                   Result.CallbackStatesComplete &&
                                   Result.RegistrationLifetimeComplete;
  if (Result.ChainOperationsComplete)
    for (const auto &[Identity, Access] : ChainAccesses)
      Result.ChainAccesses.push_back(Access);
  if (Result.ChainOperationsComplete)
    Result.CxxPrologue = CxxPrologueEffect;
  if (Result.ChainOperationsComplete)
    Result.SEH4Prologue = SEH4PrologueEffect;
  Result.IncomingFrameAccessesComplete = ConsistentIncomingAccesses;
  Result.SourceCallFrameEffectsComplete = CompleteCalls && Result.Complete &&
                                          Result.CallbackStatesComplete &&
                                          Result.RegistrationLifetimeComplete;
  Result.CallFrameEffectsComplete =
      Result.SourceCallFrameEffectsComplete && !HasSourceOnlyCallEffects;
  for (const auto &[Identity, Call] : CallEffects)
    Result.CallFrameEffects.push_back(Call);
  if (CheckCalls && !Result.SourceCallFrameEffectsComplete)
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
                                         Result.SourceCallFrameEffectsComplete;
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
      (!CxxCookies ||
       (Result.SourceCallFrameEffectsComplete &&
        Result.CleanupFrameEffectsComplete && Result.CxxCatchObjectsComplete &&
        Result.RuntimeObjectAccessesComplete)) &&
      hasCompleteCookieChecks() && (!ReadsGSCookie || !CookieChecks.empty());
  if (Result.SecurityCookiesComplete) {
    Result.SecurityCookieVA = SecurityCookieVA;
    Result.CookieCheckVA = CookieCheckVA;
    Result.CxxCookieConditions = CxxCookieConditions;
    for (const auto &[Identity, Check] : CookieChecks)
      Result.CookieChecks.push_back(Check);
  } else if (CxxCookies && !CookieChecks.empty()) {
    // Transfer used these success paths instead of ordinary callee effects.
    // Without published cookie receipts they cannot close the call or runtime
    // memory proof: the original mismatch path has not been summarized.
    Result.SourceCallFrameEffectsComplete = Result.CallFrameEffectsComplete =
        Result.RuntimeObjectAccessesComplete = Result.ImageReadsComplete =
            false;
    Result.RuntimeObjectAccesses.clear();
    Result.Diagnostics.push_back("C++ cookie call success is not proven");
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
              Result.SourceCallFrameEffectsComplete =
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
      Result.CxxCookieConditions.clear();
      Result.ChainAccesses.clear();
      Result.CxxPrologue.reset();
      Result.SEH4Prologue.reset();
      Result.Diagnostics.push_back(
          "registration-frame output budget exhausted");
      return std::move(Result);
    }
    for (const auto &[Identity, Value] : FrameValues)
      if (Value.MayBeFrame)
        Result.FrameValues.push_back(
            {Identity.first, Identity.second, Value.Offset});
  }
  Result.SEHStateStoresComplete &=
      Result.Complete && Result.CallbackStatesComplete &&
      Result.RegistrationLifetimeComplete && !Exhausted;
  if (Result.SEHStateStoresComplete && charge(SEHStateStores.size()))
    for (const auto &[Identity, Store] : SEHStateStores)
      Result.SEHStateStores.push_back(Store);
  return std::move(Result);
}

} // namespace neverd::registration_state
