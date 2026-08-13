//===- AArch64LiftFPCondCompare.cpp - Floating-point conditional compare --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// FCCMP/FCCMPE: compare when the condition holds, otherwise
/// write the immediate NZCV bits.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftFPCondCompare(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                       const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // Float conditional compare
  case AARCH64_INS_FCCMP:
  case AARCH64_INS_FCCMPE: {
    if (ARM64.op_count < 3)
      break;
    NdVar A = L.operandRead(S, ARM64.operands[0]);
    NdVar B = L.operandRead(S, ARM64.operands[1]);
    uint64_t NZCVImm = 0;
    if (ARM64.operands[2].type == AARCH64_OP_IMM)
      NZCVImm = ARM64.operands[2].imm;

    NdVar CmpN = S.makeTemp(1);
    S.emit(NdOp::FLOAT_LESS, CmpN, {A, B});
    NdVar CmpZ = S.makeTemp(1);
    S.emit(NdOp::FLOAT_EQUAL, CmpZ, {A, B});
    NdVar Lt = S.makeTemp(1);
    S.emit(NdOp::FLOAT_LESS, Lt, {A, B});
    NdVar CmpC = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, CmpC, {Lt});
    NdVar NanA = S.makeTemp(1);
    NdVar NanB = S.makeTemp(1);
    S.emit(NdOp::FLOAT_ISNAN, NanA, {A});
    S.emit(NdOp::FLOAT_ISNAN, NanB, {B});
    NdVar CmpV = S.makeTemp(1);
    S.emit(NdOp::BOOL_OR, CmpV, {NanA, NanB});

    NdVar Cond = S.makeTemp(1);
    switch (ARM64.cc) {
    case AArch64CC_EQ:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::ZFLAG, 1)});
      break;
    case AArch64CC_NE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::ZFLAG, 1)});
      break;
    case AArch64CC_HS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::CFLAG, 1)});
      break;
    case AArch64CC_LO:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::CFLAG, 1)});
      break;
    case AArch64CC_MI:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::NFLAG, 1)});
      break;
    case AArch64CC_PL:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::NFLAG, 1)});
      break;
    case AArch64CC_VS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_VC:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_GE:
      S.emit(NdOp::INT_EQUAL, Cond,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_LT:
      S.emit(NdOp::INT_NOTEQUAL, Cond,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_GT: {
      NdVar NZ = S.makeTemp(1);
      NdVar EqFlags = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(a64reg::ZFLAG, 1)});
      S.emit(NdOp::INT_EQUAL, EqFlags,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NZ, EqFlags});
      break;
    }
    case AArch64CC_LE: {
      NdVar NeFlags = S.makeTemp(1);
      S.emit(NdOp::INT_NOTEQUAL, NeFlags,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(a64reg::ZFLAG, 1), NeFlags});
      break;
    }
    case AArch64CC_HI: {
      NdVar NZ = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(a64reg::ZFLAG, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NdVar::reg(a64reg::CFLAG, 1), NZ});
      break;
    }
    case AArch64CC_LS: {
      NdVar NC = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NC, {NdVar::reg(a64reg::CFLAG, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(a64reg::ZFLAG, 1), NC});
      break;
    }
    default:
      S.emit(NdOp::COPY, Cond, {NdVar::cst(1, 1)});
      break;
    }

    NdVar ImmN = NdVar::cst((NZCVImm >> 3) & 1, 1);
    NdVar ImmZ = NdVar::cst((NZCVImm >> 2) & 1, 1);
    NdVar ImmC = NdVar::cst((NZCVImm >> 1) & 1, 1);
    NdVar ImmV = NdVar::cst(NZCVImm & 1, 1);
    S.emit(NdOp::SELECT, NdVar::reg(a64reg::NFLAG, 1), {Cond, CmpN, ImmN});
    S.emit(NdOp::SELECT, NdVar::reg(a64reg::ZFLAG, 1), {Cond, CmpZ, ImmZ});
    S.emit(NdOp::SELECT, NdVar::reg(a64reg::CFLAG, 1), {Cond, CmpC, ImmC});
    S.emit(NdOp::SELECT, NdVar::reg(a64reg::VFLAG, 1), {Cond, CmpV, ImmV});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
