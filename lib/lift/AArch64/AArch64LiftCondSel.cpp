//===- AArch64LiftCondSel.cpp - Conditional select and negate -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The conditional select family CSEL/CSINC/CSINV/CSNEG (which
/// Capstone also resolves CINC/CINV/CNEG/CSET/CSETM to) and NEG.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftCondSel(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                 const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // --- Conditional select family: CSEL/CSINC/CSINV/CSNEG ---
  // Capstone 6 resolves pseudo-mnemonics (CINC/CINV/CNEG/CSET/CSETM)
  // to their Base forms (CSINC/CSINV/CSNEG) with explicit operands.
  case AARCH64_INS_CSEL:
  case AARCH64_INS_CSINC:
  case AARCH64_INS_CSINV:
  case AARCH64_INS_CSNEG: {
    // Alias forms carry 1 (CSET/CSETM) or 2 (CINC/CINV/CNEG) operands;
    // everything else falls through to the canonical form below, which reads
    // operands[1] and operands[2].
    if (ARM64.op_count < 1 || (ARM64.op_count < 3 && !Insn->is_alias))
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    uint16_t Sz = Dst.Size;

    // Build condition from ARM64.cc
    NdVar Cond = S.makeTemp(1);
    switch (ARM64.cc) {
    case AArch64CC_EQ:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::ZFLAG, 1)});
      break;
    case AArch64CC_NE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::ZFLAG, 1)});
      break;
    case AArch64CC_LT:
      S.emit(NdOp::INT_NOTEQUAL, Cond,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_GE:
      S.emit(NdOp::INT_EQUAL, Cond,
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
    case AArch64CC_HS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::CFLAG, 1)});
      break;
    case AArch64CC_LO:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::CFLAG, 1)});
      break;
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
    default:
      S.emit(NdOp::COPY, Cond, {NdVar::cst(1, 1)});
      break;
    }

    // Compute TrueVal and FalseVal based on instruction variant
    // Capstone 6 alias forms: CSET (1 op), CINC/CINV/CNEG (2 ops),
    // canonical CSEL/CSINC/CSINV/CSNEG (3 ops).
    NdVar TrueVal, FalseVal;

    if (Insn->is_alias && ARM64.op_count == 1) {
      // CSET/CSETM: Rd = Cond ? 1 : 0 (CSINC) or ~0 (CSINV)
      if (Insn->id == AARCH64_INS_CSINC) {
        TrueVal = NdVar::cst(1, Sz);
        FalseVal = NdVar::cst(0, Sz);
      } else {
        // CSETM all-ones mask; avoid shift-by-bitwidth UB when Sz==8.
        uint64_t AllOnes = (Sz >= 8) ? ~0ULL : ((1ULL << (Sz * 8)) - 1);
        TrueVal = NdVar::cst(AllOnes, Sz);
        FalseVal = NdVar::cst(0, Sz);
      }
    } else if (Insn->is_alias && ARM64.op_count == 2) {
      // CINC/CINV/CNEG: Rd = Cond ? op(Rn) : Rn
      NdVar Src = L.operandRead(S, ARM64.operands[1]);
      if (Insn->id == AARCH64_INS_CSINC) {
        TrueVal = S.makeTemp(Sz);
        S.emit(NdOp::INT_ADD, TrueVal, {Src, NdVar::cst(1, Sz)});
        FalseVal = Src;
      } else if (Insn->id == AARCH64_INS_CSINV) {
        TrueVal = S.makeTemp(Sz);
        S.emit(NdOp::INT_NOT, TrueVal, {Src});
        FalseVal = Src;
      } else {
        TrueVal = S.makeTemp(Sz);
        S.emit(NdOp::INT_NEG2, TrueVal, {Src});
        FalseVal = Src;
      }
    } else {
      // Canonical 3-operand form
      switch (Insn->id) {
      case AARCH64_INS_CSEL:
        TrueVal = L.operandRead(S, ARM64.operands[1]);
        FalseVal = L.operandRead(S, ARM64.operands[2]);
        break;
      case AARCH64_INS_CSINC: {
        TrueVal = L.operandRead(S, ARM64.operands[1]);
        NdVar Src2 = L.operandRead(S, ARM64.operands[2]);
        FalseVal = S.makeTemp(Sz);
        S.emit(NdOp::INT_ADD, FalseVal, {Src2, NdVar::cst(1, Sz)});
        break;
      }
      case AARCH64_INS_CSINV: {
        TrueVal = L.operandRead(S, ARM64.operands[1]);
        NdVar Src2 = L.operandRead(S, ARM64.operands[2]);
        FalseVal = S.makeTemp(Sz);
        S.emit(NdOp::INT_NOT, FalseVal, {Src2});
        break;
      }
      case AARCH64_INS_CSNEG: {
        TrueVal = L.operandRead(S, ARM64.operands[1]);
        NdVar Src2 = L.operandRead(S, ARM64.operands[2]);
        FalseVal = S.makeTemp(Sz);
        S.emit(NdOp::INT_NEG2, FalseVal, {Src2});
        break;
      }
      default:
        break;
      }
    }

    S.emit(NdOp::SELECT, Dst, {Cond, TrueVal, FalseVal});
    break;
  }

  // --- NEG (SUB from Zero; NEGS is now SUBS XZR handled above) ---
  case AARCH64_INS_NEG: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    // NEON vector form `neg v.4s` negates each lane independently.  A single
    // full-width INT_NEG2 would propagate borrows across lane boundaries and
    // corrupt the upper lanes (the scalar form is just one lane and is fine).
    unsigned LaneSz = 0;
    auto Vas = ARM64.operands[0].vas;
    if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    else if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Ls = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ls, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Ln = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, Ln, {Ls});
        if (I == 0) {
          Acc = Ln;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Ln, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_NEG2, Dst, {Src});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
