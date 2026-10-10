//===- COFFRegistrationCxxStateProof.cpp - Independent state-map proof ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "COFFRegistrationCxxStateProof.h"

#include "COFFRegistrationIRProof.h"

#include "neverd/backend/codegen/BinaryRewriter.h"

#include "llvm/Support/Endian.h"

#include <functional>

namespace neverd::coff_registration {
llvm::Error validateCxxGeneratedStates(
    const CxxExceptionInfo &Source, const CompiledImage &Compiled,
    const std::vector<CxxGeneratedTry> &Tries, va_t Unwind, uint32_t MaxState,
    const std::map<uint32_t, const CompiledWinEHSemanticRecord *> &Cleanups,
    COFFRegistrationCxxTableReceipt &Receipt) {
  if (Tries.size() != Source.TryBlocks.size() || !MaxState || MaxState > 128)
    return rejectIR("C++ generated try states have no complete source set");
  auto Word = [&](va_t VA) {
    return llvm::support::endian::read32le(Compiled.Bytes.data() + VA -
                                           Compiled.BaseVA);
  };
  auto &Mapping = Receipt.SourceToGeneratedStates;
  Mapping.emplace(-1, -1);
  std::map<int32_t, int32_t> GeneratedSources;
  auto Bind = [&](int32_t Native, int32_t Generated) {
    return Generated >= 0 && uint32_t(Generated) < MaxState &&
           Mapping.emplace(Native, Generated).second &&
           GeneratedSources.emplace(Generated, Native).second;
  };
  for (uint32_t Index = 0; Index < Tries.size(); ++Index) {
    const auto &Original = Source.TryBlocks[Index];
    const auto &Generated = Tries[Index];
    if (Generated.Low < 0 || Generated.Low > Generated.High ||
        Generated.High >= Generated.CatchHigh ||
        Generated.CatchHigh >= int32_t(MaxState) ||
        !Bind(Original.TryLow, Generated.Low) ||
        !Bind(Original.CatchHigh, Generated.CatchHigh))
      return rejectIR("C++ try and catch dispatch states conflict");
    for (uint32_t Other = 0; Other < Index; ++Other)
      if (Original.TryLow < Source.TryBlocks[Other].TryLow &&
          Original.TryHigh >= Source.TryBlocks[Other].CatchHigh &&
          Generated.RowVA <= Tries[Other].RowVA)
        return rejectIR("C++ nested catch search order changed");
  }
  size_t Actions = 0;
  for (uint32_t State = 0; State < Source.UnwindMap.size(); ++State) {
    if (!Source.UnwindMap[State].ActionVA)
      continue;
    ++Actions;
    const auto Row = Cleanups.find(State);
    if (Row == Cleanups.end() || !Bind(State, Row->second->GeneratedState))
      return rejectIR("C++ source cleanup lost its generated state");
  }
  if (Actions != Cleanups.size() || GeneratedSources.size() != MaxState)
    return rejectIR("C++ unwind map contains an unbound generated state");
  std::function<std::optional<int32_t>(int32_t)> Project =
      [&](int32_t State) -> std::optional<int32_t> {
    const auto Found = Mapping.find(State);
    if (Found != Mapping.end())
      return Found->second;
    if (State < 0 || uint32_t(State) >= Source.UnwindMap.size())
      return std::nullopt;
    const auto &Action = Source.UnwindMap[State];
    if (Action.ActionVA || Action.ToState < -1 || Action.ToState >= State)
      return std::nullopt;
    auto Outer = Project(Action.ToState);
    if (Outer)
      Mapping.emplace(State, *Outer);
    return Outer;
  };
  for (uint32_t State = 0; State < Source.UnwindMap.size(); ++State) {
    const auto Generated = Project(State);
    if (!Generated || *Generated < -1 || *Generated >= int32_t(MaxState))
      return rejectIR("C++ source state has no exact generated projection");
    for (uint32_t Index = 0; Index < Tries.size(); ++Index) {
      const auto &Original = Source.TryBlocks[Index];
      const auto &Try = Tries[Index];
      const bool SourceProtected = State >= uint32_t(Original.TryLow) &&
                                   State <= uint32_t(Original.TryHigh);
      const bool GeneratedProtected =
          *Generated >= Try.Low && *Generated <= Try.High;
      if (SourceProtected != GeneratedProtected)
        return rejectIR("C++ generated try changed its protected state set");
    }
  }
  for (uint32_t State = 0; State < MaxState; ++State) {
    const auto Found = GeneratedSources.find(State);
    if (Found == GeneratedSources.end())
      return rejectIR("C++ generated unwind state numbering has a hole");
    const auto &Action = Source.UnwindMap[Found->second];
    const auto Outer = Project(Action.ToState);
    if (!Outer || int32_t(Word(Unwind + State * 8)) != *Outer ||
        (!Action.ActionVA && Word(Unwind + State * 8 + 4)))
      return rejectIR(
          "C++ generated unwind edges changed the source action graph");
  }
  return llvm::Error::success();
}
} // namespace neverd::coff_registration
