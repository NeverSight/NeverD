//===- SymIntegerIntrinsics.h - Bit-vector intrinsics -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMINTEGERINTRINSICS_H
#define NEVERD_SYMBOLIC_SYMINTEGERINTRINSICS_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <optional>

namespace neverd::symbolic {

/// Count set bits at the input's exact width. Shared by symbolic execution
/// and the source adapter so a recovered spelling keeps the same relation.
inline SymRef evaluatePopulationCount(SymContext &Ctx, SymRef Input) {
  const auto Width = Ctx.width(Input);
  llvm::SmallVector<SymRef, 64> Bits;
  Bits.reserve(Width);
  for (uint32_t Bit = 0; Bit < Width; ++Bit)
    Bits.push_back(Ctx.mkZExtOrTrunc(Ctx.mkExtract(Input, Bit, 1), Width));
  return Ctx.mkAdd(Bits);
}

/// Exact integer semantics for an intrinsic whose values are bit vectors.
/// Unsupported IDs or malformed shapes produce no value and read no inputs.
/// Register upper-state and memory accesses remain owned by the lifter.
inline std::optional<SymRef>
evaluateIntegerIntrinsic(SymContext &Ctx, const LowOp &Op,
                         llvm::function_ref<SymRef(const NdVar &)> Read) {
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs != 4 ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Offset != static_cast<uint64_t>(Intrinsic::Shufps) ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      (Op.Output.Size != 16 && Op.Output.Size != 32 && Op.Output.Size != 64) ||
      Op.Inputs[1].Size != Op.Output.Size ||
      Op.Inputs[2].Size != Op.Output.Size || !Op.Inputs[3].isConst() ||
      Op.Inputs[3].Size != 1 || Op.Inputs[3].Offset > 255)
    return std::nullopt;

  // The destination may alias either source. Capture both before publishing it.
  const SymRef Left = Read(Op.Inputs[1]), Right = Read(Op.Inputs[2]);
  const unsigned Bits = unsigned(Op.Output.Size) * 8;
  if (!Left || !Right || Ctx.width(Left) != Bits || Ctx.width(Right) != Bits)
    return std::nullopt;
  const uint8_t Control = static_cast<uint8_t>(Op.Inputs[3].Offset);
  llvm::SmallVector<SymRef, 16> Elements;
  // Concat lists high bits first; selectors repeat within each 128-bit lane.
  for (unsigned I = Bits / 32; I-- > 0;) {
    const unsigned InLane = I % 4;
    const unsigned Selected = (I / 4) * 4 + ((Control >> (2 * InLane)) & 3);
    Elements.push_back(
        Ctx.mkExtract(InLane < 2 ? Left : Right, Selected * 32, 32));
  }
  return Ctx.mkConcat(Elements);
}

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMINTEGERINTRINSICS_H
