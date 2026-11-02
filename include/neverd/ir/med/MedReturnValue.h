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

/// Copy propagation can replace a return register with a constant or assemble
/// its overlapping writes into a temporary. That complete reaching value
/// precedes physical register views, including a later narrow read projection.
/// A narrow value still needs ABI reconstruction.
inline bool hasPropagatedIntegerReturnValue(const MedOp &Return, Arch TheArch,
                                            uint16_t ReturnSize) {
  return hasExplicitIntegerReturnValue(Return, TheArch, ReturnSize) &&
         (Return.Inputs[0].isConst() ||
          (Return.Inputs[0].Kind == MedVar::Temp && Return.Inputs[0].Id >= 0));
}

} // namespace neverd

#endif
