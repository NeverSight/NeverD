//===- LowRawEnvironment.h - Retained environment requirements -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Common LowIR shape for a raw-analysis environment requirement. This marker
/// preserves a domain dependency when a LowFunc is copied. It neither proves
/// the domain nor permits ordinary execution or source emission. The analysis
/// that publishes it must separately own and validate the complete receipt.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_LOWRAWENVIRONMENT_H
#define NEVERD_IR_LOW_LOWRAWENVIRONMENT_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/LowRawCall.h"

#include <array>

namespace neverd {

/// A conservative refusal recognizer, not a validity check. Even a malformed
/// ID operand must not fall through an opaque-intrinsic approximation.
inline bool isLowRawEnvironmentRequirement(const LowOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC &&
         isRawEnvironmentRequirementId(Op.Inputs[0].Offset);
}

inline bool hasLowRawEnvironmentRequirement(const LowFunc &Function) {
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      if (isLowRawEnvironmentRequirement(Op))
        return true;
  return false;
}

inline bool isLowRawAnalysisOperation(const LowOp &Op) {
  return isLowRawEnvironmentRequirement(Op) || isLowRawReturningCall(Op);
}

inline bool hasLowRawAnalysisOperation(const LowFunc &Function) {
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      if (isLowRawAnalysisOperation(Op))
        return true;
  return false;
}

/// Exactly five 64-bit scalar constants: the full ID and four digest words.
/// No output, memory ordering, address space or pointer provenance is allowed.
/// Placement, uniqueness and digest/receipt matching belong to the publisher.
inline bool lowRawEnvironmentRequirementShapeIsValid(const LowOp &Op) {
  if (!isLowRawEnvironmentRequirement(Op) || Op.NumInputs != 5 ||
      Op.Output != NdVar{} || Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.Inputs[0].Offset !=
          static_cast<uint64_t>(Intrinsic::RawEnvironmentRequirementV1))
    return false;
  for (unsigned I = 0; I != 5; ++I)
    if (!Op.Inputs[I].isConst() || Op.Inputs[I].Size != 8 ||
        Op.Inputs[I].Provenance != ConstantAddressProvenance::Scalar ||
        Op.Inputs[I].AddressOwnerVA != InvalidVA)
      return false;
  return true;
}

inline LowOp
makeLowRawEnvironmentRequirement(const std::array<uint64_t, 4> &DigestWords,
                                 va_t Address, int Seq = 0) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Addr = Address;
  Op.Seq = Seq;
  Op.addInput(NdVar::scalar(
      static_cast<uint64_t>(Intrinsic::RawEnvironmentRequirementV1), 8));
  for (uint64_t Word : DigestWords)
    Op.addInput(NdVar::scalar(Word, 8));
  return Op;
}

} // namespace neverd

#endif // NEVERD_IR_LOW_LOWRAWENVIRONMENT_H
