//===- X86LiftSIMDIntHorizontal.cpp - x86/x64 SIMD horizontal and per-lane unary
// lifter -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSSE3/AVX horizontal (adjacent-pair) integer add and
/// subtract including the saturating forms, sum of absolute
/// differences, packed absolute value, and packed sign
/// application.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftSIMDIntHorizontal(X86Lifter &L, X86Lifter::LiftState &S,
                           const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // PHADDD/PHADDW/PHSUBD/PHSUBW: horizontal add/sub of adjacent pairs.
  case X86_INS_PHADDD:
  case X86_INS_PHADDW:
  case X86_INS_PHSUBD:
  case X86_INS_PHSUBW:
  case X86_INS_VPHADDD:
  case X86_INS_VPHADDW:
  case X86_INS_VPHSUBD:
  case X86_INS_VPHSUBW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? L.operandRead(S, X86.operands[1])
                    : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 4;
    if (InsnId == X86_INS_PHADDW || InsnId == X86_INS_PHSUBW ||
        InsnId == X86_INS_VPHADDW || InsnId == X86_INS_VPHSUBW)
      LaneSz = 2;
    bool IsSub = (InsnId == X86_INS_PHSUBD || InsnId == X86_INS_PHSUBW ||
                  InsnId == X86_INS_VPHSUBD || InsnId == X86_INS_VPHSUBW);
    NdOp Opc = IsSub ? NdOp::INT_SUB : NdOp::INT_ADD;
    // Horizontal add/sub operate INDEPENDENTLY within each 128-bit lane: the
    // low half of each result lane holds src1's adjacent-pair reductions, the
    // high half holds src2's — for the SAME 128-bit lane.  Treating a 256-bit
    // ymm as one wide register ("all src1 pairs then all src2 pairs")
    // mis-routes the high-lane results.  Build each 128-bit lane separately
    // (the 64-bit MMX and 128-bit xmm forms are the NumLanes==1 special case).
    unsigned LaneWidth = Dst.Size < 16 ? Dst.Size : 16;
    unsigned HalfWidth = LaneWidth / 2;
    unsigned NumLanes = Dst.Size / LaneWidth;
    unsigned PairsPerLane = HalfWidth / LaneSz;
    auto BuildLaneHalf = [&](NdVar Src, unsigned LaneBase) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < PairsPerLane; ++I) {
        unsigned Off0 = LaneBase + I * 2 * LaneSz;
        unsigned Off1 = Off0 + LaneSz;
        NdVar E0 = S.makeTemp(LaneSz), E1 = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, E0, {Src, NdVar::cst(Off0, 4)});
        S.emit(NdOp::SUBBYTES, E1, {Src, NdVar::cst(Off1, 4)});
        NdVar R = S.makeTemp(LaneSz);
        S.emit(Opc, R, {E0, E1});
        if (I == 0) {
          Acc = R;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      return Acc;
    };
    NdVar Full = S.makeTemp(0);
    for (unsigned L = 0; L < NumLanes; ++L) {
      NdVar Alo = BuildLaneHalf(A, L * LaneWidth);
      NdVar Bhi = BuildLaneHalf(B, L * LaneWidth);
      NdVar Lane = S.makeTemp(LaneWidth);
      S.emit(NdOp::CONCAT, Lane, {Bhi, Alo});
      if (L == 0) {
        Full = Lane;
      } else {
        NdVar Next = S.makeTemp((L + 1) * LaneWidth);
        S.emit(NdOp::CONCAT, Next, {Lane, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // PSADBW: sum of absolute differences of bytes.
  case X86_INS_PSADBW:
  case X86_INS_VPSADBW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? L.operandRead(S, X86.operands[1])
                    : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    // Each destination qword holds the sum of |A[b]-B[b]| over the 8 bytes b in
    // that qword (byte offset Q*8 .. Q*8+7); the upper 48 bits are zero.  The
    // grouping is per-8-byte-group with no cross-lane interaction, so xmm (2
    // qwords) and ymm (4 qwords) differ only in the qword count — every qword
    // must be built (an earlier version returned after Q==0, silently dropping
    // the high 128-bit lane and leaving a 128-bit result in a 256-bit dst).
    unsigned NumQwords = Dst.Size / 8;
    auto BuildQword = [&](unsigned QIdx) -> NdVar {
      unsigned BaseOff = QIdx * 8;
      NdVar Sum = S.makeTemp(2);
      S.emit(NdOp::COPY, Sum, {NdVar::cst(0, 2)});
      for (unsigned I = 0; I < 8; ++I) {
        unsigned Off = BaseOff + I;
        NdVar Ab = S.makeTemp(1), Bb = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, Ab, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Bb, {B, NdVar::cst(Off, 4)});
        NdVar Ax = S.makeTemp(2), Bx = S.makeTemp(2);
        S.emit(NdOp::INT_ZEXT, Ax, {Ab});
        S.emit(NdOp::INT_ZEXT, Bx, {Bb});
        NdVar Diff = S.makeTemp(2);
        S.emit(NdOp::INT_SUB, Diff, {Ax, Bx});
        NdVar Neg = S.makeTemp(2);
        S.emit(NdOp::INT_NEG2, Neg, {Diff});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsNeg, {Diff, NdVar::cst(0, 2)});
        NdVar Abs = S.makeTemp(2);
        S.emit(NdOp::SELECT, Abs, {IsNeg, Neg, Diff});
        NdVar NewSum = S.makeTemp(2);
        S.emit(NdOp::INT_ADD, NewSum, {Sum, Abs});
        Sum = NewSum;
      }
      NdVar SumExt = S.makeTemp(8);
      S.emit(NdOp::INT_ZEXT, SumExt, {Sum});
      return SumExt;
    };
    NdVar Full = BuildQword(0);
    for (unsigned Q = 1; Q < NumQwords; ++Q) {
      NdVar Next = S.makeTemp((Q + 1) * 8);
      // CONCAT takes {mostSignificant, leastSignificant}; qword Q is higher
      // than the accumulated low qwords, so it is the MSB operand.
      S.emit(NdOp::CONCAT, Next, {BuildQword(Q), Full});
      Full = Next;
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // PABS*: per-lane absolute value.
  case X86_INS_PABSB:
  case X86_INS_PABSW:
  case X86_INS_PABSD:
  case X86_INS_VPABSB:
  case X86_INS_VPABSW:
  case X86_INS_VPABSD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 1;
    if (InsnId == X86_INS_PABSW || InsnId == X86_INS_VPABSW)
      LaneSz = 2;
    if (InsnId == X86_INS_PABSD || InsnId == X86_INS_VPABSD)
      LaneSz = 4;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(Off, 4)});
        NdVar Neg = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, Neg, {Lane});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsNeg, {Lane, NdVar::cst(0, LaneSz)});
        NdVar Abs = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, Abs, {IsNeg, Neg, Lane});
        if (I == 0) {
          Acc = Abs;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Abs, Acc});
          Acc = Next;
        }
      }
      return Acc;
    };
    NdVar LoHalf = BuildHalf(0);
    NdVar HiHalf = BuildHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // PHADDSW/PHSUBSW: horizontal add/sub with signed saturation (word).
  case X86_INS_PHADDSW:
  case X86_INS_PHSUBSW:
  case X86_INS_VPHADDSW:
  case X86_INS_VPHSUBSW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? L.operandRead(S, X86.operands[1])
                    : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    bool IsSub = (InsnId == X86_INS_PHSUBSW || InsnId == X86_INS_VPHSUBSW);
    // Like PHADD/PHSUB, the saturating word forms reduce adjacent pairs PER
    // 128-bit lane: each result lane's low 64 bits come from src1's pairs and
    // its high 64 bits from src2's pairs of the SAME lane.  Build lane-by-lane
    // (NumLanes==1 covers the 64-bit MMX and 128-bit xmm forms).
    unsigned LaneWidth = Dst.Size < 16 ? Dst.Size : 16;
    unsigned HalfWidth = LaneWidth / 2;
    unsigned NumLanes = Dst.Size / LaneWidth;
    unsigned PairsPerLane = HalfWidth / 2;
    Intrinsic SatIC = IsSub ? Intrinsic::X86_SsubSat : Intrinsic::X86_SaddSat;
    auto BuildLaneHalf = [&](NdVar Src, unsigned LaneBase) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < PairsPerLane; ++I) {
        unsigned Off0 = LaneBase + I * 4, Off1 = Off0 + 2;
        NdVar E0 = S.makeTemp(2), E1 = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, E0, {Src, NdVar::cst(Off0, 4)});
        S.emit(NdOp::SUBBYTES, E1, {Src, NdVar::cst(Off1, 4)});
        NdVar Clamped = S.makeTemp(2);
        S.emitIntrinsic(SatIC, Clamped, {E0, E1});
        if (I == 0) {
          Acc = Clamped;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + 2);
          S.emit(NdOp::CONCAT, Next, {Clamped, Acc});
          Acc = Next;
        }
      }
      return Acc;
    };
    NdVar Full = S.makeTemp(0);
    for (unsigned L = 0; L < NumLanes; ++L) {
      NdVar Alo = BuildLaneHalf(A, L * LaneWidth);
      NdVar Bhi = BuildLaneHalf(B, L * LaneWidth);
      NdVar Lane = S.makeTemp(LaneWidth);
      S.emit(NdOp::CONCAT, Lane, {Bhi, Alo});
      if (L == 0) {
        Full = Lane;
      } else {
        NdVar Next = S.makeTemp((L + 1) * LaneWidth);
        S.emit(NdOp::CONCAT, Next, {Lane, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // PSIGNB/PSIGNW/PSIGND: conditional negate based on sign of second operand.
  case X86_INS_PSIGND:
  case X86_INS_PSIGNW:
  case X86_INS_PSIGNB:
  case X86_INS_VPSIGND:
  case X86_INS_VPSIGNW:
  case X86_INS_VPSIGNB: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? L.operandRead(S, X86.operands[1])
                    : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 1;
    if (InsnId == X86_INS_PSIGNW || InsnId == X86_INS_VPSIGNW)
      LaneSz = 2;
    if (InsnId == X86_INS_PSIGND || InsnId == X86_INS_VPSIGND)
      LaneSz = 4;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        NdVar Neg = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, Neg, {La});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsNeg, {Lb, NdVar::cst(0, LaneSz)});
        NdVar IsZero = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsZero, {Lb, NdVar::cst(0, LaneSz)});
        NdVar R1 = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, R1, {IsNeg, Neg, La});
        NdVar R2 = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, R2, {IsZero, NdVar::cst(0, LaneSz), R1});
        if (I == 0) {
          Acc = R2;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {R2, Acc});
          Acc = Next;
        }
      }
      return Acc;
    };
    NdVar LoHalf = BuildHalf(0);
    NdVar HiHalf = BuildHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
