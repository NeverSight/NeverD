//===- MedShiftedReturnPaths.h - Shared integer returns ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Integer shift observations at a shared return and the SSA joins they use.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDSHIFTEDRETURNPATHS_H
#define NEVERD_IR_MED_MEDSHIFTEDRETURNPATHS_H

#include "neverd/ir/med/MedByteDemand.h"

#include "llvm/ADT/SmallBitVector.h"

namespace neverd::return_contract {

struct PhiSite {
  const PhiNode *Phi;
  const MedBlock *Block;
};

inline bool isScalarLiteral(const MedVar &V) {
  return V.isConst() && (V.Provenance == ConstantAddressProvenance::Unknown ||
                         V.Provenance == ConstantAddressProvenance::Scalar);
}

inline bool scalarShiftShape(const MedOp &Op) {
  if (Op.NumInputs != 2)
    return false;
  const auto &Value = Op.Inputs[0], &Count = Op.Inputs[1];
  return Count.Size && Count.Size <= Value.Size &&
         Value.Size == Op.Output.Size && Value.Size <= 8;
}

inline bool isScalarShift(const MedOp &Op) {
  return (Op.Opcode == NdOp::INT_LEFT || Op.Opcode == NdOp::INT_RIGHT ||
          Op.Opcode == NdOp::INT_ASHR) &&
         scalarShiftShape(Op);
}

inline bool isVariableShift(const MedOp &Op) {
  return isScalarShift(Op) && !Op.Inputs[1].isConst();
}

/// Discard unobserved preserved bytes and coalesce an unused incoming carrier
/// with the active one on its edge before source argument recovery.
bool foldUnusedShiftCarriers(MedFunc &Func, const TargetRegInfo &TRI,
                             BinaryFormat Format);

uint64_t shiftedBits(NdOp Opcode, unsigned Width, uint64_t Value,
                     uint64_t Count);

/// Bind the return register and shift count on the same incoming edge, then
/// close both sets of outcomes under all ordinary continuations to Exit.
bool selectsShiftedReturnAcrossJoins(
    const MedFunc &Func, const MedBlock &Exit, const MedVar &Result,
    const TargetRegInfo &TRI, BinaryFormat Format, unsigned Bytes,
    const MedValueMap<const MedOp *> &Defs, const MedValueMap<PhiSite> &Phis,
    const llvm::SmallBitVector &Reachable, unsigned &Work);

} // namespace neverd::return_contract
#endif
