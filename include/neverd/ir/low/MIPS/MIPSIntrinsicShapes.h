//===- MIPSIntrinsicShapes.h - LowIR scalar contracts --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate raw scalar operands before normalization or concrete execution.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_LOW_MIPS_MIPSINTRINSICSHAPES_H
#define NEVERD_IR_LOW_MIPS_MIPSINTRINSICSHAPES_H

#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"
#include "neverd/ir/low/LowIR.h"

#include <iterator>

namespace neverd {

inline bool mipsScalarLowShapeIsValid(const LowOp &Op,
                                      Arch Target = Arch::Unknown) {
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
      Op.NumInputs > std::size(Op.Inputs) || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != sizeof(Intrinsic) ||
      Op.Inputs[0].Offset >= uint64_t(Intrinsic::_Count))
    return false;
  for (unsigned I = 1; I < Op.NumInputs; ++I)
    if (!Op.Inputs[I].isConst() && !Op.Inputs[I].isReg() &&
        !Op.Inputs[I].isTemp())
      return false;
  return mips::scalarShapeIsValid(
      static_cast<Intrinsic>(Op.Inputs[0].Offset),
      {Target, Op.NumInputs, true, Op.Output.isReg() || Op.Output.isTemp(),
       Op.Output.Size, Op.NumInputs > 1 ? Op.Inputs[1].Size : uint16_t{0},
       Op.NumInputs > 2 ? Op.Inputs[2].Size : uint16_t{0},
       Op.NumInputs > 3 ? Op.Inputs[3].Size : uint16_t{0},
       Op.MemoryOrdering == NdMemoryOrdering::None &&
           Op.MemoryAddressSpace == NdMemoryAddressSpace::Default,
       Op.NumInputs > 1 && Op.Inputs[1].isConst()
           ? std::optional<uint64_t>(Op.Inputs[1].Offset)
           : std::nullopt});
}

} // namespace neverd

#endif // NEVERD_IR_LOW_MIPS_MIPSINTRINSICSHAPES_H
