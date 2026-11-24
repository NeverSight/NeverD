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
#include <utility>
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
    // An OR of two constants is their value: `orr x8, xzr, #0x7c`, which Go's
    // assembler emits for a bitmask constant MOVZ could also encode, and for
    // which Capstone names no MOV alias.
    if (Op.Opcode == NdOp::INT_OR && Op.NumInputs == 2 &&
        Op.Output.Offset == Offset && Op.Output.Size == Size &&
        Op.Inputs[0].isConst() && Op.Inputs[1].isConst()) {
      const uint64_t Value = Op.Inputs[0].Offset | Op.Inputs[1].Offset;
      return Size >= 8 ? Value : Value & ((uint64_t{1} << (8 * Size)) - 1);
    }
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

/// What \p Op does to the constant the low byte of \p Register holds:
/// nothing (nullopt), sets it to one, or leaves it unknown (negative).  A
/// call leaves it unknown, a write to another byte of the register (AH)
/// leaves it alone, and so does an extension of the register itself
/// (`xor eax, eax` writes EAX and zero-extends it into RAX).
inline std::optional<int16_t> lowByteWrite(const LowOp &Op, uint64_t Register) {
  if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
    return int16_t{-1};
  const NdVar &Out = Op.Output;
  if (!Out.isReg() || Out.Size == 0 || Out.Offset > Register ||
      Out.Offset + Out.Size <= Register)
    return std::nullopt;
  if (Out.Offset != Register)
    return int16_t{-1};
  if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 && Op.Inputs[0].isConst())
    return static_cast<int16_t>(Op.Inputs[0].Offset & 0xFF);
  if ((Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT ||
       Op.Opcode == NdOp::COPY) &&
      Op.NumInputs >= 1 && Op.Inputs[0].isReg() &&
      Op.Inputs[0].Offset == Register)
    return std::nullopt;
  return int16_t{-1};
}

/// The constant the low byte of \p Register holds where the call at
/// \p CallIndex of \p Ops reads it: the last write of that byte before the
/// call (lowByteWrite).  Nullopt where another kind of write, an earlier
/// call or the block's start comes first.
inline std::optional<uint8_t> lowByteConstantAtCall(llvm::ArrayRef<LowOp> Ops,
                                                    size_t CallIndex,
                                                    uint64_t Register) {
  for (size_t I = std::min(CallIndex, Ops.size()); I-- > 0;)
    if (const std::optional<int16_t> Written = lowByteWrite(Ops[I], Register))
      return *Written < 0
                 ? std::nullopt
                 : std::optional<uint8_t>(static_cast<uint8_t>(*Written));
  return std::nullopt;
}

