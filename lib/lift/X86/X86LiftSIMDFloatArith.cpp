//===- X86LiftSIMDFloatArith.cpp - x86/x64 SIMD floating-point arithmetic lifter
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSE/AVX packed and scalar floating-point arithmetic:
/// add/subtract/multiply/divide, horizontal add/subtract, the
/// alternating add-subtract forms, minimum/maximum, and the
/// square-root and reciprocal approximations.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftSIMDFloatArith(X86Lifter &L, X86Lifter::LiftState &S,
                        const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // HADDPS/HADDPD/HSUBPS/HSUBPD — horizontal pair-wise add/sub.
  // HADDPS: Dst[0]=A[0]+A[1], Dst[1]=A[2]+A[3], Dst[2]=B[0]+B[1],
  // Dst[3]=B[2]+B[3] HADDPD: Dst[0]=A[0]+A[1], Dst[1]=B[0]+B[1]
  case X86_INS_VHADDPS:
  case X86_INS_VHSUBPS:
  case X86_INS_HADDPS:
  case X86_INS_HSUBPS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    bool IsSub = (InsnId == X86_INS_VHSUBPS || InsnId == X86_INS_HSUBPS);
    NdOp Opc = IsSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD;
    // Horizontal pair-add/sub is per-128-bit lane: each result lane's low 64
    // bits are src1's two pair reductions, its high 64 bits src2's (for the
    // SAME lane).  The old code only read the low 128 bits, so the 256-bit ymm
    // (VEX) form computed garbage for the high lane.  Build lane-by-lane.
    unsigned NumLanes = Dst.Size >= 16 ? Dst.Size / 16 : 1;
    auto laneF = [&](NdVar Src, unsigned Off) -> NdVar {
      NdVar E = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, E, {Src, NdVar::cst(Off, 4)});
      return E;
    };
    NdVar Full = S.makeTemp(0);
    for (unsigned L = 0; L < NumLanes; ++L) {
      unsigned Base = L * 16;
      NdVar R0 = S.makeTemp(4), R1 = S.makeTemp(4), R2 = S.makeTemp(4),
            R3 = S.makeTemp(4);
      S.emit(Opc, R0, {laneF(A, Base + 0), laneF(A, Base + 4)});
      S.emit(Opc, R1, {laneF(A, Base + 8), laneF(A, Base + 12)});
      S.emit(Opc, R2, {laneF(B, Base + 0), laneF(B, Base + 4)});
      S.emit(Opc, R3, {laneF(B, Base + 8), laneF(B, Base + 12)});
      NdVar Lo = S.makeTemp(8), Hi = S.makeTemp(8), Lane = S.makeTemp(16);
      S.emit(NdOp::CONCAT, Lo, {R1, R0});
      S.emit(NdOp::CONCAT, Hi, {R3, R2});
      S.emit(NdOp::CONCAT, Lane, {Hi, Lo});
      if (L == 0) {
        Full = Lane;
      } else {
        NdVar Next = S.makeTemp((L + 1) * 16);
        S.emit(NdOp::CONCAT, Next, {Lane, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }
  case X86_INS_VHADDPD:
  case X86_INS_VHSUBPD:
  case X86_INS_HADDPD:
  case X86_INS_HSUBPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    bool IsSub = (InsnId == X86_INS_VHSUBPD || InsnId == X86_INS_HSUBPD);
    NdOp Opc = IsSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD;
    // Per-128-bit lane (each lane holds two doubles): result lane low = src1's
    // pair sum, high = src2's pair sum.  The old code handled only the low 128
    // bits; rebuild lane-by-lane so the 256-bit ymm form is correct.
    unsigned NumLanes = Dst.Size >= 16 ? Dst.Size / 16 : 1;
    auto laneD = [&](NdVar Src, unsigned Off) -> NdVar {
      NdVar E = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, E, {Src, NdVar::cst(Off, 4)});
      return E;
    };
    NdVar Full = S.makeTemp(0);
    for (unsigned L = 0; L < NumLanes; ++L) {
      unsigned Base = L * 16;
      NdVar R0 = S.makeTemp(8), R1 = S.makeTemp(8), Lane = S.makeTemp(16);
      S.emit(Opc, R0, {laneD(A, Base + 0), laneD(A, Base + 8)});
      S.emit(Opc, R1, {laneD(B, Base + 0), laneD(B, Base + 8)});
      S.emit(NdOp::CONCAT, Lane, {R1, R0});
      if (L == 0) {
        Full = Lane;
      } else {
        NdVar Next = S.makeTemp((L + 1) * 16);
        S.emit(NdOp::CONCAT, Next, {Lane, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // ADDSUBPS/ADDSUBPD — alternating sub/add per lane.
  // PS: Dst[0]=A[0]-B[0], Dst[1]=A[1]+B[1], Dst[2]=A[2]-B[2], Dst[3]=A[3]+B[3]
  // PD: Dst[0]=A[0]-B[0], Dst[1]=A[1]+B[1]
  case X86_INS_VADDSUBPS:
  case X86_INS_ADDSUBPS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    // Even single lanes subtract, odd lanes add — a uniform per-element pattern
    // across the whole register.  The old code only did the low 4 floats, so
    // the 256-bit ymm (VEX) form left the high 128 bits uncomputed.
    unsigned NElems = Dst.Size / 4;
    NdVar Full = S.makeTemp(0);
    for (unsigned I = 0; I < NElems; ++I) {
      NdVar Ae = S.makeTemp(4), Be = S.makeTemp(4), R = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Ae, {A, NdVar::cst(I * 4, 4)});
      S.emit(NdOp::SUBBYTES, Be, {B, NdVar::cst(I * 4, 4)});
      S.emit((I & 1) ? NdOp::FLOAT_ADD : NdOp::FLOAT_SUB, R, {Ae, Be});
      if (I == 0) {
        Full = R;
      } else {
        NdVar Next = S.makeTemp((I + 1) * 4);
        S.emit(NdOp::CONCAT, Next, {R, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }
  case X86_INS_VADDSUBPD:
  case X86_INS_ADDSUBPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    // Even double lanes subtract, odd lanes add; uniform per-element across the
    // whole register (the old code only did the low 2 doubles → ymm truncated).
    unsigned NElems = Dst.Size / 8;
    NdVar Full = S.makeTemp(0);
    for (unsigned I = 0; I < NElems; ++I) {
      NdVar Ae = S.makeTemp(8), Be = S.makeTemp(8), R = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Ae, {A, NdVar::cst(I * 8, 4)});
      S.emit(NdOp::SUBBYTES, Be, {B, NdVar::cst(I * 8, 4)});
      S.emit((I & 1) ? NdOp::FLOAT_ADD : NdOp::FLOAT_SUB, R, {Ae, Be});
      if (I == 0) {
        Full = R;
      } else {
        NdVar Next = S.makeTemp((I + 1) * 8);
        S.emit(NdOp::CONCAT, Next, {R, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // SQRTSS/SQRTSD/SQRTPS/SQRTPD — float square root.
  case X86_INS_SQRTSS:
  case X86_INS_SQRTSD:
  case X86_INS_SQRTPS:
  case X86_INS_SQRTPD:
  case X86_INS_RSQRTSS:
  case X86_INS_RSQRTPS:
  case X86_INS_RCPSS:
  case X86_INS_RCPPS:
  case X86_INS_VSQRTSS:
  case X86_INS_VSQRTSD:
  case X86_INS_VSQRTPS:
  case X86_INS_VSQRTPD:
  case X86_INS_VRSQRTSS:
  case X86_INS_VRSQRTPS:
  case X86_INS_VRCPSS:
  case X86_INS_VRCPPS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);

    bool IsPacked = false;
    unsigned LaneSz = 0;
    switch (InsnId) {
    case X86_INS_SQRTPS:
    case X86_INS_VSQRTPS:
    case X86_INS_RSQRTPS:
    case X86_INS_VRSQRTPS:
    case X86_INS_RCPPS:
    case X86_INS_VRCPPS:
      IsPacked = true;
      LaneSz = 4;
      break;
    case X86_INS_SQRTPD:
    case X86_INS_VSQRTPD:
      IsPacked = true;
      LaneSz = 8;
      break;
    default:
      break;
    }

    bool IsRcp = (InsnId == X86_INS_RCPPS || InsnId == X86_INS_RCPSS ||
                  InsnId == X86_INS_VRCPPS || InsnId == X86_INS_VRCPSS);
    bool IsRsqrt = (InsnId == X86_INS_RSQRTPS || InsnId == X86_INS_RSQRTSS ||
                    InsnId == X86_INS_VRSQRTPS || InsnId == X86_INS_VRSQRTSS);

    auto emitLaneOp = [&](NdVar In, NdVar Out) {
      if (IsRcp) {
        NdVar One = NdVar::cst(0x3F800000, In.Size); // 1.0f
        S.emit(NdOp::FLOAT_DIV, Out, {One, In});
      } else if (IsRsqrt) {
        NdVar Sq = S.makeTemp(In.Size);
        S.emit(NdOp::FLOAT_SQRT, Sq, {In});
        NdVar One = NdVar::cst(0x3F800000, In.Size); // 1.0f
        S.emit(NdOp::FLOAT_DIV, Out, {One, Sq});
      } else {
        S.emit(NdOp::FLOAT_SQRT, Out, {In});
      }
    };

    if (IsPacked && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Res = S.makeTemp(LaneSz);
        emitLaneOp(Lane, Res);
        if (I == 0) {
          Acc = Res;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Res, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else if (Dst.Size > 8) {
      // Scalar form (SQRTSS/SD, RSQRTSS, RCPSS): operate on the low element
      // only and keep the upper lanes.  Extracting the scalar — instead of
      // handing the whole 16B XMM to the float emitter, which infers double
      // from the width — is what makes a *SS op single-precision (else SQRTSS
      // became sqrt.f64).
      unsigned ScalarSz =
          (InsnId == X86_INS_SQRTSD || InsnId == X86_INS_VSQRTSD) ? 8 : 4;
      NdVar In = S.makeTemp(ScalarSz);
      S.emit(NdOp::SUBBYTES, In, {Src, NdVar::cst(0, 4)});
      NdVar Res = S.makeTemp(ScalarSz);
      emitLaneOp(In, Res);
      // VEX scalar takes the upper lanes from src1 (operand 1); SSE keeps
      // Dst's.
      NdVar Upper =
          (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1]) : Dst;
      NdVar Hi = S.makeTemp(Dst.Size - ScalarSz);
      S.emit(NdOp::SUBBYTES, Hi, {Upper, NdVar::cst(ScalarSz, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Res});
    } else {
      emitLaneOp(Src, Dst);
    }
    break;
  }

  // Packed min/max — float: per-lane FLOAT_LESS + SELECT
  case X86_INS_MINSS:
  case X86_INS_MINSD:
  case X86_INS_MINPS:
  case X86_INS_MINPD:
  case X86_INS_MAXSS:
  case X86_INS_MAXSD:
  case X86_INS_MAXPS:
  case X86_INS_MAXPD:
  case X86_INS_VMINSS:
  case X86_INS_VMINSD:
  case X86_INS_VMINPS:
  case X86_INS_VMINPD:
  case X86_INS_VMAXSS:
  case X86_INS_VMAXSD:
  case X86_INS_VMAXPS:
  case X86_INS_VMAXPD: {
    if (X86.op_count < 2)
      break;
    bool IsVEX = (X86.op_count == 3);
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = IsVEX ? L.operandRead(S, X86.operands[1])
                    : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);

    bool IsMin = (InsnId == X86_INS_MINSS || InsnId == X86_INS_MINSD ||
                  InsnId == X86_INS_MINPS || InsnId == X86_INS_MINPD ||
                  InsnId == X86_INS_VMINSS || InsnId == X86_INS_VMINSD ||
                  InsnId == X86_INS_VMINPS || InsnId == X86_INS_VMINPD);

    bool IsScalar = (InsnId == X86_INS_MINSS || InsnId == X86_INS_MAXSS ||
                     InsnId == X86_INS_VMINSS || InsnId == X86_INS_VMAXSS ||
                     InsnId == X86_INS_MINSD || InsnId == X86_INS_MAXSD ||
                     InsnId == X86_INS_VMINSD || InsnId == X86_INS_VMAXSD);

    bool IsDouble = (InsnId == X86_INS_MINSD || InsnId == X86_INS_MAXSD ||
                     InsnId == X86_INS_MINPD || InsnId == X86_INS_MAXPD ||
                     InsnId == X86_INS_VMINSD || InsnId == X86_INS_VMAXSD ||
                     InsnId == X86_INS_VMINPD || InsnId == X86_INS_VMAXPD);

    unsigned LaneSz = IsDouble ? 8 : 4;
    unsigned NLanes = IsScalar ? 1 : (Dst.Size / LaneSz);

    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar La = S.makeTemp(LaneSz);
      NdVar Lb = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
      S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
      NdVar Cmp = S.makeTemp(1);
      if (IsMin)
        S.emit(NdOp::FLOAT_LESS, Cmp, {La, Lb});
      else
        S.emit(NdOp::FLOAT_LESS, Cmp, {Lb, La});
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

    if (IsScalar && Dst.Size > LaneSz) {
      NdVar Upper = S.makeTemp(Dst.Size - LaneSz);
      S.emit(NdOp::SUBBYTES, Upper, {A, NdVar::cst(LaneSz, 4)});
      NdVar Full = S.makeTemp(Dst.Size);
      S.emit(NdOp::CONCAT, Full, {Upper, Acc});
      S.emit(NdOp::COPY, Dst, {Full});
    } else if (Acc.Size < Dst.Size) {
      NdVar Wide = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, Wide, {Acc});
      S.emit(NdOp::COPY, Dst, {Wide});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }

  // AVX float arithmetic
  case X86_INS_VADDSS:
  case X86_INS_VADDSD:
  case X86_INS_VADDPS:
  case X86_INS_VADDPD:
  case X86_INS_VSUBSS:
  case X86_INS_VSUBSD:
  case X86_INS_VSUBPS:
  case X86_INS_VSUBPD:
  case X86_INS_VMULSS:
  case X86_INS_VMULSD:
  case X86_INS_VMULPS:
  case X86_INS_VMULPD:
  case X86_INS_VDIVSS:
  case X86_INS_VDIVSD:
  case X86_INS_VDIVPS:
  case X86_INS_VDIVPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    NdOp Opc;
    switch (InsnId) {
    case X86_INS_VADDSS:
    case X86_INS_VADDSD:
    case X86_INS_VADDPS:
    case X86_INS_VADDPD:
      Opc = NdOp::FLOAT_ADD;
      break;
    case X86_INS_VSUBSS:
    case X86_INS_VSUBSD:
    case X86_INS_VSUBPS:
    case X86_INS_VSUBPD:
      Opc = NdOp::FLOAT_SUB;
      break;
    case X86_INS_VMULSS:
    case X86_INS_VMULSD:
    case X86_INS_VMULPS:
    case X86_INS_VMULPD:
      Opc = NdOp::FLOAT_MULT;
      break;
    default:
      Opc = NdOp::FLOAT_DIV;
    }
    bool IsPacked = (InsnId == X86_INS_VADDPS || InsnId == X86_INS_VADDPD ||
                     InsnId == X86_INS_VSUBPS || InsnId == X86_INS_VSUBPD ||
                     InsnId == X86_INS_VMULPS || InsnId == X86_INS_VMULPD ||
                     InsnId == X86_INS_VDIVPS || InsnId == X86_INS_VDIVPD);
    if (IsPacked && Dst.Size >= 16) {
      bool IsPD = (InsnId == X86_INS_VADDPD || InsnId == X86_INS_VSUBPD ||
                   InsnId == X86_INS_VMULPD || InsnId == X86_INS_VDIVPD);
      unsigned ElemSz = IsPD ? 8 : 4;
      unsigned NLanes = Dst.Size / ElemSz;
      std::vector<NdVar> Lanes(NLanes);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar LA = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, LA, {A, NdVar::cst(I * ElemSz, 4)});
        NdVar LB = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, LB, {B, NdVar::cst(I * ElemSz, 4)});
        Lanes[I] = S.makeTemp(ElemSz);
        S.emit(Opc, Lanes[I], {LA, LB});
      }
      if (NLanes == 2) {
        S.emit(NdOp::CONCAT, Dst, {Lanes[1], Lanes[0]});
      } else {
        NdVar Lo = S.makeTemp(ElemSz * 2);
        S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
        NdVar Hi = S.makeTemp(ElemSz * 2);
        S.emit(NdOp::CONCAT, Hi, {Lanes[3], Lanes[2]});
        S.emit(NdOp::CONCAT, Dst, {Hi, Lo});
      }
    } else {
      bool IsSS = (InsnId == X86_INS_VADDSS || InsnId == X86_INS_VSUBSS ||
                   InsnId == X86_INS_VMULSS || InsnId == X86_INS_VDIVSS);
      bool IsSD = (InsnId == X86_INS_VADDSD || InsnId == X86_INS_VSUBSD ||
                   InsnId == X86_INS_VMULSD || InsnId == X86_INS_VDIVSD);
      if ((IsSS || IsSD) && Dst.Size > 8) {
        unsigned ScalarSz = IsSS ? 4 : 8;
        NdVar SA = S.makeTemp(ScalarSz);
        S.emit(NdOp::SUBBYTES, SA, {A, NdVar::cst(0, 4)});
        NdVar SB = S.makeTemp(ScalarSz);
        S.emit(NdOp::SUBBYTES, SB, {B, NdVar::cst(0, 4)});
        NdVar Res = S.makeTemp(ScalarSz);
        S.emit(Opc, Res, {SA, SB});
        unsigned HiSz = Dst.Size - ScalarSz;
        NdVar Hi = S.makeTemp(HiSz);
        S.emit(NdOp::SUBBYTES, Hi, {A, NdVar::cst(ScalarSz, 4)});
        S.emit(NdOp::CONCAT, Dst, {Hi, Res});
      } else {
        S.emit(Opc, Dst, {A, B});
      }
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
