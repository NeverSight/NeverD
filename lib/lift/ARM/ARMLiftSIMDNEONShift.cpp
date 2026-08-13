//===- ARMLiftSIMDNEONShift.cpp - ARM32 NEON shift lifter ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-lane NEON shifts: the widening VSHLL, the saturating and
/// rounding variable shifts, VSHL, the insert forms VSLI/VSRI, the
/// right shifts VSHR/VRSHR/VSRA/VRSRA and the narrowing forms.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftSIMDNEONShift(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                       const cs_arm &ARM) {
  switch (Insn->id) {
  // VSHLL (vector shift left long): widen each narrow D-source lane to a
  // double-width Q-dest lane (sign/zero-extend per `.s`/`.u`), then shift left
  // by the immediate.  Sharing the plain-VSHL path wrongly treated it as a
  // same-width per-lane shift: it read past the 8-byte source and never
  // widened, corrupting every i64-from-i32 construction.
  case ARM_INS_VSHLL: {
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned SrcLaneSz = LI.LaneSz;
    uint64_t Imm = (ARM.op_count >= 3 && ARM.operands[2].type == ARM_OP_IMM)
                       ? static_cast<uint64_t>(ARM.operands[2].imm)
                       : 0;
    if (SrcLaneSz > 0 && SrcLaneSz < 8 && Src.Size >= SrcLaneSz) {
      unsigned DstLaneSz = SrcLaneSz * 2;
      unsigned NLanes = Src.Size / SrcLaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar SLane = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, SLane, {Src, NdVar::cst(I * SrcLaneSz, 4)});
        NdVar WLane = S.makeTemp(DstLaneSz);
        S.emit(LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WLane, {SLane});
        NdVar Shifted = S.makeTemp(DstLaneSz);
        S.emit(NdOp::INT_LEFT, Shifted, {WLane, NdVar::cst(Imm, DstLaneSz)});
        if (I == 0) {
          Acc = Shifted;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {Shifted, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_LEFT, Dst, {Src, NdVar::cst(Imm, Dst.Size)});
    }
    break;
  }

  // NEON saturating / rounding variable shift left (VQSHL/VQSHLU register or
  // immediate, VQRSHL register, VRSHL register).  Map to the ARM NEON intrinsic
  // (per-lane signed shift amount, negative = right; immediate forms splat
  // +imm).  Was wrongly folded into the plain VSHL handler — VQ* lost
  // saturation and VRSHL lost the rounding bias.  The intrinsic is the hardware
  // instruction, so rounding is done in wide precision (no lane overflow).
  case ARM_INS_VQSHL:
  case ARM_INS_VQSHLU:
  case ARM_INS_VRSHL:
  case ARM_INS_VQRSHL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned ElemSz = LI.LaneSz ? LI.LaneSz : Dst.Size;
    unsigned NLanes = ElemSz ? Dst.Size / ElemSz : 1;
    NdVar ShiftVec;
    if (ARM.operands[2].type == ARM_OP_IMM) {
      NdVar C =
          NdVar::cst(static_cast<uint64_t>(ARM.operands[2].imm), ElemSz);
      NdVar Acc = C;
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Next = S.makeTemp(Acc.Size + ElemSz);
        S.emit(NdOp::CONCAT, Next, {C, Acc});
        Acc = Next;
      }
      ShiftVec = Acc;
    } else {
      ShiftVec = L.operandRead(S, ARM.operands[2]);
    }
    Intrinsic II;
    if (Insn->id == ARM_INS_VQSHLU)
      II = Intrinsic::ArmVqshiftsu;
    else if (Insn->id == ARM_INS_VQRSHL)
      II = LI.IsSigned ? Intrinsic::ArmVqrshifts : Intrinsic::ArmVqrshiftu;
    else if (Insn->id == ARM_INS_VRSHL)
      II = LI.IsSigned ? Intrinsic::ArmVrshifts : Intrinsic::ArmVrshiftu;
    else
      II = LI.IsSigned ? Intrinsic::ArmVqshifts : Intrinsic::ArmVqshiftu;
    S.emitIntrinsic(II, Dst, {Src, ShiftVec, NdVar::cst(ElemSz, 4)});
    break;
  }
  // NEON shift (non-rounding; VRSHL is handled above via its rounding
  // intrinsic)
  case ARM_INS_VSHL:
  case ARM_INS_VSHLC:
  case ARM_INS_VSHLLB:
  case ARM_INS_VSHLLT: {
    if (ARM.op_count < 3) {
      if (ARM.op_count >= 2) {
        NdVar Dst = L.operandWrite(ARM.operands[0]);
        NdVar Src = L.operandRead(S, ARM.operands[1]);
        S.emit(NdOp::COPY, Dst, {Src});
      }
      break;
    }
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    if (LI.LaneSz > 0 && Dst.Size > LI.LaneSz) {
      unsigned NLanes = Dst.Size / LI.LaneSz;
      // `vshl.iN dD, dM, #imm` shifts every lane by the SAME scalar immediate;
      // operandRead widens the immediate past the lane width, so testing only
      // `B.Size <= LaneSz` wrongly SUBBYTES'd it per-lane (lane 0 shifted, the
      // rest by 0).  Treat an immediate shift amount as a scalar broadcast.
      // (Same fix as VSHR — was missing here, breaking the s8-saturating-add
      //  idiom `vshl.i16 #8; vqadd.s16; vshr.s16 #8`.)
      bool BImm = (ARM.operands[2].type == ARM_OP_IMM);
      bool BScalar = BImm || (B.Size <= LI.LaneSz);
      // VSHL/VRSHL/VQSHL/VQRSHL by a *register* use a per-lane SIGNED shift
      // amount (low byte of each Vm lane): >=0 left-shifts, <0 right-shifts by
      // the magnitude (arithmetic for `.s`, logical for `.u`).  The previous
      // unconditional INT_LEFT turned a negative amount into a huge unsigned
      // left shift (poison/0) — clang emits `vneg; vshl` to express `a >> n`.
      bool IsVarReg = !BScalar && (Insn->id == ARM_INS_VSHL);
      NdOp RightOp = LI.IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
      NdVar BSc = B;
      if (BImm)
        BSc =
            NdVar::cst(static_cast<uint64_t>(ARM.operands[2].imm), LI.LaneSz);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar Lb = BScalar ? BSc : S.makeTemp(LI.LaneSz);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar R = S.makeTemp(LI.LaneSz);
        if (IsVarReg) {
          // Sign of the shift amount comes from the low byte of the lane.
          NdVar ShByte = S.makeTemp(1);
          S.emit(NdOp::SUBBYTES, ShByte, {Lb, NdVar::cst(0, 4)});
          NdVar ShAmt = S.makeTemp(LI.LaneSz);
          S.emit(NdOp::INT_SEXT, ShAmt, {ShByte});
          NdVar IsNeg = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, IsNeg, {ShAmt, NdVar::cst(0, LI.LaneSz)});
          NdVar NegAmt = S.makeTemp(LI.LaneSz);
          S.emit(NdOp::INT_NEG2, NegAmt, {ShAmt});
          NdVar RightR = S.makeTemp(LI.LaneSz);
          S.emit(RightOp, RightR, {La, NegAmt});
          NdVar LeftR = S.makeTemp(LI.LaneSz);
          S.emit(NdOp::INT_LEFT, LeftR, {La, ShAmt});
          S.emit(NdOp::SELECT, R, {IsNeg, RightR, LeftR});
        } else {
          S.emit(NdOp::INT_LEFT, R, {La, Lb});
        }
        if (I == 0)
          Acc = R;
        else {
          NdVar Next = S.makeTemp(Acc.Size + LI.LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_LEFT, Dst, {A, B});
    }
    break;
  }
  // VSLI/VSRI — per-lane shift-left/right and insert, keeping the bits the
  // shift vacates from the old destination.  A full-width shift would bleed
  // bits across lanes; previously these were folded into the plain shift
  // handlers.
  case ARM_INS_VSLI:
  case ARM_INS_VSRI: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSli = (Insn->id == ARM_INS_VSLI);
    unsigned Sh = static_cast<unsigned>(ARM.operands[2].imm);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned LaneSz = LI.LaneSz ? LI.LaneSz : Dst.Size;
    unsigned NLanes = LaneSz ? Dst.Size / LaneSz : 1;
    unsigned LaneBits = LaneSz * 8;
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
  // VSHR / VRSHR — same-width per-lane right shift by a scalar immediate (or a
  // per-lane register count).  Source and destination element widths are equal.
  case ARM_INS_VSHR:
  case ARM_INS_VRSHR: {
    if (ARM.op_count < 3) {
      if (ARM.op_count >= 2) {
        NdVar Dst = L.operandWrite(ARM.operands[0]);
        NdVar Src = L.operandRead(S, ARM.operands[1]);
        S.emit(NdOp::COPY, Dst, {Src});
      }
      break;
    }
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp ShOp = LI.IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
    // VRSHR adds a 1<<(n-1) rounding bias before the shift (in wider
    // precision).
    bool IsRound = (Insn->id == ARM_INS_VRSHR);
    unsigned ImmAmt = (ARM.operands[2].type == ARM_OP_IMM)
                          ? (unsigned)ARM.operands[2].imm
                          : 0;
    if (LI.LaneSz > 0 && Dst.Size > LI.LaneSz) {
      unsigned NLanes = Dst.Size / LI.LaneSz;
      // `vshr.N dD, dM, #imm` shifts every lane by the SAME scalar immediate.
      // operandRead widens the immediate past the lane width, so the old
      // `B.Size <= LaneSz` test wrongly treated it as a per-lane vector and
      // SUBBYTES'd it — giving 0 for every lane but lane 0.
      bool BImm = (ARM.operands[2].type == ARM_OP_IMM);
      bool BScalar = BImm || (B.Size <= LI.LaneSz);
      bool Round = IsRound && BImm && ImmAmt > 0 && ImmAmt <= LI.LaneSz * 8;
      NdVar BSc = B;
      if (BImm)
        BSc =
            NdVar::cst(static_cast<uint64_t>(ARM.operands[2].imm), LI.LaneSz);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar Lb = BScalar ? BSc : S.makeTemp(LI.LaneSz);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar R = Round ? S.emitRoundedShr(La, LI.LaneSz, ImmAmt, LI.IsSigned)
                          : S.makeTemp(LI.LaneSz);
        if (!Round)
          S.emit(ShOp, R, {La, Lb});
        if (I == 0)
          Acc = R;
        else {
          NdVar Next = S.makeTemp(Acc.Size + LI.LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else if (IsRound && ImmAmt > 0 && LI.LaneSz > 0 &&
               ImmAmt <= Dst.Size * 8) {
      NdVar R = S.emitRoundedShr(A, Dst.Size, ImmAmt, LI.IsSigned);
      S.emit(NdOp::COPY, Dst, {R});
    } else {
      S.emit(ShOp, Dst, {A, B});
    }
    break;
  }
  // VSHRN / VRSHRN — vector shift right and NARROW: each source lane (Q, width
  // 2*DstLane) is shifted right by #imm and truncated to its low half, packed
  // into the D destination.  This used to share the same-width VSHR body above:
  // for the narrowing form `Dst.Size > LaneSz` is false (Dst is the D half,
  // LaneSz is the wide source element), so it shifted the whole 128-bit source
  // and kept only the low 64 bits — correct for lane 0, wrong for the high
  // lane. Narrow truncation makes the right shift sign-agnostic (logical is
  // correct).
  case ARM_INS_VSHRN:
  case ARM_INS_VRSHRN: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned SrcLaneSz = LI.LaneSz;
    uint64_t Imm = (ARM.operands[2].type == ARM_OP_IMM)
                       ? static_cast<uint64_t>(ARM.operands[2].imm)
                       : 0;
    bool IsRound = (Insn->id == ARM_INS_VRSHRN);
    if (SrcLaneSz >= 2 && A.Size >= SrcLaneSz) {
      unsigned DstLaneSz = SrcLaneSz / 2;
      unsigned NLanes = A.Size / SrcLaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar SLane = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, SLane, {A, NdVar::cst(I * SrcLaneSz, 4)});
        NdVar Shifted = SLane;
        if (Imm > 0) {
          if (IsRound) {
            NdVar Rounded = S.makeTemp(SrcLaneSz);
            S.emit(NdOp::INT_ADD, Rounded,
                   {SLane, NdVar::cst(1ULL << (Imm - 1), SrcLaneSz)});
            SLane = Rounded;
          }
          Shifted = S.makeTemp(SrcLaneSz);
          S.emit(NdOp::INT_RIGHT, Shifted,
                 {SLane, NdVar::cst(Imm, SrcLaneSz)});
        }
        NdVar NLane = S.makeTemp(DstLaneSz);
        S.emit(NdOp::SUBBYTES, NLane, {Shifted, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = NLane;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {NLane, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::SUBBYTES, Dst, {A, NdVar::cst(0, 4)});
    }
    break;
  }
  // MVE bottom/top narrowing variants: keep the previous same-width behaviour
  // (M-profile Helium, not generated for A32 NEON targets, not
  // roundtrip-tested).
  case ARM_INS_VSHRNB:
  case ARM_INS_VSHRNT:
  case ARM_INS_VRSHRNB:
  case ARM_INS_VRSHRNT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp ShOp = LI.IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
    S.emit(ShOp, Dst, {A, B});
    break;
  }
  // VSRA/VRSRA — vector shift right and accumulate: Dd += (Dn >> #imm) per
  // lane. Must be per-lane: a full-width shift crosses lane boundaries and a
  // full-width add carries across lanes.  VRSRA adds the 1<<(n-1) rounding bias
  // before the shift in wider precision (see emitRoundedShr).
  case ARM_INS_VSRA:
  case ARM_INS_VRSRA: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp ShOp = LI.IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
    // VRSRA adds a 1<<(n-1) rounding bias before the shift (in wider
    // precision).
    bool IsRound = (Insn->id == ARM_INS_VRSRA);
    unsigned ImmAmt = (ARM.operands[2].type == ARM_OP_IMM)
                          ? (unsigned)ARM.operands[2].imm
                          : 0;
    if (LI.LaneSz > 0 && Dst.Size > LI.LaneSz) {
      unsigned NLanes = Dst.Size / LI.LaneSz;
      bool BImm = (ARM.operands[2].type == ARM_OP_IMM);
      NdVar BSc = B;
      if (BImm)
        BSc =
            NdVar::cst(static_cast<uint64_t>(ARM.operands[2].imm), LI.LaneSz);
      bool BScalar = BImm || (B.Size <= LI.LaneSz);
      bool Round = IsRound && BImm && ImmAmt > 0 && ImmAmt <= LI.LaneSz * 8;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar Lb = BScalar ? BSc : S.makeTemp(LI.LaneSz);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar Sh = Round
                         ? S.emitRoundedShr(La, LI.LaneSz, ImmAmt, LI.IsSigned)
                         : S.makeTemp(LI.LaneSz);
        if (!Round)
          S.emit(ShOp, Sh, {La, Lb});
        NdVar Ld = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, Ld, {OldDst, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar R = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::INT_ADD, R, {Ld, Sh});
        if (I == 0)
          Acc = R;
        else {
          NdVar Next = S.makeTemp(Acc.Size + LI.LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Sh;
      if (IsRound && ImmAmt > 0 && LI.LaneSz > 0 && ImmAmt <= Dst.Size * 8)
        Sh = S.emitRoundedShr(A, Dst.Size, ImmAmt, LI.IsSigned);
      else {
        Sh = S.makeTemp(Dst.Size);
        S.emit(ShOp, Sh, {A, B});
      }
      S.emit(NdOp::INT_ADD, Dst, {OldDst, Sh});
    }
    break;
  }
  // Narrowing saturating shift-right by immediate.  Map the A32/NEON forms to
  // the ARM NEON intrinsic (wide source + immediate -> narrow, with
  // saturation); the MVE bottom/top (B/T) variants keep the simple fallback.
  // Was a plain full-width INT_RIGHT (no narrowing/saturation/per-lane).
  case ARM_INS_VQSHRN:
  case ARM_INS_VQSHRNB:
  case ARM_INS_VQSHRNT:
  case ARM_INS_VQSHRUN:
  case ARM_INS_VQSHRUNB:
  case ARM_INS_VQSHRUNT:
  case ARM_INS_VQRSHRN:
  case ARM_INS_VQRSHRNB:
  case ARM_INS_VQRSHRNT:
  case ARM_INS_VQRSHRUN:
  case ARM_INS_VQRSHRUNB:
  case ARM_INS_VQRSHRUNT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    unsigned Imm = static_cast<unsigned>(ARM.operands[2].imm);
    bool IsBT =
        (Insn->id == ARM_INS_VQSHRNB || Insn->id == ARM_INS_VQSHRNT ||
         Insn->id == ARM_INS_VQSHRUNB || Insn->id == ARM_INS_VQSHRUNT ||
         Insn->id == ARM_INS_VQRSHRNB || Insn->id == ARM_INS_VQRSHRNT ||
         Insn->id == ARM_INS_VQRSHRUNB || Insn->id == ARM_INS_VQRSHRUNT);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned WideElem = LI.LaneSz;
    if (IsBT || WideElem < 2 || Src.Size != 16) {
      S.emit(NdOp::INT_RIGHT, Dst, {Src, NdVar::cst(Imm, Dst.Size)});
      break;
    }
    unsigned NarrowElem = WideElem / 2;
    bool Round = (Insn->id == ARM_INS_VQRSHRN || Insn->id == ARM_INS_VQRSHRUN);
    bool UnsResult =
        (Insn->id == ARM_INS_VQSHRUN || Insn->id == ARM_INS_VQRSHRUN);
    Intrinsic II;
    if (UnsResult)
      II = Round ? Intrinsic::ArmVqrshiftnsu : Intrinsic::ArmVqshiftnsu;
    else if (LI.IsSigned)
      II = Round ? Intrinsic::ArmVqrshiftns : Intrinsic::ArmVqshiftns;
    else
      II = Round ? Intrinsic::ArmVqrshiftnu : Intrinsic::ArmVqshiftnu;
    S.emitIntrinsic(II, Dst,
                    {Src, NdVar::cst(Imm, 4), NdVar::cst(NarrowElem, 4)});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