/// The successor a block enters with \p Register zero because it ends by
/// branching on that: the target of `test r, r; je`, or the fall-through of
/// `test r, r; jne` (\p Taken says which), at any width of the register.
/// Nullopt where the block ends another way, or writes the register's low
/// byte between the test and the branch.
inline std::optional<va_t> lowZeroTestedBranch(const LowBlock &Block,
                                               uint64_t Register, bool &Taken) {
  const std::vector<LowOp> &Ops = Block.Ops;
  if (Ops.size() < 3)
    return std::nullopt;
  const LowOp &Branch = Ops.back();
  if (Branch.Opcode != NdOp::COND_BR || Branch.NumInputs < 2 ||
      !Branch.Inputs[0].isConst())
    return std::nullopt;
  // The last write of \p Value before Ops[End].
  const auto WriteOf = [&](const NdVar &Value,
                           size_t End) -> std::optional<size_t> {
    for (size_t I = End; I-- > 0;)
      if (Ops[I].Output == Value)
        return I;
    return std::nullopt;
  };
  // The condition is the zero flag, or its negation.
  const std::optional<size_t> Condition =
      WriteOf(Branch.Inputs[1], Ops.size() - 1);
  if (!Condition || Ops[*Condition].NumInputs != 1 ||
      !Ops[*Condition].Inputs[0].isReg() ||
      (Ops[*Condition].Opcode != NdOp::COPY &&
       Ops[*Condition].Opcode != NdOp::BOOL_NOT))
    return std::nullopt;
  // The flag says whether the register, ANDed with itself, is zero.
  const std::optional<size_t> Flag =
      WriteOf(Ops[*Condition].Inputs[0], *Condition);
  if (!Flag || Ops[*Flag].Opcode != NdOp::INT_EQUAL ||
      Ops[*Flag].NumInputs != 2 || !Ops[*Flag].Inputs[1].isConst() ||
      Ops[*Flag].Inputs[1].Offset != 0)
    return std::nullopt;
  const std::optional<size_t> Test = WriteOf(Ops[*Flag].Inputs[0], *Flag);
  if (!Test || Ops[*Test].Opcode != NdOp::INT_AND ||
      Ops[*Test].NumInputs != 2 || !Ops[*Test].Inputs[0].isReg() ||
      Ops[*Test].Inputs[0] != Ops[*Test].Inputs[1] ||
      Ops[*Test].Inputs[0].Offset != Register ||
      Ops[*Test].Inputs[0].Size == 0 ||
      Ops[*Test].Output.Size != Ops[*Test].Inputs[0].Size ||
      Ops[*Flag].Inputs[0].Size != Ops[*Test].Output.Size)
    return std::nullopt;
  for (size_t I = *Test; I < Ops.size(); ++I)
    if (lowByteWrite(Ops[I], Register))
      return std::nullopt;
  Taken = Ops[*Condition].Opcode == NdOp::COPY;
  return static_cast<va_t>(Branch.Inputs[0].Offset);
}

/// lowByteConstantAtCall across the blocks of one function: where the start
/// of the call's block comes before any write of the byte, the one constant
/// every way into that block leaves in it.  GCC sets AL with the
/// `xor eax, eax` that is also the result of the path that returns, before
/// the branch to the block that calls a variadic function.  The byte is
/// unknown where the function is entered, in a block no other leads to and
/// in one an exception enters, so a constant here holds on every path.  It
/// is zero on the edge a branch takes because the register is
/// (lowZeroTestedBranch): GCC also passes the AL it has just tested.
/// System V's AL, the count of vector registers a call passes a variadic
/// callee, is such a byte: MedIR (MedOp::VariadicVectorCount) and the callee
/// read summaries (RegisterStep::VectorCount) take it from here alike.
class LowByteConstantsAtCalls {
public:
  /// \p Blocks, which must outlive this, are the blocks of one function,
  /// entered at the first.  \p AcrossBlocks is false where a block may be
  /// entered some way its predecessors do not show (lowBlocksShowEveryEntry):
  /// only a call's own block answers then.
  LowByteConstantsAtCalls(llvm::ArrayRef<LowBlock> Blocks, uint64_t Register,
                          bool AcrossBlocks)
      : Blocks(Blocks), Register(Register), AcrossBlocks(AcrossBlocks) {}

  /// The constant at the call at \p CallIndex of \p Block, one of the
  /// function's blocks.
  std::optional<uint8_t> at(const LowBlock &Block, size_t CallIndex) {
    for (size_t I = std::min(CallIndex, Block.Ops.size()); I-- > 0;)
      if (const std::optional<int16_t> Written =
              lowByteWrite(Block.Ops[I], Register))
        return known(*Written);
    if (!AcrossBlocks || Blocks.empty())
      return std::nullopt;
    if (AtStart.empty())
      solve();
    const size_t Index = static_cast<size_t>(&Block - Blocks.data());
    return Index < AtStart.size() ? known(AtStart[Index]) : std::nullopt;
  }

private:
  /// A byte no constant is known for, and one no path has reached yet.
  static constexpr int16_t Unknown = -1, Unreached = -2;

