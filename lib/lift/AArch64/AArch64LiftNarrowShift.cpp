//===- AArch64LiftNarrowShift.cpp - NEON narrowing shifts and pairwise ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shift-right-and-narrow (SHRN/RSHRN), rounding and saturating
/// variable shifts (SQRSHL/UQRSHL/SRSHL/URSHL), narrowing
/// saturating shift-right (SQSHRN/SQRSHRN/UQSHRN/... families),
/// absolute-difference accumulate (SABA/UABA/SABDL/SABAL) and
/// pairwise min/max (SMAXP/SMINP/UMAXP/UMINP).
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftNarrowShift(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // ========================================================================
  // NEON additional variants: RSHRN, SHRN, SABA, UABA, ABD long, etc.
  // ========================================================================
  // SHRN/SHRN2/RSHRN/RSHRN2 — shift right and NARROW: each wide source lane (Q
  // register, element 2*NarrowSz) is shifted right by #imm and truncated to its
  // low half.  SHRN writes the 64-bit narrowed result to the low half of the
  // dest (zeroing the high); the "2" variants write it to the high 64 bits,
  // preserving the low.  The old code emitted a single full-width INT_RIGHT,
  // shifting the whole 128-bit register and keeping only the low 64 bits —
  // correct for lane 0, wrong for the high lanes.  Narrow truncation makes the
  // shift sign-agnostic, so a logical shift is always correct.  (The saturating
  // SQSHRN/UQSHRN family below was already per-lane; plain SHRN/RSHRN was not.)
  case AARCH64_INS_RSHRN:
  case AARCH64_INS_RSHRN2:
  case AARCH64_INS_SHRN:
  case AARCH64_INS_SHRN2: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    unsigned Imm = static_cast<unsigned>(ARM64.operands[2].imm);
    bool IsRound =
        (Insn->id == AARCH64_INS_RSHRN || Insn->id == AARCH64_INS_RSHRN2);
    bool Is2 =
        (Insn->id == AARCH64_INS_SHRN2 || Insn->id == AARCH64_INS_RSHRN2);
    unsigned NarrowSz = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_16B:
    case AARCH64LAYOUT_VL_8B:
      NarrowSz = 1;
      break;
    case AARCH64LAYOUT_VL_8H:
    case AARCH64LAYOUT_VL_4H:
      NarrowSz = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
    case AARCH64LAYOUT_VL_2S:
      NarrowSz = 4;
      break;
    default:
      break;
    }
    if (NarrowSz == 0 || Src.Size != 16) {
      S.emit(NdOp::INT_RIGHT, Dst, {Src, NdVar::cst(Imm, Dst.Size)});
      break;
    }
    unsigned WideSz = NarrowSz * 2;
    unsigned NLanes = 16 / WideSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar SLane = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, SLane,
             {Src, NdVar::cst(static_cast<uint64_t>(I) * WideSz, 4)});
      if (IsRound && Imm > 0) {
        NdVar Rounded = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ADD, Rounded,
               {SLane, NdVar::cst(1ull << (Imm - 1), WideSz)});
        SLane = Rounded;
      }
      NdVar Shifted = S.makeTemp(WideSz);
      S.emit(NdOp::INT_RIGHT, Shifted, {SLane, NdVar::cst(Imm, WideSz)});
      NdVar NLane = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, NLane, {Shifted, NdVar::cst(0, 4)});
      if (I == 0)
        Acc = NLane;
      else {
        NdVar Next = S.makeTemp(Acc.Size + NarrowSz);
        S.emit(NdOp::CONCAT, Next, {NLane, Acc});
        Acc = Next;
      }
    }
    if (Is2) {
      NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
      NdVar Lo = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Lo, {OldDst, NdVar::cst(0, 4)});
      NdVar Full = S.makeTemp(16);
      S.emit(NdOp::CONCAT, Full, {Acc, Lo});
      S.emit(NdOp::COPY, Dst, {Full});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }
  // Saturating / rounding variable shift (register form, per-lane signed
  // amount).  Map to the AArch64 NEON intrinsic; the old code was a plain
  // full-width INT_LEFT (no saturation/rounding, no per-lane, no right shift).
  case AARCH64_INS_SQRSHL:
  case AARCH64_INS_UQRSHL:
  case AARCH64_INS_SRSHL:
  case AARCH64_INS_URSHL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    unsigned ElemSz = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_16B:
    case AARCH64LAYOUT_VL_8B:
      ElemSz = 1;
      break;
    case AARCH64LAYOUT_VL_8H:
    case AARCH64LAYOUT_VL_4H:
      ElemSz = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
    case AARCH64LAYOUT_VL_2S:
      ElemSz = 4;
      break;
    case AARCH64LAYOUT_VL_2D:
    case AARCH64LAYOUT_VL_1D:
      ElemSz = 8;
      break;
    default:
      break;
    }
    if (ElemSz == 0)
      ElemSz = Dst.Size;
    Intrinsic II = (Insn->id == AARCH64_INS_SQRSHL)   ? Intrinsic::A64_Sqrshl
                   : (Insn->id == AARCH64_INS_UQRSHL) ? Intrinsic::A64_Uqrshl
                   : (Insn->id == AARCH64_INS_SRSHL)  ? Intrinsic::A64_Srshl
                                                      : Intrinsic::A64_Urshl;
    S.emitIntrinsic(II, Dst, {A, B, NdVar::cst(ElemSz, 4)});
    break;
  }
  // Narrowing saturating shift-right by immediate.  Map to the AArch64 NEON
  // intrinsic (wide vector + i32 imm -> narrow vector); the "2" variants write
  // the narrowed result into the high 64 bits, preserving the low 64.  The old
  // code was a plain full-width INT_RIGHT (no narrowing/saturation/per-lane).
  case AARCH64_INS_SQRSHRN:
  case AARCH64_INS_SQRSHRN2:
  case AARCH64_INS_SQRSHRUN:
  case AARCH64_INS_SQRSHRUN2:
  case AARCH64_INS_SQSHRN:
  case AARCH64_INS_SQSHRN2:
  case AARCH64_INS_SQSHRUN:
  case AARCH64_INS_SQSHRUN2:
  case AARCH64_INS_UQRSHRN:
  case AARCH64_INS_UQRSHRN2:
  case AARCH64_INS_UQSHRN:
  case AARCH64_INS_UQSHRN2: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    unsigned Imm = static_cast<unsigned>(ARM64.operands[2].imm);
    unsigned NarrowSz = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_16B:
    case AARCH64LAYOUT_VL_8B:
      NarrowSz = 1;
      break;
    case AARCH64LAYOUT_VL_8H:
    case AARCH64LAYOUT_VL_4H:
      NarrowSz = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
    case AARCH64LAYOUT_VL_2S:
      NarrowSz = 4;
      break;
    default:
      break;
    }
    bool Is2 =
        (Insn->id == AARCH64_INS_SQRSHRN2 ||
         Insn->id == AARCH64_INS_SQRSHRUN2 || Insn->id == AARCH64_INS_SQSHRN2 ||
         Insn->id == AARCH64_INS_SQSHRUN2 || Insn->id == AARCH64_INS_UQRSHRN2 ||
         Insn->id == AARCH64_INS_UQSHRN2);
    if (NarrowSz == 0 || Src.Size != 16) {
      S.emit(NdOp::INT_RIGHT, Dst, {Src, NdVar::cst(Imm, Dst.Size)});
      break;
    }
    Intrinsic II;
    switch (Insn->id) {
    case AARCH64_INS_SQSHRN:
    case AARCH64_INS_SQSHRN2:
      II = Intrinsic::A64_Sqshrn;
      break;
    case AARCH64_INS_SQRSHRN:
    case AARCH64_INS_SQRSHRN2:
      II = Intrinsic::A64_Sqrshrn;
      break;
    case AARCH64_INS_UQSHRN:
    case AARCH64_INS_UQSHRN2:
      II = Intrinsic::A64_Uqshrn;
      break;
    case AARCH64_INS_SQSHRUN:
    case AARCH64_INS_SQSHRUN2:
      II = Intrinsic::A64_Sqshrun;
      break;
    case AARCH64_INS_SQRSHRUN:
    case AARCH64_INS_SQRSHRUN2:
      II = Intrinsic::A64_Sqrshrun;
      break;
    default:
      II = Intrinsic::A64_Uqrshrn;
      break;
    }
    NdVar Narrow = S.makeTemp((16u / (NarrowSz * 2)) * NarrowSz);
    S.emitIntrinsic(II, Narrow,
                    {Src, NdVar::cst(Imm, 4), NdVar::cst(NarrowSz, 4)});
    if (Is2) {
      NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
      NdVar Lo = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Lo, {OldDst, NdVar::cst(0, 4)});
      NdVar Full = S.makeTemp(16);
      S.emit(NdOp::CONCAT, Full, {Narrow, Lo});
      S.emit(NdOp::COPY, Dst, {Full});
    } else {
      S.emit(NdOp::COPY, Dst, {Narrow});
    }
    break;
  }
  case AARCH64_INS_SABA:
  case AARCH64_INS_UABA: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar OldD = L.operandRead(S, ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    bool IsSigned = (Insn->id == AARCH64_INS_SABA);
    unsigned LaneSz = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_16B:
    case AARCH64LAYOUT_VL_8B:
      LaneSz = 1;
      break;
    case AARCH64LAYOUT_VL_8H:
    case AARCH64LAYOUT_VL_4H:
      LaneSz = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
    case AARCH64LAYOUT_VL_2S:
      LaneSz = 4;
      break;
    default:
      break;
    }
    // dst[i] += |a[i] - b[i]| per lane.  A full-width INT_SUB would let the
    // borrow cross lanes and drops the absolute value entirely.
    if (LaneSz > 0 && A.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Ld = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ld, {OldD, NdVar::cst(I * LaneSz, 4)});
        NdVar Diff = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_SUB, Diff, {La, Lb});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS, IsNeg, {La, Lb});
        NdVar NegDiff = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, NegDiff, {Diff});
        NdVar AbsDiff = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, AbsDiff, {IsNeg, NegDiff, Diff});
        NdVar LaneRes = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_ADD, LaneRes, {Ld, AbsDiff});
        if (I == 0) {
          Acc = LaneRes;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {LaneRes, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Diff = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SUB, Diff, {A, B});
      NdVar IsNeg = S.makeTemp(1);
      S.emit(IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS, IsNeg, {A, B});
      NdVar NegDiff = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NEG2, NegDiff, {Diff});
      NdVar AbsDiff = S.makeTemp(Dst.Size);
      S.emit(NdOp::SELECT, AbsDiff, {IsNeg, NegDiff, Diff});
      S.emit(NdOp::INT_ADD, Dst, {OldD, AbsDiff});
    }
    break;
  }
  case AARCH64_INS_SABDL:
  case AARCH64_INS_SABDL2:
  case AARCH64_INS_UABDL:
  case AARCH64_INS_UABDL2:
  case AARCH64_INS_SABAL:
  case AARCH64_INS_SABAL2:
  case AARCH64_INS_UABAL:
  case AARCH64_INS_UABAL2: {
    // Widening absolute difference (and accumulate): Dst[i](wide) =
    // |widen(A[i]) - widen(B[i])| (+ old Dst[i] for the AL variants).  A plain
    // full-width INT_SUB drops the per-lane absolute value and lets borrows
    // cross lanes; the "2" variants take the upper 64 bits of the narrow
    // sources.
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    bool IsSigned =
        (Insn->id == AARCH64_INS_SABDL || Insn->id == AARCH64_INS_SABDL2 ||
         Insn->id == AARCH64_INS_SABAL || Insn->id == AARCH64_INS_SABAL2);
    bool IsUpper =
        (Insn->id == AARCH64_INS_SABDL2 || Insn->id == AARCH64_INS_UABDL2 ||
         Insn->id == AARCH64_INS_SABAL2 || Insn->id == AARCH64_INS_UABAL2);
    bool IsAccum =
        (Insn->id == AARCH64_INS_SABAL || Insn->id == AARCH64_INS_SABAL2 ||
         Insn->id == AARCH64_INS_UABAL || Insn->id == AARCH64_INS_UABAL2);
    unsigned DstLane = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_8H:
      DstLane = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
      DstLane = 4;
      break;
    case AARCH64LAYOUT_VL_2D:
      DstLane = 8;
      break;
    default:
      break;
    }
    if (DstLane > 0 && Dst.Size > DstLane) {
      unsigned NLanes = Dst.Size / DstLane;
      unsigned NarrowLane = DstLane / 2;
      unsigned NarrowBase = IsUpper ? 8 : 0;
      NdVar OldD = IsAccum ? L.operandRead(S, ARM64.operands[0]) : NdVar();
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar NarrA = S.makeTemp(NarrowLane);
        S.emit(NdOp::SUBBYTES, NarrA,
               {A, NdVar::cst(NarrowBase + I * NarrowLane, 4)});
        NdVar La = S.makeTemp(DstLane);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, La, {NarrA});
        NdVar NarrB = S.makeTemp(NarrowLane);
        S.emit(NdOp::SUBBYTES, NarrB,
               {B, NdVar::cst(NarrowBase + I * NarrowLane, 4)});
        NdVar Lb = S.makeTemp(DstLane);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Lb, {NarrB});
        NdVar Diff = S.makeTemp(DstLane);
        S.emit(NdOp::INT_SUB, Diff, {La, Lb});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS, IsNeg, {La, Lb});
        NdVar NegDiff = S.makeTemp(DstLane);
        S.emit(NdOp::INT_NEG2, NegDiff, {Diff});
        NdVar AbsDiff = S.makeTemp(DstLane);
        S.emit(NdOp::SELECT, AbsDiff, {IsNeg, NegDiff, Diff});
        NdVar LaneRes = AbsDiff;
        if (IsAccum) {
          NdVar OldLane = S.makeTemp(DstLane);
          S.emit(NdOp::SUBBYTES, OldLane, {OldD, NdVar::cst(I * DstLane, 4)});
          LaneRes = S.makeTemp(DstLane);
          S.emit(NdOp::INT_ADD, LaneRes, {OldLane, AbsDiff});
        }
        if (I == 0) {
          Acc = LaneRes;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLane);
          S.emit(NdOp::CONCAT, Next, {LaneRes, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Diff = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SUB, Diff, {A, B});
      if (IsAccum)
        S.emit(NdOp::INT_ADD, Dst, {Dst, Diff});
      else
        S.emit(NdOp::COPY, Dst, {Diff});
    }
    break;
  }
  case AARCH64_INS_SMAXP:
  case AARCH64_INS_SMINP:
  case AARCH64_INS_UMAXP:
  case AARCH64_INS_UMINP: {
    // Pairwise min/max: concatenate A then B, reduce adjacent pairs.  The lanes
    // from A occupy the low half of the result, B's the high half.  The old
    // `COPY Dst,A` placeholder dropped B and never reduced the pairs.
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    bool IsSigned =
        (Insn->id == AARCH64_INS_SMAXP || Insn->id == AARCH64_INS_SMINP);
    bool IsMax =
        (Insn->id == AARCH64_INS_SMAXP || Insn->id == AARCH64_INS_UMAXP);
    unsigned LaneSz = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_16B:
    case AARCH64LAYOUT_VL_8B:
      LaneSz = 1;
      break;
    case AARCH64LAYOUT_VL_8H:
    case AARCH64LAYOUT_VL_4H:
      LaneSz = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
    case AARCH64LAYOUT_VL_2S:
      LaneSz = 4;
      break;
    case AARCH64LAYOUT_VL_2D:
      LaneSz = 8;
      break;
    default:
      break;
    }
    if (LaneSz > 0 && A.Size > LaneSz) {
      unsigned NPairs = A.Size / (LaneSz * 2);
      NdOp CmpOp = IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS;
      NdVar Acc = S.makeTemp(0);
      for (unsigned H = 0; H < 2; ++H) {
        NdVar Src = (H == 0) ? A : B;
        for (unsigned I = 0; I < NPairs; ++I) {
          NdVar Lo = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(I * 2 * LaneSz, 4)});
          NdVar Hi = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Hi,
                 {Src, NdVar::cst(I * 2 * LaneSz + LaneSz, 4)});
          NdVar Cmp = S.makeTemp(1);
          if (IsMax)
            S.emit(CmpOp, Cmp, {Hi, Lo});
          else
            S.emit(CmpOp, Cmp, {Lo, Hi});
          NdVar Sel = S.makeTemp(LaneSz);
          S.emit(NdOp::SELECT, Sel, {Cmp, Lo, Hi});
          unsigned Idx = H * NPairs + I;
          if (Idx == 0) {
            Acc = Sel;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Sel, Acc});
            Acc = Next;
          }
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::COPY, Dst, {A});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
