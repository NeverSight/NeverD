//===- AArch64LiftSVEArith.cpp - SVE integer arithmetic and compare -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Carry-propagating and narrowing add/subtract, cross-lane
/// reductions, reversed shifts and divides, complex add/multiply
/// (CADD/CDOT/CMLA/FCMLA), multiply-accumulate (MAD/MSB), the
/// three-way logical ops (EOR3/BSL1N/RAX1/XAR/NAND/NOR) and the
/// SVE predicate-result compares (CMPEQ/CMPGT/...).
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftSVEArith(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // ========================================================================
  // SVE / SVE2 — integer arithmetic
  // ========================================================================
  case AARCH64_INS_ADCLB:
  case AARCH64_INS_ADCLT:
  case AARCH64_INS_SBCLB:
  case AARCH64_INS_SBCLT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    break;
  }
  case AARCH64_INS_ADDHA:
  case AARCH64_INS_ADDVA: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    break;
  }
  case AARCH64_INS_ADDHNB:
  case AARCH64_INS_ADDHNT:
  case AARCH64_INS_SUBHNB:
  case AARCH64_INS_SUBHNT:
  case AARCH64_INS_RADDHNB:
  case AARCH64_INS_RADDHNT:
  case AARCH64_INS_RSUBHNB:
  case AARCH64_INS_RSUBHNT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    break;
  }
  case AARCH64_INS_ADDQV:
  case AARCH64_INS_ANDQV:
  case AARCH64_INS_ANDV:
  case AARCH64_INS_EORQV:
  case AARCH64_INS_EORV:
  case AARCH64_INS_ORQV:
  case AARCH64_INS_ORV:
  case AARCH64_INS_SMAXQV:
  case AARCH64_INS_SMINQV:
  case AARCH64_INS_UMAXQV:
  case AARCH64_INS_UMINQV:
  case AARCH64_INS_SADDV:
  case AARCH64_INS_UADDV:
  case AARCH64_INS_FADDQV:
  case AARCH64_INS_FADDV:
  case AARCH64_INS_FADDA:
  case AARCH64_INS_FMAXNMQV:
  case AARCH64_INS_FMAXQV:
  case AARCH64_INS_FMINNMQV:
  case AARCH64_INS_FMINQV: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_ASRD:
  case AARCH64_INS_ASRR:
  case AARCH64_INS_LSLR:
  case AARCH64_INS_LSRR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdOp Opc = NdOp::INT_ASHR;
    if (Insn->id == AARCH64_INS_LSLR)
      Opc = NdOp::INT_LEFT;
    else if (Insn->id == AARCH64_INS_LSRR)
      Opc = NdOp::INT_RIGHT;
    S.emit(Opc, Dst, {A, B});
    break;
  }
  case AARCH64_INS_CADD:
  case AARCH64_INS_SQCADD: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    break;
  }
  case AARCH64_INS_CDOT:
  case AARCH64_INS_CMLA:
  // FCMLA — rotated complex floating-point multiply-accumulate (FEAT_FCMA).
  // Per complex pair {re@2k, im@2k+1} with the #rot table:
  //   rot 0:   re += Vn.re*Vm.re;  im += Vn.re*Vm.im
  //   rot 90:  re -= Vn.im*Vm.im;  im += Vn.im*Vm.re
  //   rot 180: re -= Vn.re*Vm.re;  im -= Vn.re*Vm.im
  //   rot 270: re += Vn.im*Vm.im;  im -= Vn.im*Vm.re
  // The old code was a whole-register INT_MULT+INT_ADD (integer ops on FP data,
  // no rotation, no per-lane).  The indexed form broadcasts one Vm pair.
  case AARCH64_INS_FCMLA: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Vn = L.operandRead(S, ARM64.operands[1]);
    NdVar Vm = L.operandRead(S, ARM64.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    int64_t Rot = ARM64.operands[3].imm;
    int VecIdx = ARM64.operands[2].vector_index;
    unsigned ES = complexElemSize(ARM64.operands[0].vas);
    if ((ES != 4 && ES != 8) || Dst.Size < 2 * ES) {
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::FLOAT_MULT, Prod, {Vn, Vm});
      S.emit(NdOp::FLOAT_ADD, Dst, {OldDst, Prod});
      break;
    }
    bool UseVnIm = (Rot == 90 || Rot == 270);
    bool ReUseVmIm = (Rot == 90 || Rot == 270);
    bool ImUseVmIm = (Rot == 0 || Rot == 180);
    bool ReSub = (Rot == 90 || Rot == 180);
    bool ImSub = (Rot == 180 || Rot == 270);
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
      unsigned BPair = (VecIdx >= 0) ? (unsigned)VecIdx : K;
      NdVar VnEl = lane(Vn, 2 * K + (UseVnIm ? 1 : 0));
      NdVar VmRe = lane(Vm, 2 * BPair);
      NdVar VmIm = lane(Vm, 2 * BPair + 1);
      NdVar AccRe = lane(OldDst, 2 * K);
      NdVar AccIm = lane(OldDst, 2 * K + 1);
      NdVar PRe = S.makeTemp(ES), PIm = S.makeTemp(ES);
      S.emit(NdOp::FLOAT_MULT, PRe, {VnEl, ReUseVmIm ? VmIm : VmRe});
      S.emit(NdOp::FLOAT_MULT, PIm, {VnEl, ImUseVmIm ? VmIm : VmRe});
      NdVar OutRe = S.makeTemp(ES), OutIm = S.makeTemp(ES);
      S.emit(ReSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD, OutRe, {AccRe, PRe});
      S.emit(ImSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD, OutIm, {AccIm, PIm});
      append(OutRe);
      append(OutIm);
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case AARCH64_INS_CNOT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::INT_EQUAL, Dst, {Src, NdVar::cst(0, Src.Size)});
    break;
  }
  case AARCH64_INS_SUBPT:
  case AARCH64_INS_SUBR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    if (Insn->id == AARCH64_INS_SUBR)
      S.emit(NdOp::INT_SUB, Dst, {B, A});
    else
      S.emit(NdOp::INT_SUB, Dst, {A, B});
    break;
  }
  case AARCH64_INS_SDIVR:
  case AARCH64_INS_UDIVR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    if (Insn->id == AARCH64_INS_SDIVR)
      S.emit(NdOp::INT_SDIV, Dst, {B, A});
    else
      S.emit(NdOp::INT_DIV, Dst, {B, A});
    break;
  }
  case AARCH64_INS_FDIVR:
  case AARCH64_INS_FSUBR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    if (Insn->id == AARCH64_INS_FDIVR)
      S.emit(NdOp::FLOAT_DIV, Dst, {B, A});
    else
      S.emit(NdOp::FLOAT_SUB, Dst, {B, A});
    break;
  }
  case AARCH64_INS_MAD:
  case AARCH64_INS_MADPT:
  case AARCH64_INS_MADDPT:
  case AARCH64_INS_MLAPT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    if (ARM64.op_count >= 4) {
      NdVar C = L.operandRead(S, ARM64.operands[3]);
      S.emit(NdOp::INT_ADD, Dst, {Prod, C});
    } else {
      S.emit(NdOp::COPY, Dst, {Prod});
    }
    break;
  }
  case AARCH64_INS_MSB:
  case AARCH64_INS_MSUBPT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    if (ARM64.op_count >= 4) {
      NdVar C = L.operandRead(S, ARM64.operands[3]);
      S.emit(NdOp::INT_SUB, Dst, {C, Prod});
    } else {
      S.emit(NdOp::INT_NEG2, Dst, {Prod});
    }
    break;
  }
  case AARCH64_INS_EOR3:
  case AARCH64_INS_NBSL:
  case AARCH64_INS_BSL1N:
  case AARCH64_INS_BSL2N:
  case AARCH64_INS_RAX1: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    break;
  }
  case AARCH64_INS_XAR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    break;
  }
  case AARCH64_INS_EORBT:
  case AARCH64_INS_EORTB: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    break;
  }
  case AARCH64_INS_EORS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    break;
  }
  case AARCH64_INS_ORRS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_OR, Dst, {A, B});
    break;
  }
  case AARCH64_INS_ORNS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar NB = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, NB, {B});
    S.emit(NdOp::INT_OR, Dst, {A, NB});
    break;
  }
  case AARCH64_INS_NAND:
  case AARCH64_INS_NANDS: {
    // NAND: Dst = ~(a & b)
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar Anded = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, Anded, {A, B});
    S.emit(NdOp::INT_NOT, Dst, {Anded});
    break;
  }
  case AARCH64_INS_NOR:
  case AARCH64_INS_NORS: {
    // NOR: Dst = ~(a | b)
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar Ored = S.makeTemp(A.Size);
    S.emit(NdOp::INT_OR, Ored, {A, B});
    S.emit(NdOp::INT_NOT, Dst, {Ored});
    break;
  }

  // SVE compare (predicate Result)
  case AARCH64_INS_CMPEQ:
  case AARCH64_INS_MATCH: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar A = L.operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_EQUAL, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_CMPNE:
  case AARCH64_INS_NMATCH: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar A = L.operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_NOTEQUAL, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_CMPGE: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar A = L.operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_SLESSEQUAL, Dst, {B, A});
    }
    break;
  }
  case AARCH64_INS_CMPGT: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar A = L.operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_SLESS, Dst, {B, A});
    }
    break;
  }
  case AARCH64_INS_CMPLE: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar A = L.operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_SLESSEQUAL, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_CMPLT: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar A = L.operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_SLESS, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_CMPHI: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar A = L.operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_LESS, Dst, {B, A});
    }
    break;
  }
  case AARCH64_INS_CMPHS: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar A = L.operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_LESSEQUAL, Dst, {B, A});
    }
    break;
  }
  case AARCH64_INS_CMPLO: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar A = L.operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_LESS, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_CMPLS: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar A = L.operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::INT_LESSEQUAL, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_FCMNE:
  case AARCH64_INS_FCMUO: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar A = L.operandRead(S, ARM64.operands[ARM64.op_count - 2]);
      NdVar B = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::FLOAT_NOTEQUAL, Dst, {A, B});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
