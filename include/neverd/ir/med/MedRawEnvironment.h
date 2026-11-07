//===- MedRawEnvironment.h - Environment refusal adapter -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognizes retained raw-environment requirements before MedIR consumers
/// perform preparation, body masking, intrinsic narrowing or approximation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDRAWENVIRONMENT_H
#define NEVERD_IR_MED_MEDRAWENVIRONMENT_H

#include "neverd/ir/med/MedIR.h"

namespace neverd {

/// Inspect actual storage, independently of a possibly malformed declared
/// arity or value kind. This is a conservative refusal check, not permission
/// to consume the domain or accept the marker's structure.
inline bool isMedRawEnvironmentRequirement(const MedOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && !Op.Inputs.empty() &&
         isRawEnvironmentRequirementId(Op.Inputs.front().ConstVal);
}

inline bool isMedArchitecturalValue(const MedOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && !Op.Inputs.empty() &&
         isArchitecturalValueId(Op.Inputs.front().ConstVal);
}

} // namespace neverd

#endif // NEVERD_IR_MED_MEDRAWENVIRONMENT_H
