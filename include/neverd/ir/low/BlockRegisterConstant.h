//===- BlockRegisterConstant.h - Constant registers -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Scalar register constants and bit-mask identities shared by IR consumers.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_BLOCKREGISTERCONSTANT_H
#define NEVERD_IR_LOW_BLOCKREGISTERCONSTANT_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {

/// Whether \p Op XORs a register or temporary with itself: a machine zero
/// idiom at any width, 128/256/512-bit vector containers included, which
/// reads nothing of its input.  An effect-bearing operation or a slice of
/// another width is no such idiom.
inline bool isLowSelfXor(const LowOp &Op) {
  return Op.Opcode == NdOp::INT_XOR && Op.NumInputs == 2 &&
         Op.MemoryOrdering == NdMemoryOrdering::None &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         (Op.Output.isReg() || Op.Output.isTemp()) && Op.Output.Size &&
         (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp()) &&
         Op.Inputs[0] == Op.Inputs[1] && Op.Output.Size == Op.Inputs[0].Size;
}

/// A scalar bit mask that fixes the result regardless of the other operand.
/// Callers must establish scalar provenance and matching operand widths.
inline std::optional<uint64_t>
absorbingBitwiseConstant(NdOp Opcode, uint16_t Bytes, uint64_t Value) {
  if (!Bytes || Bytes > sizeof(uint64_t))
    return std::nullopt;
  const uint64_t Mask = Bytes == sizeof(uint64_t)
                            ? ~uint64_t{0}
                            : (uint64_t{1} << (8 * Bytes)) - 1;
  if (Opcode == NdOp::INT_OR && (Value & Mask) == Mask)
    return Mask;
  if (Opcode == NdOp::INT_AND && (Value & Mask) == 0)
    return 0;
  return std::nullopt;
}

/// The constant `or r, -1` or `and r, 0` writes whatever the register held
/// (MSVC sets a register to all ones or zero this way): such an op reads
/// nothing of its register input.
inline std::optional<uint64_t> lowAbsorbingConstant(const LowOp &Op) {
  if ((Op.Opcode != NdOp::INT_OR && Op.Opcode != NdOp::INT_AND) ||
      Op.NumInputs != 2 || Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !(Op.Output.isReg() || Op.Output.isTemp()) || !Op.Output.Size ||
      Op.Output.Size > 8)
    return std::nullopt;
  for (unsigned I = 0; I < 2; ++I) {
    const NdVar &C = Op.Inputs[I];
    if (!C.isConst() || isAddressProvenance(C.Provenance) ||
        C.Size != Op.Output.Size || Op.Inputs[1 - I].Size != Op.Output.Size)
      continue;
    if (const auto Value =
            absorbingBitwiseConstant(Op.Opcode, Op.Output.Size, C.Offset))
      return Value;
  }
  return std::nullopt;
}

/// The constant register \p Offset of \p Size holds before Ops[End], when
/// the block sets it to one there (`mov w8, #93` writes w8, then extends it
/// into x8).  A write this cannot follow leaves the value unknown.
inline std::optional<uint64_t>
blockRegisterConstant(const std::vector<LowOp> &Ops, size_t End,
                      uint64_t Offset, uint16_t Size) {
  for (size_t K = End; K-- > 0;) {
    const LowOp &Op = Ops[K];
    if (!Op.Output.isReg() || Op.Output.Offset >= Offset + Size ||
        Offset >= Op.Output.Offset + Op.Output.Size)
      continue;
    if (Op.Output.Offset != Offset || Op.Output.Size != Size ||
        Op.NumInputs != 1)
      return std::nullopt;
    if (Op.Opcode == NdOp::COPY && Op.Inputs[0].isConst())
      return Size >= 8
                 ? Op.Inputs[0].Offset
                 : Op.Inputs[0].Offset & ((uint64_t{1} << (8 * Size)) - 1);
    if (Op.Opcode != NdOp::INT_ZEXT || !Op.Inputs[0].isReg() ||
        Op.Inputs[0].Offset != Offset || Op.Inputs[0].Size >= Size)
      return std::nullopt;
    Size = Op.Inputs[0].Size;
  }
  return std::nullopt;
}

/// The constant the low byte of \p Register holds where the call at
/// \p CallIndex of \p Ops reads it: the last write of that byte before the
/// call, through extensions of the register itself (`xor eax, eax` writes
/// EAX and zero-extends it into RAX).  Nullopt where another kind of write,
/// an earlier call or the block's start comes first.  System V's AL, the
/// count of vector registers a call passes a variadic callee, is such a
/// byte: MedIR (MedOp::VariadicVectorCount) and the callee read summaries
/// (RegisterStep::VectorCount) take it from here alike.
inline std::optional<uint8_t> lowByteConstantAtCall(llvm::ArrayRef<LowOp> Ops,
                                                    size_t CallIndex,
                                                    uint64_t Register) {
  for (size_t I = std::min(CallIndex, Ops.size()); I-- > 0;) {
    const LowOp &Op = Ops[I];
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
      return std::nullopt;
    const NdVar &Out = Op.Output;
    // A write that leaves the byte alone, such as one to AH.
    if (!Out.isReg() || Out.Size == 0 || Out.Offset > Register ||
        Out.Offset + Out.Size <= Register)
      continue;
    if (Out.Offset != Register)
      return std::nullopt;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 && Op.Inputs[0].isConst())
      return static_cast<uint8_t>(Op.Inputs[0].Offset & 0xFF);
    if ((Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT ||
         Op.Opcode == NdOp::COPY) &&
        Op.NumInputs >= 1 && Op.Inputs[0].isReg() &&
        Op.Inputs[0].Offset == Register)
      continue;
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_IR_LOW_BLOCKREGISTERCONSTANT_H
