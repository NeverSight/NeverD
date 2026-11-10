//===- MIPSIntrinsicShapes.h - MedIR scalar contracts --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapts MedIR operands to the MIPS scalar intrinsic contract.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MIPS_MIPSINTRINSICSHAPES_H
#define NEVERD_IR_MED_MIPS_MIPSINTRINSICSHAPES_H

#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd {

inline bool mipsScalarMedShapeIsValid(const MedOp &Op,
                                      Arch Target = Arch::Unknown) {
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
      Op.NumInputs > Op.Inputs.size() || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != sizeof(Intrinsic) ||
      Op.Inputs[0].ConstVal >= uint64_t(Intrinsic::_Count) ||
      !Op.IntrinsicOutputs.empty())
    return false;
  const auto ObserveArch = [&](const MedVar &Value) {
    if (Value.isConst() || !Value.Size || Value.TheArch == Arch::Unknown)
      return true;
    if (Target == Arch::Unknown)
      Target = Value.TheArch;
    return Target == Value.TheArch;
  };
  if (!ObserveArch(Op.Output))
    return false;
  for (unsigned I = 1; I < Op.NumInputs; ++I) {
    const auto &V = Op.Inputs[I];
    if ((!V.isConst() && V.Kind != MedVar::Reg && V.Kind != MedVar::Temp &&
         V.Kind != MedVar::Param) ||
        !ObserveArch(V))
      return false;
  }
  return mips::scalarShapeIsValid(
      static_cast<Intrinsic>(Op.Inputs[0].ConstVal),
      {Target, Op.NumInputs, true,
       Op.Output.Kind == MedVar::Reg || Op.Output.Kind == MedVar::Temp,
       Op.Output.Size, Op.NumInputs > 1 ? Op.Inputs[1].Size : uint16_t{0},
       Op.NumInputs > 2 ? Op.Inputs[2].Size : uint16_t{0},
       Op.NumInputs > 3 ? Op.Inputs[3].Size : uint16_t{0},
       Op.MemoryOrdering == NdMemoryOrdering::None &&
           Op.MemoryAddressSpace == NdMemoryAddressSpace::Default,
       Op.NumInputs > 1 && Op.Inputs[1].isConst()
           ? std::optional<uint64_t>(Op.Inputs[1].ConstVal)
           : std::nullopt});
}

} // namespace neverd

#endif // NEVERD_IR_MED_MIPS_MIPSINTRINSICSHAPES_H
