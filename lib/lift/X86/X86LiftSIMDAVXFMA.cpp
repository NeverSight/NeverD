//===- X86LiftSIMDAVXFMA.cpp - x86/x64 FMA instruction lifter -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fused multiply-add: the FMA3 132/213/231 operand
/// orderings, the FMA4 legacy forms, the negated-product and
/// subtract-addend variants, and the alternating
/// add-subtract forms.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

// Emit a single-rounding x86 FMA into Dst: the fused product Fa*Fb plus Fc with
// ONE rounding (NeverD FLOAT_FMA -> llvm.fma), matching the AArch64/ARM
// lowering.  Modeling FMA as a separate FLOAT_MULT + FLOAT_ADD rounds twice and
// diverges from hardware in the low mantissa bit.  NegProd negates the product
// (FNMADD/FNMSUB); SubAdd subtracts the addend (FMSUB/FNMSUB).  Scalar SS/SD
// forms compute only the low ElemSz element and carry the destination's upper
// bytes over; packed PS/PD forms fuse every ElemSz-wide lane.  Fa/Fb/Fc are the
// full-width operands.
static void emitX86Fma(LiftStateBase &S, NdVar Dst, NdVar Fa, NdVar Fb,
                       NdVar Fc, bool NegProd, bool SubAdd, bool Scalar,
                       unsigned ElemSz) {
  auto laneFma = [&](NdVar A, NdVar B, NdVar C) {
    NdVar AA = A;
    if (NegProd) {
      AA = S.makeTemp(ElemSz);
      S.emit(NdOp::FLOAT_NEG, AA, {A});
    }
    NdVar CC = C;
    if (SubAdd) {
      CC = S.makeTemp(ElemSz);
      S.emit(NdOp::FLOAT_NEG, CC, {C});
    }
    NdVar R = S.makeTemp(ElemSz);
    S.emit(NdOp::FLOAT_FMA, R, {AA, B, CC});
    return R;
  };
  auto lane = [&](NdVar V, unsigned Off) {
    if (V.Size == ElemSz && Off == 0)
      return V;
    NdVar L = S.makeTemp(ElemSz);
    S.emit(NdOp::SUBBYTES, L, {V, NdVar::cst(Off, 4)});
    return L;
  };

  if (Scalar || Dst.Size <= ElemSz) {
    NdVar R = laneFma(lane(Fa, 0), lane(Fb, 0), lane(Fc, 0));
    if (Dst.Size > ElemSz) {
      NdVar Hi = S.makeTemp(Dst.Size - ElemSz);
      S.emit(NdOp::SUBBYTES, Hi, {Dst, NdVar::cst(ElemSz, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, R});
    } else {
      S.emit(NdOp::COPY, Dst, {R});
    }
    return;
  }

  unsigned NLanes = Dst.Size / ElemSz;
  NdVar Acc = laneFma(lane(Fa, 0), lane(Fb, 0), lane(Fc, 0));
  unsigned AccSz = ElemSz;
  for (unsigned I = 1; I < NLanes; ++I) {
    NdVar Ln = laneFma(lane(Fa, I * ElemSz), lane(Fb, I * ElemSz),
                       lane(Fc, I * ElemSz));
    NdVar Next = S.makeTemp(AccSz + ElemSz);
    S.emit(NdOp::CONCAT, Next, {Ln, Acc});
    Acc = Next;
    AccSz += ElemSz;
  }
  S.emit(NdOp::COPY, Dst, {Acc});
}

bool liftSIMDAVXFMA(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                    const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // FMA (Fused Multiply-Add) — Dst = a * b ± c with correct operand mapping.
  // 132: Dst = Dst * src3 + src2     (op0 = op0 * op2 + op1)
  // 213: Dst = src2 * Dst + src3     (op0 = op1 * op0 + op2)
  // 231: Dst = src2 * src3 + Dst     (op0 = op1 * op2 + op0)
  // FNMADD: negate first multiplicand; FMSUB/FNMSUB: subtract addend.
  // ========================================================================
  case X86_INS_VFMADD132PD:
  case X86_INS_VFMADD132PS:
  case X86_INS_VFMADD132SD:
  case X86_INS_VFMADD132SS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[2]);
    bool Sc = InsnId == X86_INS_VFMADD132SS || InsnId == X86_INS_VFMADD132SD;
    unsigned E =
        (InsnId == X86_INS_VFMADD132SD || InsnId == X86_INS_VFMADD132PD) ? 8
                                                                         : 4;
    emitX86Fma(S, Dst, Dst, Op2, Op1, false, false, Sc, E); // 132
    break;
  }
  case X86_INS_VFMADD213PD:
  case X86_INS_VFMADD213PS:
  case X86_INS_VFMADD213SD:
  case X86_INS_VFMADD213SS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[2]);
    bool Sc = InsnId == X86_INS_VFMADD213SS || InsnId == X86_INS_VFMADD213SD;
    unsigned E =
        (InsnId == X86_INS_VFMADD213SD || InsnId == X86_INS_VFMADD213PD) ? 8
                                                                         : 4;
    emitX86Fma(S, Dst, Op1, Dst, Op2, false, false, Sc, E); // 213
    break;
  }
  case X86_INS_VFMADD231PD:
  case X86_INS_VFMADD231PS:
  case X86_INS_VFMADD231SD:
  case X86_INS_VFMADD231SS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[2]);
    bool Sc = InsnId == X86_INS_VFMADD231SS || InsnId == X86_INS_VFMADD231SD;
    unsigned E =
        (InsnId == X86_INS_VFMADD231SD || InsnId == X86_INS_VFMADD231PD) ? 8
                                                                         : 4;
    emitX86Fma(S, Dst, Op1, Op2, Dst, false, false, Sc, E); // 231
    break;
  }
  case X86_INS_VFMADDPD:
  case X86_INS_VFMADDPS:
  case X86_INS_VFMADDSD:
  case X86_INS_VFMADDSS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[X86.op_count - 1]);
    bool Sc = InsnId == X86_INS_VFMADDSS || InsnId == X86_INS_VFMADDSD;
    unsigned E =
        (InsnId == X86_INS_VFMADDSD || InsnId == X86_INS_VFMADDPD) ? 8 : 4;
    emitX86Fma(S, Dst, Dst, Op1, Op2, false, false, Sc, E); // FMA4 legacy
    break;
  }
  case X86_INS_VFMSUB132PD:
  case X86_INS_VFMSUB132PS:
  case X86_INS_VFMSUB132SD:
  case X86_INS_VFMSUB132SS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[2]);
    bool Sc = InsnId == X86_INS_VFMSUB132SS || InsnId == X86_INS_VFMSUB132SD;
    unsigned E =
        (InsnId == X86_INS_VFMSUB132SD || InsnId == X86_INS_VFMSUB132PD) ? 8
                                                                         : 4;
    emitX86Fma(S, Dst, Dst, Op2, Op1, false, true, Sc, E); // 132 sub
    break;
  }
  case X86_INS_VFMSUB213PD:
  case X86_INS_VFMSUB213PS:
  case X86_INS_VFMSUB213SD:
  case X86_INS_VFMSUB213SS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[2]);
    bool Sc = InsnId == X86_INS_VFMSUB213SS || InsnId == X86_INS_VFMSUB213SD;
    unsigned E =
        (InsnId == X86_INS_VFMSUB213SD || InsnId == X86_INS_VFMSUB213PD) ? 8
                                                                         : 4;
    emitX86Fma(S, Dst, Op1, Dst, Op2, false, true, Sc, E); // 213 sub
    break;
  }
  case X86_INS_VFMSUB231PD:
  case X86_INS_VFMSUB231PS:
  case X86_INS_VFMSUB231SD:
  case X86_INS_VFMSUB231SS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[2]);
    bool Sc = InsnId == X86_INS_VFMSUB231SS || InsnId == X86_INS_VFMSUB231SD;
    unsigned E =
        (InsnId == X86_INS_VFMSUB231SD || InsnId == X86_INS_VFMSUB231PD) ? 8
                                                                         : 4;
    emitX86Fma(S, Dst, Op1, Op2, Dst, false, true, Sc, E); // 231 sub
    break;
  }
  case X86_INS_VFMSUBPD:
  case X86_INS_VFMSUBPS:
  case X86_INS_VFMSUBSD:
  case X86_INS_VFMSUBSS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[X86.op_count - 1]);
    bool Sc = InsnId == X86_INS_VFMSUBSS || InsnId == X86_INS_VFMSUBSD;
    unsigned E =
        (InsnId == X86_INS_VFMSUBSD || InsnId == X86_INS_VFMSUBPD) ? 8 : 4;
    emitX86Fma(S, Dst, Dst, Op1, Op2, false, true, Sc, E); // FMA4 sub
    break;
  }
  case X86_INS_VFNMADD132PD:
  case X86_INS_VFNMADD132PS:
  case X86_INS_VFNMADD132SD:
  case X86_INS_VFNMADD132SS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[2]);
    bool Sc = InsnId == X86_INS_VFNMADD132SS || InsnId == X86_INS_VFNMADD132SD;
    unsigned E =
        (InsnId == X86_INS_VFNMADD132SD || InsnId == X86_INS_VFNMADD132PD) ? 8
                                                                           : 4;
    emitX86Fma(S, Dst, Dst, Op2, Op1, true, false, Sc, E); // 132 nmadd
    break;
  }
  case X86_INS_VFNMADD213PD:
  case X86_INS_VFNMADD213PS:
  case X86_INS_VFNMADD213SD:
  case X86_INS_VFNMADD213SS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[2]);
    bool Sc = InsnId == X86_INS_VFNMADD213SS || InsnId == X86_INS_VFNMADD213SD;
    unsigned E =
        (InsnId == X86_INS_VFNMADD213SD || InsnId == X86_INS_VFNMADD213PD) ? 8
                                                                           : 4;
    emitX86Fma(S, Dst, Op1, Dst, Op2, true, false, Sc, E); // 213 nmadd
    break;
  }
  case X86_INS_VFNMADD231PD:
  case X86_INS_VFNMADD231PS:
  case X86_INS_VFNMADD231SD:
  case X86_INS_VFNMADD231SS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[2]);
    bool Sc = InsnId == X86_INS_VFNMADD231SS || InsnId == X86_INS_VFNMADD231SD;
    unsigned E =
        (InsnId == X86_INS_VFNMADD231SD || InsnId == X86_INS_VFNMADD231PD) ? 8
                                                                           : 4;
    emitX86Fma(S, Dst, Op1, Op2, Dst, true, false, Sc, E); // 231 nmadd
    break;
  }
  case X86_INS_VFNMADDPD:
  case X86_INS_VFNMADDPS:
  case X86_INS_VFNMADDSD:
  case X86_INS_VFNMADDSS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[X86.op_count - 1]);
    bool Sc = InsnId == X86_INS_VFNMADDSS || InsnId == X86_INS_VFNMADDSD;
    unsigned E =
        (InsnId == X86_INS_VFNMADDSD || InsnId == X86_INS_VFNMADDPD) ? 8 : 4;
    emitX86Fma(S, Dst, Dst, Op1, Op2, true, false, Sc, E); // FMA4 nmadd
    break;
  }
  case X86_INS_VFNMSUB132PD:
  case X86_INS_VFNMSUB132PS:
  case X86_INS_VFNMSUB132SD:
  case X86_INS_VFNMSUB132SS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[2]);
    bool Sc = InsnId == X86_INS_VFNMSUB132SS || InsnId == X86_INS_VFNMSUB132SD;
    unsigned E =
        (InsnId == X86_INS_VFNMSUB132SD || InsnId == X86_INS_VFNMSUB132PD) ? 8
                                                                           : 4;
    emitX86Fma(S, Dst, Dst, Op2, Op1, true, true, Sc, E); // 132 nmsub
    break;
  }
  case X86_INS_VFNMSUB213PD:
  case X86_INS_VFNMSUB213PS:
  case X86_INS_VFNMSUB213SD:
  case X86_INS_VFNMSUB213SS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[2]);
    bool Sc = InsnId == X86_INS_VFNMSUB213SS || InsnId == X86_INS_VFNMSUB213SD;
    unsigned E =
        (InsnId == X86_INS_VFNMSUB213SD || InsnId == X86_INS_VFNMSUB213PD) ? 8
                                                                           : 4;
    emitX86Fma(S, Dst, Op1, Dst, Op2, true, true, Sc, E); // 213 nmsub
    break;
  }
  case X86_INS_VFNMSUB231PD:
  case X86_INS_VFNMSUB231PS:
  case X86_INS_VFNMSUB231SD:
  case X86_INS_VFNMSUB231SS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[2]);
    bool Sc = InsnId == X86_INS_VFNMSUB231SS || InsnId == X86_INS_VFNMSUB231SD;
    unsigned E =
        (InsnId == X86_INS_VFNMSUB231SD || InsnId == X86_INS_VFNMSUB231PD) ? 8
                                                                           : 4;
    emitX86Fma(S, Dst, Op1, Op2, Dst, true, true, Sc, E); // 231 nmsub
    break;
  }
  case X86_INS_VFNMSUBPD:
  case X86_INS_VFNMSUBPS:
  case X86_INS_VFNMSUBSD:
  case X86_INS_VFNMSUBSS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[X86.op_count - 1]);
    bool Sc = InsnId == X86_INS_VFNMSUBSS || InsnId == X86_INS_VFNMSUBSD;
    unsigned E =
        (InsnId == X86_INS_VFNMSUBSD || InsnId == X86_INS_VFNMSUBPD) ? 8 : 4;
    emitX86Fma(S, Dst, Dst, Op1, Op2, true, true, Sc, E); // FMA4 nmsub
    break;
  }
  // FMADDSUB/FMSUBADD: alternate add/sub per Lane — must be intrinsic.
  case X86_INS_VFMADDSUB132PD:
  case X86_INS_VFMADDSUB132PS:
  case X86_INS_VFMADDSUB213PD:
  case X86_INS_VFMADDSUB213PS:
  case X86_INS_VFMADDSUB231PD:
  case X86_INS_VFMADDSUB231PS:
  case X86_INS_VFMADDSUBPD:
  case X86_INS_VFMADDSUBPS:
  case X86_INS_VFMSUBADD132PD:
  case X86_INS_VFMSUBADD132PS:
  case X86_INS_VFMSUBADD213PD:
  case X86_INS_VFMSUBADD213PS:
  case X86_INS_VFMSUBADD231PD:
  case X86_INS_VFMSUBADD231PS:
  case X86_INS_VFMSUBADDPD:
  case X86_INS_VFMSUBADDPS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Op1 = L.operandRead(S, X86.operands[1]);
    NdVar Op2 = L.operandRead(S, X86.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {Dst, Op1});
    S.emit(NdOp::FLOAT_ADD, Dst, {Prod, Op2});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
