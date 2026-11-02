//===- AArch64ImmediateMemoryImage.h - Image base witnesses ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact original-memory base witnesses for image-address provenance proofs.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_AARCH64IMMEDIATEMEMORYIMAGE_H
#define NEVERD_IR_LOW_AARCH64IMMEDIATEMEMORYIMAGE_H

#include "neverd/ir/low/AArch64WritebackMemory.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/AArch64MemoryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include <cstdint>

namespace neverd {

struct LowImageBaseMemoryWitness {
  const NdVar *Base = nullptr;
  uint16_t AccessSize = 0;
  bool Complete = false;
};

/// This view authenticates the address-forming base, not an ordinary effective
/// address. Only an exact image-value proof may use it: generic memory, alias
/// and no-fault analyses must continue to see the raw instruction as opaque.
inline LowImageBaseMemoryWitness
lowImageBaseMemoryWitness(const BinaryImage &Image, const LowOp &Op,
                          va_t OriginalBase, va_t Owner = InvalidVA) {
  const auto Memory = lowMemoryOperands(Op);
  if (Memory.Complete)
    return {Memory.Address, Memory.AccessSize, true};
  const auto Witness = [&](const auto &Instruction,
                           const NdVar &Base) -> LowImageBaseMemoryWitness {
    const auto *Bytes = Image.readVA(Op.Addr, kAddressMemoryWordBytes);
    if (!Base.isReg() || Base.Size != kAddressMemoryPointerBytes ||
        Base.Offset !=
            a64reg::X0 + Instruction.base() * kAddressMemoryPointerBytes ||
        !Bytes || readLE<uint32_t>(Bytes) != Instruction.Word ||
        !aarch64ImmediateMemoryImageSpan(Image, Instruction, OriginalBase,
                                         ConstantAddressProvenance::DataAddress,
                                         Owner))
      return {};
    return {&Base, static_cast<uint16_t>(Instruction.totalAccessBytes()), true};
  };
  if (lowAArch64AddressMemoryShapeIsValid(Op))
    return Witness(*aarch64AddressMemoryInstruction(
                       Op.Inputs[kAddressMemoryWordOperand + 1].Offset),
                   Op.Inputs[kAddressMemoryBaseOperand + 1]);
  if (lowAArch64WritebackMemoryShapeIsValid(Op))
    return Witness(*aarch64WritebackMemoryInstruction(
                       Op.Inputs[kWritebackWordOperand + 1].Offset),
                   Op.Inputs[kWritebackBaseOperand + 1]);
  return {};
}

/// A writeback after the exact dereference does not invalidate its incoming
/// base witness. Admit only the lifter's completed-base extraction, never an
/// arbitrary clobber or a second address computation.
inline bool
lowImageBaseMemoryWritebackCommit(const LowOp &Op, const LowOp *Memory,
                                  llvm::ArrayRef<LowOp> Instruction = {}) {
  if (!Memory || !lowAArch64WritebackMemoryShapeIsValid(*Memory) ||
      Op.Addr != Memory->Addr ||
      Op.Output != Memory->Inputs[kWritebackBaseOperand + 1] ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  if (isAArch64WritebackAuxiliaryLoad(
          static_cast<Intrinsic>(Memory->Inputs[0].Offset))) {
    const auto It = std::find_if(
        Instruction.begin(), Instruction.end(),
        [&](const LowOp &Candidate) { return &Candidate == Memory; });
    if (It == Instruction.end())
      return false;
    const auto Outputs = lowAArch64WritebackOutputs(
        *Memory, Instruction.drop_front(It - Instruction.begin() + 1));
    return Outputs && Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
           Op.Inputs[0] ==
               (*Outputs)[aarch64WritebackBaseIndex(
                   static_cast<Intrinsic>(Memory->Inputs[0].Offset))];
  }
  const bool Load = static_cast<Intrinsic>(Memory->Inputs[0].Offset) ==
                    Intrinsic::A64_LoadWriteback;
  if (!Load)
    return Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
           Op.Inputs[0] == Memory->Output;
  return Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
         Op.Inputs[0] == Memory->Output && Op.Inputs[1].isConst() &&
         Op.Inputs[1].Offset == kAddressMemoryPointerBytes;
}

} // namespace neverd

#endif // NEVERD_IR_LOW_AARCH64IMMEDIATEMEMORYIMAGE_H
