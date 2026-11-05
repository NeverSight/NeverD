//===- MedReturnValue.h - Propagated integer return values ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDRETURNVALUE_H
#define NEVERD_IR_MED_MEDRETURNVALUE_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd {

/// An explicit return operand covers the required register view. Its reaching
/// definition and any narrower physical-register reconstruction remain the
/// caller's responsibility.
inline bool hasExplicitIntegerReturnValue(const MedOp &Return, Arch TheArch,
                                          uint16_t ReturnSize) {
  return Return.Opcode == NdOp::RETURN && Return.NumInputs == 1 &&
         !Return.Inputs.empty() &&
         getTargetRegInfo(TheArch).ReturnOperandIsValue && ReturnSize != 0 &&
         Return.Inputs[0].Size >= ReturnSize;
}

/// Copy propagation can replace a return register with another register, a
/// parameter or constant, or assemble overlapping writes into a temporary.
/// That complete reaching value precedes physical return-register views,
/// including a later narrow read projection. A narrow value still needs ABI
/// reconstruction, as does a value still naming the physical return register.
inline bool hasPropagatedIntegerReturnValue(const MedOp &Return, Arch TheArch,
                                            uint16_t ReturnSize) {
  if (!hasExplicitIntegerReturnValue(Return, TheArch, ReturnSize))
    return false;
  const auto &Value = Return.Inputs[0];
  const auto &TRI = getTargetRegInfo(TheArch);
  return Value.isConst() ||
         (Value.Id >= 0 &&
          (Value.Kind == MedVar::Temp || Value.Kind == MedVar::Param ||
           (Value.Kind == MedVar::Reg && Value.RegOff != TRI.IntReturnReg &&
            !TRI.isReturnControlReg(Value.RegOff))));
}

} // namespace neverd

#endif
