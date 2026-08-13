//===- ARMLiftMulHalfProduct.cpp - ARM32 halfword multiply lifter --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The non-accumulating halfword multiplies: SMUL{BB,BT,TB,TT},
/// SMULW{B,T}, SMUAD{,X} and SMUSD{,X}.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftMulHalfProduct(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                        const cs_arm &ARM) {
  switch (Insn->id) {
  // SMUL{BB|BT|TB|TT}: extract halves then multiply
  case ARM_INS_SMULBB:
  case ARM_INS_SMULBT:
  case ARM_INS_SMULTB:
  case ARM_INS_SMULTT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar AFull = L.operandRead(S, ARM.operands[1]);
    NdVar BFull = L.operandRead(S, ARM.operands[2]);
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
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar BFull = L.operandRead(S, ARM.operands[2]);
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
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
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
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
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

  default:
    return false;
  }
  return true;
}

} // namespace neverd
