//===- AArch64LiftNEONArith.cpp - NEON integer arithmetic -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pairwise add (ADDP), min/max (SMAX/SMIN/UMAX/UMIN), absolute
/// difference (SABD/UABD), halving add/subtract (SHADD/SRHADD/
/// UHADD/URHADD/SHSUB/UHSUB) and saturating add/subtract
/// (SQADD/UQADD/SQSUB/UQSUB).
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftNEONArith(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                   const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // NEON vector arithmetic — preserve per-lane semantics via intrinsics.
  case AARCH64_INS_ADDP: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);

    // Scalar form: ADDP <V><d>, <Vn>.<T> (2 operands) — sum all lanes of the
    // single source vector pairwise into a scalar.  The only architectural
    // form is ADDP D<d>, V<n>.2D (two 64-bit lanes), but derive the lane size
    // from the source layout to be safe.
    if (ARM64.op_count == 2) {
      unsigned SrcLaneSz = 8;
      switch (ARM64.operands[1].vas) {
      case AARCH64LAYOUT_VL_2D:
        SrcLaneSz = 8;
        break;
      case AARCH64LAYOUT_VL_4S:
      case AARCH64LAYOUT_VL_2S:
        SrcLaneSz = 4;
        break;
      case AARCH64LAYOUT_VL_8H:
      case AARCH64LAYOUT_VL_4H:
        SrcLaneSz = 2;
        break;
      default:
        SrcLaneSz = 8;
        break;
      }
      NdVar Lo = S.makeTemp(SrcLaneSz);
      NdVar Hi = S.makeTemp(SrcLaneSz);
      S.emit(NdOp::SUBBYTES, Lo, {A, NdVar::cst(0, 4)});
      S.emit(NdOp::SUBBYTES, Hi, {A, NdVar::cst(SrcLaneSz, 4)});
      S.emit(NdOp::INT_ADD, Dst, {Lo, Hi});
      break;
    }

    NdVar B = L.operandRead(S, ARM64.operands[2]);

    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;

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
          S.emit(NdOp::INT_ADD, Sum, {Lo, Hi});
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
      S.emitIntrinsic(Intrinsic::A64Addp, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_SMAX:
  case AARCH64_INS_SMIN:
  case AARCH64_INS_UMAX:
  case AARCH64_INS_UMIN: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);

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

    bool IsSigned =
        (Insn->id == AARCH64_INS_SMAX || Insn->id == AARCH64_INS_SMIN);
    bool IsMax = (Insn->id == AARCH64_INS_SMAX || Insn->id == AARCH64_INS_UMAX);

    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Cmp = S.makeTemp(1);
        NdOp CmpOp = IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS;
        if (IsMax)
          S.emit(CmpOp, Cmp, {Lb, La});
        else
          S.emit(CmpOp, Cmp, {La, Lb});
        NdVar Sel = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, Sel, {Cmp, La, Lb});
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
      NdOp CmpOp = IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS;
      NdVar Cmp = S.makeTemp(1);
      if (IsMax)
        S.emit(CmpOp, Cmp, {B, A});
      else
        S.emit(CmpOp, Cmp, {A, B});
      S.emit(NdOp::SELECT, Dst, {Cmp, A, B});
    }
    break;
  }
  case AARCH64_INS_SABD:
  case AARCH64_INS_UABD: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    bool IsSigned = (Insn->id == AARCH64_INS_SABD);

    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;

    if (LaneSz > 0 && A.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Diff = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_SUB, Diff, {La, Lb});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS, IsNeg, {La, Lb});
        NdVar NegDiff = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, NegDiff, {Diff});
        NdVar AbsDiff = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, AbsDiff, {IsNeg, NegDiff, Diff});
        if (I == 0) {
          Acc = AbsDiff;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {AbsDiff, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emitIntrinsic(IsSigned ? Intrinsic::A64Sabd : Intrinsic::A64Uabd, Dst,
                      {A, B});
    }
    break;
  }
  // Halving add (optionally rounding): dst[i] = (a[i]+b[i](+1)) >> 1, per lane.
  // Previously these used unimplemented intrinsics (result became 0).
  case AARCH64_INS_SHADD:
  case AARCH64_INS_SRHADD:
  case AARCH64_INS_UHADD:
  case AARCH64_INS_URHADD: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    bool IsSigned =
        (Insn->id == AARCH64_INS_SHADD || Insn->id == AARCH64_INS_SRHADD);
    bool IsRound =
        (Insn->id == AARCH64_INS_SRHADD || Insn->id == AARCH64_INS_URHADD);
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
    if (LaneSz > 0 && Dst.Size >= LaneSz) {
      unsigned WideSz = LaneSz * 2;
      unsigned NLanes = Dst.Size / LaneSz;
      NdOp ExtOp = IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      NdOp ShOp = IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Al = S.makeTemp(LaneSz), Bl = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Al, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Bl, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Aw = S.makeTemp(WideSz), Bw = S.makeTemp(WideSz);
        S.emit(ExtOp, Aw, {Al});
        S.emit(ExtOp, Bw, {Bl});
        NdVar Sum = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ADD, Sum, {Aw, Bw});
        if (IsRound) {
          NdVar Sum1 = S.makeTemp(WideSz);
          S.emit(NdOp::INT_ADD, Sum1, {Sum, NdVar::cst(1, WideSz)});
          Sum = Sum1;
        }
        NdVar Sh = S.makeTemp(WideSz);
        S.emit(ShOp, Sh, {Sum, NdVar::cst(1, WideSz)});
        NdVar Res = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Res, {Sh, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = Res;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Res, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_ADD, Dst, {A, B});
    }
    break;
  }
  // Halving subtract (no rounding form): dst[i] = (a[i]-b[i]) >> 1, per lane.
  // Was grouped with SVE2 widening subtracts as a full-width INT_SUB
  // placeholder (no halving, cross-lane borrow).  SHSUBR/UHSUBR (SVE2 reversed)
  // stay there.
  case AARCH64_INS_SHSUB:
  case AARCH64_INS_UHSUB: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    bool IsSigned = (Insn->id == AARCH64_INS_SHSUB);
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
    if (LaneSz > 0 && Dst.Size >= LaneSz) {
      unsigned WideSz = LaneSz * 2;
      unsigned NLanes = Dst.Size / LaneSz;
      NdOp ExtOp = IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      NdOp ShOp = IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Al = S.makeTemp(LaneSz), Bl = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Al, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Bl, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Aw = S.makeTemp(WideSz), Bw = S.makeTemp(WideSz);
        S.emit(ExtOp, Aw, {Al});
        S.emit(ExtOp, Bw, {Bl});
        NdVar Diff = S.makeTemp(WideSz);
        S.emit(NdOp::INT_SUB, Diff, {Aw, Bw});
        NdVar Sh = S.makeTemp(WideSz);
        S.emit(ShOp, Sh, {Diff, NdVar::cst(1, WideSz)});
        NdVar Res = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Res, {Sh, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = Res;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Res, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_SUB, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_SQADD:
  case AARCH64_INS_UQADD:
  case AARCH64_INS_SQSUB:
  case AARCH64_INS_UQSUB: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);

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

    bool IsSigned =
        (Insn->id == AARCH64_INS_SQADD || Insn->id == AARCH64_INS_SQSUB);
    bool IsSub =
        (Insn->id == AARCH64_INS_SQSUB || Insn->id == AARCH64_INS_UQSUB);
    Intrinsic SatII = IsSigned
                          ? (IsSub ? Intrinsic::A64Sqsub : Intrinsic::A64Sqadd)
                          : (IsSub ? Intrinsic::A64Uqsub : Intrinsic::A64Uqadd);

    if (LaneSz > 0 && LaneSz <= 4 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      unsigned WideSz = LaneSz * 2;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Wa = S.makeTemp(WideSz);
        NdVar Wb = S.makeTemp(WideSz);
        if (IsSigned) {
          S.emit(NdOp::INT_SEXT, Wa, {La});
          S.emit(NdOp::INT_SEXT, Wb, {Lb});
        } else {
          S.emit(NdOp::INT_ZEXT, Wa, {La});
          S.emit(NdOp::INT_ZEXT, Wb, {Lb});
        }
        NdVar Wide = S.makeTemp(WideSz);
        if (IsSub)
          S.emit(NdOp::INT_SUB, Wide, {Wa, Wb});
        else
          S.emit(NdOp::INT_ADD, Wide, {Wa, Wb});

        NdVar Result = S.makeTemp(LaneSz);
        if (IsSigned) {
          uint64_t MaxVal = (1ULL << (LaneSz * 8 - 1)) - 1;
          uint64_t MinVal = ~MaxVal;
          NdVar WMax = NdVar::cst(MaxVal, WideSz);
          NdVar WMin = NdVar::cst(MinVal, WideSz);
          NdVar IsOver = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, IsOver, {WMax, Wide});
          NdVar Clamped1 = S.makeTemp(WideSz);
          S.emit(NdOp::SELECT, Clamped1, {IsOver, WMax, Wide});
          NdVar IsUnder = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, IsUnder, {Clamped1, WMin});
          NdVar Clamped2 = S.makeTemp(WideSz);
          S.emit(NdOp::SELECT, Clamped2, {IsUnder, WMin, Clamped1});
          S.emit(NdOp::SUBBYTES, Result, {Clamped2, NdVar::cst(0, 4)});
        } else {
          if (IsSub) {
            NdVar IsNeg = S.makeTemp(1);
            NdVar Zero = NdVar::cst(0, WideSz);
            S.emit(NdOp::INT_SLESS, IsNeg, {Wide, Zero});
            NdVar Clamped = S.makeTemp(WideSz);
            S.emit(NdOp::SELECT, Clamped, {IsNeg, Zero, Wide});
            S.emit(NdOp::SUBBYTES, Result, {Clamped, NdVar::cst(0, 4)});
          } else {
            uint64_t UMax = (LaneSz < 8) ? ((1ULL << (LaneSz * 8)) - 1) : ~0ULL;
            NdVar WUMax = NdVar::cst(UMax, WideSz);
            NdVar IsOver = S.makeTemp(1);
            S.emit(NdOp::INT_LESS, IsOver, {WUMax, Wide});
            NdVar Clamped = S.makeTemp(WideSz);
            S.emit(NdOp::SELECT, Clamped, {IsOver, WUMax, Wide});
            S.emit(NdOp::SUBBYTES, Result, {Clamped, NdVar::cst(0, 4)});
          }
        }

        if (I == 0) {
          Acc = Result;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Result, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else if (LaneSz == 8 && Dst.Size > LaneSz) {
      // 64-bit lanes (.2d): per-lane saturating intrinsic (a manual i128 clamp
      // can't represent the signed 64-bit bounds).
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar R = S.makeTemp(LaneSz);
        S.emitIntrinsic(SatII, R, {La, Lb});
        if (I == 0) {
          Acc = R;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      // Scalar form (b/h/s/d): single saturating intrinsic.
      S.emitIntrinsic(SatII, Dst, {A, B});
    }
    break;
  }
  default:
    return false;
  }
  return true;
}

} // namespace neverd
