//===- RegistrationStateUnwind.cpp - x86 EH unwind propagation ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

#include "llvm/ADT/SmallBitVector.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace neverd::registration_state {

bool RegistrationStateSolver::dispatchUnknownCxxCleanups(const Domain &Before) {
  if (!UnknownCxxCleanupRoots) {
    std::set<std::pair<va_t, int32_t>> Roots;
    // An unknown state includes every unwind state. Every action therefore
    // occurs as its own originating state, regardless of shared suffixes.
    // Missing LowIR entries have no dispatch transfer; retain the same
    // filtering as dispatch(), without repeating it at every source point.
    for (const auto &Action : EH.Cxx->UnwindMap) {
      if (!charge(1 + std::bit_width(Entries.size())))
        return false;
      if (Entries.count(Action.ActionVA)) {
        if (!charge(1 + std::bit_width(Roots.size())))
          return false;
        Roots.emplace(Action.ActionVA, Action.ToState);
      }
    }
    if (!charge(Roots.size()))
      return false;
    UnknownCxxCleanupRoots.emplace(Roots.begin(), Roots.end());
  }
  for (const auto &[Address, Level] : *UnknownCxxCleanupRoots) {
    if (!charge(1))
      return false;
    dispatch(Address, {Level}, Before, true, true);
    if (Exhausted)
      return false;
  }
  return true;
}

void RegistrationStateSolver::dispatchBlock(size_t I, const Domain &Before,
                                            const BlockFacts &Facts,
                                            bool PreserveBindings) {
  const LowBlock &Block = Function.Blocks[I];
  // A filter is a searching callback, while a finally is an unwind callback
  // at its enclosing state. Lexical callback exclusion must not erase an
  // outer exception dispatch when that finally itself faults.
  if (!Before.CanDispatch || !Before.Installed)
    return;
  if (!KnownCxx) {
    queueSEHScopes(Before, Facts.Invalid);
    return;
  }
  if (CxxCookies && !cxxCookiesReady(Before))
    CompleteCookies = false;
  if ((Before.Unknown || Facts.Invalid) && !CompleteCleanups &&
      Chain.Scopes.empty()) {
    // No per-origin cleanup effects can be published after their monotone
    // proof failure (or when no proof was requested). The remaining operation
    // is an unchanged frame transfer into each possible callback root.
    if (!dispatchUnknownCxxCleanups(Before))
      return;
  } else {
    const std::set<int32_t> &DispatchLevels =
        Before.Unknown || Facts.Invalid ? AllLevels : Before.Levels;
    // Every originating level sees the same frame snapshot here. A cleanup
    // callback receives its action's ToState, so dispatching that same action
    // again cannot add a different root. Keep per-level object proofs separate
    // while they can still establish the complete cleanup contract.
    const size_t States = EH.Cxx ? EH.Cxx->UnwindMap.size() : 0;
    if (!charge(1 + (States + 63) / 64))
      return;
    llvm::SmallBitVector Dispatched(States);
    for (int32_t Level : DispatchLevels) {
      if (!charge(1))
        break;
      int32_t Walk = Level;
      for (size_t Step = 0; Step < Chain.Scopes.size(); ++Step) {
        if (!charge(1) || Walk < 0 ||
            static_cast<size_t>(Walk) >= Chain.Scopes.size())
          break;
        const RegistrationScopeRecord &Scope = Chain.Scopes[Walk];
        dispatch(Scope.FilterVA, {Level}, Before, Facts.Invalid, true, true);
        dispatch(Scope.HandlerVA, {Scope.EnclosingLevel}, Before, Facts.Invalid,
                 Scope.IsFinally);
        Walk = Scope.EnclosingLevel;
      }
      if (!EH.Cxx || Level < 0 ||
          static_cast<uint32_t>(Level) >= EH.Cxx->MaxState)
        continue;
      const CxxExceptionInfo &Cxx = *EH.Cxx;
      Walk = Level;
      for (size_t Step = 0; Step < Cxx.UnwindMap.size(); ++Step) {
        if (!charge(1) || Walk < 0 ||
            static_cast<size_t>(Walk) >= Cxx.UnwindMap.size())
          break;
        const bool Seen = Dispatched.test(Walk);
        // A failed cleanup proof is monotone and publishes no effects. After
        // that failure (or without a requested proof), a previously visited
        // suffix has already supplied every possible callback root.
        if (Seen && !CompleteCleanups)
          break;
        Dispatched.set(Walk);
        const CxxUnwindAction &Action = Cxx.UnwindMap[Walk];
        if (CheckCleanups && CompleteCleanups && Action.ActionVA) {
          const CleanupKey Identity{Block.Id, Level, uint32_t(Walk)};
          const auto Contract = CleanupIndices.find(uint32_t(Walk));
          const auto SP = parentStackOffset(Before);
          bool Valid = !Before.Unknown && !Facts.Invalid &&
                       Contract != CleanupIndices.end() && SP;
          RegistrationCleanupFrameEffect Effect;
          if (Valid) {
            const auto &C = Result.CleanupContracts[Contract->second];
            Effect.BlockId = Block.Id;
            Effect.Range = {Block.StartAddr, Block.EndAddr};
            Effect.DispatchLevel = Level;
            Effect.ActionState = uint32_t(Walk);
            Effect.CleanupIndex = Contract->second;
            Effect.StackOffset = *SP;
            for (const auto &Call : C.Calls) {
              const auto Offset =
                  Chain.cxxSourceFrameOffset(Call.ObjectFrameOffset);
              Valid &=
                  charge(1) && Offset &&
                  projectFrameObject(Before, *Offset, SP, Call.Leaf.ECXReads,
                                     Effect.FrameReads, true) &&
                  projectFrameObject(Before, *Offset, SP, Call.Leaf.ECXWrites,
                                     Effect.FrameWrites, false);
              if (!Valid || !charge(Call.Leaf.ImageReads.size())) {
                Valid = false;
                break;
              }
              for (const auto &Read : Call.Leaf.ImageReads)
                ImageReads.emplace(Read.Begin, Read.End);
            }
          }
          if (Valid && !InvalidCleanups.count(Identity)) {
            auto [It, Inserted] = CleanupEffects.emplace(Identity, Effect);
            Valid = Inserted || It->second == Effect;
          } else
            Valid = false;
          if (!Valid) {
            InvalidCleanups.insert(Identity);
            CleanupEffects.erase(Identity);
            CompleteCleanups = false;
            CompleteImageReads = false;
          }
        }
        if (!Seen)
          dispatch(Action.ActionVA, {Action.ToState}, Before, true, true);
        Walk = Action.ToState;
      }
    }
  }
  for (const auto &Search : cxxSearches(Before, Facts.Invalid)) {
    const auto &Try = EH.Cxx->TryBlocks[Search.TryIndex];
    for (uint32_t CatchIndex = 0; CatchIndex < Try.Handlers.size();
         ++CatchIndex) {
      if (!charge(1))
        return;
      dispatch(Try.Handlers[CatchIndex].HandlerVA, {Try.TryHigh + 1}, Before,
               Facts.Invalid, true, false,
               std::make_pair(Search.TryIndex, CatchIndex), PreserveBindings);
    }
  }
}

} // namespace neverd::registration_state
