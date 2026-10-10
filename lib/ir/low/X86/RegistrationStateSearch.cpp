//===- RegistrationStateSearch.cpp - PE32 catch search contexts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

namespace neverd::registration_state {

size_t retainedCxxCatchDepth(const Domain::CatchStack &Stack,
                             const CxxExceptionInfo &Cxx, uint32_t TryIndex) {
  size_t Depth = Stack.size();
  const auto &Target = Cxx.TryBlocks[TryIndex];
  // A guard searches tries inside its catch. If none matches, search proceeds
  // to the next registration; selecting an enclosing try unwinds every guard
  // above it. Those callbacks will not return to their old continuations.
  while (Depth &&
         Target.TryLow <= Cxx.TryBlocks[Stack[Depth - 1].TryIndex].TryHigh)
    --Depth;
  return Depth;
}

bool RegistrationStateSolver::enterCxxCatch(Domain &Root, const Domain &Source,
                                            uint32_t TryIndex,
                                            uint32_t CatchIndex) {
  const int32_t Slot = *Chain.RegistrationOffset - 4;
  const FrameValue Current = Source.Frame.load(Slot, 4);
  std::optional<FrameValue> Restored;
  auto Enter = [&](Domain::CatchStack Stack, FrameValue Saved) {
    Stack.push_back({TryIndex, CatchIndex, Saved.Offset});
    Root.CxxCatchStacks.insert(std::move(Stack));
    Restored = Restored ? registration_state::join(*Restored, Saved) : Saved;
  };
  if (Source.Parent && !Source.Callback) {
    Enter({}, Current);
  } else if (!Source.Parent && Source.Callback && !Source.OtherCallback &&
             !Source.CxxCatchStacks.empty()) {
    for (auto Stack : Source.CxxCatchStacks) {
      if (!charge(Stack.size() + 1))
        return false;
      const size_t Retained = retainedCxxCatchDepth(Stack, *EH.Cxx, TryIndex);
      FrameValue Saved = Current;
      if (Retained < Stack.size()) {
        // Windows restores the last exited catch guard's pre-entry SavedESP
        // before the enclosing catch can resume. The current cell can name
        // a discarded callback allocation and cannot supply that snapshot.
        Saved = {{}, {}, false, true};
        Saved.Offset = Stack[Retained].SavedStackOffset;
      }
      Stack.resize(Retained);
      Enter(std::move(Stack), Saved);
    }
  } else {
    Enter({}, Current);
    Root.Unknown = true;
  }
  Root.Frame.store(Slot, 4, *Restored);
  return true;
}

std::vector<RegistrationCxxSearch>
RegistrationStateSolver::cxxSearches(size_t I, const Domain &State) {
  if (!EH.Cxx || !State.CanDispatch || !State.Installed)
    return {};
  const auto &Cxx = *EH.Cxx;
  const auto &Levels =
      State.Unknown || Facts[I].Invalid ? AllLevels : State.Levels;
  std::set<RegistrationCxxSearch> Searches;
  for (int32_t Level : Levels)
    for (uint32_t Index = 0; Index < Cxx.TryBlocks.size(); ++Index) {
      if (!charge(1))
        return {};
      const auto &Try = Cxx.TryBlocks[Index];
      if (Level < Try.TryLow || Level > Try.TryHigh)
        continue;
      if (State.Parent || State.OtherCallback || State.CxxCatchStacks.empty())
        Searches.insert({Level, Index, 0});
      for (const auto &Stack : State.CxxCatchStacks) {
        if (!charge(Stack.size() + 1))
          return {};
        const size_t Retained = retainedCxxCatchDepth(Stack, Cxx, Index);
        Searches.insert({Level, Index, uint32_t(Stack.size() - Retained)});
      }
    }
  if (!charge(Searches.size()))
    return {};
  return {Searches.begin(), Searches.end()};
}

} // namespace neverd::registration_state
