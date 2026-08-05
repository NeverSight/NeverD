//===- AArch64LiftFP.cpp - AArch64 floating-point instruction lifter -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AArch64 scalar floating-point instruction handlers: FADD/FSUB/FMUL/FDIV,
/// FNEG/FABS/FSQRT, FMOV, FMADD/FMSUB, FMIN/FMAX, FCMP/FCSEL, float↔int
/// conversion (UCVTF/SCVTF/FCVT*), and rounding (FRINT*).
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/Support/Debug.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-aarch64"

namespace neverd {

// Encode a floating-point value as IEEE-754 binary16 (half) bits.  A half and a
// float have different layouts, so reinterpreting the low 2 bytes of a `float`
// (the old `memcpy(&h16, &f32, 2)`) yields garbage -- e.g. 1.0f is 0x3F800000,
// whose low 16 bits are 0x0000 = half 0.0, silently zeroing every `fmov h,
// #imm` constant.  Round-convert through APFloat instead.
static uint16_t encodeHalfBits(double V) {
  llvm::APFloat APF(V);
  bool LosesInfo = false;
  APF.convert(llvm::APFloat::IEEEhalf(), llvm::APFloat::rmNearestTiesToEven,
              &LosesInfo);
  return static_cast<uint16_t>(APF.bitcastToAPInt().getZExtValue());
}

bool AArch64Lifter::liftFP(LiftState &S, const cs_insn *Insn,
                           const cs_aarch64 &ARM64) {
  switch (Insn->id) {

  // ====================================================================
  // Floating-point scalar arithmetic
  // ====================================================================
  case AARCH64_INS_FADD:
  case AARCH64_INS_FSUB:
  case AARCH64_INS_FMUL:
  case AARCH64_INS_FDIV: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdOp Opc;
    switch (Insn->id) {
    case AARCH64_INS_FADD:
      Opc = NdOp::FLOAT_ADD;
      break;
    case AARCH64_INS_FSUB:
      Opc = NdOp::FLOAT_SUB;
      break;
    case AARCH64_INS_FMUL:
      Opc = NdOp::FLOAT_MULT;
      break;
    default:
      Opc = NdOp::FLOAT_DIV;
      break;
    }
    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2; // half-precision (FEAT_FP16) vectors are also per-lane
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned HalfSz = Dst.Size / 2;
      unsigned LanesPerHalf = HalfSz / LaneSz;
      // By-element `fmul v.T, v.T, vN.<ty>[idx]` broadcasts one source lane;
      // operandRead returns the whole register (high lanes 0), so a per-lane
      // read would multiply most lanes by 0.  (FADD/FSUB/FDIV have no indexed
      // form, so vector_index is always -1 for them.)
      int BLane = ARM64.operands[2].vector_index;
      NdVar BElem;
      bool BBroadcast = false;
      if (BLane >= 0) {
        NdVar BFull = operandWrite(ARM64.operands[2]);
        BElem = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, BElem,
               {BFull, NdVar::cst(static_cast<uint64_t>(BLane) * LaneSz, 4)});
        BBroadcast = true;
      }
      auto BuildFPHalf = [&](unsigned BaseOff) -> NdVar {
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < LanesPerHalf; ++I) {
          unsigned Off = BaseOff + I * LaneSz;
          NdVar La = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
          NdVar Lb = BBroadcast ? BElem : S.makeTemp(LaneSz);
          if (!BBroadcast)
            S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
          NdVar Lr = S.makeTemp(LaneSz);
          S.emit(Opc, Lr, {La, Lb});
          if (I == 0) {
            Acc = Lr;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Lr, Acc});
            Acc = Next;
          }
        }
        return Acc;
      };
      NdVar LoH = BuildFPHalf(0);
      NdVar HiH = BuildFPHalf(HalfSz);
      NdVar Full = S.makeTemp(Dst.Size);
      S.emit(NdOp::CONCAT, Full, {HiH, LoH});
      S.emit(NdOp::COPY, Dst, {Full});
    } else {
      S.emit(Opc, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_FNEG:
  case AARCH64_INS_FABS:
  case AARCH64_INS_FSQRT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    NdOp Opc = (Insn->id == AARCH64_INS_FNEG)   ? NdOp::FLOAT_NEG
               : (Insn->id == AARCH64_INS_FABS) ? NdOp::FLOAT_ABS
                                                : NdOp::FLOAT_SQRT;
    // Vector forms `fneg/fabs/fsqrt v.4s` / `v.2d` are PER-LANE; a single
    // scalar op on the whole i128 yields one garbage double (VectorAlgo8
    // fabs/fsqrt read wrong / NaN values).
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
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Lr = S.makeTemp(LaneSz);
        S.emit(Opc, Lr, {Lane});
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
      S.emit(Opc, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_FMOV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    // Vector FP immediate `fmov v.<T>, #imm` splats the per-lane FP immediate
    // across all lanes.  Without this it falls through to copying the 8-byte
    // double bits into the wide register (e.g. 1.0 -> lanes [0,1.875,0,0]).
    auto Vas = ARM64.operands[0].vas;
    bool IsVecLayout = Vas == AARCH64LAYOUT_VL_2S ||
                       Vas == AARCH64LAYOUT_VL_4S ||
                       Vas == AARCH64LAYOUT_VL_2D ||
                       Vas == AARCH64LAYOUT_VL_4H || Vas == AARCH64LAYOUT_VL_8H;
    if (ARM64.operands[1].type == AARCH64_OP_FP && IsVecLayout &&
        Dst.Size >= 8) {
      unsigned LaneSz =
          (Vas == AARCH64LAYOUT_VL_2D)                                 ? 8
          : (Vas == AARCH64LAYOUT_VL_4H || Vas == AARCH64LAYOUT_VL_8H) ? 2
                                                                       : 4;
      double dv = ARM64.operands[1].fp;
      uint64_t Bits = 0;
      if (LaneSz == 8) {
        std::memcpy(&Bits, &dv, 8);
      } else if (LaneSz == 4) {
        float fv = static_cast<float>(dv);
        uint32_t b;
        std::memcpy(&b, &fv, 4);
        Bits = b;
      } else {
        Bits = encodeHalfBits(dv);
      }
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Lane = NdVar::cst(Bits, LaneSz);
      NdVar Acc = Lane;
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Next = S.makeTemp(LaneSz * (I + 1));
        S.emit(NdOp::CONCAT, Next, {Lane, Acc});
        Acc = Next;
      }
      S.emit(NdOp::COPY, Dst, {Acc});
      break;
    }
    NdVar Src = operandRead(S, ARM64.operands[1]);
    if (ARM64.operands[1].type == AARCH64_OP_FP && Dst.Size == 4 &&
        Src.Size == 8) {
      float fval = static_cast<float>(ARM64.operands[1].fp);
      uint32_t fbits;
      std::memcpy(&fbits, &fval, 4);
      Src = NdVar::cst(fbits, 4);
    } else if (ARM64.operands[1].type == AARCH64_OP_FP && Dst.Size == 2 &&
               Src.Size == 8) {
      Src = NdVar::cst(encodeHalfBits(ARM64.operands[1].fp), 2);
    }
    S.emit(NdOp::COPY, Dst, {Src});
    if (Dst.Offset >= a64reg::V0 && Dst.Offset < a64reg::V0 + 32 * 16 &&
        Dst.Size < 16) {
      uint64_t QOff = a64reg::V0 + ((Dst.Offset - a64reg::V0) / 16) * 16;
      S.emit(NdOp::INT_ZEXT, NdVar::reg(QOff, 16), {Dst});
    }
    break;
  }
  case AARCH64_INS_FMADD: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar N = operandRead(S, ARM64.operands[1]);
    NdVar M = operandRead(S, ARM64.operands[2]);
    NdVar A = operandRead(S, ARM64.operands[3]);
    // Fused (single rounding): A + N*M = fma(N,M,A).
    S.emit(NdOp::FLOAT_FMA, Dst, {N, M, A});
    break;
  }
  case AARCH64_INS_FMSUB: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar N = operandRead(S, ARM64.operands[1]);
    NdVar M = operandRead(S, ARM64.operands[2]);
    NdVar A = operandRead(S, ARM64.operands[3]);
    // Fused (single rounding): A - N*M = fma(-N,M,A).
    NdVar NN = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_NEG, NN, {N});
    S.emit(NdOp::FLOAT_FMA, Dst, {NN, M, A});
    break;
  }
  // FMIN/FMAX — IEEE-754 minimum/maximum (NaN-propagating, -0 < +0).  A naive
  // (a<b)?a:b select mishandles NaN inputs and signed zeros, and the scalar
  // form was also (wrongly) used for the vector form without per-lane handling.
  case AARCH64_INS_FMIN:
  case AARCH64_INS_FMAX: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdOp MM =
        (Insn->id == AARCH64_INS_FMIN) ? NdOp::FLOAT_MIN : NdOp::FLOAT_MAX;
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

  // --- FCMP / FCCMP (float compare -> NZCV) ---
  case AARCH64_INS_FCMP:
  case AARCH64_INS_FCMPE: {
    if (ARM64.op_count < 2)
      break;
    NdVar A = operandRead(S, ARM64.operands[0]);
    NdVar B =
        (ARM64.operands[1].type == AARCH64_OP_FP && ARM64.operands[1].fp == 0.0)
            ? NdVar::cst(0, A.Size)
            : operandRead(S, ARM64.operands[1]);
    NdVar Eq = S.makeTemp(1);
    NdVar Lt = S.makeTemp(1);
    S.emit(NdOp::FLOAT_EQUAL, Eq, {A, B});
    S.emit(NdOp::FLOAT_LESS, Lt, {A, B});
    S.emit(NdOp::COPY, NdVar::reg(a64reg::NFLAG, 1), {Lt});
    S.emit(NdOp::COPY, NdVar::reg(a64reg::ZFLAG, 1), {Eq});
    S.emit(NdOp::BOOL_NOT, NdVar::reg(a64reg::CFLAG, 1), {Lt});
    NdVar Gt = S.makeTemp(1);
    S.emit(NdOp::FLOAT_LESS, Gt, {B, A});
    NdVar Ordered = S.makeTemp(1);
    S.emit(NdOp::BOOL_OR, Ordered, {Eq, Lt});
    NdVar Ordered2 = S.makeTemp(1);
    S.emit(NdOp::BOOL_OR, Ordered2, {Ordered, Gt});
    S.emit(NdOp::BOOL_NOT, NdVar::reg(a64reg::VFLAG, 1), {Ordered2});
    break;
  }

  // --- FCSEL (float conditional select) ---
  case AARCH64_INS_FCSEL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Cond = S.makeTemp(1);
    switch (ARM64.cc) {
    case AArch64CC_EQ:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::ZFLAG, 1)});
      break;
    case AArch64CC_NE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::ZFLAG, 1)});
      break;
    case AArch64CC_HS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::CFLAG, 1)});
      break;
    case AArch64CC_LO:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::CFLAG, 1)});
      break;
    case AArch64CC_MI:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::NFLAG, 1)});
      break;
    case AArch64CC_PL:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::NFLAG, 1)});
      break;
    case AArch64CC_VS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_VC:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_LT:
      S.emit(NdOp::INT_NOTEQUAL, Cond,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_GE:
      S.emit(NdOp::INT_EQUAL, Cond,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_GT: {
      NdVar NZ = S.makeTemp(1);
      NdVar EqFlags = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(a64reg::ZFLAG, 1)});
      S.emit(NdOp::INT_EQUAL, EqFlags,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NZ, EqFlags});
      break;
    }
    case AArch64CC_LE: {
      NdVar NeFlags = S.makeTemp(1);
      S.emit(NdOp::INT_NOTEQUAL, NeFlags,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(a64reg::ZFLAG, 1), NeFlags});
      break;
    }
    case AArch64CC_HI: {
      NdVar NZ = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(a64reg::ZFLAG, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NdVar::reg(a64reg::CFLAG, 1), NZ});
      break;
    }
    case AArch64CC_LS: {
      NdVar NC = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NC, {NdVar::reg(a64reg::CFLAG, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(a64reg::ZFLAG, 1), NC});
      break;
    }
    default:
      S.emit(NdOp::COPY, Cond, {NdVar::cst(1, 1)});
      break;
    }
    S.emit(NdOp::SELECT, Dst, {Cond, A, B});
    break;
  }

  // ====================================================================
  // Float <-> integer conversion
  // ====================================================================
  case AARCH64_INS_UCVTF:
  case AARCH64_INS_SCVTF: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
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
    auto pow2neg = [&](unsigned Sz) -> NdVar {
      if (Sz == 8)
        return NdVar::cst(static_cast<uint64_t>(1023u - FBits) << 52, 8);
      if (Sz == 2) // half: bias 15, 10 mantissa bits
        return NdVar::cst(static_cast<uint64_t>((15u - FBits) << 10), 2);
      return NdVar::cst(static_cast<uint64_t>((127u - FBits) << 23), 4);
    };
    auto convertLane = [&](NdVar Lane, unsigned Sz) -> NdVar {
      NdVar Cvt = S.makeTemp(Sz);
      S.emit(CvtOp, Cvt, {Lane});
      if (FBits > 0) {
        NdVar Scaled = S.makeTemp(Sz);
        S.emit(NdOp::FLOAT_MULT, Scaled, {Cvt, pow2neg(Sz)});
        return Scaled;
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
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
    // 2^FBits as a float/double bit pattern of width `Sz`.
    auto pow2 = [&](unsigned Sz) -> NdVar {
      if (Sz == 8)
        return NdVar::cst(static_cast<uint64_t>(1023u + FBits) << 52, 8);
      if (Sz == 2) // half: bias 15, 10 mantissa bits
        return NdVar::cst(static_cast<uint64_t>((15u + FBits) << 10), 2);
      return NdVar::cst(static_cast<uint64_t>((127u + FBits) << 23), 4);
    };
    auto convertLane = [&](NdVar Lane, unsigned Sz) -> NdVar {
      if (FBits > 0) {
        NdVar Scaled = S.makeTemp(Sz);
        S.emit(NdOp::FLOAT_MULT, Scaled, {Lane, pow2(Sz)});
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
    } else if (FBits > 0) {
      // Scalar fixed-point: scale by 2^fbits in the SOURCE fp width, then
      // convert straight to the integer dest width.  Using Dst.Size (the int
      // width) for the scale multiplied a double source by a single-precision
      // 2^fbits with a narrowed result (e.g. `fcvtzs w, d, #8`), corrupting it.
      unsigned FpSz = (Src.Size == 8) ? 8 : (Src.Size == 2 ? 2 : 4);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
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
  // FRINT{A,I,M,N,P,X,Z} — round float to integral float.  A ties away, N/I/X
  // tie to even (IEEE default; I/X follow FPCR which defaults to even), M
  // floor, P ceil, Z toward zero.  Vector forms round each lane independently.
  case AARCH64_INS_FRINTA:
  case AARCH64_INS_FRINTI:
  case AARCH64_INS_FRINTM:
  case AARCH64_INS_FRINTN:
  case AARCH64_INS_FRINTP:
  case AARCH64_INS_FRINTX:
  case AARCH64_INS_FRINTZ: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    auto roundLane = [&](NdVar Lane, unsigned Sz) -> NdVar {
      NdVar R = S.makeTemp(Sz);
      if (Insn->id == AARCH64_INS_FRINTZ) {
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
