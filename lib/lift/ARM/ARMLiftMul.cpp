//===- ARMLiftMul.cpp - ARM32 multiply/divide instruction lifter ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM32 multiply and divide instruction handlers: MUL, MLA, MLS, SDIV, UDIV,
/// SMULL, UMULL, and all signed/unsigned multiply-accumulate long, halfword
/// multiply, dual multiply, and saturating multiply variants.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

/// Rounding constant the "R" variants (SMMULR/SMMLAR/SMMLSR) add to the 64-bit
/// intermediate before extracting the most-significant word; it rounds the
/// truncated high half to nearest and its carry must reach bit 32.
static constexpr uint64_t kSMMulRound = 0x80000000ULL;

bool ARMLifter::liftMul(LiftState &S, const cs_insn *Insn, const cs_arm &ARM) {
  switch (Insn->id) {

  // --- MUL / MLA / MLS ---
  case ARM_INS_MUL: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    S.emit(NdOp::INT_MULT, Dst, {A, B});
    if (ARM.update_flags)
      emitNZ(S, Dst);
    break;
  }
  case ARM_INS_MLA: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar C = operandRead(S, ARM.operands[3]);
    NdVar Prod = S.makeTemp(4);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_ADD, Dst, {Prod, C});
    break;
  }
  case ARM_INS_MLS: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar C = operandRead(S, ARM.operands[3]);
    NdVar Prod = S.makeTemp(4);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_SUB, Dst, {C, Prod});
    break;
  }

  // --- SDIV / UDIV ---
  case ARM_INS_SDIV: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    S.emit(NdOp::INT_SDIV, Dst, {A, B});
    break;
  }
  case ARM_INS_UDIV: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    S.emit(NdOp::INT_DIV, Dst, {A, B});
    break;
  }

  // --- SMULL / UMULL ---
  case ARM_INS_SMULL: {
    if (ARM.op_count < 4)
      break;
    NdVar DstLo = operandWrite(ARM.operands[0]);
    NdVar DstHi = operandWrite(ARM.operands[1]);
    NdVar A = operandRead(S, ARM.operands[2]);
    NdVar B = operandRead(S, ARM.operands[3]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, AExt, {A});
    S.emit(NdOp::INT_SEXT, BExt, {B});
    NdVar Prod = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod, {AExt, BExt});
    S.emit(NdOp::SUBBYTES, DstLo, {Prod, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, DstHi, {Prod, NdVar::cst(4, 4)});
    break;
  }
  case ARM_INS_UMULL: {
    if (ARM.op_count < 4)
      break;
    NdVar DstLo = operandWrite(ARM.operands[0]);
    NdVar DstHi = operandWrite(ARM.operands[1]);
    NdVar A = operandRead(S, ARM.operands[2]);
    NdVar B = operandRead(S, ARM.operands[3]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, AExt, {A});
    S.emit(NdOp::INT_ZEXT, BExt, {B});
    NdVar Prod = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod, {AExt, BExt});
    S.emit(NdOp::SUBBYTES, DstLo, {Prod, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, DstHi, {Prod, NdVar::cst(4, 4)});
    break;
  }

  // SMLAL (signed multiply-accumulate long): {RdHi:RdLo} += Rn * Rm
  case ARM_INS_SMLAL: {
    if (ARM.op_count < 4)
      break;
    NdVar DstLo = operandWrite(ARM.operands[0]);
    NdVar DstHi = operandWrite(ARM.operands[1]);
    NdVar A = operandRead(S, ARM.operands[2]);
    NdVar B = operandRead(S, ARM.operands[3]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, AExt, {A});
    S.emit(NdOp::INT_SEXT, BExt, {B});
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
  // UMLAL (unsigned multiply-accumulate long): {RdHi:RdLo} += Rn * Rm
  case ARM_INS_UMLAL: {
    if (ARM.op_count < 4)
      break;
    NdVar DstLo = operandWrite(ARM.operands[0]);
    NdVar DstHi = operandWrite(ARM.operands[1]);
    NdVar A = operandRead(S, ARM.operands[2]);
    NdVar B = operandRead(S, ARM.operands[3]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, AExt, {A});
    S.emit(NdOp::INT_ZEXT, BExt, {B});
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
  // UMAAL: {RdHi:RdLo} = Rn * Rm + RdHi + RdLo (double accumulate)
  case ARM_INS_UMAAL: {
    if (ARM.op_count < 4)
      break;
    NdVar DstLo = operandWrite(ARM.operands[0]);
    NdVar DstHi = operandWrite(ARM.operands[1]);
    NdVar A = operandRead(S, ARM.operands[2]);
    NdVar B = operandRead(S, ARM.operands[3]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, AExt, {A});
    S.emit(NdOp::INT_ZEXT, BExt, {B});
    NdVar Prod = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod, {AExt, BExt});
    NdVar AddLo = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, AddLo, {NdVar::reg(DstLo.Offset, 4)});
    NdVar AddHi = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, AddHi, {NdVar::reg(DstHi.Offset, 4)});
    NdVar Sum1 = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, Sum1, {Prod, AddLo});
    NdVar Result = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, Result, {Sum1, AddHi});
    S.emit(NdOp::SUBBYTES, DstLo, {Result, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, DstHi, {Result, NdVar::cst(4, 4)});
    break;
  }

  // SMLA{BB|BT|TB|TT}: extract 16-bit halves, multiply, accumulate
  case ARM_INS_SMLABB:
  case ARM_INS_SMLABT:
  case ARM_INS_SMLATB:
  case ARM_INS_SMLATT: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar AFull = operandRead(S, ARM.operands[1]);
    NdVar BFull = operandRead(S, ARM.operands[2]);
    NdVar C = operandRead(S, ARM.operands[3]);
    NdVar AHalf = S.makeTemp(4);
    if (Insn->id == ARM_INS_SMLABB || Insn->id == ARM_INS_SMLABT) {
      NdVar A16 = S.makeTemp(2);
      S.emit(NdOp::SUBBYTES, A16, {AFull, NdVar::cst(0, 4)});
      S.emit(NdOp::INT_SEXT, AHalf, {A16});
    } else {
      S.emit(NdOp::INT_ASHR, AHalf, {AFull, NdVar::cst(16, 4)});
    }
    NdVar BHalf = S.makeTemp(4);
    if (Insn->id == ARM_INS_SMLABB || Insn->id == ARM_INS_SMLATB) {
      NdVar B16 = S.makeTemp(2);
      S.emit(NdOp::SUBBYTES, B16, {BFull, NdVar::cst(0, 4)});
      S.emit(NdOp::INT_SEXT, BHalf, {B16});
    } else {
      S.emit(NdOp::INT_ASHR, BHalf, {BFull, NdVar::cst(16, 4)});
    }
    NdVar Prod = S.makeTemp(4);
    S.emit(NdOp::INT_MULT, Prod, {AHalf, BHalf});
    S.emit(NdOp::INT_ADD, Dst, {Prod, C});
    break;
  }
  case ARM_INS_SMLAWB:
  case ARM_INS_SMLAWT: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar BFull = operandRead(S, ARM.operands[2]);
    NdVar C = operandRead(S, ARM.operands[3]);
    NdVar BHalf = S.makeTemp(4);
    if (Insn->id == ARM_INS_SMLAWB) {
      NdVar B16 = S.makeTemp(2);
      S.emit(NdOp::SUBBYTES, B16, {BFull, NdVar::cst(0, 4)});
      S.emit(NdOp::INT_SEXT, BHalf, {B16});
    } else {
      S.emit(NdOp::INT_ASHR, BHalf, {BFull, NdVar::cst(16, 4)});
    }
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, AExt, {A});
    S.emit(NdOp::INT_SEXT, BExt, {BHalf});
    NdVar Prod64 = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod64, {AExt, BExt});
    NdVar ProdHi = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, ProdHi, {Prod64, NdVar::cst(2, 4)});
    S.emit(NdOp::INT_ADD, Dst, {ProdHi, C});
    break;
  }
  case ARM_INS_SMLAD:
  case ARM_INS_SMLADX: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar C = operandRead(S, ARM.operands[3]);
    NdVar ALo16 = S.makeTemp(2);
    NdVar AHi16 = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, ALo16, {A, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_ASHR, AHi16, {A, NdVar::cst(16, 4)});
    NdVar ALo = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, ALo, {ALo16});
    NdVar BLo16 = S.makeTemp(2);
    NdVar BHi16 = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, BLo16, {B, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_ASHR, BHi16, {B, NdVar::cst(16, 4)});
    NdVar BLo = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, BLo, {BLo16});
    NdVar Prod1 = S.makeTemp(4);
    NdVar Prod2 = S.makeTemp(4);
    if (Insn->id == ARM_INS_SMLADX) {
      S.emit(NdOp::INT_MULT, Prod1, {ALo, BHi16});
      S.emit(NdOp::INT_MULT, Prod2, {AHi16, BLo});
    } else {
      S.emit(NdOp::INT_MULT, Prod1, {ALo, BLo});
      S.emit(NdOp::INT_MULT, Prod2, {AHi16, BHi16});
    }
    NdVar Sum = S.makeTemp(4);
    S.emit(NdOp::INT_ADD, Sum, {Prod1, Prod2});
    S.emit(NdOp::INT_ADD, Dst, {Sum, C});
    break;
  }
  case ARM_INS_SMLSD:
  case ARM_INS_SMLSDX: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar C = operandRead(S, ARM.operands[3]);
    NdVar ALo16 = S.makeTemp(2);
    NdVar AHi16 = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, ALo16, {A, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_ASHR, AHi16, {A, NdVar::cst(16, 4)});
    NdVar ALo = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, ALo, {ALo16});
    NdVar BLo16 = S.makeTemp(2);
    NdVar BHi16 = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, BLo16, {B, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_ASHR, BHi16, {B, NdVar::cst(16, 4)});
    NdVar BLo = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, BLo, {BLo16});
    NdVar Prod1 = S.makeTemp(4);
    NdVar Prod2 = S.makeTemp(4);
    if (Insn->id == ARM_INS_SMLSDX) {
      S.emit(NdOp::INT_MULT, Prod1, {ALo, BHi16});
      S.emit(NdOp::INT_MULT, Prod2, {AHi16, BLo});
    } else {
      S.emit(NdOp::INT_MULT, Prod1, {ALo, BLo});
      S.emit(NdOp::INT_MULT, Prod2, {AHi16, BHi16});
    }
    NdVar Diff = S.makeTemp(4);
    S.emit(NdOp::INT_SUB, Diff, {Prod1, Prod2});
    S.emit(NdOp::INT_ADD, Dst, {Diff, C});
    break;
  }
  case ARM_INS_SMLALBB:
  case ARM_INS_SMLALBT:
  case ARM_INS_SMLALTB:
  case ARM_INS_SMLALTT: {
    if (ARM.op_count < 4)
      break;
    NdVar DstLo = operandWrite(ARM.operands[0]);
    NdVar DstHi = operandWrite(ARM.operands[1]);
    NdVar AFull = operandRead(S, ARM.operands[2]);
    NdVar BFull = operandRead(S, ARM.operands[3]);
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
    NdVar DstLo = operandWrite(ARM.operands[0]);
    NdVar DstHi = operandWrite(ARM.operands[1]);
    NdVar A = operandRead(S, ARM.operands[2]);
    NdVar B = operandRead(S, ARM.operands[3]);
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
    NdVar DstLo = operandWrite(ARM.operands[0]);
    NdVar DstHi = operandWrite(ARM.operands[1]);
    NdVar A = operandRead(S, ARM.operands[2]);
    NdVar B = operandRead(S, ARM.operands[3]);
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

  // SMUL{BB|BT|TB|TT}: extract halves then multiply
  case ARM_INS_SMULBB:
  case ARM_INS_SMULBT:
  case ARM_INS_SMULTB:
  case ARM_INS_SMULTT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar AFull = operandRead(S, ARM.operands[1]);
    NdVar BFull = operandRead(S, ARM.operands[2]);
    NdVar AHalf = S.makeTemp(4);
    if (Insn->id == ARM_INS_SMULBB || Insn->id == ARM_INS_SMULBT) {
      NdVar A16 = S.makeTemp(2);
      S.emit(NdOp::SUBBYTES, A16, {AFull, NdVar::cst(0, 4)});
      S.emit(NdOp::INT_SEXT, AHalf, {A16});
    } else {
      S.emit(NdOp::INT_ASHR, AHalf, {AFull, NdVar::cst(16, 4)});
    }
    NdVar BHalf = S.makeTemp(4);
    if (Insn->id == ARM_INS_SMULBB || Insn->id == ARM_INS_SMULTB) {
      NdVar B16 = S.makeTemp(2);
      S.emit(NdOp::SUBBYTES, B16, {BFull, NdVar::cst(0, 4)});
      S.emit(NdOp::INT_SEXT, BHalf, {B16});
    } else {
      S.emit(NdOp::INT_ASHR, BHalf, {BFull, NdVar::cst(16, 4)});
    }
    S.emit(NdOp::INT_MULT, Dst, {AHalf, BHalf});
    break;
  }
  case ARM_INS_SMULWB:
  case ARM_INS_SMULWT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar BFull = operandRead(S, ARM.operands[2]);
    NdVar BHalf = S.makeTemp(4);
    if (Insn->id == ARM_INS_SMULWB) {
      NdVar B16 = S.makeTemp(2);
      S.emit(NdOp::SUBBYTES, B16, {BFull, NdVar::cst(0, 4)});
      S.emit(NdOp::INT_SEXT, BHalf, {B16});
    } else {
      S.emit(NdOp::INT_ASHR, BHalf, {BFull, NdVar::cst(16, 4)});
    }
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, AExt, {A});
    S.emit(NdOp::INT_SEXT, BExt, {BHalf});
    NdVar Prod64 = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod64, {AExt, BExt});
    S.emit(NdOp::SUBBYTES, Dst, {Prod64, NdVar::cst(2, 4)});
    break;
  }
  case ARM_INS_SMUAD:
  case ARM_INS_SMUADX: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar ALo16 = S.makeTemp(2);
    NdVar AHi16 = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, ALo16, {A, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_ASHR, AHi16, {A, NdVar::cst(16, 4)});
    NdVar ALo = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, ALo, {ALo16});
    NdVar BLo16 = S.makeTemp(2);
    NdVar BHi16 = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, BLo16, {B, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_ASHR, BHi16, {B, NdVar::cst(16, 4)});
    NdVar BLo = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, BLo, {BLo16});
    NdVar Prod1 = S.makeTemp(4);
    NdVar Prod2 = S.makeTemp(4);
    if (Insn->id == ARM_INS_SMUADX) {
      S.emit(NdOp::INT_MULT, Prod1, {ALo, BHi16});
      S.emit(NdOp::INT_MULT, Prod2, {AHi16, BLo});
    } else {
      S.emit(NdOp::INT_MULT, Prod1, {ALo, BLo});
      S.emit(NdOp::INT_MULT, Prod2, {AHi16, BHi16});
    }
    S.emit(NdOp::INT_ADD, Dst, {Prod1, Prod2});
    break;
  }
  case ARM_INS_SMUSD:
  case ARM_INS_SMUSDX: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar ALo16 = S.makeTemp(2);
    NdVar AHi16 = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, ALo16, {A, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_ASHR, AHi16, {A, NdVar::cst(16, 4)});
    NdVar ALo = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, ALo, {ALo16});
    NdVar BLo16 = S.makeTemp(2);
    NdVar BHi16 = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, BLo16, {B, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_ASHR, BHi16, {B, NdVar::cst(16, 4)});
    NdVar BLo = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, BLo, {BLo16});
    NdVar Prod1 = S.makeTemp(4);
    NdVar Prod2 = S.makeTemp(4);
    if (Insn->id == ARM_INS_SMUSDX) {
      S.emit(NdOp::INT_MULT, Prod1, {ALo, BHi16});
      S.emit(NdOp::INT_MULT, Prod2, {AHi16, BLo});
    } else {
      S.emit(NdOp::INT_MULT, Prod1, {ALo, BLo});
      S.emit(NdOp::INT_MULT, Prod2, {AHi16, BHi16});
    }
    S.emit(NdOp::INT_SUB, Dst, {Prod1, Prod2});
    break;
  }
  // SMMUL{R}: Rd = (SInt(Rn)*SInt(Rm){ + kSMMulRound })[63:32].  Round on the
  // full 64-bit product so the rounding carry propagates into the high word.
  case ARM_INS_SMMUL:
  case ARM_INS_SMMULR: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, AExt, {A});
    S.emit(NdOp::INT_SEXT, BExt, {B});
    NdVar Prod64 = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod64, {AExt, BExt});
    if (Insn->id == ARM_INS_SMMULR)
      S.emit(NdOp::INT_ADD, Prod64, {Prod64, NdVar::cst(kSMMulRound, 8)});
    S.emit(NdOp::SUBBYTES, Dst, {Prod64, NdVar::cst(4, 4)});
    break;
  }
  // SMMLA{R}: Rd = ((SInt(Ra)<<32) + SInt(Rn)*SInt(Rm){ + kSMMulRound
  // })[63:32]. Accumulate at full width so the rounding carry out of the low
  // half reaches bit 32.
  case ARM_INS_SMMLA:
  case ARM_INS_SMMLAR: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar C = operandRead(S, ARM.operands[3]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, AExt, {A});
    S.emit(NdOp::INT_SEXT, BExt, {B});
    NdVar Prod64 = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod64, {AExt, BExt});
    NdVar CHi = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, CHi, {C});
    S.emit(NdOp::INT_LEFT, CHi, {CHi, NdVar::cst(32, 8)});
    NdVar Acc = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, Acc, {CHi, Prod64});
    if (Insn->id == ARM_INS_SMMLAR)
      S.emit(NdOp::INT_ADD, Acc, {Acc, NdVar::cst(kSMMulRound, 8)});
    S.emit(NdOp::SUBBYTES, Dst, {Acc, NdVar::cst(4, 4)});
    break;
  }
  // SMMLS{R}: Rd = ((SInt(Ra)<<32) - SInt(Rn)*SInt(Rm){ + kSMMulRound
  // })[63:32]. The subtraction borrows out of the low half into bit 32, so it
  // must run at full 64-bit width (the old `Ra - prodHi` form dropped that
  // borrow).
  case ARM_INS_SMMLS:
  case ARM_INS_SMMLSR: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar C = operandRead(S, ARM.operands[3]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, AExt, {A});
    S.emit(NdOp::INT_SEXT, BExt, {B});
    NdVar Prod64 = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod64, {AExt, BExt});
    NdVar CHi = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, CHi, {C});
    S.emit(NdOp::INT_LEFT, CHi, {CHi, NdVar::cst(32, 8)});
    NdVar Acc = S.makeTemp(8);
    S.emit(NdOp::INT_SUB, Acc, {CHi, Prod64});
    if (Insn->id == ARM_INS_SMMLSR)
      S.emit(NdOp::INT_ADD, Acc, {Acc, NdVar::cst(kSMMulRound, 8)});
    S.emit(NdOp::SUBBYTES, Dst, {Acc, NdVar::cst(4, 4)});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
