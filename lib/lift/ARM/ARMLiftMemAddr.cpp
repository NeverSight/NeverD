//===- ARMLiftMemAddr.cpp - ARM32 memory addressing helpers --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Effective-address computation and base-register writeback for the ARM32
/// single-register and doubleword load/store forms.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/lift/ARMLifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

NdVar ARMLifter::emitLdrdStrdEA(LiftState &S, const cs_arm_op &MemOp,
                                bool PostIndex) {
  NdVar EA = S.makeTemp(4);
  bool First = true;
  auto Acc = [&](NdVar V) {
    if (First) {
      S.emit(NdOp::COPY, EA, {V});
      First = false;
    } else
      S.emit(NdOp::INT_ADD, EA, {EA, V});
  };
  if (MemOp.mem.base != ARM_REG_INVALID) {
    auto RI = mapCapstoneReg(static_cast<arm_reg>(MemOp.mem.base));
    Acc(NdVar::reg(RI.Offset, 4));
  }
  if (MemOp.mem.index != ARM_REG_INVALID) {
    auto RI = mapCapstoneReg(static_cast<arm_reg>(MemOp.mem.index));
    NdVar IdxVal = NdVar::reg(RI.Offset, 4);
    // Shift the index first, then apply its sign exactly once: capstone signals
    // a subtracted index via mem.scale == -1 and/or `subtracted`, which denote
    // the SAME sign — applying both double-negates `[Rn,-Rm]` (#387).
    if (MemOp.shift.type != ARM_SFT_INVALID &&
        (MemOp.shift.value != 0 || MemOp.shift.type == ARM_SFT_RRX))
      IdxVal = emitImmShift(S, IdxVal, MemOp.shift.type, MemOp.shift.value);
    if (MemOp.mem.scale == -1 || MemOp.subtracted) {
      NdVar Neg = S.makeTemp(4);
      S.emit(NdOp::INT_NEG2, Neg, {IdxVal});
      Acc(Neg);
    } else {
      Acc(IdxVal);
    }
  }
  // Post-indexed: capstone carries the post-increment in mem.disp, but the
  // access is at the unmodified base — exclude the displacement here (the
  // writeback helper applies it to the base afterward).
  if (!PostIndex && MemOp.mem.disp != 0) {
    int64_t SignedDisp = MemOp.mem.disp;
    if (MemOp.subtracted && MemOp.mem.index == ARM_REG_INVALID)
      SignedDisp = -(SignedDisp < 0 ? -SignedDisp : SignedDisp);
    Acc(NdVar::cst(static_cast<uint64_t>(SignedDisp), 4));
  }
  if (First)
    Acc(NdVar::cst(0, 4));
  return EA;
}

void ARMLifter::emitLdrdStrdWriteback(LiftState &S, const cs_insn *Insn,
                                      const cs_arm &ARM, const cs_arm_op &MemOp,
                                      NdVar EA) {
  if (!Insn->detail->writeback || MemOp.mem.base == ARM_REG_INVALID)
    return;
  auto RI = mapCapstoneReg(static_cast<arm_reg>(MemOp.mem.base));
  NdVar BaseReg = NdVar::reg(RI.Offset, 4);
  if (ARM.post_index) {
    // Post-indexed: the access used the unmodified base (capstone leaves
    // mem.disp==0 and carries the increment in the trailing operand, which for
    // LDRD/STRD follows the memory operand at operands[3]).
    int64_t WBOffset = MemOp.mem.disp;
    if (ARM.op_count >= 4 && ARM.operands[3].type == ARM_OP_IMM)
      WBOffset = ARM.operands[3].imm;
    if (MemOp.subtracted)
      WBOffset = -(WBOffset < 0 ? -WBOffset : WBOffset);
    if (WBOffset != 0)
      S.emit(NdOp::INT_ADD, BaseReg,
             {BaseReg,
              NdVar::cst(
                  static_cast<uint64_t>(static_cast<uint32_t>(WBOffset)), 4)});
  } else {
    // Pre-indexed: base takes the computed access address.
    S.emit(NdOp::COPY, BaseReg, {EA});
  }
}

