//===- AArch64LiftFPRound.cpp - Floating-point convert and rounding -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// FABD, the JavaScript convert FJCVTZS, round-to-odd narrowing
/// FCVTXN, the range-clamped FRINT32/FRINT64 rounding family and
/// the FEAT_FHM fp16->fp32 widening FMA (FMLAL/FMLSL) plus
/// FMMLA.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftFPRound(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                 const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // Float: FABD, FJCVTZS, FCVTXN, FRINT32/64
  case AARCH64_INS_FABD: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    // Vector `fabd v.4s/.2d` is PER-LANE |A[i]-B[i]|; a single FLOAT_SUB/ABS on
    // the whole i128 makes the emitter treat 16 bytes as one FP value and drops
    // lanes (a64v8_fabs lost half its elements).
    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2; // half-precision (FEAT_FP16) vectors are also per-lane
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar D = S.makeTemp(LaneSz);
        S.emit(NdOp::FLOAT_SUB, D, {La, Lb});
        NdVar R = S.makeTemp(LaneSz);
        S.emit(NdOp::FLOAT_ABS, R, {D});
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
      NdVar Diff = S.makeTemp(Dst.Size);
      S.emit(NdOp::FLOAT_SUB, Diff, {A, B});
      S.emit(NdOp::FLOAT_ABS, Dst, {Diff});
    }
    break;
  }
  // FJCVTZS (FEAT_JSCVT): JavaScript convert double->int32, round toward zero
  // with modulo-2^32 WRAP on overflow (NaN/Inf -> 0).  A plain FLOAT_TRUNC
  // (FPToSI) instead SATURATES out-of-range / Inf inputs, so the two diverge
  // outside [-2^31, 2^31).  Map to the real llvm.aarch64.fjcvtzs intrinsic so
  // codegen emits `fjcvtzs` and the recompiled code is bit-exact under Unicorn.
  case AARCH64_INS_FJCVTZS: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::A64_Fjcvtzs, Dst, {Src});

    // FJCVTZS sets Z when the source is exactly the signed integer represented
    // by Wd.  Converting Wd back to double catches fractional inputs,
    // NaN/Inf, denormals and modulo-2^32 overflow.  IEEE equality considers
    // -0.0 equal to +0.0, but the JavaScript conversion defines -0.0 as
    // inexact, so reject that bit pattern explicitly.  N, C and V are cleared.
    NdVar RoundTrip = S.makeTemp(Src.Size);
    S.emit(NdOp::FLOAT_INT2FLOAT, RoundTrip, {Dst});
    NdVar SameValue = S.makeTemp(1);
    S.emit(NdOp::FLOAT_EQUAL, SameValue, {Src, RoundTrip});
    NdVar NegativeZero = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, NegativeZero,
           {Src, NdVar::cst(uint64_t{1} << 63, Src.Size)});
    NdVar NotNegativeZero = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NotNegativeZero, {NegativeZero});
    S.emit(NdOp::BOOL_AND, NdVar::reg(a64reg::ZFLAG, 1),
           {SameValue, NotNegativeZero});
    S.emit(NdOp::COPY, NdVar::reg(a64reg::NFLAG, 1), {NdVar::cst(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(a64reg::CFLAG, 1), {NdVar::cst(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(a64reg::VFLAG, 1), {NdVar::cst(0, 1)});
    break;
  }
  // FCVTXN/FCVTXN2: FP inexact narrowing f64->f32 with round-to-ODD (jamming),
  // not round-to-nearest-even.  A plain FLOAT_FLOAT2FLOAT rounds to even, which
  // differs on every inexact narrowing; the "2" form additionally writes the
  // narrowed pair into the HIGH 64 bits of Vd (keeping the low half).  Map to
  // the real sisd/neon fcvtxn intrinsic (round-to-odd, per-lane) so codegen
  // emits `fcvtxn` and the recompiled code is bit-exact under Unicorn.
  case AARCH64_INS_FCVTXN:
  case AARCH64_INS_FCVTXN2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    if (Insn->id == AARCH64_INS_FCVTXN2) {
      // Narrow the two f64 source lanes to two f32 and place them in the HIGH
      // 64 bits of Vd, preserving the existing low 64 bits.
      NdVar OldVd = L.operandRead(S, ARM64.operands[0]);
      NdVar Narrow = S.makeTemp(8);
      S.emitIntrinsic(Intrinsic::A64_Fcvtxn, Narrow, {Src});
      NdVar Low = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Low, {OldVd, NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {Narrow, Low});
    } else {
      S.emitIntrinsic(Intrinsic::A64_Fcvtxn, Dst, {Src});
    }
    break;
  }
  // FRINT32Z/FRINT32X/FRINT64Z/FRINT64X (FEAT_FRINTTS): round each f32/f64 lane
  // to an integral float, then clamp to the signed 32-bit (FRINT32*) or 64-bit
  // (FRINT64*) integer range.  Out-of-range, NaN or Inf inputs yield INT_MIN as
  // a float (-2^31 for FRINT32*, -2^63 for FRINT64*); the "Z" forms round
  // toward zero, the "X" forms use the FPCR rounding mode (default
  // round-to-nearest- even).  The old code used a single whole-register
  // FLOAT_ROUND, which is wrong three ways: ties-away rounding (not toward-zero
  // / to-even), no range clamping, and -- for vectors -- it collapsed all lanes
  // into one FP value.
  // +/-2^31 and +/-2^63 are exactly representable in both f32 and f64, so the
  // float-comparison clamp matches QEMU's frint_s/frint_d exponent check bit-
  // exactly.
  case AARCH64_INS_FRINT32X:
  case AARCH64_INS_FRINT32Z:
  case AARCH64_INS_FRINT64X:
  case AARCH64_INS_FRINT64Z: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    bool IsZ =
        (Insn->id == AARCH64_INS_FRINT32Z || Insn->id == AARCH64_INS_FRINT64Z);
    bool Is32 =
        (Insn->id == AARCH64_INS_FRINT32X || Insn->id == AARCH64_INS_FRINT32Z);
    auto frintLane = [&](NdVar Lane, unsigned Sz) -> NdVar {
      // 1) Round to an integral float in the requested mode.
      NdVar R = S.makeTemp(Sz);
      if (IsZ) {
        // Round toward zero = floor for non-negative, ceil for negative.
        NdVar Fl = S.makeTemp(Sz), Ce = S.makeTemp(Sz), Neg = S.makeTemp(1);
        S.emit(NdOp::FLOAT_FLOOR, Fl, {Lane});
        S.emit(NdOp::FLOAT_CEIL, Ce, {Lane});
        S.emit(NdOp::FLOAT_LESS, Neg, {Lane, NdVar::cst(0, Sz)});
        S.emit(NdOp::SELECT, R, {Neg, Ce, Fl});
      } else {
        S.emit(NdOp::FLOAT_ROUNDEVEN, R, {Lane});
      }
      // 2) Bit patterns for +/-2^(N-1) at this lane width (N = 32 or 64).
      uint64_t BoundPos, BoundNeg;
      if (Sz == 4) {
        BoundPos = Is32 ? 0x4F000000ULL : 0x5F000000ULL; // 2^31 / 2^63 (f32)
        BoundNeg = Is32 ? 0xCF000000ULL : 0xDF000000ULL; // -2^31 / -2^63
      } else {
        BoundPos = Is32 ? 0x41E0000000000000ULL  // 2^31 (f64)
                        : 0x43E0000000000000ULL; // 2^63 (f64)
        BoundNeg = Is32 ? 0xC1E0000000000000ULL  // -2^31
                        : 0xC3E0000000000000ULL; // -2^63
      }
      // 3) Overflow if NaN/Inf, or the rounded result is out of
      //    [-2^(N-1), 2^(N-1)).  (Inf is caught by the magnitude checks.)
      NdVar IsNan = S.makeTemp(1);
      S.emit(NdOp::FLOAT_ISNAN, IsNan, {Lane});
      NdVar Hi = S.makeTemp(1); // r >= BoundPos  <=>  BoundPos <= r
      S.emit(NdOp::FLOAT_LESSEQUAL, Hi, {NdVar::cst(BoundPos, Sz), R});
      NdVar Lo = S.makeTemp(1); // r < BoundNeg
      S.emit(NdOp::FLOAT_LESS, Lo, {R, NdVar::cst(BoundNeg, Sz)});
      NdVar Ovf1 = S.makeTemp(1), Ovf = S.makeTemp(1);
      S.emit(NdOp::BOOL_OR, Ovf1, {IsNan, Hi});
      S.emit(NdOp::BOOL_OR, Ovf, {Ovf1, Lo});
      // 4) INT_MIN (as float, == BoundNeg) on overflow, else the rounded value.
      NdVar Out = S.makeTemp(Sz);
      S.emit(NdOp::SELECT, Out, {Ovf, NdVar::cst(BoundNeg, Sz), R});
      return Out;
    };
    auto DstVas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (DstVas == AARCH64LAYOUT_VL_4S || DstVas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar R = frintLane(Lane, LaneSz);
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
      S.emit(NdOp::COPY, Dst, {frintLane(Src, Dst.Size)});
    }
    break;
  }

  // FMLAL/FMLSL/FMLAL2/FMLSL2 (FEAT_FHM): fp16->fp32 widening fused multiply-
  // add/subtract.  Each f32 destination lane i accumulates
  // fma(+/-widen(Vn.h[j]), widen(Vm.h[k]), Vd[i]) with a SINGLE rounding.  The
  // "2" variants read the HIGH NLanes fp16 lanes of the source register(s); the
  // by-element form broadcasts one fp16 lane of Vm (index 0..7 of the full
  // reg). The old code used a naive whole-register FLOAT_MULT + separate
  // FLOAT_ADD/SUB (no fp16->fp32 widening, no per-lane structure, double
  // rounding).
  case AARCH64_INS_FMLAL:
  case AARCH64_INS_FMLSL:
  case AARCH64_INS_FMLAL2:
  case AARCH64_INS_FMLSL2: {
    if (ARM64.op_count < 3)
      break;
    bool IsSub =
        (Insn->id == AARCH64_INS_FMLSL || Insn->id == AARCH64_INS_FMLSL2);
    bool IsHigh =
        (Insn->id == AARCH64_INS_FMLAL2 || Insn->id == AARCH64_INS_FMLSL2);
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar DstR = NdVar::reg(Dst.Offset, Dst.Size);
    unsigned NLanes = Dst.Size / 4;             // f32 destination lanes
    int BLane = ARM64.operands[2].vector_index; // >=0 for the by-element form
    // The "2" variants reach fp16 lanes NLanes..2*NLanes-1, which live in the
    // high half of the V register; the .4h/.2h operand view is only 8/4 bytes,
    // so re-read the full 16-byte register and offset the lane base.
    NdVar ASrc = A, BSrc = B;
    unsigned ALaneBase = 0, BLaneBase = 0;
    if (IsHigh) {
      ASrc = NdVar::reg(A.Offset, 16);
      ALaneBase = NLanes;
      if (BLane < 0) { // vector form: Vm also uses the high lanes
        BSrc = NdVar::reg(B.Offset, 16);
        BLaneBase = NLanes;
      }
    }
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Ah = S.makeTemp(2);
      S.emit(NdOp::SUBBYTES, Ah, {ASrc, NdVar::cst((ALaneBase + I) * 2, 4)});
      NdVar Bh;
      if (BLane >= 0 && B.Size <= 2) {
        Bh = B; // operandRead already returned the single indexed fp16 lane
      } else {
        unsigned BOff = (BLane >= 0 ? (unsigned)BLane : (BLaneBase + I)) * 2;
        Bh = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Bh, {BSrc, NdVar::cst(BOff, 4)});
      }
      NdVar Wa = S.makeTemp(4), Wb = S.makeTemp(4);
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Wa, {Ah}); // fp16 -> fp32 widen
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Wb, {Bh});
      if (IsSub) {
        NdVar NWa = S.makeTemp(4);
        S.emit(NdOp::FLOAT_NEG, NWa, {Wa});
        Wa = NWa;
      }
      NdVar Dl = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Dl, {DstR, NdVar::cst(I * 4, 4)});
      NdVar R = S.makeTemp(4);
      S.emit(NdOp::FLOAT_FMA, R, {Wa, Wb, Dl}); // single rounding (fused)
      if (I == 0) {
        Acc = R;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + 4);
        S.emit(NdOp::CONCAT, Next, {R, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // The FEAT_FP8 variants (FMLALB/FMLALT/FMLALL*, FMLSLB/FMLSLT) widen fp8 (not
  // fp16) with different lane selection and are not yet modelled; keep the
  // naive behaviour pending a dedicated pass.  (FMLAL2/FMLSL2 are handled above
  // by the fp16 FEAT_FHM widening path.)
  case AARCH64_INS_FMLALB:
  case AARCH64_INS_FMLALT:
  case AARCH64_INS_FMLALL:
  case AARCH64_INS_FMLALLBB:
  case AARCH64_INS_FMLALLBT:
  case AARCH64_INS_FMLALLTB:
  case AARCH64_INS_FMLALLTT: {
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
  case AARCH64_INS_FMLSLB:
  case AARCH64_INS_FMLSLT: {
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
  case AARCH64_INS_FMMLA: {
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

  default:
    return false;
  }
  return true;
}

} // namespace neverd
