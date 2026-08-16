//===- AArch64LiftFPConvert.cpp - Float conversion and rounding -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Integer-to-float (UCVTF/SCVTF), float-to-integer with
/// truncation (FCVTZS/FCVTZU) and with an explicit rounding mode
/// (FCVTAS/FCVTNS/FCVTMS/FCVTPS and the unsigned forms), the
/// FCVT width change, and the FRINTA/I/M/N/P/X/Z round-to-
/// integral family.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftFPConvert(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                   const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // ====================================================================
  // Float <-> integer conversion
  // ====================================================================
  case AARCH64_INS_UCVTF:
  case AARCH64_INS_SCVTF: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    NdOp CvtOp = (Insn->id == AARCH64_INS_UCVTF) ? NdOp::FLOAT_UINT2FLOAT
                                                 : NdOp::FLOAT_INT2FLOAT;
    // Fixed-point form `scvtf Sd, Wn, #fbits` converts then divides by 2^fbits
    // (clang folds `(float)x * 0.5f` into `scvtf s, w, #1`).  Ignoring #fbits
    // dropped the scale — dotacc's x/y came out 2x/4x too large.  (Mirror of
    // the FCVTZS/FCVTZU fixed-point handling below.)
    unsigned FBits = 0;
    if (ARM64.op_count >= 3 &&
        ARM64.operands[ARM64.op_count - 1].type == AARCH64_OP_IMM)
      FBits = static_cast<unsigned>(ARM64.operands[ARM64.op_count - 1].imm);
    // 2^-FBits as a float/double bit pattern of width `Sz` (bias - FBits).
    // Fixed FP16 vectors use binary32 as an exact working format: every int16
    // lane and every permitted power-of-two scale is exactly representable,
    // then one final narrowing performs the architectural FP16 rounding.
    auto pow2neg = [&](unsigned Sz) -> NdVar {
      if (Sz == 8)
        return NdVar::cst(static_cast<uint64_t>(1023u - FBits) << 52, 8);
      return NdVar::cst(static_cast<uint64_t>((127u - FBits) << 23), 4);
    };
    auto convertLane = [&](NdVar Lane, unsigned Sz) -> NdVar {
      unsigned WorkSz = (FBits > 0 && Sz == 2) ? 4 : Sz;
      NdVar Cvt = S.makeTemp(WorkSz);
      S.emit(CvtOp, Cvt, {Lane});
      if (FBits > 0) {
        NdVar Scaled = S.makeTemp(WorkSz);
        S.emit(NdOp::FLOAT_MULT, Scaled, {Cvt, pow2neg(WorkSz)});
        Cvt = Scaled;
      }
      if (WorkSz != Sz) {
        NdVar Narrowed = S.makeTemp(Sz);
        S.emit(NdOp::FLOAT_FLOAT2FLOAT, Narrowed, {Cvt});
        return Narrowed;
      }
      return Cvt;
    };
    // Vector forms `scvtf/ucvtf v.4s, v.4s` / `v.2d, v.2d` are PER-LANE int->fp
    // conversions; converting the whole i128 as one `sitofp i128 to double`
    // produces a single garbage double (the VectorAlgo8 FP loops all read 0).
    auto DstVas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (DstVas == AARCH64LAYOUT_VL_4S || DstVas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    else if (DstVas == AARCH64LAYOUT_VL_8H || DstVas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2; // half-precision (FEAT_FP16) per-lane conversion
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Cvt = convertLane(Lane, LaneSz);
        if (I == 0) {
          Acc = Cvt;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Cvt, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else if (FBits > 0 && Dst.Size == 2) {
      // An integer -> binary16 fixed conversion is a single architectural
      // rounding operation.  Widening through binary32/64 can double-round,
      // while converting to half before scaling can overflow prematurely.
      Intrinsic IC = (Insn->id == AARCH64_INS_UCVTF)
                         ? Intrinsic::A64_UcvtfFixed
                         : Intrinsic::A64_ScvtfFixed;
      S.emitIntrinsic(IC, Dst, {Src, NdVar::cst(FBits, 4)});
    } else if (FBits > 0) {
      NdVar R = convertLane(Src, Dst.Size);
      S.emit(NdOp::COPY, Dst, {R});
    } else {
      S.emit(CvtOp, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_FCVTZS:
  case AARCH64_INS_FCVTZU: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    bool IsUnsigned = (Insn->id == AARCH64_INS_FCVTZU);
    NdOp CvtOp = IsUnsigned ? NdOp::FLOAT_FLOAT2UINT : NdOp::FLOAT_FLOAT2INT;
    // Fixed-point form `fcvtzs Vd, Vn, #fbits` scales by 2^fbits *before*
    // truncating (clang folds e.g. `(v-w)*2.0f` then `(int)` into a single
    // `fcvtzs v.4s, v.4s, #1`).  Ignoring #fbits dropped the multiply and
    // halved the VectorAlgo8 results.
    unsigned FBits = 0;
    if (ARM64.op_count >= 3 &&
        ARM64.operands[ARM64.op_count - 1].type == AARCH64_OP_IMM)
      FBits = static_cast<unsigned>(ARM64.operands[ARM64.op_count - 1].imm);
    auto DstVas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (DstVas == AARCH64LAYOUT_VL_4S || DstVas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    else if (DstVas == AARCH64LAYOUT_VL_8H || DstVas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2; // half-precision (FEAT_FP16) per-lane conversion
    // 2^FBits as a float/double bit pattern of width `Sz`.  Fixed FP16 vector
    // lanes are first extended exactly to binary32 so the scale stays finite.
    auto pow2 = [&](unsigned Sz) -> NdVar {
      if (Sz == 8)
        return NdVar::cst(static_cast<uint64_t>(1023u + FBits) << 52, 8);
      return NdVar::cst(static_cast<uint64_t>((127u + FBits) << 23), 4);
    };
    auto convertLane = [&](NdVar Lane, unsigned Sz) -> NdVar {
      unsigned WorkSz = (FBits > 0 && Sz == 2) ? 4 : Sz;
      if (WorkSz != Sz) {
        NdVar Extended = S.makeTemp(WorkSz);
        S.emit(NdOp::FLOAT_FLOAT2FLOAT, Extended, {Lane});
        Lane = Extended;
      }
      if (FBits > 0) {
        NdVar Scaled = S.makeTemp(WorkSz);
        S.emit(NdOp::FLOAT_MULT, Scaled, {Lane, pow2(WorkSz)});
        Lane = Scaled;
      }
      NdVar Cvt = S.makeTemp(Sz);
      S.emit(CvtOp, Cvt, {Lane});
      return Cvt;
    };
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Cvt = convertLane(Lane, LaneSz);
        if (I == 0) {
          Acc = Cvt;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Cvt, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else if (FBits > 0 && Src.Size == 2) {
      Intrinsic IC =
          IsUnsigned ? Intrinsic::A64_FcvtzuFixed : Intrinsic::A64_FcvtzsFixed;
      S.emitIntrinsic(IC, Dst, {Src, NdVar::cst(FBits, 4)});
    } else if (FBits > 0) {
      // Scalar fixed-point: scale by 2^fbits in the SOURCE fp width, then
      // convert straight to the integer dest width.  Using Dst.Size (the int
      // width) for the scale multiplied a double source by a single-precision
      // 2^fbits with a narrowed result (e.g. `fcvtzs w, d, #8`), corrupting it.
      unsigned FpSz = (Src.Size == 8) ? 8 : 4;
      NdVar Scaled = S.makeTemp(FpSz);
      S.emit(NdOp::FLOAT_MULT, Scaled, {Src, pow2(FpSz)});
      S.emit(CvtOp, Dst, {Scaled});
    } else {
      S.emit(CvtOp, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_FCVT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::FLOAT_FLOAT2FLOAT, Dst, {Src});
    break;
  }
  // FCVT{A,N,M,P}{S,U} — float→int with an explicit rounding mode: A ties away
  // from zero, N ties to even (IEEE default), M toward -inf (floor), P toward
  // +inf (ceil).  N must use round-to-even, not round-half-away.  Vector forms
  // round each lane independently.
  case AARCH64_INS_FCVTAS:
  case AARCH64_INS_FCVTNS:
  case AARCH64_INS_FCVTMS:
  case AARCH64_INS_FCVTPS:
  case AARCH64_INS_FCVTAU:
  case AARCH64_INS_FCVTNU:
  case AARCH64_INS_FCVTMU:
  case AARCH64_INS_FCVTPU: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    bool IsUnsigned =
        (Insn->id == AARCH64_INS_FCVTAU || Insn->id == AARCH64_INS_FCVTNU ||
         Insn->id == AARCH64_INS_FCVTMU || Insn->id == AARCH64_INS_FCVTPU);
    NdOp CvtOp = IsUnsigned ? NdOp::FLOAT_FLOAT2UINT : NdOp::FLOAT_FLOAT2INT;
    NdOp RoundOp;
    switch (Insn->id) {
    case AARCH64_INS_FCVTMS:
    case AARCH64_INS_FCVTMU:
      RoundOp = NdOp::FLOAT_FLOOR;
      break;
    case AARCH64_INS_FCVTPS:
    case AARCH64_INS_FCVTPU:
      RoundOp = NdOp::FLOAT_CEIL;
      break;
    case AARCH64_INS_FCVTAS:
    case AARCH64_INS_FCVTAU:
      RoundOp = NdOp::FLOAT_ROUND;
      break;
    default:
      RoundOp = NdOp::FLOAT_ROUNDEVEN;
      break;
    }
    auto cvtLane = [&](NdVar Lane, unsigned Sz) -> NdVar {
      NdVar R = S.makeTemp(Sz);
      S.emit(RoundOp, R, {Lane});
      NdVar C = S.makeTemp(Sz);
      S.emit(CvtOp, C, {R});
      return C;
    };
    auto DstVas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (DstVas == AARCH64LAYOUT_VL_4S || DstVas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    else if (DstVas == AARCH64LAYOUT_VL_8H || DstVas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2; // half-precision (FEAT_FP16) per-lane conversion
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar C = cvtLane(Lane, LaneSz);
        if (I == 0) {
          Acc = C;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {C, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Rounded = S.makeTemp(Src.Size);
      S.emit(RoundOp, Rounded, {Src});
      S.emit(CvtOp, Dst, {Rounded});
    }
    break;
  }
  // FRINT{A,I,M,N,P,X,Z} — round float to integral float.  A ties away, N ties
  // to even, I/X follow FPCR, M floors, P ceils, and Z rounds toward zero.
  // Vector forms round each lane independently.
  case AARCH64_INS_FRINTA:
  case AARCH64_INS_FRINTI:
  case AARCH64_INS_FRINTM:
  case AARCH64_INS_FRINTN:
  case AARCH64_INS_FRINTP:
  case AARCH64_INS_FRINTX:
  case AARCH64_INS_FRINTZ: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    auto roundLane = [&](NdVar Lane, unsigned Sz) -> NdVar {
      NdVar R = S.makeTemp(Sz);
      if (Insn->id == AARCH64_INS_FRINTI) {
        S.emitIntrinsic(Intrinsic::A64_Frinti, R, {Lane, NdVar::cst(Sz, 1)});
      } else if (Insn->id == AARCH64_INS_FRINTZ) {
        // Round toward zero = floor for non-negative, ceil for negative.
        NdVar Fl = S.makeTemp(Sz), Ce = S.makeTemp(Sz), IsNeg = S.makeTemp(1);
        S.emit(NdOp::FLOAT_FLOOR, Fl, {Lane});
        S.emit(NdOp::FLOAT_CEIL, Ce, {Lane});
        S.emit(NdOp::FLOAT_LESS, IsNeg, {Lane, NdVar::cst(0, Sz)});
        S.emit(NdOp::SELECT, R, {IsNeg, Ce, Fl});
      } else {
        NdOp Ro;
        switch (Insn->id) {
        case AARCH64_INS_FRINTM:
          Ro = NdOp::FLOAT_FLOOR;
          break;
        case AARCH64_INS_FRINTP:
          Ro = NdOp::FLOAT_CEIL;
          break;
        case AARCH64_INS_FRINTA:
          Ro = NdOp::FLOAT_ROUND;
          break;
        default:
          Ro = NdOp::FLOAT_ROUNDEVEN;
          break;
        }
        S.emit(Ro, R, {Lane});
      }
      return R;
    };
    auto DstVas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (DstVas == AARCH64LAYOUT_VL_4S || DstVas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    else if (DstVas == AARCH64LAYOUT_VL_8H || DstVas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2; // half-precision (FEAT_FP16) per-lane conversion
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar R = roundLane(Lane, LaneSz);
        if (I == 0) {
          Acc = R;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::COPY, Dst, {roundLane(Src, Dst.Size)});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
