//===- AArch64LiftSIMDExt.cpp - AArch64 BF16/SVE/SVE2/MOPS lifter --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// BF16 (Brain Float 16), SVE/SVE2 (Scalable Vector Extension), and
/// FEAT_MOPS (Memory Copy/Set) instruction handlers for AArch64.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-aarch64"

namespace neverd {

// Floating-point element size (bytes) of a NEON vector arrangement, for the
// complex (FCADD/FCMLA) per-pair lifting.  Returns 0 for non-FP/unknown.
static unsigned complexElemSize(AArch64Layout_VectorLayout Vas) {
  switch (Vas) {
  case AARCH64LAYOUT_VL_8H:
  case AARCH64LAYOUT_VL_4H:
    return 2;
  case AARCH64LAYOUT_VL_4S:
  case AARCH64LAYOUT_VL_2S:
    return 4;
  case AARCH64LAYOUT_VL_2D:
    return 8;
  default:
    return 0;
  }
}

bool AArch64Lifter::liftSIMDExt(LiftState &S, const cs_insn *Insn,
                                const cs_aarch64 &ARM64) {
  switch (Insn->id) {

  // ========================================================================
  // BF16 (Brain Float 16)
  // ========================================================================
  case AARCH64_INS_BFADD:
  case AARCH64_INS_BFSUB:
  case AARCH64_INS_BFMUL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdOp Opc = NdOp::FLOAT_ADD;
    if (Insn->id == AARCH64_INS_BFSUB)
      Opc = NdOp::FLOAT_SUB;
    else if (Insn->id == AARCH64_INS_BFMUL)
      Opc = NdOp::FLOAT_MULT;
    S.emit(Opc, Dst, {A, B});
    break;
  }
  case AARCH64_INS_BFMAX:
  case AARCH64_INS_BFMAXNM:
  case AARCH64_INS_BFMIN:
  case AARCH64_INS_BFMINNM: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Cmp = S.makeTemp(1);
    S.emit(NdOp::FLOAT_LESS, Cmp, {A, B});
    bool IsMin =
        (Insn->id == AARCH64_INS_BFMIN || Insn->id == AARCH64_INS_BFMINNM);
    if (IsMin)
      S.emit(NdOp::SELECT, Dst, {Cmp, A, B});
    else
      S.emit(NdOp::SELECT, Dst, {Cmp, B, A});
    break;
  }
  case AARCH64_INS_BFCVT:
  case AARCH64_INS_BFCVTN:
  case AARCH64_INS_BFCVTN2:
  case AARCH64_INS_BFCVTNT:
  case AARCH64_INS_BF1CVT:
  case AARCH64_INS_BF1CVTL:
  case AARCH64_INS_BF1CVTL2:
  case AARCH64_INS_BF1CVTLT:
  case AARCH64_INS_BF2CVT:
  case AARCH64_INS_BF2CVTL:
  case AARCH64_INS_BF2CVTL2:
  case AARCH64_INS_BF2CVTLT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::FLOAT_FLOAT2FLOAT, Dst, {Src});
    break;
  }
  case AARCH64_INS_BFDOT:
  case AARCH64_INS_BFVDOT:
  case AARCH64_INS_BFMLA:
  case AARCH64_INS_BFMLAL:
  case AARCH64_INS_BFMLALB:
  case AARCH64_INS_BFMLALT:
  case AARCH64_INS_BFMMLA:
  case AARCH64_INS_BFMOPA:
  case AARCH64_INS_BFMOPS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_ADD, Dst, {Dst, Prod});
    break;
  }
  case AARCH64_INS_BFMLS:
  case AARCH64_INS_BFMLSL:
  case AARCH64_INS_BFMLSLB:
  case AARCH64_INS_BFMLSLT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_SUB, Dst, {Dst, Prod});
    break;
  }
  case AARCH64_INS_BFCLAMP: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // F1CVT / F2CVT (FP8 to Half precision)
  case AARCH64_INS_F1CVT:
  case AARCH64_INS_F1CVTL:
  case AARCH64_INS_F1CVTL2:
  case AARCH64_INS_F1CVTLT:
  case AARCH64_INS_F2CVT:
  case AARCH64_INS_F2CVTL:
  case AARCH64_INS_F2CVTL2:
  case AARCH64_INS_F2CVTLT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::FLOAT_FLOAT2FLOAT, Dst, {Src});
    break;
  }

  // ========================================================================
  // SVE / SVE2 — predicate and control operations
  // ========================================================================
  case AARCH64_INS_PTRUE:
  case AARCH64_INS_PTRUES: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      S.emit(NdOp::COPY, Dst, {NdVar::cst(0xFFFFFFFFFFFFFFFFULL, Dst.Size)});
    }
    break;
  }
  case AARCH64_INS_PFALSE: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
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
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[1]);
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
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[1]);
      if (ARM64.op_count >= 3) {
        NdVar Imm = operandRead(S, ARM64.operands[2]);
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
  case AARCH64_INS_CNTW:
  case AARCH64_INS_CNTP: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      uint64_t Val = 16;
      if (Insn->id == AARCH64_INS_CNTD)
        Val = 2;
      else if (Insn->id == AARCH64_INS_CNTH)
        Val = 8;
      else if (Insn->id == AARCH64_INS_CNTW)
        Val = 4;
      S.emit(NdOp::COPY, Dst, {NdVar::cst(Val, Dst.Size)});
    }
    break;
  }
  case AARCH64_INS_INCB:
  case AARCH64_INS_INCD:
  case AARCH64_INS_INCH:
  case AARCH64_INS_INCW:
  case AARCH64_INS_INCP: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      S.emit(NdOp::INT_ADD, Dst,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(1, Dst.Size)});
    }
    break;
  }
  case AARCH64_INS_DECB:
  case AARCH64_INS_DECD:
  case AARCH64_INS_DECH:
  case AARCH64_INS_DECW:
  case AARCH64_INS_DECP: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
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
      NdVar Dst = operandWrite(ARM64.operands[0]);
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
      NdVar Dst = operandWrite(ARM64.operands[0]);
      S.emit(NdOp::INT_ADD, Dst,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(1, Dst.Size)});
    }
    break;
  }

  // SVE element operations
  case AARCH64_INS_SEL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {A});
    break;
  }
  case AARCH64_INS_SPLICE:
  case AARCH64_INS_COMPACT: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
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
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_INDEX:
  case AARCH64_INS_INSR:
  case AARCH64_INS_DUPM:
  case AARCH64_INS_DUPQ: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_MOVPRFX: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case AARCH64_INS_MOVT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
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
      NdVar Dst = operandWrite(ARM64.operands[0]);
      S.emit(NdOp::COPY, Dst, {NdVar::cst(0xFFFFFFFFFFFFFFFFULL, Dst.Size)});
    }
    break;
  }
  case AARCH64_INS_CTERMEQ:
  case AARCH64_INS_CTERMNE: {
    if (ARM64.op_count < 2)
      break;
    NdVar A = operandRead(S, ARM64.operands[0]);
    NdVar B = operandRead(S, ARM64.operands[1]);
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
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // ========================================================================
  // SVE load/store
  // ========================================================================
  case AARCH64_INS_LD1B:
  case AARCH64_INS_LD1D:
  case AARCH64_INS_LD1H:
  case AARCH64_INS_LD1Q:
  case AARCH64_INS_LD1W:
  case AARCH64_INS_LD1SB:
  case AARCH64_INS_LD1SH:
  case AARCH64_INS_LD1SW:
  case AARCH64_INS_LD1RB:
  case AARCH64_INS_LD1RD:
  case AARCH64_INS_LD1RH:
  case AARCH64_INS_LD1RW:
  case AARCH64_INS_LD1RSB:
  case AARCH64_INS_LD1RSH:
  case AARCH64_INS_LD1RSW:
  case AARCH64_INS_LD1ROB:
  case AARCH64_INS_LD1ROD:
  case AARCH64_INS_LD1ROH:
  case AARCH64_INS_LD1ROW:
  case AARCH64_INS_LD1RQB:
  case AARCH64_INS_LD1RQD:
  case AARCH64_INS_LD1RQH:
  case AARCH64_INS_LD1RQW:
  case AARCH64_INS_LD2B:
  case AARCH64_INS_LD2D:
  case AARCH64_INS_LD2H:
  case AARCH64_INS_LD2Q:
  case AARCH64_INS_LD2W:
  case AARCH64_INS_LD3B:
  case AARCH64_INS_LD3D:
  case AARCH64_INS_LD3H:
  case AARCH64_INS_LD3Q:
  case AARCH64_INS_LD3W:
  case AARCH64_INS_LD4B:
  case AARCH64_INS_LD4D:
  case AARCH64_INS_LD4H:
  case AARCH64_INS_LD4Q:
  case AARCH64_INS_LD4W:
  case AARCH64_INS_LDFF1B:
  case AARCH64_INS_LDFF1D:
  case AARCH64_INS_LDFF1H:
  case AARCH64_INS_LDFF1SB:
  case AARCH64_INS_LDFF1SH:
  case AARCH64_INS_LDFF1SW:
  case AARCH64_INS_LDFF1W:
  case AARCH64_INS_LDNF1B:
  case AARCH64_INS_LDNF1D:
  case AARCH64_INS_LDNF1H:
  case AARCH64_INS_LDNF1SB:
  case AARCH64_INS_LDNF1SH:
  case AARCH64_INS_LDNF1SW:
  case AARCH64_INS_LDNF1W:
  case AARCH64_INS_LDNT1B:
  case AARCH64_INS_LDNT1D:
  case AARCH64_INS_LDNT1H:
  case AARCH64_INS_LDNT1SB:
  case AARCH64_INS_LDNT1SH:
  case AARCH64_INS_LDNT1SW:
  case AARCH64_INS_LDNT1W: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar EA = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::LOAD, Dst, {EA});
    }
    break;
  }
  case AARCH64_INS_LDX:
  case AARCH64_INS_LDY:
  case AARCH64_INS_LDZ:
  case AARCH64_INS_LDZI: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar EA = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::LOAD, Dst, {EA});
    }
    break;
  }
  case AARCH64_INS_ST1B:
  case AARCH64_INS_ST1D:
  case AARCH64_INS_ST1H:
  case AARCH64_INS_ST1Q:
  case AARCH64_INS_ST1W:
  case AARCH64_INS_ST2B:
  case AARCH64_INS_ST2D:
  case AARCH64_INS_ST2H:
  case AARCH64_INS_ST2Q:
  case AARCH64_INS_ST2W:
  case AARCH64_INS_ST3B:
  case AARCH64_INS_ST3D:
  case AARCH64_INS_ST3H:
  case AARCH64_INS_ST3Q:
  case AARCH64_INS_ST3W:
  case AARCH64_INS_ST4B:
  case AARCH64_INS_ST4D:
  case AARCH64_INS_ST4H:
  case AARCH64_INS_ST4Q:
  case AARCH64_INS_ST4W:
  case AARCH64_INS_STNT1B:
  case AARCH64_INS_STNT1D:
  case AARCH64_INS_STNT1H:
  case AARCH64_INS_STNT1W: {
    if (ARM64.op_count >= 2) {
      NdVar Src = operandRead(S, ARM64.operands[0]);
      NdVar EA = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::STORE, {}, {EA, Src});
    }
    break;
  }
  case AARCH64_INS_STX:
  case AARCH64_INS_STY:
  case AARCH64_INS_STZ:
  case AARCH64_INS_STZI: {
    if (ARM64.op_count >= 2) {
      NdVar Src = operandRead(S, ARM64.operands[0]);
      NdVar EA = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::STORE, {}, {EA, Src});
    }
    break;
  }
  case AARCH64_INS_PRFB:
  case AARCH64_INS_PRFD:
  case AARCH64_INS_PRFH:
  case AARCH64_INS_PRFW:
  case AARCH64_INS_RPRFM:
    S.emit(NdOp::NOP, {}, {});
    break;

  // ========================================================================
  // SVE / SVE2 — integer arithmetic
  // ========================================================================
  case AARCH64_INS_ADCLB:
  case AARCH64_INS_ADCLT:
  case AARCH64_INS_SBCLB:
  case AARCH64_INS_SBCLT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    break;
  }
  case AARCH64_INS_ADDHA:
  case AARCH64_INS_ADDVA: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    break;
  }
  case AARCH64_INS_ADDHNB:
  case AARCH64_INS_ADDHNT:
  case AARCH64_INS_SUBHNB:
  case AARCH64_INS_SUBHNT:
  case AARCH64_INS_RADDHNB:
  case AARCH64_INS_RADDHNT:
  case AARCH64_INS_RSUBHNB:
  case AARCH64_INS_RSUBHNT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    break;
  }
  case AARCH64_INS_ADDQV:
  case AARCH64_INS_ANDQV:
  case AARCH64_INS_ANDV:
  case AARCH64_INS_EORQV:
  case AARCH64_INS_EORV:
  case AARCH64_INS_ORQV:
  case AARCH64_INS_ORV:
  case AARCH64_INS_SMAXQV:
  case AARCH64_INS_SMINQV:
  case AARCH64_INS_UMAXQV:
  case AARCH64_INS_UMINQV:
  case AARCH64_INS_SADDV:
  case AARCH64_INS_UADDV:
  case AARCH64_INS_FADDQV:
  case AARCH64_INS_FADDV:
  case AARCH64_INS_FADDA:
  case AARCH64_INS_FMAXNMQV:
  case AARCH64_INS_FMAXQV:
  case AARCH64_INS_FMINNMQV:
  case AARCH64_INS_FMINQV: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_ASRD:
  case AARCH64_INS_ASRR:
  case AARCH64_INS_LSLR:
  case AARCH64_INS_LSRR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdOp Opc = NdOp::INT_ASHR;
    if (Insn->id == AARCH64_INS_LSLR)
      Opc = NdOp::INT_LEFT;
    else if (Insn->id == AARCH64_INS_LSRR)
      Opc = NdOp::INT_RIGHT;
    S.emit(Opc, Dst, {A, B});
    break;
  }
  case AARCH64_INS_CADD:
  case AARCH64_INS_SQCADD: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    break;
  }
  case AARCH64_INS_CDOT:
  case AARCH64_INS_CMLA:
  // FCMLA — rotated complex floating-point multiply-accumulate (FEAT_FCMA).
  // Per complex pair {re@2k, im@2k+1} with the #rot table:
  //   rot 0:   re += Vn.re*Vm.re;  im += Vn.re*Vm.im
  //   rot 90:  re -= Vn.im*Vm.im;  im += Vn.im*Vm.re
  //   rot 180: re -= Vn.re*Vm.re;  im -= Vn.re*Vm.im
  //   rot 270: re += Vn.im*Vm.im;  im -= Vn.im*Vm.re
  // The old code was a whole-register INT_MULT+INT_ADD (integer ops on FP data,
  // no rotation, no per-lane).  The indexed form broadcasts one Vm pair.
  case AARCH64_INS_FCMLA: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Vn = operandRead(S, ARM64.operands[1]);
    NdVar Vm = operandRead(S, ARM64.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    int64_t Rot = ARM64.operands[3].imm;
    int VecIdx = ARM64.operands[2].vector_index;
    unsigned ES = complexElemSize(ARM64.operands[0].vas);
    if ((ES != 4 && ES != 8) || Dst.Size < 2 * ES) {
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::FLOAT_MULT, Prod, {Vn, Vm});
      S.emit(NdOp::FLOAT_ADD, Dst, {OldDst, Prod});
      break;
    }
    bool UseVnIm = (Rot == 90 || Rot == 270);
    bool ReUseVmIm = (Rot == 90 || Rot == 270);
    bool ImUseVmIm = (Rot == 0 || Rot == 180);
    bool ReSub = (Rot == 90 || Rot == 180);
    bool ImSub = (Rot == 180 || Rot == 270);
    auto lane = [&](NdVar V, unsigned Idx) {
      NdVar T = S.makeTemp(ES);
      S.emit(NdOp::SUBBYTES, T,
             {V, NdVar::cst(static_cast<uint64_t>(Idx) * ES, 4)});
      return T;
    };
    NdVar Acc = NdVar::cst(0, 0);
    bool First = true;
    auto append = [&](NdVar L) {
      if (First) {
        Acc = L;
        First = false;
        return;
      }
      NdVar N = S.makeTemp(Acc.Size + ES);
      S.emit(NdOp::CONCAT, N, {L, Acc});
      Acc = N;
    };
    unsigned NLanes = Dst.Size / ES;
    for (unsigned K = 0; K < NLanes / 2; ++K) {
      unsigned BPair = (VecIdx >= 0) ? (unsigned)VecIdx : K;
      NdVar VnEl = lane(Vn, 2 * K + (UseVnIm ? 1 : 0));
      NdVar VmRe = lane(Vm, 2 * BPair);
      NdVar VmIm = lane(Vm, 2 * BPair + 1);
      NdVar AccRe = lane(OldDst, 2 * K);
      NdVar AccIm = lane(OldDst, 2 * K + 1);
      NdVar PRe = S.makeTemp(ES), PIm = S.makeTemp(ES);
      S.emit(NdOp::FLOAT_MULT, PRe, {VnEl, ReUseVmIm ? VmIm : VmRe});
      S.emit(NdOp::FLOAT_MULT, PIm, {VnEl, ImUseVmIm ? VmIm : VmRe});
      NdVar OutRe = S.makeTemp(ES), OutIm = S.makeTemp(ES);
      S.emit(ReSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD, OutRe, {AccRe, PRe});
      S.emit(ImSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD, OutIm, {AccIm, PIm});
      append(OutRe);
      append(OutIm);
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case AARCH64_INS_CNOT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::INT_EQUAL, Dst, {Src, NdVar::cst(0, Src.Size)});
    break;
  }
  case AARCH64_INS_SUBPT:
  case AARCH64_INS_SUBR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    if (Insn->id == AARCH64_INS_SUBR)
      S.emit(NdOp::INT_SUB, Dst, {B, A});
    else
      S.emit(NdOp::INT_SUB, Dst, {A, B});
    break;
  }
  case AARCH64_INS_SDIVR:
  case AARCH64_INS_UDIVR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    if (Insn->id == AARCH64_INS_SDIVR)
      S.emit(NdOp::INT_SDIV, Dst, {B, A});
    else
      S.emit(NdOp::INT_DIV, Dst, {B, A});
    break;
  }
  case AARCH64_INS_FDIVR:
  case AARCH64_INS_FSUBR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    if (Insn->id == AARCH64_INS_FDIVR)
      S.emit(NdOp::FLOAT_DIV, Dst, {B, A});
    else
      S.emit(NdOp::FLOAT_SUB, Dst, {B, A});
    break;
  }
  case AARCH64_INS_MAD:
  case AARCH64_INS_MADPT:
  case AARCH64_INS_MADDPT:
  case AARCH64_INS_MLAPT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    if (ARM64.op_count >= 4) {
      NdVar C = operandRead(S, ARM64.operands[3]);
      S.emit(NdOp::INT_ADD, Dst, {Prod, C});
    } else {
      S.emit(NdOp::COPY, Dst, {Prod});
    }
    break;
  }
  case AARCH64_INS_MSB:
  case AARCH64_INS_MSUBPT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    if (ARM64.op_count >= 4) {
      NdVar C = operandRead(S, ARM64.operands[3]);
      S.emit(NdOp::INT_SUB, Dst, {C, Prod});
    } else {
      S.emit(NdOp::INT_NEG2, Dst, {Prod});
    }
    break;
  }
  case AARCH64_INS_EOR3:
  case AARCH64_INS_NBSL:
  case AARCH64_INS_BSL1N:
  case AARCH64_INS_BSL2N:
  case AARCH64_INS_RAX1: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    break;
  }
  case AARCH64_INS_XAR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    break;
  }
  case AARCH64_INS_EORBT:
  case AARCH64_INS_EORTB: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    break;
  }
  case AARCH64_INS_EORS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    break;
  }
  case AARCH64_INS_ORRS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_OR, Dst, {A, B});
    break;
  }
  case AARCH64_INS_ORNS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar NB = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, NB, {B});
    S.emit(NdOp::INT_OR, Dst, {A, NB});
    break;
  }
  case AARCH64_INS_NAND:
  case AARCH64_INS_NANDS: {
    // NAND: Dst = ~(a & b)
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Anded = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, Anded, {A, B});
    S.emit(NdOp::INT_NOT, Dst, {Anded});
    break;
  }
  case AARCH64_INS_NOR:
  case AARCH64_INS_NORS: {
    // NOR: Dst = ~(a | b)
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Ored = S.makeTemp(A.Size);
    S.emit(NdOp::INT_OR, Ored, {A, B});
    S.emit(NdOp::INT_NOT, Dst, {Ored});
    break;
  }

  // SVE compare (predicate Result)
  case AARCH64_INS_CMPEQ:
  case AARCH64_INS_MATCH: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar A = operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_EQUAL, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_CMPNE:
  case AARCH64_INS_NMATCH: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar A = operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_NOTEQUAL, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_CMPGE: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar A = operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_SLESSEQUAL, Dst, {B, A});
    }
    break;
  }
  case AARCH64_INS_CMPGT: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar A = operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_SLESS, Dst, {B, A});
    }
    break;
  }
  case AARCH64_INS_CMPLE: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar A = operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_SLESSEQUAL, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_CMPLT: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar A = operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_SLESS, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_CMPHI: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar A = operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_LESS, Dst, {B, A});
    }
    break;
  }
  case AARCH64_INS_CMPHS: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar A = operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_LESSEQUAL, Dst, {B, A});
    }
    break;
  }
  case AARCH64_INS_CMPLO: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar A = operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_LESS, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_CMPLS: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar A = operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_LESSEQUAL, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_FCMNE:
  case AARCH64_INS_FCMUO: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar A = operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::FLOAT_NOTEQUAL, Dst, {A, B});
    }
    break;
  }

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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    NdVar Amt = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_RIGHT, Dst, {Src, Amt});
    break;
  }
  case AARCH64_INS_SSHLLB:
  case AARCH64_INS_SSHLLT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    if (ARM64.op_count >= 3) {
      NdVar Ext = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SEXT, Ext, {Src});
      NdVar Amt = operandRead(S, ARM64.operands[2]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    if (ARM64.op_count >= 3) {
      NdVar Ext = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, Ext, {Src});
      NdVar Amt = operandRead(S, ARM64.operands[2]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_LEFT, Dst, {A, B});
    break;
  }
  case AARCH64_INS_SQSUBR:
  case AARCH64_INS_UQSUBR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
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
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_SQRDCMLAH: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_SUB, Dst, {A, B});
    break;
  }
  case AARCH64_INS_SABALB:
  case AARCH64_INS_SABALT:
  case AARCH64_INS_UABALB:
  case AARCH64_INS_UABALT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case AARCH64_INS_HISTCNT:
  case AARCH64_INS_HISTSEG: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    S.emit(NdOp::COPY, Dst, {NdVar::cst(0, Dst.Size)});
    break;
  }
  case AARCH64_INS_BDEP:
  case AARCH64_INS_BEXT:
  case AARCH64_INS_BGRP: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
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
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[1]);
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
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_REVB:
  case AARCH64_INS_REVD:
  case AARCH64_INS_REVH:
  case AARCH64_INS_REVW: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // SVE / NEON misc permute
  case AARCH64_INS_EXTQ:
  case AARCH64_INS_EXTRX:
  case AARCH64_INS_EXTRY:
  case AARCH64_INS_TBLQ:
  case AARCH64_INS_TBXQ:
  case AARCH64_INS_UZP:
  case AARCH64_INS_UZPQ1:
  case AARCH64_INS_UZPQ2:
  case AARCH64_INS_ZIP:
  case AARCH64_INS_ZIPQ1:
  case AARCH64_INS_ZIPQ2: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // SVE float misc
  // FCADD — rotated complex floating-point add (FEAT_FCMA).  Per complex pair:
  //   rot 90:  re = Vn.re - Vm.im;  im = Vn.im + Vm.re
  //   rot 270: re = Vn.re + Vm.im;  im = Vn.im - Vm.re
  // The old code was a whole-register FLOAT_ADD (no rotation, no per-lane).
  case AARCH64_INS_FCADD: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Vn = operandRead(S, ARM64.operands[1]);
    NdVar Vm = operandRead(S, ARM64.operands[2]);
    int64_t Rot = ARM64.operands[3].imm;
    unsigned ES = complexElemSize(ARM64.operands[0].vas);
    if ((ES != 4 && ES != 8) || Dst.Size < 2 * ES) {
      S.emit(NdOp::FLOAT_ADD, Dst, {Vn, Vm});
      break;
    }
    auto lane = [&](NdVar V, unsigned Idx) {
      NdVar T = S.makeTemp(ES);
      S.emit(NdOp::SUBBYTES, T,
             {V, NdVar::cst(static_cast<uint64_t>(Idx) * ES, 4)});
      return T;
    };
    NdVar Acc = NdVar::cst(0, 0);
    bool First = true;
    auto append = [&](NdVar L) {
      if (First) {
        Acc = L;
        First = false;
        return;
      }
      NdVar N = S.makeTemp(Acc.Size + ES);
      S.emit(NdOp::CONCAT, N, {L, Acc});
      Acc = N;
    };
    unsigned NLanes = Dst.Size / ES;
    for (unsigned K = 0; K < NLanes / 2; ++K) {
      NdVar VnRe = lane(Vn, 2 * K), VnIm = lane(Vn, 2 * K + 1);
      NdVar VmRe = lane(Vm, 2 * K), VmIm = lane(Vm, 2 * K + 1);
      NdVar OutRe = S.makeTemp(ES), OutIm = S.makeTemp(ES);
      if (Rot == 270) {
        S.emit(NdOp::FLOAT_ADD, OutRe, {VnRe, VmIm});
        S.emit(NdOp::FLOAT_SUB, OutIm, {VnIm, VmRe});
      } else {
        S.emit(NdOp::FLOAT_SUB, OutRe, {VnRe, VmIm});
        S.emit(NdOp::FLOAT_ADD, OutIm, {VnIm, VmRe});
      }
      append(OutRe);
      append(OutIm);
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case AARCH64_INS_FCPY:
  case AARCH64_INS_FDUP: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_FEXPA:
  case AARCH64_INS_FLOGB: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case AARCH64_INS_FMAD:
  case AARCH64_INS_FNMAD: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar C = operandRead(S, ARM64.operands[3]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_ADD, Dst, {Prod, C});
    break;
  }
  case AARCH64_INS_FMSB:
  case AARCH64_INS_FNMSB: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar C = operandRead(S, ARM64.operands[3]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_SUB, Dst, {C, Prod});
    break;
  }
  case AARCH64_INS_FNMLA:
  case AARCH64_INS_FNMLS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_ADD, Dst, {Dst, Prod});
    break;
  }
  case AARCH64_INS_FTMAD:
  case AARCH64_INS_FTSMUL:
  case AARCH64_INS_FTSSEL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::FLOAT_MULT, Dst, {A, B});
    break;
  }
  case AARCH64_INS_FSCALE:
  case AARCH64_INS_FAMAX:
  case AARCH64_INS_FAMIN: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::FLOAT_MULT, Dst, {A, B});
    break;
  }
  case AARCH64_INS_FCVTLT:
  case AARCH64_INS_FCVTNB:
  case AARCH64_INS_FCVTNT:
  case AARCH64_INS_FCVTX:
  case AARCH64_INS_FCVTXNT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::FLOAT_FLOAT2FLOAT, Dst, {Src});
    break;
  }

  // SME: FMA/FMS/MAC
  case AARCH64_INS_FMA16:
  case AARCH64_INS_FMA32:
  case AARCH64_INS_FMA64:
  case AARCH64_INS_FMS16:
  case AARCH64_INS_FMS32:
  case AARCH64_INS_FMS64:
  case AARCH64_INS_MAC16:
  case AARCH64_INS_MATFP:
  case AARCH64_INS_MATINT: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar A = operandRead(S, ARM64.operands[1]);
      NdVar B = operandRead(S, ARM64.operands[2]);
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {A, B});
      S.emit(NdOp::INT_ADD, Dst, {Dst, Prod});
    }
    break;
  }
  case AARCH64_INS_MUL53HI:
  case AARCH64_INS_MUL53LO: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_MULT, Dst, {A, B});
    break;
  }

  // SME outer products
  case AARCH64_INS_SMOPA:
  case AARCH64_INS_SMOPS:
  case AARCH64_INS_UMOPA:
  case AARCH64_INS_UMOPS:
  case AARCH64_INS_SUMOPA:
  case AARCH64_INS_SUMOPS:
  case AARCH64_INS_USMOPA:
  case AARCH64_INS_USMOPS:
  case AARCH64_INS_FMOPA:
  case AARCH64_INS_FMOPS:
  case AARCH64_INS_BMOPA:
  case AARCH64_INS_BMOPS: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar A = operandRead(S, ARM64.operands[1]);
      NdVar B = operandRead(S, ARM64.operands[2]);
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {A, B});
      S.emit(NdOp::INT_ADD, Dst, {Dst, Prod});
    }
    break;
  }

  // SME tile ops
  case AARCH64_INS_MOVA:
  case AARCH64_INS_MOVAZ:
  case AARCH64_INS_PMOV: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_ZERO:
  case AARCH64_INS_CLR:
  case AARCH64_INS_VECFP:
  case AARCH64_INS_VECINT:
    S.emit(NdOp::NOP, {}, {});
    break;
  case AARCH64_INS_LUTI2:
  case AARCH64_INS_LUTI4:
  case AARCH64_INS_GENLUT: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // ========================================================================
  // Memory Copy / Set (ARMv8.8 FEAT_MOPS) — ~96+32 variants
  // All CPY* are memory copy, all SET* are memory set.
  // ========================================================================
  case AARCH64_INS_CPYE:
  case AARCH64_INS_CPYEN:
  case AARCH64_INS_CPYERN:
  case AARCH64_INS_CPYERT:
  case AARCH64_INS_CPYERTN:
  case AARCH64_INS_CPYERTRN:
  case AARCH64_INS_CPYERTWN:
  case AARCH64_INS_CPYET:
  case AARCH64_INS_CPYETN:
  case AARCH64_INS_CPYETRN:
  case AARCH64_INS_CPYETWN:
  case AARCH64_INS_CPYEWN:
  case AARCH64_INS_CPYEWT:
  case AARCH64_INS_CPYEWTN:
  case AARCH64_INS_CPYEWTRN:
  case AARCH64_INS_CPYEWTWN:
  case AARCH64_INS_CPYFE:
  case AARCH64_INS_CPYFEN:
  case AARCH64_INS_CPYFERN:
  case AARCH64_INS_CPYFERT:
  case AARCH64_INS_CPYFERTN:
  case AARCH64_INS_CPYFERTRN:
  case AARCH64_INS_CPYFERTWN:
  case AARCH64_INS_CPYFET:
  case AARCH64_INS_CPYFETN:
  case AARCH64_INS_CPYFETRN:
  case AARCH64_INS_CPYFETWN:
  case AARCH64_INS_CPYFEWN:
  case AARCH64_INS_CPYFEWT:
  case AARCH64_INS_CPYFEWTN:
  case AARCH64_INS_CPYFEWTRN:
  case AARCH64_INS_CPYFEWTWN:
  case AARCH64_INS_CPYFM:
  case AARCH64_INS_CPYFMN:
  case AARCH64_INS_CPYFMRN:
  case AARCH64_INS_CPYFMRT:
  case AARCH64_INS_CPYFMRTN:
  case AARCH64_INS_CPYFMRTRN:
  case AARCH64_INS_CPYFMRTWN:
  case AARCH64_INS_CPYFMT:
  case AARCH64_INS_CPYFMTN:
  case AARCH64_INS_CPYFMTRN:
  case AARCH64_INS_CPYFMTWN:
  case AARCH64_INS_CPYFMWN:
  case AARCH64_INS_CPYFMWT:
  case AARCH64_INS_CPYFMWTN:
  case AARCH64_INS_CPYFMWTRN:
  case AARCH64_INS_CPYFMWTWN:
  case AARCH64_INS_CPYFP:
  case AARCH64_INS_CPYFPN:
  case AARCH64_INS_CPYFPRN:
  case AARCH64_INS_CPYFPRT:
  case AARCH64_INS_CPYFPRTN:
  case AARCH64_INS_CPYFPRTRN:
  case AARCH64_INS_CPYFPRTWN:
  case AARCH64_INS_CPYFPT:
  case AARCH64_INS_CPYFPTN:
  case AARCH64_INS_CPYFPTRN:
  case AARCH64_INS_CPYFPTWN:
  case AARCH64_INS_CPYFPWN:
  case AARCH64_INS_CPYFPWT:
  case AARCH64_INS_CPYFPWTN:
  case AARCH64_INS_CPYFPWTRN:
  case AARCH64_INS_CPYFPWTWN:
  case AARCH64_INS_CPYM:
  case AARCH64_INS_CPYMN:
  case AARCH64_INS_CPYMRN:
  case AARCH64_INS_CPYMRT:
  case AARCH64_INS_CPYMRTN:
  case AARCH64_INS_CPYMRTRN:
  case AARCH64_INS_CPYMRTWN:
  case AARCH64_INS_CPYMT:
  case AARCH64_INS_CPYMTN:
  case AARCH64_INS_CPYMTRN:
  case AARCH64_INS_CPYMTWN:
  case AARCH64_INS_CPYMWN:
  case AARCH64_INS_CPYMWT:
  case AARCH64_INS_CPYMWTN:
  case AARCH64_INS_CPYMWTRN:
  case AARCH64_INS_CPYMWTWN:
  case AARCH64_INS_CPYP:
  case AARCH64_INS_CPYPN:
  case AARCH64_INS_CPYPRN:
  case AARCH64_INS_CPYPRT:
  case AARCH64_INS_CPYPRTN:
  case AARCH64_INS_CPYPRTRN:
  case AARCH64_INS_CPYPRTWN:
  case AARCH64_INS_CPYPT:
  case AARCH64_INS_CPYPTN:
  case AARCH64_INS_CPYPTRN:
  case AARCH64_INS_CPYPTWN:
  case AARCH64_INS_CPYPWN:
  case AARCH64_INS_CPYPWT:
  case AARCH64_INS_CPYPWTN:
  case AARCH64_INS_CPYPWTRN:
  case AARCH64_INS_CPYPWTWN:
  case AARCH64_INS_SET:
  case AARCH64_INS_SETE:
  case AARCH64_INS_SETEN:
  case AARCH64_INS_SETET:
  case AARCH64_INS_SETETN:
  case AARCH64_INS_SETGE:
  case AARCH64_INS_SETGEN:
  case AARCH64_INS_SETGET:
  case AARCH64_INS_SETGETN:
  case AARCH64_INS_SETGM:
  case AARCH64_INS_SETGMN:
  case AARCH64_INS_SETGMT:
  case AARCH64_INS_SETGMTN:
  case AARCH64_INS_SETGP:
  case AARCH64_INS_SETGPN:
  case AARCH64_INS_SETGPT:
  case AARCH64_INS_SETGPTN:
  case AARCH64_INS_SETM:
  case AARCH64_INS_SETMN:
  case AARCH64_INS_SETMT:
  case AARCH64_INS_SETMTN:
  case AARCH64_INS_SETP:
  case AARCH64_INS_SETPN:
  case AARCH64_INS_SETPT:
  case AARCH64_INS_SETPTN: {
    if (ARM64.op_count >= 3) {
      NdVar DstAddr = operandRead(S, ARM64.operands[0]);
      NdVar SrcVal = operandRead(S, ARM64.operands[1]);
      NdVar Val = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, Val, {SrcVal, NdVar::cst(0, 4)});
      S.emit(NdOp::STORE, {}, {DstAddr, Val});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
