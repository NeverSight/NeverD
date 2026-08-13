//===- X86LiftSIMDConvert.cpp - x86/x64 SIMD pack, unpack and convert lifter
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSE/AVX width-changing instructions: integer/float packed
/// conversions, lane interleaving (unpack), saturating pack to a
/// narrower lane, and the zero/sign-extending lane widenings.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftSIMDConvert(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                     const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // SSE int<->float conversions, additional variants.
  case X86_INS_CVTDQ2PS:
  case X86_INS_CVTDQ2PD:
  case X86_INS_CVTPI2PS:
  case X86_INS_CVTPI2PD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    bool IsPD = (InsnId == X86_INS_CVTDQ2PD || InsnId == X86_INS_CVTPI2PD);
    unsigned IntSz = 4;
    unsigned FPSz = IsPD ? 8 : 4;
    unsigned NLanes = Src.Size / IntSz;
    if (NLanes < 2 || NLanes > 4) {
      S.emit(NdOp::FLOAT_INT2FLOAT, Dst, {Src});
      break;
    }
    if (IsPD && NLanes > 2)
      NLanes = 2;
    std::vector<NdVar> Lanes(NLanes);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Elem = S.makeTemp(IntSz);
      S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(I * IntSz, 4)});
      Lanes[I] = S.makeTemp(FPSz);
      S.emit(NdOp::FLOAT_INT2FLOAT, Lanes[I], {Elem});
    }
    if (NLanes == 2) {
      unsigned PairSz = FPSz * 2;
      NdVar Pair = S.makeTemp(PairSz);
      S.emit(NdOp::CONCAT, Pair, {Lanes[1], Lanes[0]});
      if (Dst.Size == PairSz) {
        S.emit(NdOp::COPY, Dst, {Pair});
      } else {
        NdVar ZHi = S.makeTemp(Dst.Size - PairSz);
        S.emit(NdOp::COPY, ZHi, {NdVar::cst(0, (uint16_t)(Dst.Size - PairSz))});
        S.emit(NdOp::CONCAT, Dst, {ZHi, Pair});
      }
    } else {
      NdVar Lo = S.makeTemp(FPSz * 2);
      S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
      NdVar Hi = S.makeTemp(FPSz * 2);
      S.emit(NdOp::CONCAT, Hi, {Lanes[3], Lanes[2]});
      S.emit(NdOp::CONCAT, Dst, {Hi, Lo});
    }
    break;
  }
  case X86_INS_CVTPS2DQ:
  case X86_INS_CVTPD2DQ:
  case X86_INS_CVTPS2PI:
  case X86_INS_CVTPD2PI:
  case X86_INS_CVTTPS2DQ:
  case X86_INS_CVTTPD2DQ:
  case X86_INS_CVTTPS2PI:
  case X86_INS_CVTTPD2PI: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    bool IsPD = (InsnId == X86_INS_CVTPD2DQ || InsnId == X86_INS_CVTTPD2DQ ||
                 InsnId == X86_INS_CVTPD2PI || InsnId == X86_INS_CVTTPD2PI);
    // The non-T variants round using MXCSR (default: nearest, ties to even);
    // only the CVTT* variants truncate toward zero.
    bool IsTrunc =
        (InsnId == X86_INS_CVTTPS2DQ || InsnId == X86_INS_CVTTPD2DQ ||
         InsnId == X86_INS_CVTTPS2PI || InsnId == X86_INS_CVTTPD2PI);
    unsigned FPSz = IsPD ? 8 : 4;
    unsigned DstElemSz = 4;
    unsigned NLanes = Src.Size / FPSz;
    unsigned DstLanes = Dst.Size / DstElemSz;
    if (NLanes < 2 || NLanes > 4) {
      S.emit(NdOp::FLOAT_TRUNC, Dst, {Src});
      break;
    }
    if (NLanes > DstLanes)
      NLanes = DstLanes;
    std::vector<NdVar> Lanes(NLanes);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Elem = S.makeTemp(FPSz);
      S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(I * FPSz, 4)});
      Lanes[I] = S.makeTemp(DstElemSz);
      if (IsTrunc) {
        S.emit(NdOp::FLOAT_TRUNC, Lanes[I], {Elem});
      } else {
        NdVar Rnd = S.makeTemp(FPSz);
        S.emit(NdOp::FLOAT_ROUNDEVEN, Rnd, {Elem});
        S.emit(NdOp::FLOAT_FLOAT2INT, Lanes[I], {Rnd});
      }
    }
    if (NLanes == 2) {
      if (Dst.Size == 8) {
        S.emit(NdOp::CONCAT, Dst, {Lanes[1], Lanes[0]});
      } else {
        NdVar Lo = S.makeTemp(8);
        S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
        NdVar ZHi = S.makeTemp(8);
        S.emit(NdOp::COPY, ZHi, {NdVar::cst(0, 8)});
        S.emit(NdOp::CONCAT, Dst, {ZHi, Lo});
      }
    } else {
      NdVar Lo = S.makeTemp(8);
      S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
      NdVar Hi = S.makeTemp(8);
      S.emit(NdOp::CONCAT, Hi, {Lanes[3], Lanes[2]});
      S.emit(NdOp::CONCAT, Dst, {Hi, Lo});
    }
    break;
  }
  // Packed FP width conversions (scalar CVTSS2SD/CVTSD2SS are handled in
  // liftCore, which runs first).  Each lane must carry its true FP width so
  // the emitter picks fpext/fptrunc correctly.
  case X86_INS_CVTPS2PD: {
    // Low two single-precision lanes -> two double-precision lanes.
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    std::vector<NdVar> Lanes(2);
    for (unsigned I = 0; I < 2; ++I) {
      NdVar Elem = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(I * 4, 4)});
      Lanes[I] = S.makeTemp(8);
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Lanes[I], {Elem});
    }
    S.emit(NdOp::CONCAT, Dst, {Lanes[1], Lanes[0]});
    break;
  }
  case X86_INS_CVTPD2PS: {
    // Two double-precision lanes -> two single-precision lanes (low 64 bits);
    // the upper 64 bits of the destination are zeroed.
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    std::vector<NdVar> Lanes(2);
    for (unsigned I = 0; I < 2; ++I) {
      NdVar Elem = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(I * 8, 4)});
      Lanes[I] = S.makeTemp(4);
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Lanes[I], {Elem});
    }
    NdVar Lo = S.makeTemp(8);
    S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
    if (Dst.Size > 8) {
      NdVar ZHi = S.makeTemp(Dst.Size - 8);
      S.emit(NdOp::COPY, ZHi, {NdVar::cst(0, (uint16_t)(Dst.Size - 8))});
      S.emit(NdOp::CONCAT, Dst, {ZHi, Lo});
    } else {
      S.emit(NdOp::COPY, Dst, {Lo});
    }
    break;
  }

  // VEX 3-operand unpacks
  case X86_INS_VPUNPCKLBW: {
    if (X86.op_count < 3)
      break;
    NdVar D = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpcklbw, D, {A, B});
    break;
  }
  case X86_INS_VPUNPCKHBW: {
    if (X86.op_count < 3)
      break;
    NdVar D = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpckhbw, D, {A, B});
    break;
  }
  case X86_INS_VPUNPCKLWD: {
    if (X86.op_count < 3)
      break;
    NdVar D = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpcklwd, D, {A, B});
    break;
  }
  case X86_INS_VPUNPCKHWD: {
    if (X86.op_count < 3)
      break;
    NdVar D = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpckhwd, D, {A, B});
    break;
  }
  case X86_INS_VPUNPCKLDQ: {
    if (X86.op_count < 3)
      break;
    NdVar D = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpckldq, D, {A, B});
    break;
  }
  case X86_INS_VPUNPCKHDQ: {
    if (X86.op_count < 3)
      break;
    NdVar D = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpckhdq, D, {A, B});
    break;
  }
  case X86_INS_VPUNPCKLQDQ: {
    if (X86.op_count < 3)
      break;
    NdVar D = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpcklqdq, D, {A, B});
    break;
  }
  case X86_INS_VPUNPCKHQDQ: {
    if (X86.op_count < 3)
      break;
    NdVar D = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpckhqdq, D, {A, B});
    break;
  }

  // PMOVZX/PMOVSX — packed move with per-element zero/sign extension.
  // Each of the N low source elements (byte/word/dword) is independently
  // extended to the wider destination element.  A single whole-register
  // INT_ZEXT/INT_SEXT is wrong: it would treat the packed source as one
  // scalar and only fill lane 0 (and corrupt the rest).
  case X86_INS_PMOVZXBW:
  case X86_INS_PMOVZXBD:
  case X86_INS_PMOVZXBQ:
  case X86_INS_PMOVZXWD:
  case X86_INS_PMOVZXWQ:
  case X86_INS_PMOVZXDQ:
  case X86_INS_VPMOVZXBW:
  case X86_INS_VPMOVZXBD:
  case X86_INS_VPMOVZXBQ:
  case X86_INS_VPMOVZXWD:
  case X86_INS_VPMOVZXWQ:
  case X86_INS_VPMOVZXDQ:
  case X86_INS_PMOVSXBW:
  case X86_INS_PMOVSXBD:
  case X86_INS_PMOVSXBQ:
  case X86_INS_PMOVSXWD:
  case X86_INS_PMOVSXWQ:
  case X86_INS_PMOVSXDQ:
  case X86_INS_VPMOVSXBW:
  case X86_INS_VPMOVSXBD:
  case X86_INS_VPMOVSXBQ:
  case X86_INS_VPMOVSXWD:
  case X86_INS_VPMOVSXWQ:
  case X86_INS_VPMOVSXDQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint16_t SrcElemSz = 1, DstElemSz = 2;
    bool IsSigned = false;
    switch (InsnId) {
    case X86_INS_PMOVZXBW:
    case X86_INS_VPMOVZXBW:
      SrcElemSz = 1;
      DstElemSz = 2;
      break;
    case X86_INS_PMOVZXBD:
    case X86_INS_VPMOVZXBD:
      SrcElemSz = 1;
      DstElemSz = 4;
      break;
    case X86_INS_PMOVZXBQ:
    case X86_INS_VPMOVZXBQ:
      SrcElemSz = 1;
      DstElemSz = 8;
      break;
    case X86_INS_PMOVZXWD:
    case X86_INS_VPMOVZXWD:
      SrcElemSz = 2;
      DstElemSz = 4;
      break;
    case X86_INS_PMOVZXWQ:
    case X86_INS_VPMOVZXWQ:
      SrcElemSz = 2;
      DstElemSz = 8;
      break;
    case X86_INS_PMOVZXDQ:
    case X86_INS_VPMOVZXDQ:
      SrcElemSz = 4;
      DstElemSz = 8;
      break;
    case X86_INS_PMOVSXBW:
    case X86_INS_VPMOVSXBW:
      SrcElemSz = 1;
      DstElemSz = 2;
      IsSigned = true;
      break;
    case X86_INS_PMOVSXBD:
    case X86_INS_VPMOVSXBD:
      SrcElemSz = 1;
      DstElemSz = 4;
      IsSigned = true;
      break;
    case X86_INS_PMOVSXBQ:
    case X86_INS_VPMOVSXBQ:
      SrcElemSz = 1;
      DstElemSz = 8;
      IsSigned = true;
      break;
    case X86_INS_PMOVSXWD:
    case X86_INS_VPMOVSXWD:
      SrcElemSz = 2;
      DstElemSz = 4;
      IsSigned = true;
      break;
    case X86_INS_PMOVSXWQ:
    case X86_INS_VPMOVSXWQ:
      SrcElemSz = 2;
      DstElemSz = 8;
      IsSigned = true;
      break;
    case X86_INS_PMOVSXDQ:
    case X86_INS_VPMOVSXDQ:
      SrcElemSz = 4;
      DstElemSz = 8;
      IsSigned = true;
      break;
    default:
      break;
    }
    unsigned NLanes = DstElemSz ? (Dst.Size / DstElemSz) : 0;
    if (NLanes == 0) {
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Dst, {Src});
      break;
    }
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar El = S.makeTemp(SrcElemSz);
      S.emit(NdOp::SUBBYTES, El, {Src, NdVar::cst(I * SrcElemSz, 4)});
      NdVar Ext = S.makeTemp(DstElemSz);
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Ext, {El});
      if (I == 0) {
        Acc = Ext;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + DstElemSz);
        S.emit(NdOp::CONCAT, Next, {Ext, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  // Packed convert/pack — per-lane saturating narrow.
  case X86_INS_PACKUSWB:
  case X86_INS_PACKUSDW:
  case X86_INS_PACKSSWB:
  case X86_INS_PACKSSDW:
  case X86_INS_VPACKUSWB:
  case X86_INS_VPACKUSDW:
  case X86_INS_VPACKSSWB:
  case X86_INS_VPACKSSDW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar DstR = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                     : L.operandRead(S, X86.operands[0]);
    NdVar Src2 = L.operandRead(S, X86.operands[X86.op_count - 1]);

    bool IsSigned =
        (InsnId == X86_INS_PACKSSWB || InsnId == X86_INS_PACKSSDW ||
         InsnId == X86_INS_VPACKSSWB || InsnId == X86_INS_VPACKSSDW);
    bool IsWord = (InsnId == X86_INS_PACKSSWB || InsnId == X86_INS_PACKUSWB ||
                   InsnId == X86_INS_VPACKSSWB || InsnId == X86_INS_VPACKUSWB);
    unsigned SrcLaneSz = IsWord ? 2 : 4;
    unsigned DstLaneSz = IsWord ? 1 : 2;
    // PACK operates within each 128-bit lane: the lane's low half comes from
    // src1's lane, the high half from src2's lane.  The 256-bit (VEX.256) form
    // therefore interleaves the two operands PER 128-bit LANE
    // (dst = [pack(s1.lane0), pack(s2.lane0), pack(s1.lane1), pack(s2.lane1)]),
    // NOT as one 256-bit-wide pack — emitting all of src1 then all of src2 (the
    // naive extension) mislays the high lane.  MMX (64-bit dst) is a single
    // 8-byte lane; XMM one 16-byte lane; YMM two 16-byte lanes.
    unsigned LaneBytes = (Dst.Size >= 16) ? 16u : Dst.Size;
    unsigned NLanes128 = Dst.Size / LaneBytes; // MMX:1, XMM:1, YMM:2
    unsigned SrcElemsPerLane = LaneBytes / SrcLaneSz;

    auto ClampLane = [&](NdVar Src, unsigned Off) -> NdVar {
      NdVar Lane = S.makeTemp(SrcLaneSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(Off, 4)});

      // Narrowing saturate: trunc to DstLaneSz, sext back, compare with
      // original.  If equal the value fits; otherwise pick 127 / -128 (signed)
      // or 255 / 0 (unsigned) based on the sign of the original.  This avoids
      // both the fork's InstCombine crash on @llvm.smax/@llvm.smin chains AND
      // the constant-folding mis-compute on INT_SLESS+SELECT clamp patterns.
      NdVar Narrow = S.makeTemp(DstLaneSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Lane, NdVar::cst(0, 4)});
      NdVar BackWide = S.makeTemp(SrcLaneSz);
      if (IsSigned)
        S.emit(NdOp::INT_SEXT, BackWide, {Narrow});
      else
        S.emit(NdOp::INT_ZEXT, BackWide, {Narrow});
      NdVar Fits = S.makeTemp(1);
      S.emit(NdOp::INT_EQUAL, Fits, {Lane, BackWide});
      NdVar IsPos = S.makeTemp(1);
      int64_t Hi = IsSigned ? (1LL << (DstLaneSz * 8 - 1)) - 1
                            : (1LL << (DstLaneSz * 8)) - 1;
      int64_t Lo = IsSigned ? -(1LL << (DstLaneSz * 8 - 1)) : 0;
      if (IsSigned) {
        S.emit(NdOp::INT_SLESS, IsPos, {NdVar::cst(0, SrcLaneSz), Lane});
      } else {
        S.emit(NdOp::INT_SLESS, IsPos,
               {NdVar::cst(static_cast<uint64_t>(Hi), SrcLaneSz), Lane});
      }
      NdVar OverflowVal = S.makeTemp(DstLaneSz);
      NdVar HiNarrow = NdVar::cst(static_cast<uint64_t>(Hi), DstLaneSz);
      NdVar LoNarrow = NdVar::cst(static_cast<uint64_t>(Lo), DstLaneSz);
      S.emit(NdOp::SELECT, OverflowVal, {IsPos, HiNarrow, LoNarrow});
      NdVar Result = S.makeTemp(DstLaneSz);
      S.emit(NdOp::SELECT, Result, {Fits, Narrow, OverflowVal});
      return Result;
    };

    // Build the result low-to-high.  For each 128-bit lane L, first the
    // SrcElemsPerLane clamped elements of src1's lane L, then src2's lane L.
    // CONCAT(hi, lo) prepends `hi` above the accumulator, so iterating elements
    // in increasing significance yields the correct little-endian layout.
    bool First = true;
    NdVar Acc = S.makeTemp(0);
    auto AppendLane = [&](NdVar Src, unsigned Lane) {
      unsigned Base = Lane * LaneBytes; // byte offset of this lane in Src
      for (unsigned E = 0; E < SrcElemsPerLane; ++E) {
        NdVar B = ClampLane(Src, Base + E * SrcLaneSz);
        if (First) {
          Acc = B;
          First = false;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {B, Acc});
          Acc = Next;
        }
      }
    };
    for (unsigned L = 0; L < NLanes128; ++L) {
      AppendLane(DstR, L); // src1 lane L -> low half of dst lane L
      AppendLane(Src2, L); // src2 lane L -> high half of dst lane L
    }
    if (Acc.Size < Dst.Size) {
      NdVar Wide = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, Wide, {Acc});
      S.emit(NdOp::COPY, Dst, {Wide});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
