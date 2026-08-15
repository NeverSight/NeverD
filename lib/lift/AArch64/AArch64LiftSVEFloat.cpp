//===- AArch64LiftSVEFloat.cpp - SVE permute and floating-point ops -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Quadword permutes (EXTQ/TBLQ/UZPQ/ZIPQ), the rotated complex
/// add FCADD, FCPY/FDUP, FEXPA/FLOGB, the reversed and negated
/// fused multiply-adds (FMAD/FMSB/FNMLA/FNMLS), the
/// trigonometric helpers (FTMAD/FTSMUL/FTSSEL), FSCALE/FAMAX and
/// the top/bottom FP converts.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftSVEFloat(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // SVE / NEON misc permute
  case AARCH64_INS_EXTQ:
  case AARCH64_INS_EXTRX:
  case AARCH64_INS_EXTRY:
  case AARCH64_INS_TBLQ:
  case AARCH64_INS_TBXQ:
  case AARCH64_INS_UZP:
  case AARCH64_INS_UZPQ1:
  case AARCH64_INS_UZPQ2:
  case AARCH64_INS_ZIP:
  case AARCH64_INS_ZIPQ1:
  case AARCH64_INS_ZIPQ2: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // SVE float misc
  // FCADD — rotated complex floating-point add (FEAT_FCMA).  Per complex pair:
  //   rot 90:  re = Vn.re - Vm.im;  im = Vn.im + Vm.re
  //   rot 270: re = Vn.re + Vm.im;  im = Vn.im - Vm.re
  // The old code was a whole-register FLOAT_ADD (no rotation, no per-lane).
  case AARCH64_INS_FCADD: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Vn = L.operandRead(S, ARM64.operands[1]);
    NdVar Vm = L.operandRead(S, ARM64.operands[2]);
    int64_t Rot = ARM64.operands[3].imm;
    unsigned ES = complexElemSize(ARM64.operands[0].vas);
    if ((ES != 4 && ES != 8) || Dst.Size < 2 * ES) {
      S.emit(NdOp::FLOAT_ADD, Dst, {Vn, Vm});
      break;
    }
    auto lane = [&](NdVar V, unsigned Idx) {
      NdVar T = S.makeTemp(ES);
      S.emit(NdOp::SUBBYTES, T,
             {V, NdVar::cst(static_cast<uint64_t>(Idx) * ES, 4)});
      return T;
    };
    NdVar Acc = NdVar::cst(0, 0);
    bool First = true;
    auto append = [&](NdVar L) {
      if (First) {
        Acc = L;
        First = false;
        return;
      }
      NdVar N = S.makeTemp(Acc.Size + ES);
      S.emit(NdOp::CONCAT, N, {L, Acc});
      Acc = N;
    };
    unsigned NLanes = Dst.Size / ES;
    for (unsigned K = 0; K < NLanes / 2; ++K) {
      NdVar VnRe = lane(Vn, 2 * K), VnIm = lane(Vn, 2 * K + 1);
      NdVar VmRe = lane(Vm, 2 * K), VmIm = lane(Vm, 2 * K + 1);
      NdVar OutRe = S.makeTemp(ES), OutIm = S.makeTemp(ES);
      if (Rot == 270) {
        S.emit(NdOp::FLOAT_ADD, OutRe, {VnRe, VmIm});
        S.emit(NdOp::FLOAT_SUB, OutIm, {VnIm, VmRe});
      } else {
        S.emit(NdOp::FLOAT_SUB, OutRe, {VnRe, VmIm});
        S.emit(NdOp::FLOAT_ADD, OutIm, {VnIm, VmRe});
      }
      append(OutRe);
      append(OutIm);
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case AARCH64_INS_FCPY:
  case AARCH64_INS_FDUP: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_FEXPA:
  case AARCH64_INS_FLOGB: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case AARCH64_INS_FMAD:
  case AARCH64_INS_FNMAD: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar C = L.operandRead(S, ARM64.operands[3]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_ADD, Dst, {Prod, C});
    break;
  }
  case AARCH64_INS_FMSB:
  case AARCH64_INS_FNMSB: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar C = L.operandRead(S, ARM64.operands[3]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_SUB, Dst, {C, Prod});
    break;
  }
  case AARCH64_INS_FNMLA:
  case AARCH64_INS_FNMLS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_ADD, Dst, {Dst, Prod});
    break;
  }
  case AARCH64_INS_FTMAD:
  case AARCH64_INS_FTSMUL:
  case AARCH64_INS_FTSSEL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::FLOAT_MULT, Dst, {A, B});
    break;
  }
  case AARCH64_INS_FSCALE: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::FLOAT_MULT, Dst, {A, B});
    break;
  }
  // FAMIN/FAMAX are absolute min/max, not multiply: each result lane is the
  // IEEE minimum/maximum of fabs(A[i]) and fabs(B[i]).
  case AARCH64_INS_FAMAX:
  case AARCH64_INS_FAMIN: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    unsigned LaneSz = neonElemSize(ARM64.operands[0].vas);
    if (!isFPLaneSize(LaneSz) || Dst.Size < LaneSz || Dst.Size % LaneSz != 0)
      break;
    Intrinsic Id = (Insn->id == AARCH64_INS_FAMIN) ? Intrinsic::A64_Famin
                                                   : Intrinsic::A64_Famax;
    S.emitIntrinsic(Id, Dst,
                    {A, B, NdVar::cst(Dst.Size, 4), NdVar::cst(LaneSz, 4)});
    break;
  }
  case AARCH64_INS_FCVTLT:
  case AARCH64_INS_FCVTNB:
  case AARCH64_INS_FCVTNT:
  case AARCH64_INS_FCVTX:
  case AARCH64_INS_FCVTXNT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::FLOAT_FLOAT2FLOAT, Dst, {Src});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
