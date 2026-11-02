//===- MedScalarShapes.h - Shared explicit scalar operand shapes -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Operand counts, input compatibility and exact byte projections. These checks
/// do not establish a complete execution profile, value provenance or outputs.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDSCALARSHAPES_H
#define NEVERD_IR_MED_MEDSCALARSHAPES_H

#include "neverd/ir/med/MedIR.h"

namespace neverd {

// Only explicit, unconditional scalar definitions belong to this subset.
// Intrinsics/calls can carry implicit or conditional outputs and need their
// own contracts. NOP/dead outputs are never definitions.
inline int medScalarInputCount(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::COPY:
  case NdOp::LOAD:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
  case NdOp::INT_NEG2:
  case NdOp::BOOL_NOT:
  case NdOp::POPCOUNT:
  case NdOp::LZCOUNT:
    return 1;
  case NdOp::STORE:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_MULT:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::CONCAT:
  case NdOp::SUBBYTES:
    return 2;
  case NdOp::SELECT:
    return 3;
  default:
    return -1;
  }
}

inline bool medScalarInputWidthsCompatible(const MedOp &O,
                                           unsigned PointerSize) {
  const int Count = medScalarInputCount(O.Opcode);
  if (Count < 0 || O.NumInputs != Count || O.NumInputs > O.Inputs.size())
    return false;
  switch (O.Opcode) {
  case NdOp::POPCOUNT:
  case NdOp::LZCOUNT:
    return O.Inputs[0].Size <= 8;
  case NdOp::LOAD:
  case NdOp::STORE:
    return O.Inputs[0].Size == PointerSize;
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
    return O.Inputs[1].Size <= O.Inputs[0].Size;
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_MULT:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
    return O.Inputs[0].Size == O.Inputs[1].Size;
  case NdOp::SELECT:
    return O.Inputs[1].Size == O.Inputs[2].Size;
  default:
    return true;
  }
}
inline bool medScalarByteProjectionIsValid(const MedOp &Op) {
  if (Op.Opcode != NdOp::SUBBYTES && Op.Opcode != NdOp::CONCAT)
    return true;
  if (Op.NumInputs != 2 || Op.NumInputs > Op.Inputs.size())
    return false;
  if (Op.Opcode == NdOp::CONCAT)
    return Op.Output.Size == Op.Inputs[0].Size + Op.Inputs[1].Size;
  return Op.Inputs[1].isConst() && Op.Inputs[1].ConstVal <= Op.Inputs[0].Size &&
         Op.Output.Size <= Op.Inputs[0].Size - Op.Inputs[1].ConstVal;
}

} // namespace neverd

#endif // NEVERD_IR_MED_MEDSCALARSHAPES_H
