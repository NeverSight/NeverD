//===- X86LiftSIMDAVXFloat.cpp - x86/x64 AVX/AVX-512 float and lane-move lifter
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The remaining VEX/EVEX V* instructions: range, scale,
/// exponent/mantissa extraction, reduce, round-to-scale,
/// fixup and class tests, the 14/28-bit reciprocal and
/// reciprocal-square-root approximations, EVEX broadcast,
/// insert/extract and compress/expand, VANDN, unpack, and
/// the high/low quadword moves.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftSIMDAVXFloat(X86Lifter &L, X86Lifter::LiftState &S,
                      const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // VRANGE{PS,PD,SS,SD} — float range restriction (imm8-controlled min/max).
  case X86_INS_VRANGEPS:
  case X86_INS_VRANGEPD:
  case X86_INS_VRANGESS:
  case X86_INS_VRANGESD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VDBPSADBW — double block packed sums of absolute differences.
  case X86_INS_VDBPSADBW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::FLOAT_SQRT, Dst, {Src});
    break;
  }

  // VEXP2{PS,PD} — base-2 exponential approximation (28-bit).
  case X86_INS_VEXP2PS:
  case X86_INS_VEXP2PD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    S.emitIntrinsic(Intrinsic::Vexp2, Dst, {Src});
    break;
  }

  // V4FMA{DD,PS}SS / V4FNMA{DD,PS}SS — quad FMA.
  case X86_INS_V4FMADDPS:
  case X86_INS_V4FMADDSS:
  case X86_INS_V4FNMADDPS:
  case X86_INS_V4FNMADDSS: {
    if (X86.op_count >= 2) {
      NdVar Dst = L.operandWrite(X86.operands[0]);
      NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
      S.emit(NdOp::FLOAT_ADD, Dst, {Dst, Src});
    }
    break;
  }

  // VANDNPS / VANDNPD (AVX/AVX-512 bitwise AND-NOT float).
  case X86_INS_VANDNPS:
  case X86_INS_VANDNPD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src1 = L.operandRead(S, X86.operands[X86.op_count >= 3 ? 1 : 0]);
    NdVar Src2 = L.operandRead(S, X86.operands[X86.op_count - 1]);
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
  // dropped the memory write on the 2-operand store form (L.operandWrite() of a
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
      NdVar Src = L.operandRead(S, X86.operands[1]);
      NdVar Half = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Half, {Src, NdVar::cst(IsHigh ? 8 : 0, 4)});
      S.storeToMem(X86.operands[0], Half);
    } else {
      // Load form is dst,src1,m64 — a narrower operand list would read a stale
      // operands[2] slot.
      if (X86.op_count < 3)
        break;
      NdVar Dst = L.operandWrite(X86.operands[0]);
      NdVar Src1 = L.operandRead(S, X86.operands[1]);
      NdVar Mem = L.operandRead(S, X86.operands[2]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src1 = L.operandRead(S, X86.operands[1]);
    NdVar Src2 = L.operandRead(S, X86.operands[2]);
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

} // namespace neverd
