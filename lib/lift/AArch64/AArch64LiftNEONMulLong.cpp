//===- AArch64LiftNEONMulLong.cpp - NEON widening multiply ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Widening multiply (SMULL2/UMULL2/SQDMULL), polynomial multiply
/// (PMUL/PMULL), widening multiply-accumulate (SMLAL/UMLAL/
/// SQDMLAL/SMLSL/UMLSL/SQDMLSL), saturating doubling multiply
/// high (SQDMULH/SQRDMULH/SQRDMLAH/SQRDMLSH) and the pairwise
/// add-long accumulate SADDLP/UADDLP/SADALP/UADALP.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftNEONMulLong(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // NEON widening multiply (long).  SMULL2/UMULL2 take the UPPER half of the
  // 128-bit source operands; SQDMULL/SQDMULL2 additionally double the product
  // and saturate.  All are per-lane widening (narrow*narrow -> wide); the
  // previous full-width INT_MULT i128 propagated carries across lanes and was
  // silently wrong (broke e.g. mulhi's smull2-based high-half products).
  case AARCH64_INS_SMULL2:
  case AARCH64_INS_UMULL2:
  case AARCH64_INS_SQDMULL:
  case AARCH64_INS_SQDMULL2: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    bool IsUpper =
        (Insn->id == AARCH64_INS_SMULL2 || Insn->id == AARCH64_INS_UMULL2 ||
         Insn->id == AARCH64_INS_SQDMULL2);
    bool IsDoubling =
        (Insn->id == AARCH64_INS_SQDMULL || Insn->id == AARCH64_INS_SQDMULL2);
    bool IsSigned = (Insn->id != AARCH64_INS_UMULL2);
    unsigned DstLane = 0;
    auto DstVas = ARM64.operands[0].vas;
    if (DstVas == AARCH64LAYOUT_VL_4S)
      DstLane = 4;
    else if (DstVas == AARCH64LAYOUT_VL_8H)
      DstLane = 2;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      DstLane = 8;
    if (DstLane > 0 && Dst.Size > DstLane) {
      unsigned NLanes = Dst.Size / DstLane;
      unsigned NarrowLane = DstLane / 2;
      unsigned Base = IsUpper ? (NLanes * NarrowLane) : 0; // upper 64 bits
      // By-element `smull2/sqdmull v.Ts, v.Th, vN.<ty>[idx]`: operandRead
      // returns just the selected element (no Base/upper-half offset), so
      // broadcast it.
      bool BScalar = (B.Size <= NarrowLane);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar NarrA = S.makeTemp(NarrowLane);
        S.emit(NdOp::SUBBYTES, NarrA,
               {A, NdVar::cst(Base + I * NarrowLane, 4)});
        NdVar WA = S.makeTemp(DstLane);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WA, {NarrA});
        NdVar NarrB = BScalar ? B : S.makeTemp(NarrowLane);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, NarrB,
                 {B, NdVar::cst(Base + I * NarrowLane, 4)});
        NdVar WB = S.makeTemp(DstLane);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WB, {NarrB});
        NdVar Lr = S.makeTemp(DstLane);
        S.emit(NdOp::INT_MULT, Lr, {WA, WB});
        if (IsDoubling && DstLane <= 4) {
          // result = saturate(2 * prod).  Only the (MIN,MIN) product overflows;
          // compute in a 2x-wide temp, double, clamp to the signed range.
          unsigned Wide = DstLane * 2;
          unsigned Bits = DstLane * 8;
          int64_t MaxV = (1LL << (Bits - 1)) - 1;
          int64_t MinV = -(1LL << (Bits - 1));
          NdVar P2 = S.makeTemp(Wide);
          S.emit(NdOp::INT_SEXT, P2, {Lr});
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
          Lr = Narrowed;
        } else if (IsDoubling) {
          // 64-bit dest: doubling the i64 product can only overflow when both
          // narrow sources are INT_MIN (2*MIN*MIN == 2^63); saturate that lane
          // to INT64_MAX (a 2x-wide i128 clamp can't represent the bound).
          NdVar Dbl = S.makeTemp(DstLane);
          S.emit(NdOp::INT_LEFT, Dbl, {Lr, NdVar::cst(1, DstLane)});
          unsigned NBits = NarrowLane * 8;
          uint64_t NMin = 1ULL << (NBits - 1);
          NdVar AMin = S.makeTemp(1), BMin = S.makeTemp(1);
          S.emit(NdOp::INT_EQUAL, AMin, {NarrA, NdVar::cst(NMin, NarrowLane)});
          S.emit(NdOp::INT_EQUAL, BMin, {NarrB, NdVar::cst(NMin, NarrowLane)});
          NdVar Both = S.makeTemp(1);
          S.emit(NdOp::INT_AND, Both, {AMin, BMin});
          NdVar Sat = S.makeTemp(DstLane);
          S.emit(NdOp::SELECT, Sat,
                 {Both, NdVar::cst(0x7FFFFFFFFFFFFFFFULL, DstLane), Dbl});
          Lr = Sat;
        }
        if (I == 0) {
          Acc = Lr;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLane);
          S.emit(NdOp::CONCAT, Next, {Lr, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      // Scalar / unknown-arrangement fallback: widen then multiply.
      NdVar ExtA = S.makeTemp(8), ExtB = S.makeTemp(8);
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, ExtA, {A});
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, ExtB, {B});
      S.emit(NdOp::INT_MULT, Dst, {ExtA, ExtB});
    }
    break;
  }
  // PMUL — polynomial (carry-less) multiply, same element width (i8 lanes,
  // `.8b`/`.16b`).  GF(2)[x] multiply, NOT integer multiply; map to the AArch64
  // NEON intrinsic.  Was grouped with SVE2 widening mults as a bare INT_MULT.
  case AARCH64_INS_PMUL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emitIntrinsic(Intrinsic::A64_Pmul, Dst, {A, B});
    break;
  }
  // PMULL/PMULL2 — polynomial (carry-less) multiply long.  Two element widths:
  // p8 (`.8b`→`.8h`, 8 byte-pairs → 8 halfwords) and p64 (`.1d`→`.1q`, one
  // doubleword → 128-bit).  The "2" form multiplies the *upper* 64-bit lane.
  // The element width is passed to the emitter as a trailing constant so it can
  // pick @llvm.aarch64.neon.pmull (p8) vs pmull64 (p64); the old handler
  // emitted a bare intrinsic with no handler and silently returned 0.
  case AARCH64_INS_PMULL:
  case AARCH64_INS_PMULL2: {
    if (ARM64.op_count < 3)
      break;
    bool IsUpper = (Insn->id == AARCH64_INS_PMULL2);
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    unsigned ElemSz = neonElemSize(ARM64.operands[1].vas);
    if (ElemSz != 1 && ElemSz != 8) {
      unsigned DstElem = neonElemSize(ARM64.operands[0].vas);
      ElemSz = (DstElem >= 16) ? 8 : 1; // 1Q dst -> p64, else p8
    }
    auto lowOrHigh = [&](NdVar V) -> NdVar {
      if (V.Size <= 8)
        return V;
      NdVar H = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, H, {V, NdVar::cst(IsUpper ? 8 : 0, 4)});
      return H;
    };
    A = lowOrHigh(A);
    B = lowOrHigh(B);
    S.emitIntrinsic(Intrinsic::A64_Pmull, Dst, {A, B, NdVar::cst(ElemSz, 4)});
    break;
  }
  // NEON widening multiply-accumulate (long).  The wide accumulator Dst is
  // updated per-lane: Dst.lane += / -= widen(narrow A.lane) * widen(narrow
  // B.lane).  The "2" variants take the UPPER half of the 128-bit sources;
  // SQDMLAL/SQDMLSL double+saturate the product and saturate the accumulate;
  // the indexed form (`smlal v.4s, v.4h, v.h[idx]`) broadcasts a single B lane.
  // The previous full-width INT_MULT i128 + INT_ADD ignored widening, lane
  // structure, signedness and the "2" upper-half read (broke q15 sums).
  case AARCH64_INS_SMLAL:
  case AARCH64_INS_SMLAL2:
  case AARCH64_INS_UMLAL:
  case AARCH64_INS_UMLAL2:
  case AARCH64_INS_SQDMLAL:
  case AARCH64_INS_SQDMLAL2:
  case AARCH64_INS_SMLSL:
  case AARCH64_INS_SMLSL2:
  case AARCH64_INS_UMLSL:
  case AARCH64_INS_UMLSL2:
  case AARCH64_INS_SQDMLSL:
  case AARCH64_INS_SQDMLSL2: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSub =
        (Insn->id == AARCH64_INS_SMLSL || Insn->id == AARCH64_INS_SMLSL2 ||
         Insn->id == AARCH64_INS_UMLSL || Insn->id == AARCH64_INS_UMLSL2 ||
         Insn->id == AARCH64_INS_SQDMLSL || Insn->id == AARCH64_INS_SQDMLSL2);
    bool IsUpper =
        (Insn->id == AARCH64_INS_SMLAL2 || Insn->id == AARCH64_INS_UMLAL2 ||
         Insn->id == AARCH64_INS_SQDMLAL2 || Insn->id == AARCH64_INS_SMLSL2 ||
         Insn->id == AARCH64_INS_UMLSL2 || Insn->id == AARCH64_INS_SQDMLSL2);
    bool IsDoubling =
        (Insn->id == AARCH64_INS_SQDMLAL || Insn->id == AARCH64_INS_SQDMLAL2 ||
         Insn->id == AARCH64_INS_SQDMLSL || Insn->id == AARCH64_INS_SQDMLSL2);
    bool IsSigned =
        (Insn->id != AARCH64_INS_UMLAL && Insn->id != AARCH64_INS_UMLAL2 &&
         Insn->id != AARCH64_INS_UMLSL && Insn->id != AARCH64_INS_UMLSL2);
    unsigned DstLane = 0;
    auto DstVas = ARM64.operands[0].vas;
    if (DstVas == AARCH64LAYOUT_VL_4S)
      DstLane = 4;
    else if (DstVas == AARCH64LAYOUT_VL_8H)
      DstLane = 2;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      DstLane = 8;
    if (DstLane > 0 && Dst.Size > DstLane) {
      unsigned NLanes = Dst.Size / DstLane;
      unsigned NarrowLane = DstLane / 2;
      unsigned Base = IsUpper ? (NLanes * NarrowLane) : 0; // upper 64 bits
      bool BScalar = (B.Size <= NarrowLane); // indexed scalar broadcast
      NdOp ExtOp = IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      // Signed saturating add/sub at the lane width (no i128): used for the
      // SQDMLAL/SQDMLSL ACCUMULATE, which clamps Vd +/- product to the dest
      // element's signed range.  Overflow is detected from operand/result signs
      // (ADD overflows only when addends share a sign, SUB only when they
      // differ, and in either case the result sign flips away from the first
      // operand's), then the lane is forced to MIN (first operand negative) or
      // MAX.  Plain SMLAL/UMLAL do not saturate, so this is gated on
      // IsDoubling.
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
          S.emit(NdOp::BOOL_XOR, SignCond, {ANeg, BNeg}); // operands differ
        else
          S.emit(NdOp::INT_EQUAL, SignCond, {ANeg, BNeg}); // operands match
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
        NdVar NarrA = S.makeTemp(NarrowLane);
        S.emit(NdOp::SUBBYTES, NarrA,
               {A, NdVar::cst(Base + I * NarrowLane, 4)});
        NdVar WA = S.makeTemp(DstLane);
        S.emit(ExtOp, WA, {NarrA});
        // Keep the narrow B lane (indexed forms broadcast the scalar B) so the
        // 64-bit doubling-saturation corner can test it for INT_MIN.
        NdVar NarrB = BScalar ? B : S.makeTemp(NarrowLane);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, NarrB,
                 {B, NdVar::cst(Base + I * NarrowLane, 4)});
        NdVar WB = S.makeTemp(DstLane);
        S.emit(ExtOp, WB, {NarrB});
        NdVar Prod = S.makeTemp(DstLane);
        S.emit(NdOp::INT_MULT, Prod, {WA, WB});
        if (IsDoubling && DstLane <= 4) {
          // SQDMLAL: product = saturate(2 * A*B) computed in a 2x-wide temp.
          unsigned Wide = DstLane * 2;
          unsigned Bits = DstLane * 8;
          int64_t MaxV = (1LL << (Bits - 1)) - 1;
          int64_t MinV = -(1LL << (Bits - 1));
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
        } else if (IsDoubling) {
          // 64-bit dest: doubling the i64 product overflows only when both
          // narrow sources are INT_MIN (2*MIN*MIN == 2^63); saturate that lane
          // to INT64_MAX (an i128 clamp can't represent the bound).  Mirrors
          // the SQDMULL handler.
          NdVar Dbl = S.makeTemp(DstLane);
          S.emit(NdOp::INT_LEFT, Dbl, {Prod, NdVar::cst(1, DstLane)});
          unsigned NBits = NarrowLane * 8;
          uint64_t NMin = 1ULL << (NBits - 1);
          NdVar AMin = S.makeTemp(1), BMin = S.makeTemp(1);
          S.emit(NdOp::INT_EQUAL, AMin, {NarrA, NdVar::cst(NMin, NarrowLane)});
          S.emit(NdOp::INT_EQUAL, BMin, {NarrB, NdVar::cst(NMin, NarrowLane)});
          NdVar Both = S.makeTemp(1);
          S.emit(NdOp::INT_AND, Both, {AMin, BMin});
          NdVar Sat = S.makeTemp(DstLane);
          S.emit(NdOp::SELECT, Sat,
                 {Both, NdVar::cst(0x7FFFFFFFFFFFFFFFULL, DstLane), Dbl});
          Prod = Sat;
        }
        NdVar LaneDst = S.makeTemp(DstLane);
        S.emit(NdOp::SUBBYTES, LaneDst, {OldDst, NdVar::cst(I * DstLane, 4)});
        NdVar Lr;
        if (IsDoubling) {
          // SQDMLAL/SQDMLSL saturate Vd +/- product to the dest element range.
          Lr = satAddSub(LaneDst, Prod, DstLane, IsSub);
        } else {
          Lr = S.makeTemp(DstLane);
          S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Lr, {LaneDst, Prod});
        }
        if (I == 0) {
          Acc = Lr;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLane);
          S.emit(NdOp::CONCAT, Next, {Lr, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      // Scalar / unknown-arrangement fallback: widen then multiply-accumulate.
      NdVar ExtA = S.makeTemp(8), ExtB = S.makeTemp(8);
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, ExtA, {A});
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, ExtB, {B});
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {ExtA, ExtB});
      if (IsDoubling) {
        NdVar Dbl = S.makeTemp(Dst.Size);
        S.emit(NdOp::INT_LEFT, Dbl, {Prod, NdVar::cst(1, Dst.Size)});
        Prod = Dbl;
      }
      S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Dst, {OldDst, Prod});
    }
    break;
  }
  // NEON saturating doubling multiply returning the high half.
  // SQDMULH:  Dst[i] = sat((2*A[i]*B[i]) >> N)              (saturates
  // A==B==MIN) SQRDMULH: Dst[i] = sat((2*A[i]*B[i] + 2^(N-1)) >> N) (rounding
  // variant) SQRDMLAH/SQRDMLSH: Dst[i] = sat(OldDst[i] +/- sqrdmulh). Old code
  // was a plain full-width INT_MULT placeholder (no doubling, high half,
  // rounding, saturation, per-lane, nor accumulate).
  case AARCH64_INS_SQDMULH:
  case AARCH64_INS_SQRDMULH:
  case AARCH64_INS_SQRDMLAH:
  case AARCH64_INS_SQRDMLSH: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    bool IsRounding =
        (Insn->id == AARCH64_INS_SQRDMULH || Insn->id == AARCH64_INS_SQRDMLAH ||
         Insn->id == AARCH64_INS_SQRDMLSH);
    bool IsAccum =
        (Insn->id == AARCH64_INS_SQRDMLAH || Insn->id == AARCH64_INS_SQRDMLSH);
    bool IsAccSub = (Insn->id == AARCH64_INS_SQRDMLSH);
    bool ByElem = (ARM64.operands[2].vector_index >= 0);

    unsigned LaneSz = 0;
    auto Vas = ARM64.operands[0].vas;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    if (LaneSz == 0)
      LaneSz = (Dst.Size == 2 || Dst.Size == 4) ? Dst.Size : 4; // scalar h/s
    unsigned NLanes = Dst.Size / LaneSz;
    unsigned Wide = LaneSz * 2;
    unsigned LaneBits = LaneSz * 8;
    uint64_t MinV = 1ULL << (LaneBits - 1);
    uint64_t MaxV = MinV - 1;
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);

    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar La = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
      NdVar Lb = S.makeTemp(LaneSz);
      if (ByElem)
        Lb = B; // by-element: same selected lane broadcast to all
      else
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
      NdVar WA = S.makeTemp(Wide), WB = S.makeTemp(Wide);
      S.emit(NdOp::INT_SEXT, WA, {La});
      S.emit(NdOp::INT_SEXT, WB, {Lb});
      NdVar Prod = S.makeTemp(Wide);
      S.emit(NdOp::INT_MULT, Prod, {WA, WB});
      NdVar Dbl = S.makeTemp(Wide);
      S.emit(NdOp::INT_LEFT, Dbl, {Prod, NdVar::cst(1, Wide)});
      if (IsRounding) {
        NdVar Rnd = S.makeTemp(Wide);
        S.emit(NdOp::INT_ADD, Rnd, {Dbl, NdVar::cst(MinV, Wide)});
        Dbl = Rnd;
      }
      NdVar High = S.makeTemp(Wide);
      S.emit(NdOp::INT_ASHR, High, {Dbl, NdVar::cst(LaneBits, Wide)});
      NdVar HighN = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, HighN, {High, NdVar::cst(0, 4)});
      // Saturation: the only overflow is A==B==INT_MIN -> INT_MAX.
      NdVar AIsMin = S.makeTemp(1), BIsMin = S.makeTemp(1);
      S.emit(NdOp::INT_EQUAL, AIsMin, {La, NdVar::cst(MinV, LaneSz)});
      S.emit(NdOp::INT_EQUAL, BIsMin, {Lb, NdVar::cst(MinV, LaneSz)});
      NdVar BothMin = S.makeTemp(1);
      S.emit(NdOp::INT_AND, BothMin, {AIsMin, BIsMin});
      NdVar MulRes = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, MulRes, {BothMin, NdVar::cst(MaxV, LaneSz), HighN});

      NdVar LaneRes = MulRes;
      if (IsAccum) {
        NdVar OLane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, OLane, {OldDst, NdVar::cst(I * LaneSz, 4)});
        NdVar WOld = S.makeTemp(Wide), WMul = S.makeTemp(Wide);
        S.emit(NdOp::INT_SEXT, WOld, {OLane});
        S.emit(NdOp::INT_SEXT, WMul, {MulRes});
        NdVar Sum = S.makeTemp(Wide);
        S.emit(IsAccSub ? NdOp::INT_SUB : NdOp::INT_ADD, Sum, {WOld, WMul});
        NdVar GtMax = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, GtMax, {NdVar::cst(MaxV, Wide), Sum});
        NdVar C1 = S.makeTemp(Wide);
        S.emit(NdOp::SELECT, C1, {GtMax, NdVar::cst(MaxV, Wide), Sum});
        NdVar LtMin = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, LtMin,
               {C1, NdVar::cst(static_cast<uint64_t>(-(int64_t)MinV), Wide)});
        NdVar C2 = S.makeTemp(Wide);
        S.emit(NdOp::SELECT, C2,
               {LtMin, NdVar::cst(static_cast<uint64_t>(-(int64_t)MinV), Wide),
                C1});
        LaneRes = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, LaneRes, {C2, NdVar::cst(0, 4)});
      }
      if (I == 0) {
        Acc = LaneRes;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {LaneRes, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // NEON pairwise add long — adjacent pairs of narrow lanes summed into wider
  // lanes.
  case AARCH64_INS_SADDLP:
  case AARCH64_INS_UADDLP: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    bool IsSigned = (Insn->id == AARCH64_INS_SADDLP);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned NarrowSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      NarrowSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      NarrowSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_16B || SrcVas == AARCH64LAYOUT_VL_8B)
      NarrowSz = 1;
    if (NarrowSz == 0) {
      S.emit(NdOp::COPY, Dst, {Src});
      break;
    }
    unsigned WideSz = NarrowSz * 2;
    unsigned NPairs = Src.Size / (NarrowSz * 2);
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NPairs; ++I) {
      NdVar Lo = S.makeTemp(NarrowSz);
      NdVar Hi = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(I * 2 * NarrowSz, 4)});
      S.emit(NdOp::SUBBYTES, Hi,
             {Src, NdVar::cst(I * 2 * NarrowSz + NarrowSz, 4)});
      NdVar WLo = S.makeTemp(WideSz);
      NdVar WHi = S.makeTemp(WideSz);
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WLo, {Lo});
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WHi, {Hi});
      NdVar Sum = S.makeTemp(WideSz);
      S.emit(NdOp::INT_ADD, Sum, {WLo, WHi});
      if (I == 0) {
        Acc = Sum;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + WideSz);
        S.emit(NdOp::CONCAT, Next, {Sum, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // SADALP/UADALP — pairwise add long AND ACCUMULATE: Dst[j] += widen(Src[2j])
  // + widen(Src[2j+1]).  Same as SADDLP/UADDLP plus accumulation into the
  // existing destination lanes.  Was an intrinsic placeholder returning 0.
  case AARCH64_INS_SADALP:
  case AARCH64_INS_UADALP: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    bool IsSigned = (Insn->id == AARCH64_INS_SADALP);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned NarrowSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      NarrowSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      NarrowSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_16B || SrcVas == AARCH64LAYOUT_VL_8B)
      NarrowSz = 1;
    if (NarrowSz == 0) {
      S.emit(NdOp::COPY, Dst, {Src});
      break;
    }
    unsigned WideSz = NarrowSz * 2;
    unsigned NPairs = Src.Size / (NarrowSz * 2);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NPairs; ++I) {
      NdVar Lo = S.makeTemp(NarrowSz);
      NdVar Hi = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(I * 2 * NarrowSz, 4)});
      S.emit(NdOp::SUBBYTES, Hi,
             {Src, NdVar::cst(I * 2 * NarrowSz + NarrowSz, 4)});
      NdVar WLo = S.makeTemp(WideSz);
      NdVar WHi = S.makeTemp(WideSz);
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WLo, {Lo});
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WHi, {Hi});
      NdVar Sum = S.makeTemp(WideSz);
      S.emit(NdOp::INT_ADD, Sum, {WLo, WHi});
      NdVar OldLane = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, OldLane, {OldDst, NdVar::cst(I * WideSz, 4)});
      NdVar Acc2 = S.makeTemp(WideSz);
      S.emit(NdOp::INT_ADD, Acc2, {Sum, OldLane});
      if (I == 0) {
        Acc = Acc2;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + WideSz);
        S.emit(NdOp::CONCAT, Next, {Acc2, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  default:
    return false;
  }
  return true;
}

} // namespace neverd
