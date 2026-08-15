//===- ARMLiftSIMDNEONSat.cpp - ARM32 NEON saturating arithmetic lifter ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Saturating NEON arithmetic: VQABS/VQNEG, the narrowing VQMOVN and
/// VQMOVUN, VQADD/VQSUB and the doubling multiplies VQDMULH,
/// VQDMULL, VQDMLAL and VQDMLSL.
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

bool liftSIMDNEONSat(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                     const cs_arm &ARM) {
  switch (Insn->id) {
  // NEON saturating ops
  // VQABS/VQNEG — per-lane signed saturating absolute / negate.  Only the
  // INT_MIN lane saturates (|MIN| and -MIN both -> MAX).  Previously these were
  // wrongly mapped to the Vqmovn placeholder (which returned 0).
  case ARM_INS_VQABS:
  case ARM_INS_VQNEG: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[ARM.op_count - 1]);
    bool IsAbs = (Insn->id == ARM_INS_VQABS);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned LaneSz = LI.LaneSz;
    if (LaneSz > 0 && LaneSz <= 8 && Dst.Size >= LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      unsigned Bits = LaneSz * 8;
      uint64_t MinV = (Bits >= 64) ? (1ULL << 63) : (1ULL << (Bits - 1));
      uint64_t MaxV = (Bits >= 64) ? ~(1ULL << 63) : ((1ULL << (Bits - 1)) - 1);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar L = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, L, {Src, NdVar::cst(static_cast<uint64_t>(I) * LaneSz, 4)});
        NdVar Neg = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, Neg, {L});
        NdVar R = Neg;
        if (IsAbs) {
          NdVar IsNeg = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, IsNeg, {L, NdVar::cst(0, LaneSz)});
          R = S.makeTemp(LaneSz);
          S.emit(NdOp::SELECT, R, {IsNeg, Neg, L});
        }
        NdVar IsMin = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsMin, {L, NdVar::cst(MinV, LaneSz)});
        NdVar Sat = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, Sat, {IsMin, NdVar::cst(MaxV, LaneSz), R});
        if (I == 0)
          Acc = Sat;
        else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Sat, Acc});
          Acc = Next;
        }
      }
      if (Acc.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Acc});
      else
        S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Neg = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NEG2, Neg, {Src});
      if (IsAbs) {
        NdVar IsNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsNeg, {Src, NdVar::cst(0, Src.Size)});
        S.emit(NdOp::SELECT, Dst, {IsNeg, Neg, Src});
      } else {
        S.emit(NdOp::COPY, Dst, {Neg});
      }
    }
    break;
  }
  // VQMOVN narrows each wide lane to half width with *saturation* into the
  // narrow lane's signed/unsigned range (per the .s/.u suffix).  The old
  // handler plain-truncated, silently dropping the saturation for out-of-range
  // lanes.
  case ARM_INS_VQMOVN:
  case ARM_INS_VQMOVNB:
  case ARM_INS_VQMOVNT: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[ARM.op_count - 1]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    bool IsSigned = LI.IsSigned;
    unsigned SrcLaneSz = LI.LaneSz;
    if (SrcLaneSz >= 2 && Src.Size > SrcLaneSz) {
      unsigned DstLaneSz = SrcLaneSz / 2;
      unsigned NLanes = Src.Size / SrcLaneSz;
      unsigned DstBits = DstLaneSz * 8;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Wide = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, Wide, {Src, NdVar::cst(I * SrcLaneSz, 4)});
        // Narrowing saturate: trunc to DstLaneSz, extend back, compare with
        // original.  If equal the value fits; otherwise pick max/min based on
        // sign.  Avoids the fork's InstCombine mis-fold on INT_SLESS+SELECT
        // and @llvm.smax/@llvm.smin clamp chains.
        NdVar Narrow = S.makeTemp(DstLaneSz);
        S.emit(NdOp::SUBBYTES, Narrow, {Wide, NdVar::cst(0, 4)});
        NdVar BackWide = S.makeTemp(SrcLaneSz);
        if (IsSigned)
          S.emit(NdOp::INT_SEXT, BackWide, {Narrow});
        else
          S.emit(NdOp::INT_ZEXT, BackWide, {Narrow});
        NdVar Fits = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, Fits, {Wide, BackWide});
        int64_t MaxV = IsSigned ? (1LL << (DstBits - 1)) - 1
                                : ((DstBits >= 64) ? (int64_t)~0ULL
                                                   : (1LL << DstBits) - 1);
        int64_t MinV = IsSigned ? -(1LL << (DstBits - 1)) : 0;
        NdVar IsPos = S.makeTemp(1);
        S.emit(IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS, IsPos,
               {NdVar::cst(0, SrcLaneSz), Wide});
        NdVar OverflowVal = S.makeTemp(DstLaneSz);
        S.emit(NdOp::SELECT, OverflowVal,
               {IsPos, NdVar::cst((uint64_t)MaxV, DstLaneSz),
                NdVar::cst((uint64_t)MinV, DstLaneSz)});
        NdVar NarrowResult = S.makeTemp(DstLaneSz);
        S.emit(NdOp::SELECT, NarrowResult, {Fits, Narrow, OverflowVal});
        if (I == 0)
          Acc = NarrowResult;
        else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {NarrowResult, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VQADD:
  case ARM_INS_VQSUB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    // capstone may leave vector_data INVALID -> recover from the mnemonic.
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    bool IsSigned = LI.IsSigned;
    bool IsAdd = (Insn->id == ARM_INS_VQADD);
    unsigned LaneSz = LI.LaneSz;
    if (LaneSz > 0 && LaneSz <= 4 && Dst.Size > LaneSz) {
      unsigned WideSz = LaneSz * 2;
      unsigned NLanes = Dst.Size / LaneSz;
      unsigned Bits = LaneSz * 8;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Wa = S.makeTemp(WideSz);
        NdVar Wb = S.makeTemp(WideSz);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Wa, {La});
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Wb, {Lb});
        NdVar Wide = S.makeTemp(WideSz);
        S.emit(IsAdd ? NdOp::INT_ADD : NdOp::INT_SUB, Wide, {Wa, Wb});
        // Saturate the wide result to the lane's range before truncating.
        NdVar Clamped = S.makeTemp(LaneSz);
        if (IsSigned) {
          int64_t MaxV = (1LL << (Bits - 1)) - 1;
          int64_t MinV = -(1LL << (Bits - 1));
          NdVar TooHi = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, TooHi,
                 {NdVar::cst((uint64_t)MaxV, WideSz), Wide});
          NdVar C1 = S.makeTemp(WideSz);
          S.emit(NdOp::SELECT, C1,
                 {TooHi, NdVar::cst((uint64_t)MaxV, WideSz), Wide});
          NdVar TooLo = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, TooLo,
                 {C1, NdVar::cst((uint64_t)MinV, WideSz)});
          NdVar C2 = S.makeTemp(WideSz);
          S.emit(NdOp::SELECT, C2,
                 {TooLo, NdVar::cst((uint64_t)MinV, WideSz), C1});
          S.emit(NdOp::SUBBYTES, Clamped, {C2, NdVar::cst(0, 4)});
        } else {
          uint64_t MaxV = (1ULL << Bits) - 1;
          NdVar C1 = S.makeTemp(WideSz);
          if (IsAdd) {
            NdVar TooHi = S.makeTemp(1);
            S.emit(NdOp::INT_LESS, TooHi,
                   {NdVar::cst(MaxV, WideSz), Wide});
            S.emit(NdOp::SELECT, C1,
                   {TooHi, NdVar::cst(MaxV, WideSz), Wide});
          } else {
            // Detect unsigned underflow from the widened operands.  Testing
            // the wrapped subtraction result after an upper clamp loses the
            // sign and incorrectly saturates underflow to MaxV instead of 0.
            NdVar Underflow = S.makeTemp(1);
            S.emit(NdOp::INT_LESS, Underflow, {Wa, Wb});
            S.emit(NdOp::SELECT, C1,
                   {Underflow, NdVar::cst(0, WideSz), Wide});
          }
          S.emit(NdOp::SUBBYTES, Clamped, {C1, NdVar::cst(0, 4)});
        }
        if (I == 0)
          Acc = Clamped;
        else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Clamped, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emitIntrinsic(IsAdd ? Intrinsic::ArmVqadd : Intrinsic::ArmVqsub, Dst,
                      {A, B});
    }
    break;
  }
  // VQMOVUN narrows each *signed* wide lane to an *unsigned* half-width lane
  // with saturation (negatives clamp to 0, large values clamp to the unsigned
  // max). The old handler emitted an unhandled intrinsic that silently returned
  // 0.
  case ARM_INS_VQMOVUN:
  case ARM_INS_VQMOVUNB:
  case ARM_INS_VQMOVUNT: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[ARM.op_count - 1]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned SrcLaneSz = LI.LaneSz;
    if (SrcLaneSz >= 2 && Src.Size > SrcLaneSz) {
      unsigned DstLaneSz = SrcLaneSz / 2;
      unsigned NLanes = Src.Size / SrcLaneSz;
      unsigned DstBits = DstLaneSz * 8;
      uint64_t MaxV = (DstBits >= 64) ? ~0ULL : ((1ULL << DstBits) - 1);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Wide = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, Wide, {Src, NdVar::cst(I * SrcLaneSz, 4)});
        NdVar TooHi = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, TooHi, {NdVar::cst(MaxV, SrcLaneSz), Wide});
        NdVar C1 = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SELECT, C1, {TooHi, NdVar::cst(MaxV, SrcLaneSz), Wide});
        NdVar Neg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, Neg, {C1, NdVar::cst(0, SrcLaneSz)});
        NdVar C2 = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SELECT, C2, {Neg, NdVar::cst(0, SrcLaneSz), C1});
        NdVar Narrow = S.makeTemp(DstLaneSz);
        S.emit(NdOp::SUBBYTES, Narrow, {C2, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = Narrow;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {Narrow, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
    }
    break;
  }
  // Saturating doubling multiply (high half VQDMULH/VQRDMULH; widening
  // VQDMULL). Map to the ARM NEON intrinsic; was a plain full-width INT_MULT
  // placeholder. The MVE bottom/top widening forms keep the fallback.
  case ARM_INS_VQDMULH:
  case ARM_INS_VQRDMULH:
  case ARM_INS_VQDMULL:
  case ARM_INS_VQDMULLB:
  case ARM_INS_VQDMULLT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned SrcElem = LI.LaneSz;
    bool MveBT = (Insn->id == ARM_INS_VQDMULLB || Insn->id == ARM_INS_VQDMULLT);
    if (SrcElem == 0 || MveBT) {
      S.emit(NdOp::INT_MULT, Dst, {A, B});
      break;
    }
    bool Widen = (Insn->id == ARM_INS_VQDMULL);
    unsigned DstElem = Widen ? SrcElem * 2 : SrcElem;
    // By-scalar `vqdmulh.s16 q,q,d[idx]` / `vqdmull.s16 q,d,d[idx]`:
    // operandRead ignores vector_index, so broadcast the selected Dm lane
    // across A.Size before the per-lane intrinsic (otherwise it multiplied
    // lane-by-lane).
    int BLane = ARM.operands[2].neon_lane >= 0 ? ARM.operands[2].neon_lane
                                               : ARM.operands[2].vector_index;
    if (BLane >= 0 && A.Size >= SrcElem) {
      NdVar Lane = S.makeTemp(SrcElem);
      S.emit(NdOp::SUBBYTES, Lane,
             {B, NdVar::cst(static_cast<uint64_t>(BLane) * SrcElem, 4)});
      NdVar Bcast = Lane;
      for (unsigned I = 1; I < A.Size / SrcElem; ++I) {
        NdVar Next = S.makeTemp(Bcast.Size + SrcElem);
        S.emit(NdOp::CONCAT, Next, {Lane, Bcast});
        Bcast = Next;
      }
      B = Bcast;
    }
    Intrinsic II = (Insn->id == ARM_INS_VQDMULH)    ? Intrinsic::ArmVqdmulh
                   : (Insn->id == ARM_INS_VQRDMULH) ? Intrinsic::ArmVqrdmulh
                                                    : Intrinsic::ArmVqdmull;
    S.emitIntrinsic(II, Dst, {A, B, NdVar::cst(DstElem, 4)});
    break;
  }
  // VQDMLAL/VQDMLSL — signed saturating doubling multiply-accumulate long:
  //   Vd[i] = SignedSat(Vd[i] +/- SignedSat(2 * Vn[i] * Vm[i]))
  // Narrow signed source lanes (s16/s32) widen to double-width dest lanes; the
  // doubled product saturates, then the accumulate saturates.  Was a bare
  // full-width INT_MULT + INT_ADD placeholder (no widening/doubling/saturation,
  // and VQDMLSL even accumulated with INT_ADD).  Mirrors AArch64
  // SQDMLAL/SQDMLSL.
  case ARM_INS_VQDMLAL:
  case ARM_INS_VQDMLSL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSub = (Insn->id == ARM_INS_VQDMLSL);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned InSz = LI.LaneSz;
    if (InSz == 0 || InSz > 4 || Dst.Size <= InSz) {
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {A, B});
      S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Dst, {OldDst, Prod});
      break;
    }
    unsigned DstLane = InSz * 2;
    unsigned NLanes = Dst.Size / DstLane;
    // Indexed form `vqdmlal.s16 q,d,d[idx]` broadcasts one B lane; operandRead
    // returns the whole Dm, so detect the lane index (same fix as #386 VMLA).
    int BLane = ARM.operands[2].neon_lane >= 0 ? ARM.operands[2].neon_lane
                                               : ARM.operands[2].vector_index;
    // Signed saturating add/sub at the dest lane width: overflow is detected
    // from operand/result signs and the lane forced to MIN/MAX (no i128
    // needed).
    auto satAddSub = [&](NdVar AVal, NdVar BVal, unsigned W,
                         bool Sub) -> NdVar {
      unsigned WBits = W * 8;
      uint64_t MaxV =
          (WBits >= 64) ? 0x7FFFFFFFFFFFFFFFULL : ((1ULL << (WBits - 1)) - 1);
      uint64_t MinV =
          (WBits >= 64) ? 0x8000000000000000ULL : (1ULL << (WBits - 1));
      NdVar Res = S.makeTemp(W);
      S.emit(Sub ? NdOp::INT_SUB : NdOp::INT_ADD, Res, {AVal, BVal});
      NdVar ANeg = S.makeTemp(1), BNeg = S.makeTemp(1), RNeg = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, ANeg, {AVal, NdVar::cst(0, W)});
      S.emit(NdOp::INT_SLESS, BNeg, {BVal, NdVar::cst(0, W)});
      S.emit(NdOp::INT_SLESS, RNeg, {Res, NdVar::cst(0, W)});
      NdVar SignCond = S.makeTemp(1);
      if (Sub)
        S.emit(NdOp::BOOL_XOR, SignCond, {ANeg, BNeg});
      else
        S.emit(NdOp::INT_EQUAL, SignCond, {ANeg, BNeg});
      NdVar SignFlip = S.makeTemp(1);
      S.emit(NdOp::BOOL_XOR, SignFlip, {ANeg, RNeg});
      NdVar Ovf = S.makeTemp(1);
      S.emit(NdOp::INT_AND, Ovf, {SignCond, SignFlip});
      NdVar SatVal = S.makeTemp(W);
      S.emit(NdOp::SELECT, SatVal,
             {ANeg, NdVar::cst(MinV, W), NdVar::cst(MaxV, W)});
      NdVar Out = S.makeTemp(W);
      S.emit(NdOp::SELECT, Out, {Ovf, SatVal, Res});
      return Out;
    };
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar NarrA = S.makeTemp(InSz);
      S.emit(NdOp::SUBBYTES, NarrA, {A, NdVar::cst(I * InSz, 4)});
      NdVar NarrB = S.makeTemp(InSz);
      uint64_t BOff = (BLane >= 0) ? static_cast<uint64_t>(BLane) * InSz
                                   : static_cast<uint64_t>(I) * InSz;
      S.emit(NdOp::SUBBYTES, NarrB, {B, NdVar::cst(BOff, 4)});
      NdVar WA = S.makeTemp(DstLane), WB = S.makeTemp(DstLane);
      S.emit(NdOp::INT_SEXT, WA, {NarrA});
      S.emit(NdOp::INT_SEXT, WB, {NarrB});
      NdVar Prod = S.makeTemp(DstLane);
      S.emit(NdOp::INT_MULT, Prod, {WA, WB});
      if (DstLane <= 4) {
        // Double in a 2x-wide temp and clamp to the dest signed range;
        // saturates only when both narrow lanes are INT_MIN (2*MIN*MIN > MAX).
        unsigned Wide = DstLane * 2, Bits = DstLane * 8;
        int64_t MaxV = (1LL << (Bits - 1)) - 1, MinV = -(1LL << (Bits - 1));
        NdVar P2 = S.makeTemp(Wide);
        S.emit(NdOp::INT_SEXT, P2, {Prod});
        NdVar Dbl = S.makeTemp(Wide);
        S.emit(NdOp::INT_LEFT, Dbl, {P2, NdVar::cst(1, Wide)});
        NdVar TooHi = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, TooHi,
               {NdVar::cst((uint64_t)MaxV, Wide), Dbl});
        NdVar C1 = S.makeTemp(Wide);
        S.emit(NdOp::SELECT, C1,
               {TooHi, NdVar::cst((uint64_t)MaxV, Wide), Dbl});
        NdVar TooLo = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, TooLo,
               {C1, NdVar::cst((uint64_t)MinV, Wide)});
        NdVar C2 = S.makeTemp(Wide);
        S.emit(NdOp::SELECT, C2,
               {TooLo, NdVar::cst((uint64_t)MinV, Wide), C1});
        NdVar Narrowed = S.makeTemp(DstLane);
        S.emit(NdOp::SUBBYTES, Narrowed, {C2, NdVar::cst(0, 4)});
        Prod = Narrowed;
      } else {
        // 64-bit dest: 2*MIN32*MIN32 == 2^63 overflows i64; saturate that lane
        // to INT64_MAX (an i128 clamp cannot represent the bound).
        NdVar Dbl = S.makeTemp(DstLane);
        S.emit(NdOp::INT_LEFT, Dbl, {Prod, NdVar::cst(1, DstLane)});
        unsigned NBits = InSz * 8;
        uint64_t NMin = 1ULL << (NBits - 1);
        NdVar AMin = S.makeTemp(1), BMin = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, AMin, {NarrA, NdVar::cst(NMin, InSz)});
        S.emit(NdOp::INT_EQUAL, BMin, {NarrB, NdVar::cst(NMin, InSz)});
        NdVar Both = S.makeTemp(1);
        S.emit(NdOp::INT_AND, Both, {AMin, BMin});
        NdVar Sat = S.makeTemp(DstLane);
        S.emit(NdOp::SELECT, Sat,
               {Both, NdVar::cst(0x7FFFFFFFFFFFFFFFULL, DstLane), Dbl});
        Prod = Sat;
      }
      NdVar LaneDst = S.makeTemp(DstLane);
      S.emit(NdOp::SUBBYTES, LaneDst, {OldDst, NdVar::cst(I * DstLane, 4)});
      NdVar Lr = satAddSub(LaneDst, Prod, DstLane, IsSub);
      if (I == 0) {
        Acc = Lr;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + DstLane);
        S.emit(NdOp::CONCAT, Next, {Lr, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case ARM_INS_VQRDMLAH:
  case ARM_INS_VQRDMLASH:
  case ARM_INS_VQRDMLSH:
  case ARM_INS_VQDMLADH:
  case ARM_INS_VQDMLADHX:
  case ARM_INS_VQDMLAH:
  case ARM_INS_VQDMLASH:
  case ARM_INS_VQDMLSDH:
  case ARM_INS_VQDMLSDHX:
  case ARM_INS_VQRDMLADH:
  case ARM_INS_VQRDMLADHX:
  case ARM_INS_VQRDMLSDH:
  case ARM_INS_VQRDMLSDHX: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_ADD, Dst, {NdVar::reg(Dst.Offset, Dst.Size), Prod});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
