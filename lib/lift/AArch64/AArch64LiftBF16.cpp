//===- AArch64LiftBF16.cpp - BF16 and FP8 conversion lifter ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Brain-float-16 arithmetic, min/max, conversion, dot product
/// and multiply-accumulate (BFADD/BFMUL/BFCVT/BFDOT/BFMLA/...),
/// plus the FP8 F1CVT/F2CVT widening conversions.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftBF16(AArch64Lifter &L, AArch64Lifter::LiftState &S,
              const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // ========================================================================
  // BF16 (Brain Float 16)
  // ========================================================================
  case AARCH64_INS_BFADD:
  case AARCH64_INS_BFSUB:
  case AARCH64_INS_BFMUL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdOp Opc = NdOp::FLOAT_ADD;
    if (Insn->id == AARCH64_INS_BFSUB)
      Opc = NdOp::FLOAT_SUB;
    else if (Insn->id == AARCH64_INS_BFMUL)
      Opc = NdOp::FLOAT_MULT;
    S.emit(Opc, Dst, {A, B});
    break;
  }
  case AARCH64_INS_BFMAX:
  case AARCH64_INS_BFMAXNM:
  case AARCH64_INS_BFMIN:
  case AARCH64_INS_BFMINNM: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar Cmp = S.makeTemp(1);
    S.emit(NdOp::FLOAT_LESS, Cmp, {A, B});
    bool IsMin =
        (Insn->id == AARCH64_INS_BFMIN || Insn->id == AARCH64_INS_BFMINNM);
    if (IsMin)
      S.emit(NdOp::SELECT, Dst, {Cmp, A, B});
    else
      S.emit(NdOp::SELECT, Dst, {Cmp, B, A});
    break;
  }
  case AARCH64_INS_BFCVT:
  case AARCH64_INS_BFCVTN:
  case AARCH64_INS_BFCVTN2:
  case AARCH64_INS_BFCVTNT:
  case AARCH64_INS_BF1CVT:
  case AARCH64_INS_BF1CVTL:
  case AARCH64_INS_BF1CVTL2:
  case AARCH64_INS_BF1CVTLT:
  case AARCH64_INS_BF2CVT:
  case AARCH64_INS_BF2CVTL:
  case AARCH64_INS_BF2CVTL2:
  case AARCH64_INS_BF2CVTLT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::FLOAT_FLOAT2FLOAT, Dst, {Src});
    break;
  }
  case AARCH64_INS_BFDOT:
  case AARCH64_INS_BFVDOT:
  case AARCH64_INS_BFMLA:
  case AARCH64_INS_BFMLAL:
  case AARCH64_INS_BFMLALB:
  case AARCH64_INS_BFMLALT:
  case AARCH64_INS_BFMOPA:
  case AARCH64_INS_BFMOPS: {
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
  case AARCH64_INS_BFMMLA: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Acc = L.operandRead(S, ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emitIntrinsic(Intrinsic::A64_Bfmmla, Dst, {Acc, A, B});
    break;
  }
  case AARCH64_INS_BFMLS:
  case AARCH64_INS_BFMLSL:
  case AARCH64_INS_BFMLSLB:
  case AARCH64_INS_BFMLSLT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_SUB, Dst, {Dst, Prod});
    break;
  }
  case AARCH64_INS_BFCLAMP: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // F1CVT / F2CVT (FP8 to Half precision)
  case AARCH64_INS_F1CVT:
  case AARCH64_INS_F1CVTL:
  case AARCH64_INS_F1CVTL2:
  case AARCH64_INS_F1CVTLT:
  case AARCH64_INS_F2CVT:
  case AARCH64_INS_F2CVTL:
  case AARCH64_INS_F2CVTL2:
  case AARCH64_INS_F2CVTLT: {
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
