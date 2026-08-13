//===- ARMLiftMulHalfAccum.cpp - ARM32 halfword multiply-accumulate lifter ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The 32-bit accumulating halfword multiplies: SMLA{BB,BT,TB,TT},
/// SMLAW{B,T}, SMLAD{,X} and SMLSD{,X}.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftMulHalfAccum(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                      const cs_arm &ARM) {
  switch (Insn->id) {
  // SMLA{BB|BT|TB|TT}: extract 16-bit halves, multiply, accumulate
  case ARM_INS_SMLABB:
  case ARM_INS_SMLABT:
  case ARM_INS_SMLATB:
  case ARM_INS_SMLATT: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar AFull = L.operandRead(S, ARM.operands[1]);
    NdVar BFull = L.operandRead(S, ARM.operands[2]);
    NdVar C = L.operandRead(S, ARM.operands[3]);
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
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar BFull = L.operandRead(S, ARM.operands[2]);
    NdVar C = L.operandRead(S, ARM.operands[3]);
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
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar C = L.operandRead(S, ARM.operands[3]);
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
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar C = L.operandRead(S, ARM.operands[3]);
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

  default:
    return false;
  }
  return true;
}

} // namespace neverd
