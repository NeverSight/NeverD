//===- MedLLVMRegistrationSEHFrameStores.cpp - Saved stack store bounds ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A saved parent ESP may remain a precise frame address inside a finally.

#include "MedLLVMRegistrationSEHProof.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/X86RegistrationSEH4PhysicalFrame.h"

namespace neverd::x86_registration {

std::optional<SEHFrameOffsets> getSEHFrameOffsets(const MedFunc &Source) {
  if (!Source.RegistrationStates || !Source.RegistrationStates->Complete ||
      !Source.RegistrationStates->CallbackStatesComplete ||
      !Source.RegistrationStates->RegistrationLifetimeComplete ||
      Source.SkippedSSA)
    return std::nullopt;
  const auto *States = getX86RegistrationSEHPhysicalStates(Source);
  if (!States)
    return std::nullopt;
  using Identity = std::pair<va_t, int>;
  using ValueKey = std::tuple<MedVar::VarKind, int, int>;
  const auto Key = [](const MedVar &V) {
    return ValueKey{V.Kind, V.Id, V.SSAVer};
  };
  const auto Valid = [](const MedVar &V) {
    return (V.Kind == MedVar::Reg || V.Kind == MedVar::Temp) && V.Id >= 0 &&
           V.SSAVer > 0 && (V.Size == 4 || V.Size == 8);
  };
  size_t Work = 0;
  std::map<Identity, std::optional<int32_t>> Offsets;
  for (const auto &Value : States->FrameValues) {
    if (++Work > limits::kMaxRegistrationEHStateWork || Value.OpSeq < 0 ||
        !Offsets
             .emplace(Identity{Value.Address, Value.OpSeq},
                      Value.EstablishedFrameOffset)
             .second)
      return std::nullopt;
  }
  struct Definition {
    const MedOp *Operation = nullptr;
    int32_t Offset = 0;
    unsigned Definitions = 0;
  };
  std::map<ValueKey, Definition> Values;
  std::map<ValueKey, std::pair<unsigned, uint16_t>> Definitions;
  std::set<Identity> Seen;
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      const auto Found = Offsets.find({Op.Addr, Op.OriginSeq});
      if (Found == Offsets.end() || !Found->second)
        continue;
      if (!Seen.insert(Found->first).second)
        return std::nullopt;
      if (!Valid(Op.Output) || Op.Dead)
        continue;
      if (!Values.emplace(Key(Op.Output), Definition{&Op, *Found->second, 0})
               .second)
        return std::nullopt;
    }
  const auto Count = [&](const MedVar &Value, bool Available) {
    if (Value.Size && !Value.isConst()) {
      auto &Definition = Definitions[Key(Value)];
      ++Definition.first;
      Definition.second = Available ? Value.Size : 0;
    }
    const auto Found = Values.find(Key(Value));
    if (Value.Size && Found != Values.end())
      ++Found->second.Definitions;
  };
  for (const auto &Block : Source.Blocks) {
    for (const auto &Phi : Block.Phis) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      Count(Phi.Output, true);
    }
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      Count(Op.Output, !Op.Dead);
      for (const auto &Output : Op.IntrinsicOutputs) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return std::nullopt;
        Count(Output, false);
      }
    }
  }
  for (const auto &Clobber : Source.CallClobbers) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return std::nullopt;
    Count(Clobber.Value, false);
  }
  SEHFrameOffsets Result;
  for (const auto &[Value, Definition] : Values)
    if (Definition.Definitions == 1)
      Result.Values.emplace(
          Value, SEHFrameOffsets::Offset{Definition.Offset,
                                         Definition.Operation->Output.Size});
  for (const auto &[Value, Definition] : Definitions)
    if (Definition.first == 1 && Definition.second)
      Result.UniqueDefinitions.emplace(Value, Definition.second);
  return Result;
}

std::optional<std::set<const MedOp *>> getSEHFrameStores(const MedFunc &Source,
                                                         uint64_t EntrySP,
                                                         uint64_t FrameBytes) {
  if (EntrySP < 4 || EntrySP > FrameBytes || FrameBytes > UINT32_MAX)
    return std::nullopt;
  const auto Offsets = getSEHFrameOffsets(Source);
  if (!Offsets)
    return std::nullopt;
  size_t Work = 0;
  std::set<const MedOp *> Result;
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (Op.Opcode != NdOp::STORE || Op.NumInputs != 2 || Op.Dead ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Op.MemoryOrdering != NdMemoryOrdering::None || !Op.Inputs[1].Size)
        continue;
      const auto FrameOffset = Offsets->get(Op.Inputs[0]);
      if (!FrameOffset)
        continue;
      const int64_t Offset = int64_t(EntrySP) - 4 + *FrameOffset;
      if (Offset >= 0 && uint64_t(Offset) <= FrameBytes &&
          Op.Inputs[1].Size <= FrameBytes - uint64_t(Offset))
        Result.insert(&Op);
    }
  return Result;
}

} // namespace neverd::x86_registration