NdVar ARMLifter::emitSingleMemAddr(LiftState &S, const cs_insn *Insn,
                                     const cs_arm &ARM,
                                     const cs_arm_op &MemOp) {
  // Post-indexed access uses the UNMODIFIED base; the entire offset (register
  // or immediate) folds into the base writeback rather than the access address
  // — otherwise it is double-applied (once to the address, once by the
  // writeback).
  bool PostIdx = Insn->detail->writeback && ARM.post_index;
  NdVar EA = S.makeTemp(4);
  bool First = true;
  auto Acc = [&](NdVar V) {
    if (First) {
      S.emit(NdOp::COPY, EA, {V});
      First = false;
    } else
      S.emit(NdOp::INT_ADD, EA, {EA, V});
  };
  if (MemOp.mem.base != ARM_REG_INVALID) {
    auto RI = mapCapstoneReg(static_cast<arm_reg>(MemOp.mem.base));
    Acc(NdVar::reg(RI.Offset, 4));
  }
  // Index register offset: shift first, then sign exactly once.  capstone marks
  // a subtracted index via mem.scale==-1 and/or `subtracted` (the SAME sign,
  // not cumulative; #387 `ldrsb r1,[r9,-r6]`).  ROR/RRX must rotate, not
  // left-shift.
  bool HasIndex = (MemOp.mem.index != ARM_REG_INVALID);
  NdVar IndexOff;
  if (HasIndex) {
    auto RI = mapCapstoneReg(static_cast<arm_reg>(MemOp.mem.index));
    NdVar IdxVal = NdVar::reg(RI.Offset, 4);
    if (MemOp.shift.type != ARM_SFT_INVALID &&
        (MemOp.shift.value != 0 || MemOp.shift.type == ARM_SFT_RRX))
      IdxVal = emitImmShift(S, IdxVal, MemOp.shift.type, MemOp.shift.value);
    if (MemOp.mem.scale == -1 || MemOp.subtracted) {
      IndexOff = S.makeTemp(4);
      S.emit(NdOp::INT_NEG2, IndexOff, {IdxVal});
    } else {
      IndexOff = IdxVal;
    }
    if (!PostIdx)
      Acc(IndexOff);
  }
  int64_t SignedDisp = MemOp.mem.disp;
  if (MemOp.subtracted && !HasIndex)
    SignedDisp = -(SignedDisp < 0 ? -SignedDisp : SignedDisp);
  if (!PostIdx && SignedDisp != 0)
    Acc(NdVar::cst(static_cast<uint64_t>(SignedDisp), 4));
  if (First)
    Acc(NdVar::cst(0, 4));

  if (Insn->detail->writeback && MemOp.mem.base != ARM_REG_INVALID) {
    auto RI = mapCapstoneReg(static_cast<arm_reg>(MemOp.mem.base));
    NdVar BaseReg = NdVar::reg(RI.Offset, 4);
    if (!PostIdx) {
      // Pre-indexed: base takes the computed access address.
      S.emit(NdOp::COPY, BaseReg, {EA});
    } else if (HasIndex) {
      // Register post-index: base advances by the (shifted/signed) index.
      S.emit(NdOp::INT_ADD, BaseReg, {BaseReg, IndexOff});
    } else {
      int64_t WBOffset = MemOp.mem.disp;
      if (ARM.op_count >= 3 && ARM.operands[2].type == ARM_OP_IMM)
        WBOffset = ARM.operands[2].imm;
      if (MemOp.subtracted)
        WBOffset = -(WBOffset < 0 ? -WBOffset : WBOffset);
      if (WBOffset != 0)
        S.emit(NdOp::INT_ADD, BaseReg,
               {BaseReg, NdVar::cst(static_cast<uint64_t>(
                                          static_cast<uint32_t>(WBOffset)),
                                      4)});
    }
  }
  return EA;
}

} // namespace neverd
