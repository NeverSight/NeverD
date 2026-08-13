//===- AArch64LiftNEONReduce.cpp - NEON unary ops and reductions ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-lane absolute value and negate (ABS/SQABS/SQNEG),
/// population count (CNT), bitwise NOT, and the cross-lane
/// reductions ADDV/SADDLV/UADDLV, SMAXV/UMAXV/SMINV/UMINV,
/// FMAXV/FMINV/FMAXNMV/FMINNMV and FADDP.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftNEONReduce(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                    const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // NEON abs / Neg — per-lane absolute value
  case AARCH64_INS_ABS:
  case AARCH64_INS_SQABS: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    // SQABS saturates abs(INT_MIN) to INT_MAX; plain ABS leaves it as INT_MIN.
    bool IsSat = (Insn->id == AARCH64_INS_SQABS);

    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;

    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      unsigned Bits = LaneSz * 8;
      uint64_t MinV = static_cast<uint64_t>(1) << (Bits - 1);
      uint64_t MaxV = MinV - 1;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Neg = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, Neg, {Lane});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsNeg, {Lane, NdVar::cst(0, LaneSz)});
        NdVar Sel = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, Sel, {IsNeg, Neg, Lane});
        if (IsSat) {
          NdVar IsMin = S.makeTemp(1);
          S.emit(NdOp::INT_EQUAL, IsMin, {Lane, NdVar::cst(MinV, LaneSz)});
          NdVar Sat = S.makeTemp(LaneSz);
          S.emit(NdOp::SELECT, Sat, {IsMin, NdVar::cst(MaxV, LaneSz), Sel});
          Sel = Sat;
        }
        if (I == 0) {
          Acc = Sel;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Sel, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Neg = S.makeTemp(Src.Size);
      S.emit(NdOp::INT_NEG2, Neg, {Src});
      NdVar IsNeg = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, IsNeg, {Src, NdVar::cst(0, Src.Size)});
      NdVar Sel = S.makeTemp(Src.Size);
      S.emit(NdOp::SELECT, Sel, {IsNeg, Neg, Src});
      if (IsSat && Src.Size > 0 && Src.Size <= 8) {
        unsigned Bits = Src.Size * 8;
        uint64_t MinV = static_cast<uint64_t>(1) << (Bits - 1);
        NdVar IsMin = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsMin, {Src, NdVar::cst(MinV, Src.Size)});
        S.emit(NdOp::SELECT, Dst, {IsMin, NdVar::cst(MinV - 1, Src.Size), Sel});
      } else {
        S.emit(NdOp::COPY, Dst, {Sel});
      }
    }
    break;
  }
  case AARCH64_INS_SQNEG: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    // Signed saturating negate is PER-LANE and clamps `-INT_MIN` to INT_MAX;
    // the old whole-register INT_NEG2 propagated borrows across lanes and
    // never saturated.
    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      unsigned Bits = LaneSz * 8;
      uint64_t MinV = static_cast<uint64_t>(1) << (Bits - 1);
      uint64_t MaxV = MinV - 1;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Neg = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, Neg, {Lane});
        NdVar IsMin = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsMin, {Lane, NdVar::cst(MinV, LaneSz)});
        NdVar Sel = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, Sel, {IsMin, NdVar::cst(MaxV, LaneSz), Neg});
        if (I == 0) {
          Acc = Sel;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Sel, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      // Scalar form (b/h/s/d): negate and saturate -INT_MIN -> INT_MAX.
      NdVar Neg = S.makeTemp(Src.Size);
      S.emit(NdOp::INT_NEG2, Neg, {Src});
      if (Src.Size > 0 && Src.Size <= 8) {
        unsigned Bits = Src.Size * 8;
        uint64_t MinV = static_cast<uint64_t>(1) << (Bits - 1);
        NdVar IsMin = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsMin, {Src, NdVar::cst(MinV, Src.Size)});
        S.emit(NdOp::SELECT, Dst, {IsMin, NdVar::cst(MinV - 1, Src.Size), Neg});
      } else {
        S.emit(NdOp::COPY, Dst, {Neg});
      }
    }
    break;
  }
  // NEON CNT — per-byte population count.
  case AARCH64_INS_CNT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    unsigned NBytes = Dst.Size;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NBytes; ++I) {
      NdVar Byte = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, Byte, {Src, NdVar::cst(I, 4)});
      NdVar Cnt = S.makeTemp(1);
      S.emit(NdOp::POPCOUNT, Cnt, {Byte});
      if (I == 0) {
        Acc = Cnt;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + 1);
        S.emit(NdOp::CONCAT, Next, {Cnt, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case AARCH64_INS_NOT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::INT_NOT, Dst, {Src});
    break;
  }
  // NEON horizontal reductions — sum all lanes.
  case AARCH64_INS_ADDV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned LaneSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_16B || SrcVas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    if (LaneSz > 0 && Src.Size > LaneSz) {
      unsigned NLanes = Src.Size / LaneSz;
      NdVar Sum = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, Sum, {Src, NdVar::cst(0, 4)});
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar NewSum = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_ADD, NewSum, {Sum, Lane});
        Sum = NewSum;
      }
      if (Dst.Size > LaneSz) {
        S.emit(NdOp::INT_ZEXT, Dst, {Sum});
      } else {
        S.emit(NdOp::COPY, Dst, {Sum});
      }
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_SADDLV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned LaneSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_16B || SrcVas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    if (LaneSz > 0 && Src.Size > LaneSz) {
      unsigned NLanes = Src.Size / LaneSz;
      unsigned DstLaneSz = LaneSz * 2;
      if (DstLaneSz > Dst.Size)
        DstLaneSz = Dst.Size;
      NdVar First = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, First, {Src, NdVar::cst(0, 4)});
      NdVar Sum = S.makeTemp(DstLaneSz);
      S.emit(NdOp::INT_SEXT, Sum, {First});
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Wide = S.makeTemp(DstLaneSz);
        S.emit(NdOp::INT_SEXT, Wide, {Lane});
        NdVar NewSum = S.makeTemp(DstLaneSz);
        S.emit(NdOp::INT_ADD, NewSum, {Sum, Wide});
        Sum = NewSum;
      }
      if (Dst.Size > DstLaneSz) {
        S.emit(NdOp::INT_SEXT, Dst, {Sum});
      } else {
        S.emit(NdOp::COPY, Dst, {Sum});
      }
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  // UADDLV — unsigned add-long-across-lanes (each lane zero-extended then
  // summed).
  case AARCH64_INS_UADDLV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned LaneSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_16B || SrcVas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    if (LaneSz > 0 && Src.Size > LaneSz) {
      unsigned NLanes = Src.Size / LaneSz;
      unsigned DstLaneSz = LaneSz * 2;
      if (DstLaneSz > Dst.Size)
        DstLaneSz = Dst.Size;
      NdVar First = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, First, {Src, NdVar::cst(0, 4)});
      NdVar Sum = S.makeTemp(DstLaneSz);
      S.emit(NdOp::INT_ZEXT, Sum, {First});
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Wide = S.makeTemp(DstLaneSz);
        S.emit(NdOp::INT_ZEXT, Wide, {Lane});
        NdVar NewSum = S.makeTemp(DstLaneSz);
        S.emit(NdOp::INT_ADD, NewSum, {Sum, Wide});
        Sum = NewSum;
      }
      if (Dst.Size > DstLaneSz) {
        S.emit(NdOp::INT_ZEXT, Dst, {Sum});
      } else {
        S.emit(NdOp::COPY, Dst, {Sum});
      }
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  // {S,U}{MIN,MAX}V <V><d>, <Vn>.<T> — reduce min/max across all lanes into a
  // scalar.  Previously these emitted an unimplemented intrinsic that
  // the backend dropped (result became 0).  Lower to an explicit per-lane
  // compare/select reduction chain.
  case AARCH64_INS_SMAXV:
  case AARCH64_INS_UMAXV:
  case AARCH64_INS_SMINV:
  case AARCH64_INS_UMINV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    unsigned LaneSz = 0, NLanes = 0;
    switch (ARM64.operands[1].vas) {
    case AARCH64LAYOUT_VL_16B:
      LaneSz = 1;
      NLanes = 16;
      break;
    case AARCH64LAYOUT_VL_8B:
      LaneSz = 1;
      NLanes = 8;
      break;
    case AARCH64LAYOUT_VL_8H:
      LaneSz = 2;
      NLanes = 8;
      break;
    case AARCH64LAYOUT_VL_4H:
      LaneSz = 2;
      NLanes = 4;
      break;
    case AARCH64LAYOUT_VL_4S:
      LaneSz = 4;
      NLanes = 4;
      break;
    case AARCH64LAYOUT_VL_2S:
      LaneSz = 4;
      NLanes = 2;
      break;
    default:
      break;
    }
    if (LaneSz == 0 || NLanes < 2) {
      S.emit(NdOp::COPY, Dst, {Src});
      break;
    }
    bool IsMin =
        (Insn->id == AARCH64_INS_SMINV || Insn->id == AARCH64_INS_UMINV);
    bool IsSigned =
        (Insn->id == AARCH64_INS_SMINV || Insn->id == AARCH64_INS_SMAXV);
    NdOp LessOp = IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS;
    NdVar Acc = S.makeTemp(LaneSz);
    S.emit(NdOp::SUBBYTES, Acc, {Src, NdVar::cst(0, 4)});
    for (unsigned I = 1; I < NLanes; ++I) {
      NdVar L = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, L, {Src, NdVar::cst(I * LaneSz, 4)});
      NdVar Cmp = S.makeTemp(1);
      if (IsMin)
        S.emit(LessOp, Cmp, {L, Acc}); // pick L when L < Acc
      else
        S.emit(LessOp, Cmp, {Acc, L}); // pick L when Acc < L
      NdVar NewAcc = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, NewAcc, {Cmp, L, Acc});
      Acc = NewAcc;
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // FMAXV/FMINV (FMIN/FMAX semantics) / FMAXNMV/FMINNMV (minNum/maxNum) —
  // horizontal FP max/min reduction across lanes to a scalar.  The V/NMV split
  // is NaN handling: V propagates NaN (llvm.minimum/maximum), NMV suppresses it
  // (llvm.minnum/maxnum); a plain FLOAT_LESS+SELECT got both wrong.
  case AARCH64_INS_FMAXV:
  case AARCH64_INS_FMINV:
  case AARCH64_INS_FMAXNMV:
  case AARCH64_INS_FMINNMV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    bool IsMin =
        (Insn->id == AARCH64_INS_FMINV || Insn->id == AARCH64_INS_FMINNMV);
    bool IsNM =
        (Insn->id == AARCH64_INS_FMINNMV || Insn->id == AARCH64_INS_FMAXNMV);
    NdOp MM = IsNM ? (IsMin ? NdOp::FLOAT_MINNUM : NdOp::FLOAT_MAXNUM)
                   : (IsMin ? NdOp::FLOAT_MIN : NdOp::FLOAT_MAX);
    unsigned ElemSz = neonElemSize(ARM64.operands[1].vas);
    if (ElemSz < 4 || Src.Size <= ElemSz) {
      // Unknown / half-precision / scalar: just take the low element.
      NdVar Lo = S.makeTemp(ElemSz ? ElemSz : Dst.Size);
      S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
      S.emit(NdOp::COPY, Dst, {Lo});
      break;
    }
    unsigned NLanes = Src.Size / ElemSz;
    NdVar Acc = S.makeTemp(ElemSz);
    S.emit(NdOp::SUBBYTES, Acc, {Src, NdVar::cst(0, 4)});
    for (unsigned I = 1; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(ElemSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * ElemSz, 4)});
      NdVar New = S.makeTemp(ElemSz);
      S.emit(MM, New, {Acc, Lane});
      Acc = New;
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case AARCH64_INS_FADDP: {
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    if (ARM64.op_count == 2) {
      // Scalar pairwise: faddp Sd, Vn.2S  →  Sd = Vn[0] + Vn[1]
      NdVar Src = L.operandRead(S, ARM64.operands[1]);
      auto SrcVas = ARM64.operands[1].vas;
      unsigned LaneSz = 0;
      if (SrcVas == AARCH64LAYOUT_VL_2S)
        LaneSz = 4;
      else if (SrcVas == AARCH64LAYOUT_VL_2D)
        LaneSz = 8;
      if (LaneSz > 0 && Src.Size >= 2 * LaneSz) {
        NdVar Lo = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        NdVar Hi = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Hi, {Src, NdVar::cst(LaneSz, 4)});
        S.emit(NdOp::FLOAT_ADD, Dst, {Lo, Hi});
      } else {
        S.emit(NdOp::COPY, Dst, {Src});
      }
      break;
    }
    if (ARM64.op_count < 3)
      break;
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);

    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;

    if (LaneSz > 0 && A.Size > LaneSz) {
      unsigned NPairs = A.Size / (LaneSz * 2);
      NdVar Acc = S.makeTemp(0);
      for (unsigned H = 0; H < 2; ++H) {
        NdVar Src = (H == 0) ? A : B;
        for (unsigned I = 0; I < NPairs; ++I) {
          NdVar Lo = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(I * 2 * LaneSz, 4)});
          NdVar Hi = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Hi,
                 {Src, NdVar::cst(I * 2 * LaneSz + LaneSz, 4)});
          NdVar Sum = S.makeTemp(LaneSz);
          S.emit(NdOp::FLOAT_ADD, Sum, {Lo, Hi});
          unsigned Idx = H * NPairs + I;
          if (Idx == 0) {
            Acc = Sum;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Sum, Acc});
            Acc = Next;
          }
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::FLOAT_ADD, Dst, {A, B});
    }
    break;
  }
  default:
    return false;
  }
  return true;
}

} // namespace neverd
