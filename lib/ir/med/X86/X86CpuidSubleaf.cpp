//===- X86CpuidSubleaf.cpp - CPUID's ignored sub-leaf ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Drop the ECX operand of a CPUID whose constant leaf ignores it.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/X86/X86CpuidSubleaf.h"

#include "neverd/ir/X86/X86CpuidLeaves.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/DenseMap.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <tuple>

namespace neverd {

bool dropIgnoredCpuidSubleaves(MedFunc &Func) {
  const auto IsCpuid = [](const MedOp &Op) {
    return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 3 &&
           Op.Inputs[0].isConst() &&
           Op.Inputs[0].ConstVal == static_cast<uint64_t>(Intrinsic::Cpuid) &&
           !Op.Inputs[2].isConst();
  };
  bool Any = false;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      Any |= IsCpuid(Op);
  if (!Any || Func.SkippedSSA)
    return false;
  using ValueKey = std::tuple<MedVar::VarKind, int, int>;
  const auto Key = [](const MedVar &V) -> ValueKey {
    return {V.Kind, V.Id, V.SSAVer};
  };
  llvm::DenseMap<ValueKey, const MedOp *> Defs;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Output.Size && !Op.Output.isConst())
        Defs.try_emplace(Key(Op.Output), &Op);
  // The constant \p V holds, through copies, zero extensions and low
  // views of one.
  const auto Constant = [&](MedVar V) -> std::optional<uint64_t> {
    const auto Low = [](uint64_t Value, uint16_t Size) {
      return Size >= 8 ? Value : Value & ((uint64_t{1} << (8 * Size)) - 1);
    };
    uint16_t Size = V.Size;
    for (unsigned Depth = 0; Depth < 8; ++Depth) {
      if (V.isConst())
        return Low(V.ConstVal, std::min(Size, V.Size));
      const auto Found = Defs.find(Key(V));
      if (Found == Defs.end())
        return std::nullopt;
      const MedOp &Def = *Found->second;
      const bool Passes =
          (Def.Opcode == NdOp::COPY || Def.Opcode == NdOp::INT_ZEXT) &&
          Def.NumInputs == 1;
      const bool Slices = Def.Opcode == NdOp::SUBBYTES && Def.NumInputs == 2 &&
                          Def.Inputs[1].isConst() &&
                          Def.Inputs[1].ConstVal == 0;
      if (!Passes && !Slices)
        return std::nullopt;
      Size = std::min(Size, V.Size);
      V = Def.Inputs[0];
    }
    return std::nullopt;
  };
  bool Changed = false;
  for (MedBlock &Block : Func.Blocks)
    for (MedOp &Op : Block.Ops) {
      if (!IsCpuid(Op))
        continue;
      const std::optional<uint64_t> Leaf = Constant(Op.Inputs[1]);
      if (!Leaf || !x86CpuidLeafIgnoresSubleaf(*Leaf & 0xffffffffu))
        continue;
      Op.Inputs[2] = MedVar::makeConst(0, Op.Inputs[2].Size);
      Changed = true;
    }
  return Changed;
}

} // namespace neverd
