//===- X86LiftSIMDAVX.cpp - x86/x64 AVX/AVX-512/FMA instruction lifter --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// FMA, Gather/Scatter, AVX-512 opmask, and AVX-512 packed integer/float
/// instruction handlers for x86/x64.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

// Emit a single-rounding x86 FMA into Dst: the fused product Fa*Fb plus Fc with
// ONE rounding (NeverD FLOAT_FMA -> llvm.fma), matching the AArch64/ARM
// lowering.  Modeling FMA as a separate FLOAT_MULT + FLOAT_ADD rounds twice and
// diverges from hardware in the low mantissa bit.  NegProd negates the product
// (FNMADD/FNMSUB); SubAdd subtracts the addend (FMSUB/FNMSUB).  Scalar SS/SD
// forms compute only the low ElemSz element and carry the destination's upper
// bytes over; packed PS/PD forms fuse every ElemSz-wide lane.  Fa/Fb/Fc are the
// full-width operand varnodes.
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

bool X86Lifter::liftSIMDAVX(LiftState &S, const cs_insn *Insn,
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[2]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[2]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[2]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[X86.op_count - 1]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[2]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[2]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[2]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[X86.op_count - 1]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[2]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[2]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[2]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[X86.op_count - 1]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[2]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[2]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[2]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[X86.op_count - 1]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Op1 = operandRead(S, X86.operands[1]);
    NdVar Op2 = operandRead(S, X86.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {Dst, Op1});
    S.emit(NdOp::FLOAT_ADD, Dst, {Prod, Op2});
    break;
  }

  // ========================================================================
  // Gather — native per-lane conditional load (see liftVectorGather).  The FP
  // forms are bit-identical to the integer VPGATHER forms (the value is only
  // FP-typed), so they share the same lowering.
  // ========================================================================
  case X86_INS_VGATHERDPD:
  case X86_INS_VGATHERDPS:
  case X86_INS_VGATHERQPD:
  case X86_INS_VGATHERQPS: {
    if (!liftVectorGather(S, X86, InsnId) && X86.op_count >= 1) {
      NdVar Dst = operandWrite(X86.operands[0]);
      S.emitIntrinsic(Intrinsic::VGather, Dst);
    }
    break;
  }
  // AVX-512 gather prefetch hints carry no architectural data movement; keep an
  // opaque side effect (clang never emits these for the supported targets).
  case X86_INS_VGATHERPF0DPD:
  case X86_INS_VGATHERPF0DPS:
  case X86_INS_VGATHERPF0QPD:
  case X86_INS_VGATHERPF0QPS:
  case X86_INS_VGATHERPF1DPD:
  case X86_INS_VGATHERPF1DPS:
  case X86_INS_VGATHERPF1QPD:
  case X86_INS_VGATHERPF1QPS: {
    if (X86.op_count >= 1) {
      NdVar Dst = operandWrite(X86.operands[0]);
      S.emitIntrinsic(Intrinsic::VGather, Dst);
    }
    break;
  }
  case X86_INS_VSCATTERDPD:
  case X86_INS_VSCATTERDPS:
  case X86_INS_VSCATTERQPD:
  case X86_INS_VSCATTERQPS:
  case X86_INS_VSCATTERPF0DPD:
  case X86_INS_VSCATTERPF0DPS:
  case X86_INS_VSCATTERPF0QPD:
  case X86_INS_VSCATTERPF0QPS:
  case X86_INS_VSCATTERPF1DPD:
  case X86_INS_VSCATTERPF1DPS:
  case X86_INS_VSCATTERPF1QPD:
  case X86_INS_VSCATTERPF1QPS: {
    if (X86.op_count >= 1) {
      NdVar Dst = operandWrite(X86.operands[0]);
      S.emitIntrinsic(Intrinsic::VScatter, Dst);
    }
    break;
  }

  // ========================================================================
  // P0: SSE — MOVBE, non-temporal stores/loads, ROUND, dot product,
  //     extract/insert, Mask moves, LDDQU, MOVDDUP, MPSADBW, PHMINPOSUW,
  //     MOVSHDUP, MOVSLDUP, cache control extensions.
  // ========================================================================

  // MOVBE — byte-swap load/store (endian convert on the moved value).
  case X86_INS_MOVBE: {
    if (X86.op_count < 2)
      break;
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Swapped = S.emitByteSwap(Src);
    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], Swapped);
    } else {
      S.emit(NdOp::COPY, Dst, {Swapped});
    }
    break;
  }

  // Non-temporal stores/loads — semantically identical to normal moves for
  // our purposes (the non-temporal hint only affects cache behavior).
  case X86_INS_MOVNTDQ:
  case X86_INS_MOVNTDQA:
  case X86_INS_MOVNTI:
  case X86_INS_MOVNTPD:
  case X86_INS_MOVNTPS:
  case X86_INS_MOVNTQ:
  case X86_INS_MOVNTSD:
  case X86_INS_MOVNTSS:
  case X86_INS_VMOVNTDQ:
  case X86_INS_VMOVNTDQA:
  case X86_INS_VMOVNTPD:
  case X86_INS_VMOVNTPS: {
    if (X86.op_count < 2)
      break;
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Dst = operandWrite(X86.operands[0]);
    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], Src);
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // LDDQU — load unaligned double quadword (semantically identical to
  // MOVDQU for our purposes).
  case X86_INS_LDDQU:
  case X86_INS_VLDDQU: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // MOVDDUP / MOVSHDUP / MOVSLDUP — duplicate lanes.
  case X86_INS_MOVDDUP:
  case X86_INS_VMOVDDUP: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Movddup, Dst, {Src});
    break;
  }
  case X86_INS_MOVSHDUP:
  case X86_INS_VMOVSHDUP: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Movshdup, Dst, {Src});
    break;
  }
  case X86_INS_MOVSLDUP:
  case X86_INS_VMOVSLDUP: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Movsldup, Dst, {Src});
    break;
  }

  // ROUNDSS/VROUNDSS — scalar single float rounding (lowest 32 bits).
  // Legacy: roundss xmm, xmm/m32, imm8 (3 operands: dst, src, imm)
  // VEX:    vroundss xmm, xmm, xmm/m32, imm8 (4 operands)
  case X86_INS_ROUNDSS:
  case X86_INS_VROUNDSS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    bool IsVEX = (X86.op_count >= 4);
    NdVar PassThru = operandRead(S, X86.operands[IsVEX ? 1 : 0]);
    NdVar Src = operandRead(S, X86.operands[IsVEX ? 2 : 1]);
    uint8_t Imm = X86.operands[X86.op_count - 1].imm & 0x3;
    NdVar Lo = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
    NdVar Rounded = S.makeTemp(4);
    if (Imm == 3) {
      // Round toward zero = floor for non-negative, ceil for negative.  Using
      // floor/ceil (not float->int->float) stays correct past 2^31/2^63.
      NdVar Fl = S.makeTemp(4), Ce = S.makeTemp(4), IsNeg = S.makeTemp(1);
      S.emit(NdOp::FLOAT_FLOOR, Fl, {Lo});
      S.emit(NdOp::FLOAT_CEIL, Ce, {Lo});
      S.emit(NdOp::FLOAT_LESS, IsNeg, {Lo, NdVar::cst(0, 4)});
      S.emit(NdOp::SELECT, Rounded, {IsNeg, Ce, Fl});
    } else {
      NdOp RndOp = Imm == 1 ? NdOp::FLOAT_FLOOR
                   : Imm == 2
                       ? NdOp::FLOAT_CEIL
                       : NdOp::FLOAT_ROUNDEVEN; // imm0: nearest, ties even
      S.emit(RndOp, Rounded, {Lo});
    }
    if (Dst.Size > 4) {
      NdVar Hi = S.makeTemp(Dst.Size - 4);
      S.emit(NdOp::SUBBYTES, Hi, {PassThru, NdVar::cst(4, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Rounded});
    } else {
      S.emit(NdOp::COPY, Dst, {Rounded});
    }
    break;
  }
  // ROUNDSD/VROUNDSD — scalar double float rounding (lowest 64 bits).
  case X86_INS_ROUNDSD:
  case X86_INS_VROUNDSD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    bool IsVEX = (X86.op_count >= 4);
    NdVar PassThru = operandRead(S, X86.operands[IsVEX ? 1 : 0]);
    NdVar Src = operandRead(S, X86.operands[IsVEX ? 2 : 1]);
    uint8_t Imm = X86.operands[X86.op_count - 1].imm & 0x3;
    NdVar Lo = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
    NdVar Rounded = S.makeTemp(8);
    if (Imm == 3) {
      // Round toward zero = floor for non-negative, ceil for negative.  Using
      // floor/ceil (not float->int->float) stays correct past 2^31/2^63.
      NdVar Fl = S.makeTemp(8), Ce = S.makeTemp(8), IsNeg = S.makeTemp(1);
      S.emit(NdOp::FLOAT_FLOOR, Fl, {Lo});
      S.emit(NdOp::FLOAT_CEIL, Ce, {Lo});
      S.emit(NdOp::FLOAT_LESS, IsNeg, {Lo, NdVar::cst(0, 8)});
      S.emit(NdOp::SELECT, Rounded, {IsNeg, Ce, Fl});
    } else {
      NdOp RndOp = Imm == 1 ? NdOp::FLOAT_FLOOR
                   : Imm == 2
                       ? NdOp::FLOAT_CEIL
                       : NdOp::FLOAT_ROUNDEVEN; // imm0: nearest, ties even
      S.emit(RndOp, Rounded, {Lo});
    }
    if (Dst.Size > 8) {
      NdVar Hi = S.makeTemp(Dst.Size - 8);
      S.emit(NdOp::SUBBYTES, Hi, {PassThru, NdVar::cst(8, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Rounded});
    } else {
      S.emit(NdOp::COPY, Dst, {Rounded});
    }
    break;
  }
  // ROUNDPS/ROUNDPD/VROUNDPS/VROUNDPD — packed float rounding (per-lane).
  case X86_INS_ROUNDPD:
  case X86_INS_ROUNDPS:
  case X86_INS_VROUNDPD:
  case X86_INS_VROUNDPS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count >= 3 ? 1 : 0]);
    uint8_t Imm = X86.operands[X86.op_count - 1].imm & 0x3;
    bool IsPD = (InsnId == X86_INS_ROUNDPD || InsnId == X86_INS_VROUNDPD);
    unsigned LaneSz = IsPD ? 8 : 4;
    unsigned NLanes = Dst.Size / LaneSz;
    std::vector<NdVar> Lanes;
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
      NdVar R = S.makeTemp(LaneSz);
      if (Imm == 3) {
        // Round toward zero = floor for non-negative, ceil for negative.  Using
        // floor/ceil (not float->int->float) stays correct past 2^31/2^63.
        NdVar Fl = S.makeTemp(LaneSz), Ce = S.makeTemp(LaneSz),
                IsNeg = S.makeTemp(1);
        S.emit(NdOp::FLOAT_FLOOR, Fl, {Lane});
        S.emit(NdOp::FLOAT_CEIL, Ce, {Lane});
        S.emit(NdOp::FLOAT_LESS, IsNeg, {Lane, NdVar::cst(0, LaneSz)});
        S.emit(NdOp::SELECT, R, {IsNeg, Ce, Fl});
      } else {
        NdOp RndOp = Imm == 1 ? NdOp::FLOAT_FLOOR
                     : Imm == 2
                         ? NdOp::FLOAT_CEIL
                         : NdOp::FLOAT_ROUNDEVEN; // imm0: nearest, ties even
        S.emit(RndOp, R, {Lane});
      }
      Lanes.push_back(R);
    }
    NdVar Acc = Lanes[0];
    for (unsigned I = 1; I < NLanes; ++I) {
      NdVar W = S.makeTemp((I + 1) * LaneSz);
      S.emit(NdOp::CONCAT, W, {Lanes[I], Acc});
      Acc = W;
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  // DPPS / DPPD — dot product with imm8 Lane Mask.
  // DPPS/DPPD — dot product with immediate lane mask.
  // imm8[7:4] = which src lanes to multiply, imm8[3:0] = which dst lanes get
  // result.
  case X86_INS_DPPS:
  case X86_INS_VDPPS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    unsigned SrcIdx = (X86.op_count >= 4) ? 1 : 0;
    NdVar A = operandRead(S, X86.operands[SrcIdx]);
    NdVar B = operandRead(S, X86.operands[SrcIdx + 1]);
    uint8_t Imm = (uint8_t)X86.operands[X86.op_count - 1].imm;
    NdVar Sum;
    bool HasSum = false;
    for (unsigned I = 0; I < 4; ++I) {
      if (!(Imm & (1u << (I + 4))))
        continue;
      NdVar AI = S.makeTemp(4), BI = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, AI, {A, NdVar::cst(I * 4, 4)});
      S.emit(NdOp::SUBBYTES, BI, {B, NdVar::cst(I * 4, 4)});
      NdVar Prod = S.makeTemp(4);
      S.emit(NdOp::FLOAT_MULT, Prod, {AI, BI});
      if (!HasSum) {
        Sum = Prod;
        HasSum = true;
      } else {
        NdVar NewSum = S.makeTemp(4);
        S.emit(NdOp::FLOAT_ADD, NewSum, {Sum, Prod});
        Sum = NewSum;
      }
    }
    if (!HasSum) {
      Sum = S.makeTemp(4);
      S.emit(NdOp::COPY, Sum, {NdVar::cst(0, 4)});
    }
    std::vector<NdVar> Lanes;
    for (unsigned I = 0; I < 4; ++I) {
      if (Imm & (1u << I))
        Lanes.push_back(Sum);
      else
        Lanes.push_back(NdVar::cst(0, 4));
    }
    NdVar Lo = S.makeTemp(8), Hi = S.makeTemp(8);
    S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
    S.emit(NdOp::CONCAT, Hi, {Lanes[3], Lanes[2]});
    S.emit(NdOp::CONCAT, Dst, {Hi, Lo});
    break;
  }
  case X86_INS_DPPD:
  case X86_INS_VDPPD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    unsigned SrcIdx = (X86.op_count >= 4) ? 1 : 0;
    NdVar A = operandRead(S, X86.operands[SrcIdx]);
    NdVar B = operandRead(S, X86.operands[SrcIdx + 1]);
    uint8_t Imm = (uint8_t)X86.operands[X86.op_count - 1].imm;
    NdVar Sum;
    bool HasSum = false;
    for (unsigned I = 0; I < 2; ++I) {
      if (!(Imm & (1u << (I + 4))))
        continue;
      NdVar AI = S.makeTemp(8), BI = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, AI, {A, NdVar::cst(I * 8, 4)});
      S.emit(NdOp::SUBBYTES, BI, {B, NdVar::cst(I * 8, 4)});
      NdVar Prod = S.makeTemp(8);
      S.emit(NdOp::FLOAT_MULT, Prod, {AI, BI});
      if (!HasSum) {
        Sum = Prod;
        HasSum = true;
      } else {
        NdVar NewSum = S.makeTemp(8);
        S.emit(NdOp::FLOAT_ADD, NewSum, {Sum, Prod});
        Sum = NewSum;
      }
    }
    if (!HasSum) {
      Sum = S.makeTemp(8);
      S.emit(NdOp::COPY, Sum, {NdVar::cst(0, 8)});
    }
    NdVar L0 = (Imm & 1) ? Sum : NdVar::cst(0, 8);
    NdVar L1 = (Imm & 2) ? Sum : NdVar::cst(0, 8);
    S.emit(NdOp::CONCAT, Dst, {L1, L0});
    break;
  }

  // EXTRACTPS — extract float element to GPR/memory.
  case X86_INS_EXTRACTPS:
  case X86_INS_VEXTRACTPS: {
    if (X86.op_count < 3)
      break;
    // EXTRACTPS extracts a 32-bit element to r/m32.  A MEMORY destination needs
    // an explicit STORE (operandWrite yields a discarded ram(0) placeholder, so
    // the store was dropped); a 64-bit register destination zero-extends.
    const uint16_t ElemSz = 4;
    bool IsMem = (X86.operands[0].type == X86_OP_MEM);
    NdVar Dst = IsMem ? S.makeTemp(ElemSz) : operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar ExtSrc = Src;
    if (X86.operands[2].type == X86_OP_IMM) {
      uint64_t Idx = X86.operands[2].imm & 0x3;
      uint64_t ByteOff = Idx * 4;
      if (ByteOff > 0) {
        ExtSrc = S.makeTemp(Src.Size);
        S.emit(NdOp::INT_RIGHT, ExtSrc,
               {Src, NdVar::cst(ByteOff * 8, Src.Size)});
      }
    }
    if (ElemSz < Dst.Size) {
      NdVar Elem = S.makeTemp(ElemSz);
      S.emit(NdOp::SUBBYTES, Elem, {ExtSrc, NdVar::cst(0, 4)});
      S.emit(NdOp::INT_ZEXT, Dst, {Elem});
    } else {
      S.emit(NdOp::SUBBYTES, Dst, {ExtSrc, NdVar::cst(0, 4)});
    }
    if (IsMem)
      S.storeToMem(X86.operands[0], Dst);
    break;
  }

  // INSERTPS — insert float element with zero mask.
  // imm8[7:6]=src_elem, imm8[5:4]=dst_elem, imm8[3:0]=zero_mask
  case X86_INS_INSERTPS:
  case X86_INS_VINSERTPS: {
    bool IsVEX = (InsnId == X86_INS_VINSERTPS);
    int MinOps = IsVEX ? 4 : 3;
    if (X86.op_count < MinOps)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Base = IsVEX ? operandRead(S, X86.operands[1])
                         : operandRead(S, X86.operands[0]);
    int SrcIdx = IsVEX ? 2 : 1;
    int ImmIdx = IsVEX ? 3 : 2;
    NdVar SrcRaw = operandRead(S, X86.operands[SrcIdx]);
    uint8_t Imm = 0;
    if (ImmIdx < X86.op_count && X86.operands[ImmIdx].type == X86_OP_IMM)
      Imm = static_cast<uint8_t>(X86.operands[ImmIdx].imm);
    unsigned SrcElem = (Imm >> 6) & 0x3;
    unsigned DstElem = (Imm >> 4) & 0x3;
    unsigned ZMask = Imm & 0xF;

    NdVar Elem = S.makeTemp(4);
    if (X86.operands[SrcIdx].type == X86_OP_MEM) {
      S.emit(NdOp::SUBBYTES, Elem, {SrcRaw, NdVar::cst(0, 4)});
    } else if (SrcElem > 0) {
      NdVar Shifted = S.makeTemp(SrcRaw.Size);
      S.emit(NdOp::INT_RIGHT, Shifted,
             {SrcRaw, NdVar::cst(SrcElem * 32, SrcRaw.Size)});
      S.emit(NdOp::SUBBYTES, Elem, {Shifted, NdVar::cst(0, 4)});
    } else {
      S.emit(NdOp::SUBBYTES, Elem, {SrcRaw, NdVar::cst(0, 4)});
    }

    NdVar Lanes[4];
    for (unsigned I = 0; I < 4; ++I) {
      Lanes[I] = S.makeTemp(4);
      if (ZMask & (1U << I)) {
        S.emit(NdOp::COPY, Lanes[I], {NdVar::cst(0, 4)});
      } else if (I == DstElem) {
        S.emit(NdOp::COPY, Lanes[I], {Elem});
      } else {
        S.emit(NdOp::SUBBYTES, Lanes[I], {Base, NdVar::cst(I * 4, 4)});
      }
    }
    NdVar Lo = S.makeTemp(8);
    S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
    NdVar Hi = S.makeTemp(8);
    S.emit(NdOp::CONCAT, Hi, {Lanes[3], Lanes[2]});
    S.emit(NdOp::CONCAT, Dst, {Hi, Lo});
    break;
  }

  // MASKMOVDQU / MASKMOVQ — conditional Masked store to memory via RDI.
  case X86_INS_MASKMOVDQU:
  case X86_INS_MASKMOVQ:
  case X86_INS_VMASKMOVDQU: {
    if (X86.op_count < 2)
      break;
    NdVar Src = operandRead(S, X86.operands[0]);
    NdVar Rdi = NdVar::reg(x86reg::RDI, 8);
    S.emit(NdOp::STORE, {}, {Rdi, Src});
    break;
  }

  // MPSADBW — multiple packed sums of absolute differences.  The immediate
  // control byte selects the source block/offset and MUST be forwarded to the
  // emitter (which maps to @llvm.x86.sse41.mpsadbw); the old handler dropped it
  // and mis-read the imm as the second vector source, so the emitter (which
  // requires the imm) bailed and the result was silently 0.  SSE form is
  // `mpsadbw xmm1,xmm2/m,imm8` (xmm1 is also src1); VEX form adds an explicit
  // src1 operand.
  case X86_INS_MPSADBW:
  case X86_INS_VMPSADBW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    unsigned SrcIdx = (X86.op_count >= 4) ? 1 : 0;
    NdVar Src = operandRead(S, X86.operands[SrcIdx]);
    NdVar Src2 = operandRead(S, X86.operands[SrcIdx + 1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[X86.op_count - 1].imm);
    S.emitIntrinsic(Intrinsic::Mpsadbw, Dst, {Src, Src2, NdVar::cst(Imm, 1)});
    break;
  }

  // PHMINPOSUW — packed horizontal unsigned word minimum + index.
  case X86_INS_PHMINPOSUW:
  case X86_INS_VPHMINPOSUW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Phminposuw, Dst, {Src});
    break;
  }

  // CLFLUSHOPT / CLWB — cache-line flush/write-back extensions.
  case X86_INS_CLFLUSHOPT:
    S.emitIntrinsic(Intrinsic::Clflushopt);
    break;
  case X86_INS_CLWB:
    S.emitIntrinsic(Intrinsic::Clwb);
    break;

  // PREFETCHW / PREFETCHWT1 — prefetch with write intent.
  case X86_INS_PREFETCHW:
  case X86_INS_PREFETCHWT1:
    S.emitIntrinsic(Intrinsic::Prefetch);
    break;

  // ========================================================================
  // P1: AVX-512 k-Mask register operations (opmask k0-k7).
  // ========================================================================

  // KAND{B,W,D,Q} — Mask AND
  case X86_INS_KANDB:
  case X86_INS_KANDW:
  case X86_INS_KANDD:
  case X86_INS_KANDQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_AND, Dst, {A, B});
    break;
  }

  // KANDN{B,W,D,Q} — Mask AND-NOT
  case X86_INS_KANDNB:
  case X86_INS_KANDNW:
  case X86_INS_KANDND:
  case X86_INS_KANDNQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    NdVar NotA = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NotA, {A});
    S.emit(NdOp::INT_AND, Dst, {NotA, B});
    break;
  }

  // KOR{B,W,D,Q} — Mask OR
  case X86_INS_KORB:
  case X86_INS_KORW:
  case X86_INS_KORD:
  case X86_INS_KORQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_OR, Dst, {A, B});
    break;
  }

  // KXOR{B,W,D,Q} — Mask XOR
  case X86_INS_KXORB:
  case X86_INS_KXORW:
  case X86_INS_KXORD:
  case X86_INS_KXORQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    break;
  }

  // KXNOR{B,W,D,Q} — Mask XNOR
  case X86_INS_KXNORB:
  case X86_INS_KXNORW:
  case X86_INS_KXNORD:
  case X86_INS_KXNORQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    NdVar Xored = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_XOR, Xored, {A, B});
    S.emit(NdOp::INT_NOT, Dst, {Xored});
    break;
  }

  // KNOT{B,W,D,Q} — Mask NOT
  case X86_INS_KNOTB:
  case X86_INS_KNOTW:
  case X86_INS_KNOTD:
  case X86_INS_KNOTQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::INT_NOT, Dst, {Src});
    break;
  }

  // KMOV{B,W,D,Q} — Mask move
  case X86_INS_KMOVB:
  case X86_INS_KMOVW:
  case X86_INS_KMOVD:
  case X86_INS_KMOVQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // KSHIFTL{B,W,D,Q} — Mask shift left
  case X86_INS_KSHIFTLB:
  case X86_INS_KSHIFTLW:
  case X86_INS_KSHIFTLD:
  case X86_INS_KSHIFTLQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Cnt = operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_LEFT, Dst, {Src, Cnt});
    break;
  }

  // KSHIFTR{B,W,D,Q} — Mask shift right
  case X86_INS_KSHIFTRB:
  case X86_INS_KSHIFTRW:
  case X86_INS_KSHIFTRD:
  case X86_INS_KSHIFTRQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Cnt = operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_RIGHT, Dst, {Src, Cnt});
    break;
  }

  // KTEST{B,W,D,Q} — Mask test (set ZF/CF from AND of two Mask regs).
  case X86_INS_KTESTB:
  case X86_INS_KTESTW:
  case X86_INS_KTESTD:
  case X86_INS_KTESTQ: {
    if (X86.op_count < 2)
      break;
    NdVar A = operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[1]);
    NdVar Masked = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, Masked, {A, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Masked, NdVar::cst(0, A.Size)});
    NdVar NotA = S.makeTemp(A.Size);
    S.emit(NdOp::INT_NOT, NotA, {A});
    NdVar AndNot = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, AndNot, {NotA, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
           {AndNot, NdVar::cst(0, A.Size)});
    break;
  }

  // KORTEST{B,W,D,Q} — Mask OR test (ZF = (src1|src2)==0, CF =
  // (src1|src2)==all1s).
  case X86_INS_KORTESTB:
  case X86_INS_KORTESTW:
  case X86_INS_KORTESTD:
  case X86_INS_KORTESTQ: {
    if (X86.op_count < 2)
      break;
    NdVar A = operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[1]);
    NdVar Ored = S.makeTemp(A.Size);
    S.emit(NdOp::INT_OR, Ored, {A, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Ored, NdVar::cst(0, A.Size)});
    // CF = 1 iff all Bits of (src1|src2) are set, i.e. ~(ored) == 0.
    NdVar NotOred = S.makeTemp(A.Size);
    S.emit(NdOp::INT_NOT, NotOred, {Ored});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
           {NotOred, NdVar::cst(0, A.Size)});
    break;
  }

  // KUNPCK{BW,WD,DQ} — Mask unpack (concatenate low halves).
  case X86_INS_KUNPCKBW:
  case X86_INS_KUNPCKWD:
  case X86_INS_KUNPCKDQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Kunpck, Dst, {A, B});
    break;
  }

  // KADD{B,W,D,Q} — Mask add
  case X86_INS_KADDB:
  case X86_INS_KADDW:
  case X86_INS_KADDD:
  case X86_INS_KADDQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    break;
  }

  // ========================================================================
  // P1: AVX-512 packed VP* integer instructions.
  // ========================================================================

  // VPAND{D,Q} / VPOR{D,Q} / VPXOR{D,Q} — AVX-512 bitwise (EVEX).
  case X86_INS_VPANDD:
  case X86_INS_VPANDQ:
  case X86_INS_VPORD:
  case X86_INS_VPORQ:
  case X86_INS_VPXORD:
  case X86_INS_VPXORQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? operandRead(S, X86.operands[1])
                                    : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    NdOp Opc;
    switch (InsnId) {
    case X86_INS_VPANDD:
    case X86_INS_VPANDQ:
      Opc = NdOp::INT_AND;
      break;
    case X86_INS_VPORD:
    case X86_INS_VPORQ:
      Opc = NdOp::INT_OR;
      break;
    default:
      Opc = NdOp::INT_XOR;
    }
    S.emit(Opc, Dst, {A, B});
    break;
  }

  // VPANDN{D,Q} — AVX-512 AND-NOT.
  case X86_INS_VPANDND:
  case X86_INS_VPANDNQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    NdVar NotA = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NotA, {A});
    S.emit(NdOp::INT_AND, Dst, {NotA, B});
    break;
  }

  // VPTERNLOG{D,Q} — ternary logic (imm8 truth table).
  case X86_INS_VPTERNLOGD:
  case X86_INS_VPTERNLOGQ: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    NdVar Imm = operandRead(S, X86.operands[3]);
    S.emitIntrinsic(Intrinsic::Vpternlog, Dst, {Dst, A, B, Imm});
    break;
  }

  // VPCOMPRESS{B,W,D,Q} — compress active elements under Mask.
  case X86_INS_VPCOMPRESSB:
  case X86_INS_VPCOMPRESSW:
  case X86_INS_VPCOMPRESSD:
  case X86_INS_VPCOMPRESSQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Vpcompress, Dst, {Src});
    break;
  }

  // VPEXPAND{B,W,D,Q} — expand active elements under Mask.
  case X86_INS_VPEXPANDB:
  case X86_INS_VPEXPANDW:
  case X86_INS_VPEXPANDD:
  case X86_INS_VPEXPANDQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Vpexpand, Dst, {Src});
    break;
  }

  // VPSCATTER{DD,DQ,QD,QQ} — scatter store to memory via index vector.
  case X86_INS_VPSCATTERDD:
  case X86_INS_VPSCATTERDQ:
  case X86_INS_VPSCATTERQD:
  case X86_INS_VPSCATTERQQ: {
    if (X86.op_count >= 2) {
      NdVar Dst = operandWrite(X86.operands[0]);
      NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
      S.emitIntrinsic(Intrinsic::Vpscatter, Dst, {Src});
    }
    break;
  }

  // VPGATHER{DD,DQ,QD,QQ} — gather load from memory via index vector.
  case X86_INS_VPGATHERDD:
  case X86_INS_VPGATHERDQ:
  case X86_INS_VPGATHERQD:
  case X86_INS_VPGATHERQQ: {
    if (!liftVectorGather(S, X86, InsnId) && X86.op_count >= 2) {
      NdVar Dst = operandWrite(X86.operands[0]);
      NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
      S.emitIntrinsic(Intrinsic::Vpgather, Dst, {Src});
    }
    break;
  }

  // VPOPCNT{B,W,D,Q} — packed population count.
  case X86_INS_VPOPCNTB:
  case X86_INS_VPOPCNTW:
  case X86_INS_VPOPCNTD:
  case X86_INS_VPOPCNTQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::POPCOUNT, Dst, {Src});
    break;
  }

  // VPLZCNT{D,Q} — packed leading zero count.
  case X86_INS_VPLZCNTD:
  case X86_INS_VPLZCNTQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::LZCOUNT, Dst, {Src});
    break;
  }

  // VPCONFLICT{D,Q} — conflict detection (per-Lane broadcast equality test).
  case X86_INS_VPCONFLICTD:
  case X86_INS_VPCONFLICTQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Vpconflict, Dst, {Src});
    break;
  }

  // VPTESTM{B,W,D,Q} / VPTESTNM{B,W,D,Q} — test Mask Bits.
  case X86_INS_VPTESTMB:
  case X86_INS_VPTESTMW:
  case X86_INS_VPTESTMD:
  case X86_INS_VPTESTMQ:
  case X86_INS_VPTESTNMB:
  case X86_INS_VPTESTNMW:
  case X86_INS_VPTESTNMD:
  case X86_INS_VPTESTNMQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_AND, Dst, {A, B});
    break;
  }

  // VPROL{D,Q} / VPROR{D,Q} — packed rotate by immediate.
  case X86_INS_VPROLD:
  case X86_INS_VPROLQ:
  case X86_INS_VPRORD:
  case X86_INS_VPRORQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Cnt = operandRead(S, X86.operands[2]);
    uint16_t Bits = Dst.Size * 8;
    bool IsLeft = (InsnId == X86_INS_VPROLD || InsnId == X86_INS_VPROLQ);
    NdVar APart = S.makeTemp(Dst.Size);
    NdVar BPart = S.makeTemp(Dst.Size);
    NdVar Comp = S.makeTemp(Dst.Size);
    S.emit(IsLeft ? NdOp::INT_LEFT : NdOp::INT_RIGHT, APart, {Src, Cnt});
    S.emit(NdOp::INT_SUB, Comp, {NdVar::cst(Bits, Dst.Size), Cnt});
    S.emit(IsLeft ? NdOp::INT_RIGHT : NdOp::INT_LEFT, BPart, {Src, Comp});
    S.emit(NdOp::INT_OR, Dst, {APart, BPart});
    break;
  }

  // VPROLV{D,Q} / VPRORV{D,Q} — packed variable rotate.
  case X86_INS_VPROLVD:
  case X86_INS_VPROLVQ:
  case X86_INS_VPRORVD:
  case X86_INS_VPRORVQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Cnt = operandRead(S, X86.operands[2]);
    uint16_t Bits = Dst.Size * 8;
    bool IsLeft = (InsnId == X86_INS_VPROLVD || InsnId == X86_INS_VPROLVQ);
    NdVar APart = S.makeTemp(Dst.Size);
    NdVar BPart = S.makeTemp(Dst.Size);
    NdVar Comp = S.makeTemp(Dst.Size);
    S.emit(IsLeft ? NdOp::INT_LEFT : NdOp::INT_RIGHT, APart, {Src, Cnt});
    S.emit(NdOp::INT_SUB, Comp, {NdVar::cst(Bits, Dst.Size), Cnt});
    S.emit(IsLeft ? NdOp::INT_RIGHT : NdOp::INT_LEFT, BPart, {Src, Comp});
    S.emit(NdOp::INT_OR, Dst, {APart, BPart});
    break;
  }

  // VPSRAV{W,Q} / VPSLLV{W} / VPSRLV{W} — variable shifts (additional widths).
  case X86_INS_VPSRAVW:
  case X86_INS_VPSRAVQ:
  case X86_INS_VPSLLVW:
  case X86_INS_VPSRLVW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    NdOp Opc;
    switch (InsnId) {
    case X86_INS_VPSRAVW:
    case X86_INS_VPSRAVQ:
      Opc = NdOp::INT_ASHR;
      break;
    case X86_INS_VPSLLVW:
      Opc = NdOp::INT_LEFT;
      break;
    default:
      Opc = NdOp::INT_RIGHT;
    }
    S.emit(Opc, Dst, {A, B});
    break;
  }

  // VPMULL{D,W} / VPMULH{W,UW} / VPMUL{DQ,UDQ} — packed multiply per-lane.
  case X86_INS_VPMULLD:
  case X86_INS_VPMULLW:
  case X86_INS_VPMULHW:
  case X86_INS_VPMULHUW:
  case X86_INS_VPMULDQ:
  case X86_INS_VPMULUDQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? operandRead(S, X86.operands[1])
                                    : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 4;
    if (InsnId == X86_INS_VPMULLW || InsnId == X86_INS_VPMULHW ||
        InsnId == X86_INS_VPMULHUW)
      LaneSz = 2;
    else if (InsnId == X86_INS_VPMULDQ || InsnId == X86_INS_VPMULUDQ)
      LaneSz = 8;
    if (Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      bool IsHigh = (InsnId == X86_INS_VPMULHW || InsnId == X86_INS_VPMULHUW);
      bool IsWidening =
          (InsnId == X86_INS_VPMULDQ || InsnId == X86_INS_VPMULUDQ);
      bool IsSigned = (InsnId == X86_INS_VPMULHW || InsnId == X86_INS_VPMULDQ);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        // The widening forms consume every other narrow element, so both forms
        // step one destination lane per iteration.
        unsigned SrcLaneSz = IsWidening ? LaneSz / 2 : LaneSz;
        unsigned SrcOff = I * LaneSz;
        NdVar La = S.makeTemp(SrcLaneSz);
        NdVar Lb = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(SrcOff, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(SrcOff, 4)});
        NdVar Lr;
        if (IsHigh) {
          unsigned WideSz = LaneSz * 2;
          NdVar WA = S.makeTemp(WideSz);
          NdVar WB = S.makeTemp(WideSz);
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WA, {La});
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WB, {Lb});
          NdVar WR = S.makeTemp(WideSz);
          S.emit(NdOp::INT_MULT, WR, {WA, WB});
          Lr = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Lr, {WR, NdVar::cst(LaneSz, 4)});
        } else if (IsWidening) {
          NdVar WA = S.makeTemp(LaneSz);
          NdVar WB = S.makeTemp(LaneSz);
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WA, {La});
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WB, {Lb});
          Lr = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_MULT, Lr, {WA, WB});
        } else {
          Lr = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_MULT, Lr, {La, Lb});
        }
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
      S.emit(NdOp::INT_MULT, Dst, {A, B});
    }
    break;
  }

  // VPABSQ — packed absolute value (qword).
  case X86_INS_VPABSQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VPBLENDM{B,W,D,Q} — Masked blend.
  case X86_INS_VPBLENDMB:
  case X86_INS_VPBLENDMW:
  case X86_INS_VPBLENDMD:
  case X86_INS_VPBLENDMQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VPMOV{DB,DW,QB,QW,QD,WB} — packed truncate (down-convert).
  case X86_INS_VPMOVDB:
  case X86_INS_VPMOVDW:
  case X86_INS_VPMOVQB:
  case X86_INS_VPMOVQW:
  case X86_INS_VPMOVQD:
  case X86_INS_VPMOVWB:
  case X86_INS_VPMOVSDB:
  case X86_INS_VPMOVSDW:
  case X86_INS_VPMOVSQB:
  case X86_INS_VPMOVSQW:
  case X86_INS_VPMOVSQD:
  case X86_INS_VPMOVSWB:
  case X86_INS_VPMOVUSDB:
  case X86_INS_VPMOVUSDW:
  case X86_INS_VPMOVUSQB:
  case X86_INS_VPMOVUSQW:
  case X86_INS_VPMOVUSQD:
  case X86_INS_VPMOVUSWB: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VPMOVM2{B,W,D,Q} — Mask to vector.
  case X86_INS_VPMOVM2B:
  case X86_INS_VPMOVM2W:
  case X86_INS_VPMOVM2D:
  case X86_INS_VPMOVM2Q: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VPMOV{B,W,D,Q}2M — vector to Mask.
  case X86_INS_VPMOVB2M:
  case X86_INS_VPMOVW2M:
  case X86_INS_VPMOVD2M:
  case X86_INS_VPMOVQ2M: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VPBROADCASTM{B2Q,W2D} — broadcast Mask to vector.
  case X86_INS_VPBROADCASTMB2Q:
  case X86_INS_VPBROADCASTMW2D: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VPSHLD{D,Q,W} / VPSHRD{D,Q,W} — double shift by immediate.
  case X86_INS_VPSHLDD:
  case X86_INS_VPSHLDQ:
  case X86_INS_VPSHLDW:
  case X86_INS_VPSHRDD:
  case X86_INS_VPSHRDQ:
  case X86_INS_VPSHRDW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    bool IsLeft = (InsnId == X86_INS_VPSHLDD || InsnId == X86_INS_VPSHLDQ ||
                   InsnId == X86_INS_VPSHLDW);
    S.emit(IsLeft ? NdOp::INT_LEFT : NdOp::INT_RIGHT, Dst, {A, B});
    break;
  }

  // VPSHLDV{D,Q,W} / VPSHRDV{D,Q,W} — variable double shift.
  case X86_INS_VPSHLDVD:
  case X86_INS_VPSHLDVQ:
  case X86_INS_VPSHLDVW:
  case X86_INS_VPSHRDVD:
  case X86_INS_VPSHRDVQ:
  case X86_INS_VPSHRDVW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    bool IsLeft = (InsnId == X86_INS_VPSHLDVD || InsnId == X86_INS_VPSHLDVQ ||
                   InsnId == X86_INS_VPSHLDVW);
    S.emit(IsLeft ? NdOp::INT_LEFT : NdOp::INT_RIGHT, Dst, {A, B});
    break;
  }

  // VPDPBUSD{,S} / VPDPWSSD{,S} — VNNI dot product.
  case X86_INS_VPDPBUSD:
  case X86_INS_VPDPBUSDS:
  case X86_INS_VPDPWSSD:
  case X86_INS_VPDPWSSDS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_MULT, Dst, {A, B});
    break;
  }

  // VPMADD52{H,L}UQ — packed multiply-add 52-bit unsigned.
  case X86_INS_VPMADD52HUQ:
  case X86_INS_VPMADD52LUQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_MULT, Dst, {A, B});
    break;
  }

  // VPSHUFBITQMB — shuffle Bits into Mask register.
  case X86_INS_VPSHUFBITQMB: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Vpshufbitqmb, Dst, {A, B});
    break;
  }

  // VPERMB / VPERMW — packed byte/word permute.
  case X86_INS_VPERMB:
  case X86_INS_VPERMW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VPERM{I2,T2}{B,D,PS,PD,Q,W} already handled above in permute section.

  // VPMULTISHIFTQB — multi-shift bytes from qwords.
  case X86_INS_VPMULTISHIFTQB: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Vpmultishiftqb, Dst, {A, B});
    break;
  }

  // ========================================================================
  // P1: AVX/AVX-512 other V* instructions — VCVT*, VRANGE*, VSCALEF*,
  //     VGETEXP*, VGETMANT*, VREDUCE*, VRNDSCALE*, VFIXUPIMM*, VFPCLASS*,
  //     VBROADCAST (EVEX), VINSERT/VEXTRACT (EVEX), VCOMPRESS/VEXPAND (float).
  // ========================================================================

  // VCVT — integer ↔ float conversions (AVX/AVX-512).
  // Packed int->FP `vcvtdq2ps/pd ymm/xmm` (128- or 256-bit): convert each i32
  // lane independently.  PS keeps lane count == dword count; PD widens the low
  // dwords to f64 (dst f64-lane count).  Lifting this as a single bulk
  // FLOAT_INT2FLOAT (the old scalar-shared path) treats the whole i128/i256 as
  // one integer and is wrong.
  case X86_INS_VCVTDQ2PS:
  case X86_INS_VCVTDQ2PD: {
    if (X86.op_count < 2)
      break;
    bool IsPD = (Insn->id == X86_INS_VCVTDQ2PD);
    unsigned FPSz = IsPD ? 8 : 4;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned NLanes = Dst.Size / FPSz;
    if (NLanes < 1) {
      S.emit(NdOp::FLOAT_INT2FLOAT, Dst, {Src});
      break;
    }
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Elem = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(I * 4, 4)});
      NdVar Lane = S.makeTemp(FPSz);
      S.emit(NdOp::FLOAT_INT2FLOAT, Lane, {Elem});
      if (I == 0) {
        Acc = Lane;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + FPSz);
        S.emit(NdOp::CONCAT, Next, {Lane, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // Scalar VEX int->FP `vcvtsi2ss/sd xmm1, xmm2, r/m`: the converted scalar
  // goes in the low element (float or double) and the upper bits come from
  // xmm2.  The result temp must be the real FP width, otherwise the emitter
  // infers the type from the wide destination and produces a double for a
  // single-precision convert.
  case X86_INS_VCVTSI2SS:
  case X86_INS_VCVTSI2SD:
  case X86_INS_VCVTUSI2SS:
  case X86_INS_VCVTUSI2SD: {
    if (X86.op_count < 2)
      break;
    bool ToDouble =
        (Insn->id == X86_INS_VCVTSI2SD || Insn->id == X86_INS_VCVTUSI2SD);
    bool Unsigned =
        (Insn->id == X86_INS_VCVTUSI2SS || Insn->id == X86_INS_VCVTUSI2SD);
    unsigned DstFPSz = ToDouble ? 8 : 4;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Upper = (X86.op_count >= 3) ? operandRead(S, X86.operands[1]) : Dst;
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    NdVar Tmp = S.makeTemp(DstFPSz);
    S.emit(Unsigned ? NdOp::FLOAT_UINT2FLOAT : NdOp::FLOAT_INT2FLOAT, Tmp,
           {Src});
    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], Tmp);
    } else if (Dst.Size > DstFPSz) {
      NdVar Hi = S.makeTemp(Dst.Size - DstFPSz);
      S.emit(NdOp::SUBBYTES, Hi, {Upper, NdVar::cst(DstFPSz, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Tmp});
    } else {
      S.emit(NdOp::COPY, Dst, {Tmp});
    }
    break;
  }
  // Packed int->FP (AVX-512 VL); kept as a bulk convert pending per-lane
  // support (these EVEX forms are not modeled by Unicorn for roundtrip).
  case X86_INS_VCVTUDQ2PS:
  case X86_INS_VCVTUDQ2PD:
  case X86_INS_VCVTQQ2PS:
  case X86_INS_VCVTQQ2PD:
  case X86_INS_VCVTUQQ2PS:
  case X86_INS_VCVTUQQ2PD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::FLOAT_INT2FLOAT, Dst, {Src});
    break;
  }

  case X86_INS_VCVTPS2DQ:
  case X86_INS_VCVTPD2DQ:
  case X86_INS_VCVTTPS2DQ:
  case X86_INS_VCVTTPD2DQ:
  case X86_INS_VCVTSS2SI:
  case X86_INS_VCVTSD2SI:
  case X86_INS_VCVTTSS2SI:
  case X86_INS_VCVTTSD2SI:
  case X86_INS_VCVTPS2UDQ:
  case X86_INS_VCVTPD2UDQ:
  case X86_INS_VCVTTPS2UDQ:
  case X86_INS_VCVTTPD2UDQ:
  case X86_INS_VCVTSS2USI:
  case X86_INS_VCVTSD2USI:
  case X86_INS_VCVTTSS2USI:
  case X86_INS_VCVTTSD2USI:
  case X86_INS_VCVTPS2QQ:
  case X86_INS_VCVTPD2QQ:
  case X86_INS_VCVTTPS2QQ:
  case X86_INS_VCVTTPD2QQ:
  case X86_INS_VCVTPS2UQQ:
  case X86_INS_VCVTPD2UQQ:
  case X86_INS_VCVTTPS2UQQ:
  case X86_INS_VCVTTPD2UQQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::FLOAT_TRUNC, Dst, {Src});
    break;
  }

  // Scalar VEX precision convert `vcvtXX2YY xmm1, xmm2, xmm3/m`: the converted
  // scalar goes in the low element and the upper bits come from xmm2.  Narrow
  // the convert source to its real FP width so the emitter does not mis-infer
  // the type from the full vector and pick the wrong direction.
  case X86_INS_VCVTSS2SD:
  case X86_INS_VCVTSD2SS: {
    if (X86.op_count < 2)
      break;
    bool ToDouble = (Insn->id == X86_INS_VCVTSS2SD);
    unsigned SrcFPSz = ToDouble ? 4 : 8;
    unsigned DstFPSz = ToDouble ? 8 : 4;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    NdVar Upper = (X86.op_count >= 3) ? operandRead(S, X86.operands[1]) : Dst;
    if (Src.Size > SrcFPSz) {
      NdVar N = S.makeTemp(SrcFPSz);
      S.emit(NdOp::SUBBYTES, N, {Src, NdVar::cst(0, 4)});
      Src = N;
    }
    NdVar Tmp = S.makeTemp(DstFPSz);
    S.emit(NdOp::FLOAT_FLOAT2FLOAT, Tmp, {Src});
    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], Tmp);
    } else if (Dst.Size > DstFPSz) {
      NdVar Hi = S.makeTemp(Dst.Size - DstFPSz);
      S.emit(NdOp::SUBBYTES, Hi, {Upper, NdVar::cst(DstFPSz, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Tmp});
    } else {
      S.emit(NdOp::COPY, Dst, {Tmp});
    }
    break;
  }
  // Packed widen single->double (per dst lane); reads the low single lanes.
  case X86_INS_VCVTPS2PD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned NLanes = Dst.Size / 8;
    if (NLanes < 1) {
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Dst, {Src});
      break;
    }
    NdVar Cur;
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar E = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, E, {Src, NdVar::cst(I * 4, 4)});
      NdVar L = S.makeTemp(8);
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, L, {E});
      if (I == 0) {
        Cur = L;
      } else {
        NdVar Next = S.makeTemp((I + 1) * 8);
        S.emit(NdOp::CONCAT, Next, {L, Cur});
        Cur = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Cur});
    break;
  }
  // Packed narrow double->single (per src lane); upper dst lanes are zeroed.
  case X86_INS_VCVTPD2PS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned NLanes = Src.Size / 8;
    if (NLanes < 1) {
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Dst, {Src});
      break;
    }
    NdVar Cur;
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar E = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, E, {Src, NdVar::cst(I * 8, 4)});
      NdVar L = S.makeTemp(4);
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, L, {E});
      if (I == 0) {
        Cur = L;
      } else {
        NdVar Next = S.makeTemp((I + 1) * 4);
        S.emit(NdOp::CONCAT, Next, {L, Cur});
        Cur = Next;
      }
    }
    if (Dst.Size > NLanes * 4) {
      NdVar ZHi = S.makeTemp(Dst.Size - NLanes * 4);
      S.emit(NdOp::COPY, ZHi,
             {NdVar::cst(0, (uint16_t)(Dst.Size - NLanes * 4))});
      S.emit(NdOp::CONCAT, Dst, {ZHi, Cur});
    } else {
      S.emit(NdOp::COPY, Dst, {Cur});
    }
    break;
  }
  // Half-precision pack/unpack: the FP emitter models only float<->double, so
  // these keep the bulk-convert form (no f16 path yet).
  case X86_INS_VCVTPH2PS:
  case X86_INS_VCVTPS2PH: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::FLOAT_FLOAT2FLOAT, Dst, {Src});
    break;
  }

  // VRANGE{PS,PD,SS,SD} — float range restriction (imm8-controlled min/max).
  case X86_INS_VRANGEPS:
  case X86_INS_VRANGEPD:
  case X86_INS_VRANGESS:
  case X86_INS_VRANGESD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Vrange, Dst, {A, B});
    break;
  }

  // VSCALEF{PS,PD,SS,SD} — float * 2^int_src (scale by power of 2).
  case X86_INS_VSCALEFPS:
  case X86_INS_VSCALEFPD:
  case X86_INS_VSCALEFSS:
  case X86_INS_VSCALEFSD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Vscalef, Dst, {A, B});
    break;
  }

  // VGETEXP{PS,PD,SS,SD} — extract float exponent (IEEE binary).
  case X86_INS_VGETEXPPS:
  case X86_INS_VGETEXPPD:
  case X86_INS_VGETEXPSS:
  case X86_INS_VGETEXPSD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emitIntrinsic(Intrinsic::Vgetexp, Dst, {Src});
    break;
  }

  // VGETMANT{PS,PD,SS,SD} — get float mantissa (IEEE binary).
  case X86_INS_VGETMANTPS:
  case X86_INS_VGETMANTPD:
  case X86_INS_VGETMANTSS:
  case X86_INS_VGETMANTSD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emitIntrinsic(Intrinsic::Vgetmant, Dst, {Src});
    break;
  }

  // VREDUCE{PS,PD,SS,SD} — float range reduction per imm8.
  case X86_INS_VREDUCEPS:
  case X86_INS_VREDUCEPD:
  case X86_INS_VREDUCESS:
  case X86_INS_VREDUCESD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emitIntrinsic(Intrinsic::Vreduce, Dst, {Src});
    break;
  }

  // VRNDSCALE{PS,PD,SS,SD} — round to given scale. Map to FLOAT_ROUND.
  case X86_INS_VRNDSCALEPS:
  case X86_INS_VRNDSCALEPD:
  case X86_INS_VRNDSCALESS:
  case X86_INS_VRNDSCALESD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::FLOAT_ROUND, Dst, {Src});
    break;
  }

  // VFIXUPIMM{PS,PD,SS,SD} — fix up float special values per imm8 table.
  case X86_INS_VFIXUPIMMPS:
  case X86_INS_VFIXUPIMMPD:
  case X86_INS_VFIXUPIMMSS:
  case X86_INS_VFIXUPIMMSD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Vfixupimm, Dst, {A, B});
    break;
  }

  // VFPCLASS{PS,PD,SS,SD} — float classification test → Mask register.
  case X86_INS_VFPCLASSPS:
  case X86_INS_VFPCLASSPD:
  case X86_INS_VFPCLASSSS:
  case X86_INS_VFPCLASSSD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Vfpclass, Dst, {Src});
    break;
  }

  // VBROADCAST (EVEX 512-bit variants).
  case X86_INS_VBROADCASTF32X2:
  case X86_INS_VBROADCASTF32X4:
  case X86_INS_VBROADCASTF32X8:
  case X86_INS_VBROADCASTF64X2:
  case X86_INS_VBROADCASTF64X4:
  case X86_INS_VBROADCASTI32X2:
  case X86_INS_VBROADCASTI32X4:
  case X86_INS_VBROADCASTI32X8:
  case X86_INS_VBROADCASTI64X2:
  case X86_INS_VBROADCASTI64X4: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VINSERT{F,I}{32X4,32X8,64X2,64X4} — insert 128/256 Lane into ZMM.
  case X86_INS_VINSERTF32X4:
  case X86_INS_VINSERTF32X8:
  case X86_INS_VINSERTF64X2:
  case X86_INS_VINSERTF64X4:
  case X86_INS_VINSERTI32X4:
  case X86_INS_VINSERTI32X8:
  case X86_INS_VINSERTI64X2:
  case X86_INS_VINSERTI64X4: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VEXTRACT{F,I}{32X4,32X8,64X2,64X4} — extract 128/256 Lane from ZMM.
  case X86_INS_VEXTRACTF32X4:
  case X86_INS_VEXTRACTF32X8:
  case X86_INS_VEXTRACTF64X2:
  case X86_INS_VEXTRACTF64X4:
  case X86_INS_VEXTRACTI32X4:
  case X86_INS_VEXTRACTI32X8:
  case X86_INS_VEXTRACTI64X2:
  case X86_INS_VEXTRACTI64X4: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VCOMPRESS{PS,PD} / VEXPAND{PS,PD} — float compress/expand.
  case X86_INS_VCOMPRESSPS:
  case X86_INS_VCOMPRESSPD:
  case X86_INS_VEXPANDPS:
  case X86_INS_VEXPANDPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VDBPSADBW — double block packed sums of absolute differences.
  case X86_INS_VDBPSADBW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Vdbpsadbw, Dst, {A, B});
    break;
  }

  // VRSQRT14{PS,PD,SS,SD} / VRCP14{PS,PD,SS,SD} — reciprocal sqrt/reciprocal
  // (14-bit approx).
  case X86_INS_VRSQRT14PS:
  case X86_INS_VRSQRT14PD:
  case X86_INS_VRSQRT14SS:
  case X86_INS_VRSQRT14SD:
  case X86_INS_VRCP14PS:
  case X86_INS_VRCP14PD:
  case X86_INS_VRCP14SS:
  case X86_INS_VRCP14SD:
  case X86_INS_VRSQRT28PS:
  case X86_INS_VRSQRT28PD:
  case X86_INS_VRSQRT28SS:
  case X86_INS_VRSQRT28SD:
  case X86_INS_VRCP28PS:
  case X86_INS_VRCP28PD:
  case X86_INS_VRCP28SS:
  case X86_INS_VRCP28SD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::FLOAT_SQRT, Dst, {Src});
    break;
  }

  // VEXP2{PS,PD} — base-2 exponential approximation (28-bit).
  case X86_INS_VEXP2PS:
  case X86_INS_VEXP2PD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emitIntrinsic(Intrinsic::Vexp2, Dst, {Src});
    break;
  }

  // V4FMA{DD,PS}SS / V4FNMA{DD,PS}SS — quad FMA.
  case X86_INS_V4FMADDPS:
  case X86_INS_V4FMADDSS:
  case X86_INS_V4FNMADDPS:
  case X86_INS_V4FNMADDSS: {
    if (X86.op_count >= 2) {
      NdVar Dst = operandWrite(X86.operands[0]);
      NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
      S.emit(NdOp::FLOAT_ADD, Dst, {Dst, Src});
    }
    break;
  }

  // VANDNPS / VANDNPD (AVX/AVX-512 bitwise AND-NOT float).
  case X86_INS_VANDNPS:
  case X86_INS_VANDNPD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    NdVar NotA = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NotA, {A});
    S.emit(NdOp::INT_AND, Dst, {NotA, B});
    break;
  }

  // VUNPCKLPS/PD / VUNPCKHPS/PD — AVX unpack interleave.  The old handler just
  // COPYed the last source, dropping the interleave entirely (a 3-operand
  // `vunpcklps %xmm2,%xmm1,%xmm0` became `xmm0 = xmm2`).  Route through the
  // same unpack intrinsic as the legacy SSE form with (src1, src2) = (op1, op2)
  // for the VEX 3-operand encoding (2-operand fallback: dst is also src1).
  case X86_INS_VUNPCKLPS:
  case X86_INS_VUNPCKLPD:
  case X86_INS_VUNPCKHPS:
  case X86_INS_VUNPCKHPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src1 = operandRead(S, X86.operands[X86.op_count >= 3 ? 1 : 0]);
    NdVar Src2 = operandRead(S, X86.operands[X86.op_count - 1]);
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_VUNPCKLPS:
      Id = Intrinsic::Unpcklps;
      break;
    case X86_INS_VUNPCKHPS:
      Id = Intrinsic::Unpckhps;
      break;
    case X86_INS_VUNPCKLPD:
      Id = Intrinsic::Unpcklpd;
      break;
    default:
      Id = Intrinsic::Unpckhpd;
      break;
    }
    S.emitIntrinsic(Id, Dst, {Src1, Src2});
    break;
  }

  // VMOVHPS/VMOVHPD/VMOVLPS/VMOVLPD — partial 64-bit moves.  The old handler
  // did a flat `COPY Dst, last-operand` for everything, which (a) dropped the
  // non-destructive merge source on the 3-operand load form, (b) silently
  // dropped the memory write on the 2-operand store form (operandWrite() of a
  // MEM operand is a discarded ram(0) placeholder), and (c) stored/loaded the
  // wrong half for the HIGH variants.
  //   store (2 ops): m64 = xmm[selected half]
  //   load  (3 ops): dst = merge(src1, m64) keeping src1's other half
  case X86_INS_VMOVHPS:
  case X86_INS_VMOVHPD:
  case X86_INS_VMOVLPS:
  case X86_INS_VMOVLPD: {
    if (X86.op_count < 2)
      break;
    bool IsHigh = (InsnId == X86_INS_VMOVHPS || InsnId == X86_INS_VMOVHPD);
    if (X86.operands[0].type == X86_OP_MEM) {
      NdVar Src = operandRead(S, X86.operands[1]);
      NdVar Half = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Half, {Src, NdVar::cst(IsHigh ? 8 : 0, 4)});
      S.storeToMem(X86.operands[0], Half);
    } else {
      // Load form is dst,src1,m64 — a narrower operand list would read a stale
      // operands[2] slot.
      if (X86.op_count < 3)
        break;
      NdVar Dst = operandWrite(X86.operands[0]);
      NdVar Src1 = operandRead(S, X86.operands[1]);
      NdVar Mem = operandRead(S, X86.operands[2]);
      NdVar MemLo = Mem;
      if (Mem.Size != 8) {
        MemLo = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, MemLo, {Mem, NdVar::cst(0, 4)});
      }
      if (IsHigh) {
        // high = m64, low = src1.low → {src1.lo, m64}.
        NdVar Lo = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, Lo, {Src1, NdVar::cst(0, 4)});
        S.emit(NdOp::CONCAT, Dst, {MemLo, Lo});
      } else {
        // low = m64, high = src1.high → {m64, src1.hi}.
        NdVar Hi = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, Hi, {Src1, NdVar::cst(8, 4)});
        S.emit(NdOp::CONCAT, Dst, {Hi, MemLo});
      }
    }
    break;
  }

  // VMOVLHPS xmm1,xmm2,xmm3 → { xmm2[63:0],   xmm3[63:0]  }
  // VMOVHLPS xmm1,xmm2,xmm3 → { xmm3[127:64], xmm2[127:64]}
  case X86_INS_VMOVLHPS:
  case X86_INS_VMOVHLPS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src1 = operandRead(S, X86.operands[1]);
    NdVar Src2 = operandRead(S, X86.operands[2]);
    if (InsnId == X86_INS_VMOVLHPS) {
      NdVar S1Lo = S.makeTemp(8), S2Lo = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, S1Lo, {Src1, NdVar::cst(0, 4)});
      S.emit(NdOp::SUBBYTES, S2Lo, {Src2, NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {S2Lo, S1Lo}); // {lo=src1.lo, hi=src2.lo}
    } else {
      NdVar S1Hi = S.makeTemp(8), S2Hi = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, S1Hi, {Src1, NdVar::cst(8, 4)});
      S.emit(NdOp::SUBBYTES, S2Hi, {Src2, NdVar::cst(8, 4)});
      S.emit(NdOp::CONCAT, Dst, {S1Hi, S2Hi}); // {lo=src2.hi, hi=src1.hi}
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

