//===- ARMLiftSIMDFMA.cpp - ARM32 VFP multiply-accumulate lifter ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The fused and unfused multiply-accumulate family: VFMA, VFMS,
/// VFNMA, VFNMS, VNMUL, VNMLA and VNMLS.
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

bool liftSIMDFMA(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                 const cs_arm &ARM) {
  switch (Insn->id) {
  case ARM_INS_VFMA:
  case ARM_INS_VFMAB:
  case ARM_INS_VFMAT:
  case ARM_INS_VFMAS:
  case ARM_INS_VFMAL:
  case ARM_INS_VFMS:
  case ARM_INS_VFMSL: {
    if (ARM.op_count < 3)
      break;
    bool IsSub = (Insn->id == ARM_INS_VFMS || Insn->id == ARM_INS_VFMSL);
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar Old = NdVar::reg(Dst.Offset, Dst.Size);
    // NEON `vfma.f32 q,q,q` / `d,d,d` fuse multiply-add PER-LANE; a single
    // FLOAT_MULT/ADD on the whole D/Q register does one scalar FP op on the
    // packed bits (VectorAlgo8 ARM32 fdot/fmla read 0).
    unsigned LaneSz = (ARM.vector_data == ARM_VECTORDATA_F64) ? 8 : 4;
    if (Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        unsigned Off = I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz),
                Ld = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Ld, {Old, NdVar::cst(Off, 4)});
        // VFMA/VFMS are FUSED (single rounding): Ld + La*Lb = fma(La,Lb,Ld),
        // Ld - La*Lb = fma(-La,Lb,Ld).  Separate FMUL+FADD would round twice.
        NdVar Mul = La;
        if (IsSub) {
          Mul = S.makeTemp(LaneSz);
          S.emit(NdOp::FLOAT_NEG, Mul, {La});
        }
        NdVar Lr = S.makeTemp(LaneSz);
        S.emit(NdOp::FLOAT_FMA, Lr, {Mul, Lb, Ld});
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
      NdVar Mul = A;
      if (IsSub) {
        Mul = S.makeTemp(Dst.Size);
        S.emit(NdOp::FLOAT_NEG, Mul, {A});
      }
      S.emit(NdOp::FLOAT_FMA, Dst, {Mul, B, Old});
    }
    break;
  }
  case ARM_INS_VNMUL: {
    // VNMUL: Dst = -(a * b)
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_NEG, Dst, {Prod});
    break;
  }
  case ARM_INS_VFNMA: {
    // VFNMA is FUSED: Dst = -(Dst + a*b) = -fma(a,b,Dst) (single rounding).
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar Sum = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_FMA, Sum, {A, B, NdVar::reg(Dst.Offset, Dst.Size)});
    S.emit(NdOp::FLOAT_NEG, Dst, {Sum});
    break;
  }
  case ARM_INS_VFNMS: {
    // VFNMS is FUSED: Dst = a*b - Dst = fma(a,b,-Dst) (single rounding).
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar NegOld = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_NEG, NegOld, {NdVar::reg(Dst.Offset, Dst.Size)});
    S.emit(NdOp::FLOAT_FMA, Dst, {A, B, NegOld});
    break;
  }
  case ARM_INS_VNMLA: {
    // VNMLA is NOT fused (two roundings): Dst = -(Dst + a*b).
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    NdVar NegOld = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_NEG, NegOld, {NdVar::reg(Dst.Offset, Dst.Size)});
    S.emit(NdOp::FLOAT_SUB, Dst, {NegOld, Prod});
    break;
  }
  case ARM_INS_VNMLS: {
    // VNMLS is NOT fused (two roundings): Dst = a*b - Dst.
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_SUB, Dst, {Prod, NdVar::reg(Dst.Offset, Dst.Size)});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
