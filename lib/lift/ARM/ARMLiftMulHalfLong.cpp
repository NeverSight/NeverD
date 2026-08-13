//===- ARMLiftMulHalfLong.cpp - ARM32 long halfword multiply-accumulate lifter ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The 64-bit accumulating halfword multiplies: SMLAL{BB,BT,TB,TT},
/// SMLALD{,X} and SMLSLD{,X}.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftMulHalfLong(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                     const cs_arm &ARM) {
  switch (Insn->id) {
  case ARM_INS_SMLALBB:
  case ARM_INS_SMLALBT:
  case ARM_INS_SMLALTB:
  case ARM_INS_SMLALTT: {
    if (ARM.op_count < 4)
      break;
    NdVar DstLo = L.operandWrite(ARM.operands[0]);
    NdVar DstHi = L.operandWrite(ARM.operands[1]);
    NdVar AFull = L.operandRead(S, ARM.operands[2]);
    NdVar BFull = L.operandRead(S, ARM.operands[3]);
    NdVar AHalf = S.makeTemp(4);
    if (Insn->id == ARM_INS_SMLALBB || Insn->id == ARM_INS_SMLALBT) {
      NdVar A16 = S.makeTemp(2);
      S.emit(NdOp::SUBBYTES, A16, {AFull, NdVar::cst(0, 4)});
      S.emit(NdOp::INT_SEXT, AHalf, {A16});
    } else {
      S.emit(NdOp::INT_ASHR, AHalf, {AFull, NdVar::cst(16, 4)});
    }
    NdVar BHalf = S.makeTemp(4);
    if (Insn->id == ARM_INS_SMLALBB || Insn->id == ARM_INS_SMLALTB) {
      NdVar B16 = S.makeTemp(2);
      S.emit(NdOp::SUBBYTES, B16, {BFull, NdVar::cst(0, 4)});
      S.emit(NdOp::INT_SEXT, BHalf, {B16});
    } else {
      S.emit(NdOp::INT_ASHR, BHalf, {BFull, NdVar::cst(16, 4)});
    }
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, AExt, {AHalf});
    S.emit(NdOp::INT_SEXT, BExt, {BHalf});
    NdVar Prod = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod, {AExt, BExt});
    NdVar OldLoExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, OldLoExt, {NdVar::reg(DstLo.Offset, 4)});
    NdVar OldHiExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, OldHiExt, {NdVar::reg(DstHi.Offset, 4)});
    NdVar OldHiShifted = S.makeTemp(8);
    S.emit(NdOp::INT_LEFT, OldHiShifted, {OldHiExt, NdVar::cst(32, 8)});
    NdVar Accum = S.makeTemp(8);
    S.emit(NdOp::INT_OR, Accum, {OldHiShifted, OldLoExt});
    NdVar Result = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, Result, {Accum, Prod});
    S.emit(NdOp::SUBBYTES, DstLo, {Result, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, DstHi, {Result, NdVar::cst(4, 4)});
    break;
  }
  case ARM_INS_SMLALD:
  case ARM_INS_SMLALDX: {
    if (ARM.op_count < 4)
      break;
    NdVar DstLo = L.operandWrite(ARM.operands[0]);
    NdVar DstHi = L.operandWrite(ARM.operands[1]);
    NdVar A = L.operandRead(S, ARM.operands[2]);
    NdVar B = L.operandRead(S, ARM.operands[3]);
    NdVar ALo16 = S.makeTemp(2);
    NdVar AHi = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, ALo16, {A, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_ASHR, AHi, {A, NdVar::cst(16, 4)});
    NdVar ALo = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, ALo, {ALo16});
    NdVar BLo16 = S.makeTemp(2);
    NdVar BHi = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, BLo16, {B, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_ASHR, BHi, {B, NdVar::cst(16, 4)});
    NdVar BLo = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, BLo, {BLo16});
    NdVar Prod1 = S.makeTemp(4);
    NdVar Prod2 = S.makeTemp(4);
    if (Insn->id == ARM_INS_SMLALDX) {
      S.emit(NdOp::INT_MULT, Prod1, {ALo, BHi});
      S.emit(NdOp::INT_MULT, Prod2, {AHi, BLo});
    } else {
      S.emit(NdOp::INT_MULT, Prod1, {ALo, BLo});
      S.emit(NdOp::INT_MULT, Prod2, {AHi, BHi});
    }
    // Sum the two products in 64-bit precision before the 64-bit accumulate:
    // each signed 16x16 product is up to 2^30, so their sum can reach 2^31 and
    // overflow a signed 32-bit intermediate (Rn=Rm=0x80008000).  Summing in a
    // 4-byte temp wrapped to -2^31 and sign-extended to 0xFFFFFFFF80000000,
    // corrupting RdHi.  Sign-extend each product first (the single-product
    // SMLALBB sibling already widens before the multiply).
    NdVar P1Ext = S.makeTemp(8);
    NdVar P2Ext = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, P1Ext, {Prod1});
    S.emit(NdOp::INT_SEXT, P2Ext, {Prod2});
    NdVar DualSumExt = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, DualSumExt, {P1Ext, P2Ext});
    NdVar OldLoExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, OldLoExt, {NdVar::reg(DstLo.Offset, 4)});
    NdVar OldHiExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, OldHiExt, {NdVar::reg(DstHi.Offset, 4)});
    NdVar OldHiShifted = S.makeTemp(8);
    S.emit(NdOp::INT_LEFT, OldHiShifted, {OldHiExt, NdVar::cst(32, 8)});
    NdVar Accum = S.makeTemp(8);
    S.emit(NdOp::INT_OR, Accum, {OldHiShifted, OldLoExt});
    NdVar Result = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, Result, {Accum, DualSumExt});
    S.emit(NdOp::SUBBYTES, DstLo, {Result, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, DstHi, {Result, NdVar::cst(4, 4)});
    break;
  }
  case ARM_INS_SMLSLD:
  case ARM_INS_SMLSLDX: {
    if (ARM.op_count < 4)
      break;
    NdVar DstLo = L.operandWrite(ARM.operands[0]);
    NdVar DstHi = L.operandWrite(ARM.operands[1]);
    NdVar A = L.operandRead(S, ARM.operands[2]);
    NdVar B = L.operandRead(S, ARM.operands[3]);
    NdVar ALo16 = S.makeTemp(2);
    NdVar AHi = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, ALo16, {A, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_ASHR, AHi, {A, NdVar::cst(16, 4)});
    NdVar ALo = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, ALo, {ALo16});
    NdVar BLo16 = S.makeTemp(2);
    NdVar BHi = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, BLo16, {B, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_ASHR, BHi, {B, NdVar::cst(16, 4)});
    NdVar BLo = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, BLo, {BLo16});
    NdVar Prod1 = S.makeTemp(4);
    NdVar Prod2 = S.makeTemp(4);
    if (Insn->id == ARM_INS_SMLSLDX) {
      S.emit(NdOp::INT_MULT, Prod1, {ALo, BHi});
      S.emit(NdOp::INT_MULT, Prod2, {AHi, BLo});
    } else {
      S.emit(NdOp::INT_MULT, Prod1, {ALo, BLo});
      S.emit(NdOp::INT_MULT, Prod2, {AHi, BHi});
    }
    // Difference in 64-bit precision (consistency with SMLALD): the diff of two
    // signed 16x16 products is bounded to +/-2147450880 so it fits int32, but
    // the 64-bit accumulate should not rely on that.  Sign-extend each product
    // first, then subtract in 64 bits.
    NdVar P1Ext = S.makeTemp(8);
    NdVar P2Ext = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, P1Ext, {Prod1});
    S.emit(NdOp::INT_SEXT, P2Ext, {Prod2});
    NdVar DualDiffExt = S.makeTemp(8);
    S.emit(NdOp::INT_SUB, DualDiffExt, {P1Ext, P2Ext});
    NdVar OldLoExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, OldLoExt, {NdVar::reg(DstLo.Offset, 4)});
    NdVar OldHiExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, OldHiExt, {NdVar::reg(DstHi.Offset, 4)});
    NdVar OldHiShifted = S.makeTemp(8);
    S.emit(NdOp::INT_LEFT, OldHiShifted, {OldHiExt, NdVar::cst(32, 8)});
    NdVar Accum = S.makeTemp(8);
    S.emit(NdOp::INT_OR, Accum, {OldHiShifted, OldLoExt});
    NdVar Result = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, Result, {Accum, DualDiffExt});
    S.emit(NdOp::SUBBYTES, DstLo, {Result, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, DstHi, {Result, NdVar::cst(4, 4)});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