bool X86Lifter::liftVectorGather(LiftState &S, const cs_x86 &X86,
                                 unsigned InsnId) {
  // Index element size (D=4 / Q=8) and value element size, derived from the
  // mnemonic: VxGATHER<idx><val>.  The FP forms share the integer sizes.
  uint16_t IdxSz, ValSz;
  switch (InsnId) {
  case X86_INS_VPGATHERDD:
  case X86_INS_VGATHERDPS:
    IdxSz = 4;
    ValSz = 4;
    break;
  case X86_INS_VPGATHERDQ:
  case X86_INS_VGATHERDPD:
    IdxSz = 4;
    ValSz = 8;
    break;
  case X86_INS_VPGATHERQD:
  case X86_INS_VGATHERQPS:
    IdxSz = 8;
    ValSz = 4;
    break;
  case X86_INS_VPGATHERQQ:
  case X86_INS_VGATHERQPD:
    IdxSz = 8;
    ValSz = 8;
    break;
  default:
    return false;
  }

  // Operands: dst = operands[0]; the VSIB memory operand carries the base GPR,
  // the vector index register, scale, and disp; the remaining register operand
  // is the mask (read for sign bits, zeroed afterward).
  const cs_x86_op *Mem = nullptr;
  int MaskIdx = -1;
  for (int I = 0; I < X86.op_count; ++I) {
    if (X86.operands[I].type == X86_OP_MEM)
      Mem = &X86.operands[I];
    else if (I != 0 && X86.operands[I].type == X86_OP_REG)
      MaskIdx = I;
  }
  if (!Mem || MaskIdx < 0 || X86.operands[0].type != X86_OP_REG ||
      Mem->mem.index == X86_REG_INVALID)
    return false;

  NdVar Dst = operandWrite(X86.operands[0]);
  uint16_t DstBytes = Dst.Size;
  RegInfo IdxRI = mapCapstoneReg(static_cast<x86_reg>(Mem->mem.index));
  NdVar IdxVec = NdVar::reg(IdxRI.Offset, IdxRI.Size);
  NdVar MaskVec = operandRead(S, X86.operands[MaskIdx]);
  NdVar DstOld = operandRead(S, X86.operands[0]);

  uint16_t IdxLanes = IdxRI.Size / IdxSz;
  uint16_t ValLanes = DstBytes / ValSz;
  uint16_t NumElems = IdxLanes < ValLanes ? IdxLanes : ValLanes;
  if (NumElems == 0 || ValLanes > 8)
    return false;

  // baseAddr = base + disp (index is per-lane, added below).
  NdVar BaseAddr = S.makeTemp(8);
  bool First = true;
  auto Acc = [&](NdVar V) {
    if (First) {
      S.emit(NdOp::COPY, BaseAddr, {V});
      First = false;
    } else {
      S.emit(NdOp::INT_ADD, BaseAddr, {BaseAddr, V});
    }
  };
  if (Mem->mem.base != X86_REG_INVALID) {
    RegInfo BaseRI = mapCapstoneReg(static_cast<x86_reg>(Mem->mem.base));
    Acc(NdVar::reg(BaseRI.Offset, 8));
  }
  if (Mem->mem.disp != 0)
    Acc(NdVar::cst(static_cast<uint64_t>(Mem->mem.disp), 8));
  if (First)
    Acc(NdVar::cst(0, 8));
  unsigned Scale = Mem->mem.scale ? Mem->mem.scale : 1;

  NdVar Lanes[8];
  for (uint16_t I = 0; I < ValLanes; ++I) {
    if (I >= NumElems) {
      Lanes[I] = NdVar::cst(0, ValSz); // SDM: lanes past the gather count = 0
      continue;
    }
    // addr = baseAddr + sext(index[I]) * scale.
    NdVar IdxElem = S.makeTemp(IdxSz);
    S.emit(NdOp::SUBBYTES, IdxElem,
           {IdxVec, NdVar::cst(static_cast<uint64_t>(I) * IdxSz, 4)});
    NdVar Idx64 = IdxElem;
    if (IdxSz < 8) {
      Idx64 = S.makeTemp(8);
      S.emit(NdOp::INT_SEXT, Idx64, {IdxElem});
    }
    NdVar Off = Idx64;
    if (Scale > 1) {
      Off = S.makeTemp(8);
      S.emit(NdOp::INT_MULT, Off, {Idx64, NdVar::cst(Scale, 8)});
    }
    NdVar Addr = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, Addr, {BaseAddr, Off});
    NdVar Loaded = S.makeTemp(ValSz);
    S.emit(NdOp::LOAD, Loaded, {Addr});
    // The mask element's sign bit gates the load; a clear sign keeps the source
    // lane (operands[0] is also the merge source for masked-off elements).
    NdVar MaskElem = S.makeTemp(ValSz);
    S.emit(NdOp::SUBBYTES, MaskElem,
           {MaskVec, NdVar::cst(static_cast<uint64_t>(I) * ValSz, 4)});
    NdVar SignSet = S.makeTemp(1);
    S.emit(NdOp::INT_SLESS, SignSet, {MaskElem, NdVar::cst(0, ValSz)});
    NdVar OldLane = S.makeTemp(ValSz);
    S.emit(NdOp::SUBBYTES, OldLane,
           {DstOld, NdVar::cst(static_cast<uint64_t>(I) * ValSz, 4)});
    NdVar Res = S.makeTemp(ValSz);
    S.emit(NdOp::SELECT, Res, {SignSet, Loaded, OldLane});
    Lanes[I] = Res;
  }

  // Power-of-two CONCAT tree merges the lanes low->high into the destination.
  uint16_t Count = ValLanes, Sz = ValSz;
  while (Count > 1) {
    uint16_t Half = Count / 2;
    uint16_t NewSz = static_cast<uint16_t>(Sz * 2);
    for (uint16_t K = 0; K < Half; ++K) {
      NdVar Out = (Half == 1) ? Dst : S.makeTemp(NewSz);
      S.emit(NdOp::CONCAT, Out, {Lanes[2 * K + 1], Lanes[2 * K]});
      Lanes[K] = Out;
    }
    Count = Half;
    Sz = NewSz;
  }

  // The gather clears the entire mask register on completion.
  NdVar MaskOut = operandWrite(X86.operands[MaskIdx]);
  S.emit(NdOp::COPY, MaskOut, {NdVar::cst(0, MaskVec.Size)});
  return true;
}

} // namespace neverd
