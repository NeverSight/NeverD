//===- LowRawCall.h - Conditional raw call events -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A retained opaque invocation after its authenticated physical stack push.
/// The publisher owns the complete conditional relation and static-span
/// receipt. Each dynamic execution chooses a fresh permitted outcome. Ordinary
/// execution, proof and source emission have no contract for this implicit
/// full-state event.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_LOWRAWCALL_H
#define NEVERD_IR_LOW_LOWRAWCALL_H

#include "neverd/ir/low/LowIR.h"

#include <array>

namespace neverd {

/// Conservative refusal recognizer, including malformed arity/ID operands.
inline bool isLowRawReturningCall(const LowOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC &&
         isRawReturningCallId(Op.Inputs[0].Offset);
}

/// Full ID, four declaration-digest words, and the once-captured 64-bit target.
/// The retained physical prefix and separate receipt bind caller RSP/guest PC.
inline bool lowRawReturningCallShapeIsValid(const LowOp &Op) {
  if (!isLowRawReturningCall(Op) || Op.NumInputs != 6 ||
      Op.ConsumesX87Argument || Op.X87ScratchSlots || Op.X87ImportArguments ||
      Op.Output != NdVar{} || Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.Inputs[0].Offset !=
          static_cast<uint64_t>(Intrinsic::RawReturningCallV1))
    return false;
  for (unsigned I = 0; I != 5; ++I)
    if (!Op.Inputs[I].isConst() || Op.Inputs[I].Size != 8 ||
        Op.Inputs[I].Provenance != ConstantAddressProvenance::Scalar ||
        Op.Inputs[I].AddressOwnerVA != InvalidVA)
      return false;
  const auto &Target = Op.Inputs[5];
  return Target.Size == 8 &&
         (Target.isReg() || Target.isTemp() || Target.isConst()) &&
         Target.Provenance == (Target.isConst()
                                   ? ConstantAddressProvenance::Scalar
                                   : ConstantAddressProvenance::Unknown) &&
         Target.AddressOwnerVA == InvalidVA &&
         (Target.isConst() || Target.Offset <= UINT64_MAX - Target.Size);
}

inline LowOp makeLowRawReturningCall(const std::array<uint64_t, 4> &DigestWords,
                                     NdVar Target, va_t Address, int Seq) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Addr = Address;
  Op.Seq = Seq;
  Op.addInput(
      NdVar::scalar(static_cast<uint64_t>(Intrinsic::RawReturningCallV1), 8));
  for (uint64_t Word : DigestWords)
    Op.addInput(NdVar::scalar(Word, 8));
  Op.addInput(Target);
  return Op;
}

} // namespace neverd

#endif // NEVERD_IR_LOW_LOWRAWCALL_H
