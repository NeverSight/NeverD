//===- RegistrationCxxUnwind.cpp - PE32 source unwind destinations -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/Limits.h"
#include "neverd/backend/Limits.h"
#include "neverd/backend/llvm/X86/X86RegistrationCxxUnwind.h"
#include "neverd/loader/ExceptionInfo.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd {
std::optional<std::vector<X86RegistrationCxxUnwindTarget>>
projectX86RegistrationCxxUnwind(const CxxExceptionInfo &Cxx) {
  const bool CleanupOnly = Cxx.TryBlocks.empty();
  if (Cxx.UnwindMap.size() >
          (CleanupOnly
               ? limits::kMaxRegistrationEHRecords
               : ::neverd::limits::backend::
                     kRegistrationCxxUnwindProjectX86RegistrationCxxUnwindMapSizeLimit) ||
      Cxx.TryBlocks.size() >
          ::neverd::limits::backend::
              kRegistrationCxxUnwindProjectX86RegistrationCxxUnwindCxxTryBlocksSizeLimit ||
      !Cxx.hasValidStateGraph())
    return std::nullopt;
  if (CleanupOnly &&
      std::none_of(Cxx.UnwindMap.begin(), Cxx.UnwindMap.end(),
                   [](const auto &Action) { return Action.ActionVA != 0; }))
    return std::nullopt;
  using Target = X86RegistrationCxxUnwindTarget;
  std::map<int32_t, uint32_t> Tries;
  std::set<int32_t> Boundaries;
  size_t Handlers = 0;
  for (uint32_t Index = 0; Index < Cxx.TryBlocks.size(); ++Index) {
    const auto &Try = Cxx.TryBlocks[Index];
    if (Try.Handlers.empty() ||
        Try.Handlers.size() > limits::kMaxRegistrationEHRecords - Handlers ||
        Try.CatchHigh < Try.TryHigh + 1 || Cxx.UnwindMap[Try.TryLow].ActionVA ||
        Cxx.UnwindMap[Try.TryHigh + 1].ActionVA ||
        !Boundaries.insert(Try.TryLow).second ||
        !Boundaries.insert(Try.TryHigh + 1).second ||
        !Tries.emplace(Try.TryLow, Index).second)
      return std::nullopt;
    Handlers += Try.Handlers.size();
    for (uint32_t Other = 0; Other < Index; ++Other) {
      const auto &Outer = Cxx.TryBlocks[Other];
      const bool Disjoint =
          Try.CatchHigh < Outer.TryLow || Outer.CatchHigh < Try.TryLow;
      auto Contains = [](const auto &Parent, const auto &Child) {
        return (Child.TryLow > Parent.TryLow &&
                Child.CatchHigh <= Parent.TryHigh) ||
               (Child.TryLow > Parent.TryHigh + 1 &&
                Child.CatchHigh <= Parent.CatchHigh);
      };
      const bool Inside = Contains(Outer, Try);
      const bool Outside = Contains(Try, Outer);
      // FuncInfo searches inner rows before their enclosing try or catch.
      if ((!Disjoint && !Inside && !Outside) || Inside)
        return std::nullopt;
    }
  }
  std::vector<Target> Result(Cxx.UnwindMap.size());
  std::vector<uint32_t> CleanupDepth(CleanupOnly ? Result.size() : 0);
  for (uint32_t State = 0; State < Cxx.UnwindMap.size(); ++State) {
    const auto &Action = Cxx.UnwindMap[State];
    if (Action.ToState < -1 || Action.ToState >= int32_t(State))
      return std::nullopt;
    if (const auto Try = Tries.find(State); Try != Tries.end())
      Result[State] = {Target::Kind::Try, Try->second};
    else if (Action.ActionVA)
      Result[State] = {Target::Kind::Cleanup, State};
    else if (Action.ToState >= 0)
      Result[State] = Result[Action.ToState];
    if (CleanupOnly) {
      // Every edge descends, so its complete action depth is already known.
      // Keep native funclet recursion bounded independently of the number of
      // disjoint lifetimes. Empty states do not consume a native funclet.
      CleanupDepth[State] =
          unsigned(Action.ActionVA != 0) +
          (Action.ToState < 0 ? 0 : CleanupDepth[Action.ToState]);
      if (CleanupDepth[State] > 128)
        return std::nullopt;
    }
  }
  // With no local search regions, the checked descending action graph is the
  // whole projection. Rewalking each suffix would make a wide graph quadratic.
  if (CleanupOnly)
    return Result;
  // Every dispatch chain must search exactly the containing source tries,
  // innermost first. A state edge may not skip an outer language region.
  for (uint32_t State = 0; State < Result.size(); ++State) {
    std::vector<uint32_t> Expected, Actual;
    for (const auto &[Low, Index] : Tries)
      if (State >= uint32_t(Low) &&
          State <= uint32_t(Cxx.TryBlocks[Index].TryHigh))
        Expected.insert(Expected.begin(), Index);
    int32_t Walk = State;
    for (size_t Step = 0; Walk >= 0 && Step < Result.size(); ++Step) {
      const auto &Next = Result[Walk];
      if (Next.TargetKind == Target::Kind::Caller)
        break;
      if (Next.TargetKind == Target::Kind::Try) {
        Actual.push_back(Next.Index);
        Walk = Cxx.UnwindMap[Cxx.TryBlocks[Next.Index].TryLow].ToState;
      } else {
        if (Expected.empty())
          return std::nullopt;
        Walk = Cxx.UnwindMap[Next.Index].ToState;
      }
    }
    if (Expected != Actual)
      return std::nullopt;
  }
  return Result;
}
} // namespace neverd
