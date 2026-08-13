//===- ARMLiftSIMDConvert.cpp - ARM32 VFP compare and convert lifter -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// VCMP/VCMPE and the floating-point conversion family VCVT, VCVTB,
/// VCVTT, VCVTR, VJCVT, VCVTA, VCVTN, VCVTM and VCVTP.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftSIMDConvert(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                     const cs_arm &ARM) {
  switch (Insn->id) {
  case ARM_INS_VCMP:
  case ARM_INS_VCMPE: {
    if (ARM.op_count < 2)
      break;
    NdVar A = L.operandRead(S, ARM.operands[0]);
    NdVar B = L.operandRead(S, ARM.operands[1]);
    // VCMP writes the FPSCR flags (not the CPSR flags); a later VMRS commits
    // them to CPSR.  Writing CPSR directly here would let a second VCMP clobber
    // the flags a pending conditional still needs (d_minmax: two vcmp before
    // the first conditional move consumes the first vcmp's result).
    // N = a < b
    S.emit(NdOp::FLOAT_LESS, NdVar::reg(armreg::FP_NFLAG, 1), {A, B});
    // Z = a == b
    S.emit(NdOp::FLOAT_EQUAL, NdVar::reg(armreg::FP_ZFLAG, 1), {A, B});
    // C = !(a < b) (i.e., a >= b or unordered)
    NdVar Lt = S.makeTemp(1);
    S.emit(NdOp::FLOAT_LESS, Lt, {A, B});
    S.emit(NdOp::BOOL_NOT, NdVar::reg(armreg::FP_CFLAG, 1), {Lt});
    // V = unordered (either operand is NaN)
    NdVar NanA = S.makeTemp(1);
    NdVar NanB = S.makeTemp(1);
    S.emit(NdOp::FLOAT_ISNAN, NanA, {A});
    S.emit(NdOp::FLOAT_ISNAN, NanB, {B});
    S.emit(NdOp::BOOL_OR, NdVar::reg(armreg::FP_VFLAG, 1), {NanA, NanB});
    break;
  }
  case ARM_INS_VCVT:
  case ARM_INS_VCVTB:
  case ARM_INS_VCVTT: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    NdOp Op = NdOp::FLOAT_FLOAT2FLOAT;
    bool Is32 = false; // 32-bit-element int<->float conversion (NEON per-lane)
    switch (ARM.vector_data) {
    case ARM_VECTORDATA_F32S32:
      Is32 = true;
      [[fallthrough]];
    case ARM_VECTORDATA_F32S16:
    case ARM_VECTORDATA_F64S32:
    case ARM_VECTORDATA_F64S16:
      Op = NdOp::FLOAT_INT2FLOAT;
      break;
    case ARM_VECTORDATA_F32U32:
      Is32 = true;
      [[fallthrough]];
    case ARM_VECTORDATA_F32U16:
    case ARM_VECTORDATA_F64U32:
    case ARM_VECTORDATA_F64U16:
      // Unsigned int -> float must use the unsigned conversion (the old code
      // used the signed FLOAT_INT2FLOAT for `vcvt.f32.u32`).
      Op = NdOp::FLOAT_UINT2FLOAT;
      break;
    case ARM_VECTORDATA_S32F32:
      Is32 = true;
      Op = NdOp::FLOAT_TRUNC;
      break;
    case ARM_VECTORDATA_U32F32:
      // Unsigned float -> int must truncate as unsigned (FPToUI); the signed
      // FLOAT_TRUNC (FPToSI) saturates values in [2^31, 2^32) to INT32_MAX.
      Is32 = true;
      Op = NdOp::FLOAT_FLOAT2UINT;
      break;
    case ARM_VECTORDATA_S32F64:
    case ARM_VECTORDATA_S16F32:
    case ARM_VECTORDATA_S16F64:
      Op = NdOp::FLOAT_TRUNC;
      break;
    case ARM_VECTORDATA_U32F64:
    case ARM_VECTORDATA_U16F32:
    case ARM_VECTORDATA_U16F64:
      Op = NdOp::FLOAT_FLOAT2UINT;
      break;
    default:
      Op = NdOp::FLOAT_FLOAT2FLOAT;
      break;
    }
    // NEON `vcvt.f32.s32 q,q` / `vcvt.s32.f32 q,q` etc. are PER-LANE 32-bit
    // conversions; a single op on the whole D/Q register converts the entire
    // 64/128-bit value as one scalar (VectorAlgo8 ARM32 FP read garbage).
    if (Is32 && Dst.Size > 4) {
      unsigned LaneSz = 4;
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Cvt = S.makeTemp(LaneSz);
        S.emit(Op, Cvt, {Lane});
        if (I == 0) {
          Acc = Cvt;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Cvt, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(Op, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VCVTR:
  case ARM_INS_VJCVT: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    S.emit(NdOp::FLOAT_FLOAT2INT, Dst, {Src});
    break;
  }
  case ARM_INS_VCVTA:
  case ARM_INS_VCVTN: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    NdVar Rounded = S.makeTemp(Src.Size);
    // VCVTA rounds ties away from zero; VCVTN ties to even (IEEE default).
    S.emit(Insn->id == ARM_INS_VCVTA ? NdOp::FLOAT_ROUND
                                     : NdOp::FLOAT_ROUNDEVEN,
           Rounded, {Src});
    S.emit(NdOp::FLOAT_FLOAT2INT, Dst, {Rounded});
    break;
  }
  case ARM_INS_VCVTM: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    NdVar Floored = S.makeTemp(Src.Size);
    S.emit(NdOp::FLOAT_FLOOR, Floored, {Src});
    S.emit(NdOp::FLOAT_FLOAT2INT, Dst, {Floored});
    break;
  }
  case ARM_INS_VCVTP: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    NdVar Ceiled = S.makeTemp(Src.Size);
    S.emit(NdOp::FLOAT_CEIL, Ceiled, {Src});
    S.emit(NdOp::FLOAT_FLOAT2INT, Dst, {Ceiled});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
