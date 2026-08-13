//===- AArch64LiftSVEWiden.cpp - SVE2 widening and narrowing ops ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Top/bottom widening add/subtract and multiply (SADDLB/SMULLB/
/// SMLALB/...), matrix and dot products (SMMLA/USDOT/FDOT),
/// narrowing shifts (SHRNB/SQSHRNT/...), saturating converts,
/// absolute-difference accumulate, clamps, bit permute
/// (BDEP/BEXT/BGRP), unpacking and the byte/element reverses.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftSVEWiden(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // SVE2 widening/narrowing integer ops
  case AARCH64_INS_SADDLB:
  case AARCH64_INS_SADDLBT:
  case AARCH64_INS_SADDLT:
  case AARCH64_INS_UADDLB:
  case AARCH64_INS_UADDLT:
  case AARCH64_INS_SADDWB:
  case AARCH64_INS_SADDWT:
  case AARCH64_INS_UADDWB:
  case AARCH64_INS_UADDWT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    break;
  }
  case AARCH64_INS_SSUBLB:
  case AARCH64_INS_SSUBLBT:
  case AARCH64_INS_SSUBLT:
  case AARCH64_INS_SSUBLTB:
  case AARCH64_INS_USUBLB:
  case AARCH64_INS_USUBLT:
  case AARCH64_INS_SSUBWB:
  case AARCH64_INS_SSUBWT:
  case AARCH64_INS_USUBWB:
  case AARCH64_INS_USUBWT:
  case AARCH64_INS_SHSUBR:
  case AARCH64_INS_UHSUBR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_SUB, Dst, {A, B});
    break;
  }
  case AARCH64_INS_SMULLB:
  case AARCH64_INS_SMULLT:
  case AARCH64_INS_UMULLB:
  case AARCH64_INS_UMULLT:
  case AARCH64_INS_PMULLB:
  case AARCH64_INS_PMULLT:
  case AARCH64_INS_SQDMULLB:
  case AARCH64_INS_SQDMULLT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_MULT, Dst, {A, B});
    break;
  }
  case AARCH64_INS_SMLALB:
  case AARCH64_INS_SMLALT:
  case AARCH64_INS_SMLALL:
  case AARCH64_INS_UMLALB:
  case AARCH64_INS_UMLALT:
  case AARCH64_INS_UMLALL:
  case AARCH64_INS_SQDMLALB:
  case AARCH64_INS_SQDMLALBT:
  case AARCH64_INS_SQDMLALT:
  case AARCH64_INS_SUMLALL:
  case AARCH64_INS_USMLALL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_ADD, Dst, {Dst, Prod});
    break;
  }
  case AARCH64_INS_SMLSLB:
  case AARCH64_INS_SMLSLT:
  case AARCH64_INS_SMLSLL:
  case AARCH64_INS_UMLSLB:
  case AARCH64_INS_UMLSLT:
  case AARCH64_INS_UMLSLL:
  case AARCH64_INS_SQDMLSLB:
  case AARCH64_INS_SQDMLSLBT:
  case AARCH64_INS_SQDMLSLT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_SUB, Dst, {Dst, Prod});
    break;
  }
  case AARCH64_INS_SMMLA:
  case AARCH64_INS_UMMLA:
  case AARCH64_INS_USMMLA:
  case AARCH64_INS_SUDOT:
  case AARCH64_INS_USDOT:
  case AARCH64_INS_SUVDOT:
  case AARCH64_INS_USVDOT:
  case AARCH64_INS_SVDOT:
  case AARCH64_INS_UVDOT:
  case AARCH64_INS_FVDOT:
  case AARCH64_INS_FVDOTB:
  case AARCH64_INS_FVDOTT:
  case AARCH64_INS_FDOT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_ADD, Dst, {Dst, Prod});
    break;
  }

  // SVE2 narrowing shifts
  case AARCH64_INS_SHRNB:
  case AARCH64_INS_SHRNT:
  case AARCH64_INS_RSHRNB:
  case AARCH64_INS_RSHRNT:
  case AARCH64_INS_SQSHRNB:
  case AARCH64_INS_SQSHRNT:
  case AARCH64_INS_SQSHRUNB:
  case AARCH64_INS_SQSHRUNT:
  case AARCH64_INS_SQRSHRNB:
  case AARCH64_INS_SQRSHRNT:
  case AARCH64_INS_SQRSHRUNB:
  case AARCH64_INS_SQRSHRUNT:
  case AARCH64_INS_UQSHRNB:
  case AARCH64_INS_UQSHRNT:
  case AARCH64_INS_UQRSHRNB:
  case AARCH64_INS_UQRSHRNT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    NdVar Amt = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_RIGHT, Dst, {Src, Amt});
    break;
  }
  case AARCH64_INS_SSHLLB:
  case AARCH64_INS_SSHLLT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    if (ARM64.op_count >= 3) {
      NdVar Ext = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SEXT, Ext, {Src});
      NdVar Amt = L.operandRead(S, ARM64.operands[2]);
      S.emit(NdOp::INT_LEFT, Dst, {Ext, Amt});
    } else {
      S.emit(NdOp::INT_SEXT, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_USHLLB:
  case AARCH64_INS_USHLLT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    if (ARM64.op_count >= 3) {
      NdVar Ext = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, Ext, {Src});
      NdVar Amt = L.operandRead(S, ARM64.operands[2]);
      S.emit(NdOp::INT_LEFT, Dst, {Ext, Amt});
    } else {
      S.emit(NdOp::INT_ZEXT, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_SQRSHLR:
  case AARCH64_INS_UQRSHLR:
  case AARCH64_INS_SRSHLR:
  case AARCH64_INS_URSHLR:
  case AARCH64_INS_SQSHLR:
  case AARCH64_INS_UQSHLR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_LEFT, Dst, {A, B});
    break;
  }
  case AARCH64_INS_SQSUBR:
  case AARCH64_INS_UQSUBR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_SUB, Dst, {B, A});
    break;
  }
  case AARCH64_INS_SQCVT:
  case AARCH64_INS_SQCVTN:
  case AARCH64_INS_SQCVTU:
  case AARCH64_INS_SQCVTUN:
  case AARCH64_INS_UQCVT:
  case AARCH64_INS_UQCVTN:
  case AARCH64_INS_SQRSHR:
  case AARCH64_INS_SQRSHRU:
  case AARCH64_INS_UQRSHR: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_SQRDCMLAH: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_ADD, Dst, {Dst, Prod});
    break;
  }

  // SVE2 misc: SABDLB/SABALB/UABDLB/UABALB etc.
  case AARCH64_INS_SABDLB:
  case AARCH64_INS_SABDLT:
  case AARCH64_INS_UABDLB:
  case AARCH64_INS_UABDLT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_SUB, Dst, {A, B});
    break;
  }
  case AARCH64_INS_SABALB:
  case AARCH64_INS_SABALT:
  case AARCH64_INS_UABALB:
  case AARCH64_INS_UABALT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar Diff = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_SUB, Diff, {A, B});
    S.emit(NdOp::INT_ADD, Dst, {Dst, Diff});
    break;
  }
  case AARCH64_INS_SCLAMP:
  case AARCH64_INS_UCLAMP:
  case AARCH64_INS_FCLAMP: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case AARCH64_INS_HISTCNT:
  case AARCH64_INS_HISTSEG: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    S.emit(NdOp::COPY, Dst, {NdVar::cst(0, Dst.Size)});
    break;
  }
  case AARCH64_INS_BDEP:
  case AARCH64_INS_BEXT:
  case AARCH64_INS_BGRP: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_AND, Dst, {A, B});
    break;
  }

  // SVE narrowing / unpacking
  case AARCH64_INS_SQXTNT:
  case AARCH64_INS_SQXTUNB:
  case AARCH64_INS_SQXTUNT:
  case AARCH64_INS_UQXTNB:
  case AARCH64_INS_UQXTNT: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_SUNPKHI:
  case AARCH64_INS_SUNPKLO:
  case AARCH64_INS_UUNPKHI:
  case AARCH64_INS_UUNPKLO:
  case AARCH64_INS_SUNPK:
  case AARCH64_INS_UUNPK: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_REVB:
  case AARCH64_INS_REVD:
  case AARCH64_INS_REVH:
  case AARCH64_INS_REVW: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
