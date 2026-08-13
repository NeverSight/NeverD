//===- AArch64LiftNEONShift.cpp - NEON vector shift -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immediate and register shifts: SHL, saturating shift left
/// (SQSHL/UQSHL/SQSHLU), variable shift (SSHL/USHL), right shift
/// with optional rounding (SSHR/SRSHR/USHR/URSHR), shift-and-
/// insert (SLI/SRI) and shift-right-accumulate (SSRA/SRSRA/USRA/
/// URSRA).
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftNEONShift(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                   const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // NEON vector shift
  case AARCH64_INS_SHL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    NdVar Amt = L.operandRead(S, ARM64.operands[2]);
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
      NdVar LaneAmt = Amt;
      if (Amt.Size > LaneSz) {
        LaneAmt = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, LaneAmt, {Amt, NdVar::cst(0, 4)});
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Ls = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ls, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Lr = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_LEFT, Lr, {Ls, LaneAmt});
        if (I == 0) {
          Acc = Lr;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Lr, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_LEFT, Dst, {Src, Amt});
    }
    break;
  }
  // Saturating shift left (register form: per-lane signed amount; immediate
  // form: splat).  SQSHLU additionally saturates to the unsigned range.  Map to
  // the AArch64 NEON intrinsic for bit-exact saturation; the old code was a
  // plain full-width INT_LEFT (no saturation, no per-lane, no right-shift).
  case AARCH64_INS_SQSHL:
  case AARCH64_INS_UQSHL:
  case AARCH64_INS_SQSHLU: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
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
      ElemSz = Dst.Size; // scalar form
    unsigned NLanes = ElemSz ? Dst.Size / ElemSz : 1;
    NdVar ShiftVec;
    if (ARM64.operands[2].type == AARCH64_OP_IMM) {
      NdVar C =
          NdVar::cst(static_cast<uint64_t>(ARM64.operands[2].imm), ElemSz);
      NdVar Acc = C;
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Next = S.makeTemp(Acc.Size + ElemSz);
        S.emit(NdOp::CONCAT, Next, {C, Acc});
        Acc = Next;
      }
      ShiftVec = Acc;
    } else {
      ShiftVec = L.operandRead(S, ARM64.operands[2]);
    }
    Intrinsic II = (Insn->id == AARCH64_INS_SQSHL)   ? Intrinsic::A64_Sqshl
                   : (Insn->id == AARCH64_INS_UQSHL) ? Intrinsic::A64_Uqshl
                                                     : Intrinsic::A64_Sqshlu;
    S.emitIntrinsic(II, Dst, {Src, ShiftVec, NdVar::cst(ElemSz, 4)});
    break;
  }
  case AARCH64_INS_SSHL:
  case AARCH64_INS_USHL: {
    // Per-lane variable shift.  Each lane is shifted by the *signed* value in
    // the least-significant byte of the corresponding amount lane: positive =>
    // left, negative => right (logical for USHL, arithmetic for SSHL).  The
    // old code emitted a single full-width INT_LEFT, which is wrong both in
    // direction and in lane independence (a clang -O2 nibble extractor uses
    // `ushl v.4s` by {0,-4,-8,-12}).
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    NdVar Amt = L.operandRead(S, ARM64.operands[2]);
    bool IsSigned = (Insn->id == AARCH64_INS_SSHL);
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
    // Scalar d-form (SSHL/USHL Dd,Dn,Dm) decodes with vas == INVALID, so ElemSz
    // stays 0.  Treat the whole 64-bit register as a single lane (matching the
    // SQSHL/UQSHL/SQSHLU and SLI/SRI handlers) so the signed-amount left/right
    // SELECT below runs; without it the bare INT_LEFT fallback turns a NEGATIVE
    // (right-shift) amount into a giant left shift that flushes the lane to 0.
    if (ElemSz == 0)
      ElemSz = Dst.Size;
    if (ElemSz > 0 && Dst.Size >= ElemSz && Amt.Size >= Dst.Size) {
      unsigned NLanes = Dst.Size / ElemSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar SLane = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, SLane, {Src, NdVar::cst(I * ElemSz, 4)});
        NdVar AByte = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, AByte, {Amt, NdVar::cst(I * ElemSz, 4)});
        NdVar ShAmt = S.makeTemp(ElemSz);
        S.emit(NdOp::INT_SEXT, ShAmt, {AByte});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsNeg, {ShAmt, NdVar::cst(0, ElemSz)});
        NdVar NegAmt = S.makeTemp(ElemSz);
        S.emit(NdOp::INT_NEG2, NegAmt, {ShAmt});
        NdVar LeftRes = S.makeTemp(ElemSz);
        S.emit(NdOp::INT_LEFT, LeftRes, {SLane, ShAmt});
        NdVar RightRes = S.makeTemp(ElemSz);
        S.emit(IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT, RightRes,
               {SLane, NegAmt});
        NdVar LaneRes = S.makeTemp(ElemSz);
        S.emit(NdOp::SELECT, LaneRes, {IsNeg, RightRes, LeftRes});
        if (I == 0) {
          Acc = LaneRes;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + ElemSz);
          S.emit(NdOp::CONCAT, Next, {LaneRes, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_LEFT, Dst, {Src, Amt});
    }
    break;
  }
  // Immediate/vector right shifts.  Must be per-lane: a single full-width
  // INT_(S)RIGHT pulls each higher lane's low bits into the lane below
  // (off-by multiples of 2^shift across the lane boundary).
  // Rounding variants (SRSHR/URSHR) add 1<<(n-1) before shifting.
  case AARCH64_INS_SSHR:
  case AARCH64_INS_SRSHR:
  case AARCH64_INS_USHR:
  case AARCH64_INS_URSHR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    NdVar Amt = L.operandRead(S, ARM64.operands[2]);
    bool IsSigned =
        (Insn->id == AARCH64_INS_SSHR || Insn->id == AARCH64_INS_SRSHR);
    bool IsRounding =
        (Insn->id == AARCH64_INS_SRSHR || Insn->id == AARCH64_INS_URSHR);
    NdOp ShOp = IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
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
      NdVar LaneAmt = Amt;
      if (Amt.Size > LaneSz) {
        LaneAmt = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, LaneAmt, {Amt, NdVar::cst(0, 4)});
      }
      NdVar Acc = S.makeTemp(0);
      bool Round = IsRounding && Amt.isConst() && Amt.Offset > 0 &&
                   Amt.Offset <= LaneSz * 8;
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Ls = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ls, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Lr = Round ? S.emitRoundedShr(Ls, LaneSz, Amt.Offset, IsSigned)
                         : S.makeTemp(LaneSz);
        if (!Round)
          S.emit(ShOp, Lr, {Ls, LaneAmt});
        if (I == 0) {
          Acc = Lr;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Lr, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      if (IsRounding && Amt.isConst() && Amt.Offset > 0 &&
          Amt.Offset <= Dst.Size * 8) {
        NdVar R = S.emitRoundedShr(Src, Dst.Size, Amt.Offset, IsSigned);
        S.emit(NdOp::COPY, Dst, {R});
      } else {
        S.emit(ShOp, Dst, {Src, Amt});
      }
    }
    break;
  }
  case AARCH64_INS_SLI:
  case AARCH64_INS_SRI: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSli = (Insn->id == AARCH64_INS_SLI);
    unsigned Sh = static_cast<unsigned>(ARM64.operands[2].imm);
    // Per-lane shift-and-insert (a full-width shift would bleed bits across
    // lane boundaries on vector forms).
    unsigned LaneSz = 0;
    auto Vas = ARM64.operands[0].vas;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    if (LaneSz == 0)
      LaneSz = Dst.Size; // scalar form: single lane
    unsigned NLanes = LaneSz ? Dst.Size / LaneSz : 1;
    unsigned LaneBits = LaneSz * 8;
    // Mask of the bits the inserted (shifted) value occupies.
    uint64_t InsMask;
    if (IsSli)
      InsMask = (Sh >= LaneBits) ? 0 : (~0ULL << Sh);
    else
      InsMask = (Sh >= LaneBits)
                    ? 0
                    : ((LaneBits >= 64) ? (~0ULL >> Sh)
                                        : (((1ULL << LaneBits) - 1) >> Sh));
    NdVar ShCst = NdVar::cst(Sh, LaneSz);
    NdVar InsC = NdVar::cst(InsMask, LaneSz);
    NdVar KeepC = NdVar::cst(~InsMask, LaneSz);
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar SLane = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, SLane, {Src, NdVar::cst(I * LaneSz, 4)});
      NdVar OLane = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, OLane, {OldDst, NdVar::cst(I * LaneSz, 4)});
      NdVar Shifted = S.makeTemp(LaneSz);
      S.emit(IsSli ? NdOp::INT_LEFT : NdOp::INT_RIGHT, Shifted, {SLane, ShCst});
      NdVar Ins = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_AND, Ins, {Shifted, InsC});
      NdVar Kept = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_AND, Kept, {OLane, KeepC});
      NdVar Res = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_OR, Res, {Ins, Kept});
      if (I == 0) {
        Acc = Res;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Res, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // NEON shift-right-and-accumulate.  Must be per-lane: a single full-width
  // INT_(S)RIGHT pulls each higher lane's low bits into the lane below, and a
  // full-width INT_ADD lets the accumulate carry across lane boundaries.
  // Rounding variants (SRSRA/URSRA) add 1<<(n-1) before shifting.
  case AARCH64_INS_SSRA:
  case AARCH64_INS_SRSRA:
  case AARCH64_INS_USRA:
  case AARCH64_INS_URSRA: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    NdVar Amt = L.operandRead(S, ARM64.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSigned =
        (Insn->id == AARCH64_INS_SSRA || Insn->id == AARCH64_INS_SRSRA);
    bool IsRounding =
        (Insn->id == AARCH64_INS_SRSRA || Insn->id == AARCH64_INS_URSRA);
    NdOp ShOp = IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
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
      NdVar LaneAmt = Amt;
      if (Amt.Size > LaneSz) {
        LaneAmt = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, LaneAmt, {Amt, NdVar::cst(0, 4)});
      }
      NdVar Acc = S.makeTemp(0);
      bool Round = IsRounding && Amt.isConst() && Amt.Offset > 0 &&
                   Amt.Offset <= LaneSz * 8;
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Ls = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ls, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Sh = Round ? S.emitRoundedShr(Ls, LaneSz, Amt.Offset, IsSigned)
                         : S.makeTemp(LaneSz);
        if (!Round)
          S.emit(ShOp, Sh, {Ls, LaneAmt});
        NdVar Ld = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ld, {OldDst, NdVar::cst(I * LaneSz, 4)});
        NdVar Lr = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_ADD, Lr, {Ld, Sh});
        if (I == 0) {
          Acc = Lr;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Lr, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Shifted;
      if (IsRounding && Amt.isConst() && Amt.Offset > 0 &&
          Amt.Offset <= Dst.Size * 8) {
        Shifted = S.emitRoundedShr(Src, Dst.Size, Amt.Offset, IsSigned);
      } else {
        Shifted = S.makeTemp(Dst.Size);
        S.emit(ShOp, Shifted, {Src, Amt});
      }
      S.emit(NdOp::INT_ADD, Dst, {OldDst, Shifted});
    }
    break;
  }
  default:
    return false;
  }
  return true;
}

} // namespace neverd
