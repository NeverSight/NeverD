//===- X86LiftSIMDAVXConvert.cpp - x86/x64 AVX/AVX-512 conversion lifter --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// VEX/EVEX VCVT* conversions between packed or scalar
/// integers and floats, between float widths, and the
/// half-precision (F16C) pack/unpack.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftSIMDAVXConvert(X86Lifter &L, X86Lifter::LiftState &S,
                        const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Upper = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1]) : Dst;
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    NdVar Upper = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1]) : Dst;
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
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
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::FLOAT_FLOAT2FLOAT, Dst, {Src});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
