//===- AArch64LiftNEONFloat.cpp - NEON floating-point ops -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// FP width conversion (FCVTN/FCVTL), IEEE minNum/maxNum
/// (FMAXNM/FMINNM), pairwise FP min/max (FMAXP/FMINP/FMAXNMP/
/// FMINNMP) and the reciprocal estimate/step family (FRECPE/
/// FRSQRTE/URECPE/URSQRTE/FRECPS/FRSQRTS/FRECPX).
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftNEONFloat(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                   const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // NEON FP width convert: FCVTL/FCVTL2 widen single->double, FCVTN/FCVTN2
  // narrow double->single.  Must be per-lane with the real per-lane source
  // width, otherwise the emitter infers the type from the full 16-byte vector
  // and picks the wrong direction (a widen becomes an fptrunc).
  case AARCH64_INS_FCVTN:
  case AARCH64_INS_FCVTN2:
  case AARCH64_INS_FCVTL:
  case AARCH64_INS_FCVTL2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    bool Widen =
        (Insn->id == AARCH64_INS_FCVTL || Insn->id == AARCH64_INS_FCVTL2);
    bool Hi =
        (Insn->id == AARCH64_INS_FCVTL2 || Insn->id == AARCH64_INS_FCVTN2);
    unsigned DstLane = neonElemSize(ARM64.operands[0].vas);
    unsigned SrcLane = neonElemSize(ARM64.operands[1].vas);
    // Supported conversions: half<->float and float<->double (per-lane).
    bool ValidWiden = Widen && ((SrcLane == 4 && DstLane == 8) ||
                                (SrcLane == 2 && DstLane == 4));
    bool ValidNarrow = !Widen && ((SrcLane == 8 && DstLane == 4) ||
                                  (SrcLane == 4 && DstLane == 2));
    if (!ValidWiden && !ValidNarrow) {
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Dst, {Src});
      break;
    }
    if (Widen) {
      unsigned NLanes = Dst.Size / DstLane;
      unsigned SrcBase = Hi ? NLanes * SrcLane : 0;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar E = S.makeTemp(SrcLane);
        S.emit(NdOp::SUBBYTES, E, {Src, NdVar::cst(SrcBase + I * SrcLane, 4)});
        NdVar L = S.makeTemp(DstLane);
        S.emit(NdOp::FLOAT_FLOAT2FLOAT, L, {E});
        if (I == 0) {
          Acc = L;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLane);
          S.emit(NdOp::CONCAT, Next, {L, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      // Narrow: N wide lanes -> N narrow lanes per-lane.
      // FCVTN zeroes upper half, FCVTN2 writes upper half preserving lower.
      unsigned NLanes = Src.Size / SrcLane;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar E = S.makeTemp(SrcLane);
        S.emit(NdOp::SUBBYTES, E, {Src, NdVar::cst(I * SrcLane, 4)});
        NdVar L = S.makeTemp(DstLane);
        S.emit(NdOp::FLOAT_FLOAT2FLOAT, L, {E});
        if (I == 0) {
          Acc = L;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLane);
          S.emit(NdOp::CONCAT, Next, {L, Acc});
          Acc = Next;
        }
      }
      unsigned PairSz = NLanes * DstLane;
      if (Hi) {
        NdVar Lo = S.makeTemp(PairSz);
        S.emit(NdOp::SUBBYTES, Lo,
               {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(0, 4)});
        S.emit(NdOp::CONCAT, Dst, {Acc, Lo});
      } else if (Dst.Size > PairSz) {
        NdVar ZHi = S.makeTemp(Dst.Size - PairSz);
        S.emit(NdOp::COPY, ZHi, {NdVar::cst(0, (uint16_t)(Dst.Size - PairSz))});
        S.emit(NdOp::CONCAT, Dst, {ZHi, Acc});
      } else {
        S.emit(NdOp::COPY, Dst, {Acc});
      }
    }
    break;
  }
  // NEON float max/min
  // FMINNM/FMAXNM — element-wise IEEE minNum/maxNum (NaN-suppressing: return
  // the numeric operand when one input is NaN).  A naive (a<b)?a:b select
  // returns the wrong operand on NaN and on signed zeros, so use the dedicated
  // ops.
  case AARCH64_INS_FMAXNM:
  case AARCH64_INS_FMINNM: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdOp MM = (Insn->id == AARCH64_INS_FMINNM) ? NdOp::FLOAT_MINNUM
                                               : NdOp::FLOAT_MAXNUM;
    unsigned LaneSz = neonElemSize(ARM64.operands[0].vas);
    if ((LaneSz == 4 || LaneSz == 8) && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Lr = S.makeTemp(LaneSz);
        S.emit(MM, Lr, {La, Lb});
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
      S.emit(MM, Dst, {A, B});
    }
    break;
  }

  // FMINP/FMAXP (FMIN/FMAX semantics) and FMINNMP/FMAXNMP (minNum/maxNum) —
  // pairwise reduce: each result lane is the min/max of an ADJACENT pair drawn
  // from the concatenation of the two source vectors (low half from Vn, high
  // half from Vm), or from the single source for the scalar 2-operand form.
  // The previous code reduced element-wise (min(A[i],B[i])), which is wrong.
  case AARCH64_INS_FMAXNMP:
  case AARCH64_INS_FMINNMP:
  case AARCH64_INS_FMAXP:
  case AARCH64_INS_FMINP: {
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    bool IsMin =
        (Insn->id == AARCH64_INS_FMINP || Insn->id == AARCH64_INS_FMINNMP);
    bool IsNM =
        (Insn->id == AARCH64_INS_FMINNMP || Insn->id == AARCH64_INS_FMAXNMP);
    NdOp MM = IsNM ? (IsMin ? NdOp::FLOAT_MINNUM : NdOp::FLOAT_MAXNUM)
                   : (IsMin ? NdOp::FLOAT_MIN : NdOp::FLOAT_MAX);
    if (ARM64.op_count == 2) {
      NdVar Src = L.operandRead(S, ARM64.operands[1]);
      unsigned LaneSz = neonElemSize(ARM64.operands[1].vas);
      if ((LaneSz == 4 || LaneSz == 8) && Src.Size >= 2 * LaneSz) {
        NdVar Lo = S.makeTemp(LaneSz), Hi = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        S.emit(NdOp::SUBBYTES, Hi, {Src, NdVar::cst(LaneSz, 4)});
        S.emit(MM, Dst, {Lo, Hi});
      } else {
        S.emit(NdOp::COPY, Dst, {Src});
      }
      break;
    }
    if (ARM64.op_count < 3)
      break;
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    unsigned LaneSz = neonElemSize(ARM64.operands[0].vas);
    if ((LaneSz == 4 || LaneSz == 8) && A.Size >= 2 * LaneSz) {
      unsigned NPairs = A.Size / (LaneSz * 2);
      NdVar Acc = S.makeTemp(0);
      for (unsigned H = 0; H < 2; ++H) {
        NdVar Src = (H == 0) ? A : B;
        for (unsigned I = 0; I < NPairs; ++I) {
          NdVar Lo = S.makeTemp(LaneSz), Hi = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(I * 2 * LaneSz, 4)});
          S.emit(NdOp::SUBBYTES, Hi,
                 {Src, NdVar::cst(I * 2 * LaneSz + LaneSz, 4)});
          NdVar Pr = S.makeTemp(LaneSz);
          S.emit(MM, Pr, {Lo, Hi});
          if (H == 0 && I == 0) {
            Acc = Pr;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Pr, Acc});
            Acc = Next;
          }
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(MM, Dst, {A, B});
    }
    break;
  }
  // NEON reciprocal estimate / step.  The element width (f32 vs f64) is encoded
  // in the operand layout (4S/2S vs 2D) and is ambiguous from the register size
  // alone (16 bytes = 4×f32 or 2×f64), so pass it to the emitter as a trailing
  // constant; scalar S/D forms fall back to the destination size.
  case AARCH64_INS_FRECPE: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    if (ElemSz != 2 && ElemSz != 4 && ElemSz != 8)
      ElemSz = (Dst.Size == 2) ? 2 : (Dst.Size >= 8) ? 8 : 4;
    S.emitIntrinsic(Intrinsic::A64_Frecpe, Dst, {Src, NdVar::cst(ElemSz, 4)});
    break;
  }
  case AARCH64_INS_FRSQRTE: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    if (ElemSz != 2 && ElemSz != 4 && ElemSz != 8)
      ElemSz = (Dst.Size == 2) ? 2 : (Dst.Size >= 8) ? 8 : 4;
    S.emitIntrinsic(Intrinsic::A64_Frsqrte, Dst, {Src, NdVar::cst(ElemSz, 4)});
    break;
  }
  case AARCH64_INS_URECPE: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::A64_Urecpe, Dst, {Src});
    break;
  }
  case AARCH64_INS_URSQRTE: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::A64_Ursqrte, Dst, {Src});
    break;
  }
  case AARCH64_INS_FRECPS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    if (ElemSz != 2 && ElemSz != 4 && ElemSz != 8)
      ElemSz = (Dst.Size == 2) ? 2 : (Dst.Size >= 8) ? 8 : 4;
    S.emitIntrinsic(Intrinsic::A64_Frecps, Dst, {A, B, NdVar::cst(ElemSz, 4)});
    break;
  }
  case AARCH64_INS_FRSQRTS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    if (ElemSz != 2 && ElemSz != 4 && ElemSz != 8)
      ElemSz = (Dst.Size == 2) ? 2 : (Dst.Size >= 8) ? 8 : 4;
    S.emitIntrinsic(Intrinsic::A64_Frsqrts, Dst, {A, B, NdVar::cst(ElemSz, 4)});
    break;
  }
  // NEON / FP misc
  case AARCH64_INS_FRECPX: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::A64_Frecpe, Dst, {Src});
    break;
  }
  default:
    return false;
  }
  return true;
}

} // namespace neverd
