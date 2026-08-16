//===- AArch64LiftSVEPredicate.cpp - SVE predicate and length control -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Predicate generation and manipulation (PTRUE/PFALSE/PFIRST/
/// PNEXT/PSEL/PUNPK), vector-length arithmetic (RDVL/ADDVL/CNTB/
/// INCB/DECB/SQINCB/...), element ops (SEL/SPLICE/CPY/INDEX/
/// MOVPRFX), the WHILExx predicates and the BRK* predicate
/// breaks.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftSVEPredicate(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                      const cs_insn *Insn, const cs_aarch64 &ARM64) {
  auto sveCountIntrinsic = [](unsigned InsnId) {
    switch (InsnId) {
    case AARCH64_INS_CNTB:
    case AARCH64_INS_INCB:
    case AARCH64_INS_DECB:
      return Intrinsic::A64_SveCntb;
    case AARCH64_INS_CNTH:
    case AARCH64_INS_INCH:
    case AARCH64_INS_DECH:
      return Intrinsic::A64_SveCnth;
    case AARCH64_INS_CNTW:
    case AARCH64_INS_INCW:
    case AARCH64_INS_DECW:
      return Intrinsic::A64_SveCntw;
    case AARCH64_INS_CNTD:
    case AARCH64_INS_INCD:
    case AARCH64_INS_DECD:
      return Intrinsic::A64_SveCntd;
    default:
      return Intrinsic::None;
    }
  };

  switch (Insn->id) {
  // ========================================================================
  // SVE / SVE2 — predicate and control operations
  // ========================================================================
  case AARCH64_INS_PTRUE:
  case AARCH64_INS_PTRUES: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emit(NdOp::COPY, Dst, {NdVar::cst(0xFFFFFFFFFFFFFFFFULL, Dst.Size)});
    }
    break;
  }
  case AARCH64_INS_PFALSE: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emit(NdOp::COPY, Dst, {NdVar::cst(0, Dst.Size)});
    }
    break;
  }
  case AARCH64_INS_PFIRST:
  case AARCH64_INS_PNEXT:
  case AARCH64_INS_PSEL:
  case AARCH64_INS_PEXT:
  case AARCH64_INS_PUNPKHI:
  case AARCH64_INS_PUNPKLO: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_PTEST:
  case AARCH64_INS_RDFFR:
  case AARCH64_INS_RDFFRS:
  case AARCH64_INS_WRFFR:
    S.emitIntrinsic(Intrinsic::A64_Wrffr);
    break;
  case AARCH64_INS_SETFFR:
    S.emitIntrinsic(Intrinsic::A64_Setffr);
    break;
  case AARCH64_INS_RDSVL:
  case AARCH64_INS_RDVL:
  case AARCH64_INS_ADDVL:
  case AARCH64_INS_ADDPL:
  case AARCH64_INS_ADDSPL:
  case AARCH64_INS_ADDSVL:
  case AARCH64_INS_ADDPT: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[1]);
      if (ARM64.op_count >= 3) {
        NdVar Imm = L.operandRead(S, ARM64.operands[2]);
        S.emit(NdOp::INT_ADD, Dst, {Src, Imm});
      } else {
        S.emit(NdOp::COPY, Dst, {Src});
      }
    }
    break;
  }
  case AARCH64_INS_CNTB:
  case AARCH64_INS_CNTD:
  case AARCH64_INS_CNTH:
  case AARCH64_INS_CNTW: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emitIntrinsic(sveCountIntrinsic(Insn->id), Dst,
                      {NdVar::cst(31, 4)});
    }
    break;
  }
  case AARCH64_INS_CNTP: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emit(NdOp::COPY, Dst, {NdVar::cst(16, Dst.Size)});
    }
    break;
  }
  case AARCH64_INS_INCB:
  case AARCH64_INS_INCD:
  case AARCH64_INS_INCH:
  case AARCH64_INS_INCW: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Count = S.makeTemp(Dst.Size);
      S.emitIntrinsic(sveCountIntrinsic(Insn->id), Count,
                      {NdVar::cst(31, 4)});
      S.emit(NdOp::INT_ADD, Dst,
             {NdVar::reg(Dst.Offset, Dst.Size), Count});
    }
    break;
  }
  case AARCH64_INS_INCP: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emit(NdOp::INT_ADD, Dst,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(1, Dst.Size)});
    }
    break;
  }
  case AARCH64_INS_DECB:
  case AARCH64_INS_DECD:
  case AARCH64_INS_DECH:
  case AARCH64_INS_DECW: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Count = S.makeTemp(Dst.Size);
      S.emitIntrinsic(sveCountIntrinsic(Insn->id), Count,
                      {NdVar::cst(31, 4)});
      S.emit(NdOp::INT_SUB, Dst,
             {NdVar::reg(Dst.Offset, Dst.Size), Count});
    }
    break;
  }
  case AARCH64_INS_DECP: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emit(NdOp::INT_SUB, Dst,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(1, Dst.Size)});
    }
    break;
  }
  case AARCH64_INS_SQDECB:
  case AARCH64_INS_SQDECD:
  case AARCH64_INS_SQDECH:
  case AARCH64_INS_SQDECW:
  case AARCH64_INS_SQDECP:
  case AARCH64_INS_UQDECB:
  case AARCH64_INS_UQDECD:
  case AARCH64_INS_UQDECH:
  case AARCH64_INS_UQDECW:
  case AARCH64_INS_UQDECP: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emit(NdOp::INT_SUB, Dst,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(1, Dst.Size)});
    }
    break;
  }
  case AARCH64_INS_SQINCB:
  case AARCH64_INS_SQINCD:
  case AARCH64_INS_SQINCH:
  case AARCH64_INS_SQINCW:
  case AARCH64_INS_SQINCP:
  case AARCH64_INS_UQINCB:
  case AARCH64_INS_UQINCD:
  case AARCH64_INS_UQINCH:
  case AARCH64_INS_UQINCP:
  case AARCH64_INS_UQINCW: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emit(NdOp::INT_ADD, Dst,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(1, Dst.Size)});
    }
    break;
  }

  // SVE element operations
  case AARCH64_INS_SEL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {A});
    break;
  }
  case AARCH64_INS_SPLICE:
  case AARCH64_INS_COMPACT: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_CPY:
  case AARCH64_INS_CLASTA:
  case AARCH64_INS_CLASTB:
  case AARCH64_INS_LASTA:
  case AARCH64_INS_LASTB: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_INDEX:
  case AARCH64_INS_INSR:
  case AARCH64_INS_DUPM:
  case AARCH64_INS_DUPQ: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_MOVPRFX: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case AARCH64_INS_MOVT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // SVE WHILExx predicates
  case AARCH64_INS_WHILEGE:
  case AARCH64_INS_WHILEGT:
  case AARCH64_INS_WHILEHI:
  case AARCH64_INS_WHILEHS:
  case AARCH64_INS_WHILELE:
  case AARCH64_INS_WHILELO:
  case AARCH64_INS_WHILELS:
  case AARCH64_INS_WHILELT:
  case AARCH64_INS_WHILERW:
  case AARCH64_INS_WHILEWR: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emit(NdOp::COPY, Dst, {NdVar::cst(0xFFFFFFFFFFFFFFFFULL, Dst.Size)});
    }
    break;
  }
  case AARCH64_INS_CTERMEQ:
  case AARCH64_INS_CTERMNE: {
    if (ARM64.op_count < 2)
      break;
    NdVar A = L.operandRead(S, ARM64.operands[0]);
    NdVar B = L.operandRead(S, ARM64.operands[1]);
    NdVar Cmp = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, Cmp, {A, B});
    break;
  }

  // SVE BRKA/BRKB/BRKN/BRKPA/BRKPB — predicate break
  case AARCH64_INS_BRKA:
  case AARCH64_INS_BRKAS:
  case AARCH64_INS_BRKB:
  case AARCH64_INS_BRKBS:
  case AARCH64_INS_BRKN:
  case AARCH64_INS_BRKNS:
  case AARCH64_INS_BRKPA:
  case AARCH64_INS_BRKPAS:
  case AARCH64_INS_BRKPB:
  case AARCH64_INS_BRKPBS: {
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
