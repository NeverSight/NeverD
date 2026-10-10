//===- RegistrationStateUnwind.cpp - x86 EH unwind propagation ------------===//
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

void RegistrationStateSolver::dispatchBlock(size_t I, const Domain &Before) {
  const LowBlock &Block = Function.Blocks[I];
  const int32_t MinimumTry = EH.Cxx ? cxxMinimumTryLevel(Before, *EH.Cxx) : 0;
  // A filter is a searching callback, while a finally is an unwind callback
  // at its enclosing state. Lexical callback exclusion must not erase an
  // outer exception dispatch when that finally itself faults.
  if (!Before.CanDispatch || !Before.Installed)
    return;
  const std::set<int32_t> &DispatchLevels =
      Before.Unknown || Facts[I].Invalid ? AllLevels : Before.Levels;
  for (int32_t Level : DispatchLevels) {
    if (!charge(1))
      break;
    int32_t Walk = Level;
    for (size_t Step = 0; Step < Chain.Scopes.size(); ++Step) {
      if (!charge(1) || Walk < 0 ||
          static_cast<size_t>(Walk) >= Chain.Scopes.size())
        break;
      const RegistrationScopeRecord &Scope = Chain.Scopes[Walk];
      dispatch(Scope.FilterVA, Level, Before, Facts[I].Invalid, true, true);
      dispatch(Scope.HandlerVA, Scope.EnclosingLevel, Before, Facts[I].Invalid,
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
      const CxxUnwindAction &Action = Cxx.UnwindMap[Walk];
      if (CheckCleanups && Action.ActionVA) {
        const CleanupKey Identity{Block.Id, Level, uint32_t(Walk)};
        const auto Contract = CleanupIndices.find(uint32_t(Walk));
        auto SP = Before.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                      .Offset;
        if (!Before.Parent && Before.Callback && !Before.OtherCallback &&
            !Before.CxxCatchStacks.empty())
          SP = Before.Frame.load(*Chain.RegistrationOffset - 4, 4).Offset;
        bool Valid = !Before.Unknown && !Facts[I].Invalid &&
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
          const auto Offset = Chain.cxxSourceFrameOffset(C.ObjectFrameOffset);
          Valid &= Offset &&
                   projectFrameObject(Before, *Offset, SP, C.Leaf.ECXReads,
                                      Effect.FrameReads, true) &&
                   projectFrameObject(Before, *Offset, SP, C.Leaf.ECXWrites,
                                      Effect.FrameWrites, false);
          if (Valid && charge(C.Leaf.ImageReads.size()))
            for (const auto &Read : C.Leaf.ImageReads)
              ImageReads.emplace(Read.Begin, Read.End);
          else
            Valid = false;
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
      dispatch(Action.ActionVA, Action.ToState, Before, true, true);
      Walk = Action.ToState;
    }
    for (uint32_t TryIndex = 0; TryIndex < Cxx.TryBlocks.size(); ++TryIndex) {
      const CxxTryBlock &Try = Cxx.TryBlocks[TryIndex];
      if (!charge(1))
        break;
      if (Try.TryLow >= MinimumTry && Level >= Try.TryLow &&
          Level <= Try.TryHigh)
        for (uint32_t CatchIndex = 0; CatchIndex < Try.Handlers.size();
             ++CatchIndex)
          dispatch(Try.Handlers[CatchIndex].HandlerVA, Try.TryHigh + 1, Before,
                   Facts[I].Invalid, true, false,
                   std::make_pair(TryIndex, CatchIndex));
    }
  }
}

} // namespace neverd::registration_state