  static std::optional<uint8_t> known(int16_t State) {
    return State < 0 ? std::nullopt
                     : std::optional<uint8_t>(static_cast<uint8_t>(State));
  }

  /// The byte at the start of each block: the greatest fixed point of the
  /// meet over each block's predecessors, from the unknown roots.
  void solve() {
    const size_t Count = Blocks.size();
    AtStart.assign(Count, Unreached);
    // What each block leaves the byte as, or Unreached where it passes on
    // what it was entered with.
    std::vector<int16_t> Effect(Count, Unreached), AtEnd(Count, Unreached);
    std::vector<std::pair<int, size_t>> IndexOfId;
    IndexOfId.reserve(Count);
    for (size_t I = 0; I < Count; ++I) {
      IndexOfId.push_back({Blocks[I].Id, I});
      for (const LowOp &Op : Blocks[I].Ops)
        if (const std::optional<int16_t> Written = lowByteWrite(Op, Register))
          Effect[I] = *Written;
    }
    std::sort(IndexOfId.begin(), IndexOfId.end());
    const auto IndexOf = [&](int Id) -> std::optional<size_t> {
      const auto Found = std::lower_bound(IndexOfId.begin(), IndexOfId.end(),
                                          std::pair<int, size_t>{Id, 0});
      if (Found == IndexOfId.end() || Found->first != Id)
        return std::nullopt;
      return Found->second;
    };
    // Per block, the successor it enters with the byte zero, if any.
    std::vector<size_t> ZeroInto(Count, Count);
    for (size_t I = 0; I < Count; ++I) {
      const LowBlock &Block = Blocks[I];
      bool Taken = false;
      const std::optional<va_t> Target =
          Block.Succs.size() == 2 ? lowZeroTestedBranch(Block, Register, Taken)
                                  : std::nullopt;
      if (!Target)
        continue;
      const std::optional<size_t> First = IndexOf(Block.Succs[0]);
      const std::optional<size_t> Second = IndexOf(Block.Succs[1]);
      if (!First || !Second || *First == *Second)
        continue;
      const bool FirstIsTarget = Blocks[*First].StartAddr == *Target;
      if (FirstIsTarget == (Blocks[*Second].StartAddr == *Target))
        continue;
      ZeroInto[I] = FirstIsTarget == Taken ? *First : *Second;
    }
    std::vector<size_t> Work;
    Work.reserve(Count);
    for (size_t I = Count; I-- > 0;)
      Work.push_back(I);
    while (!Work.empty()) {
      const size_t I = Work.back();
      Work.pop_back();
      const LowBlock &Block = Blocks[I];
      int16_t In = Unreached;
      if (I == 0 || Block.Preds.empty() || !Block.ExceptionalPreds.empty())
        In = Unknown;
      else
        for (const int Pred : Block.Preds) {
          const std::optional<size_t> From = IndexOf(Pred);
          // The edge of a block reached counts, whatever the block leaves.
          int16_t Left = From ? AtEnd[*From] : Unknown;
          if (From && Left != Unreached && ZeroInto[*From] == I)
            Left = 0;
          if (Left == Unreached)
            continue;
          if (In == Unreached)
            In = Left;
          else if (In != Left)
            In = Unknown;
          if (In == Unknown)
            break;
        }
      AtStart[I] = In;
      const int16_t Out = Effect[I] == Unreached ? In : Effect[I];
      if (Out == AtEnd[I])
        continue;
      AtEnd[I] = Out;
      for (const int Succ : Block.Succs)
        if (const std::optional<size_t> To = IndexOf(Succ))
          Work.push_back(*To);
    }
  }

  llvm::ArrayRef<LowBlock> Blocks;
  uint64_t Register;
  bool AcrossBlocks;
  /// Per block, the byte at its start; empty until a call asks.
  std::vector<int16_t> AtStart;
};

} // namespace neverd

#endif // NEVERD_IR_LOW_BLOCKREGISTERCONSTANT_H
