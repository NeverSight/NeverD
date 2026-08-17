//===- ARMLiftSIMDNEONMul.cpp - ARM32 NEON multiply lifter ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-lane NEON multiply and multiply-accumulate: VMULL, VMUL high-
/// half variants, VMLA/VMLS and the widening VMLAL/VMLSL.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftSIMDNEONMul(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                     const cs_arm &ARM) {
  switch (Insn->id) {
  case ARM_INS_VMULL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    // vmull.p8 — polynomial (carry-less) widening multiply (8 i8 pairs -> 8
    // i16), NOT integer multiply; map to the ARM NEON intrinsic.
    if (ARM.vector_data == ARM_VECTORDATA_P8) {
      S.emitIntrinsic(Intrinsic::ArmVmullp, Dst, {A, B});
      break;
    }
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    if (LI.LaneSz > 0 && A.Size >= LI.LaneSz && Dst.Size > A.Size) {
      unsigned NLanes = A.Size / LI.LaneSz;
      unsigned WLaneSz = LI.LaneSz * 2;
      auto ExtOp = LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      // By-scalar `vmull.s16 q,d,d[idx]` broadcasts one Dm lane; operandRead
      // ignores vector_index and returns the whole Dm, so detect the lane index
      // and splat it (a per-lane SUBBYTES would walk d[0..N] instead).  Same
      // fix as #386 for VMUL/VMLA.
      int BLane = ARM.operands[2].neon_lane >= 0 ? ARM.operands[2].neon_lane
                                                 : ARM.operands[2].vector_index;
      NdVar ScalarB;
      if (BLane >= 0) {
        ScalarB = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, ScalarB,
               {B, NdVar::cst(static_cast<uint64_t>(BLane) * LI.LaneSz, 4)});
      }
      NdVar Acc = NdVar::cst(0, 0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar LA = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, LA,
               {A, NdVar::cst(static_cast<uint64_t>(I) * LI.LaneSz, 4)});
        NdVar LB = ScalarB;
        if (BLane < 0) {
          LB = S.makeTemp(LI.LaneSz);
          S.emit(NdOp::SUBBYTES, LB,
                 {B, NdVar::cst(static_cast<uint64_t>(I) * LI.LaneSz, 4)});
        }
        NdVar WA = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WA, {LA});
        NdVar WB = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WB, {LB});
        NdVar Prod = S.makeTemp(WLaneSz);
        S.emit(NdOp::INT_MULT, Prod, {WA, WB});
        if (I == 0) {
          Acc = Prod;
        } else {
          NdVar P = S.makeTemp(Acc.Size + WLaneSz);
          S.emit(NdOp::CONCAT, P, {Prod, Acc});
          Acc = P;
        }
      }
      if (Acc.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Acc});
      else
        S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_MULT, Dst, {A, B});
    }
    break;
  }
  case ARM_INS_VMULLB:
  case ARM_INS_VMULLT:
  case ARM_INS_VMULH:
  case ARM_INS_VRMULH: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    S.emit(NdOp::INT_MULT, Dst, {A, B});
    break;
  }
  // VMLA/VMLAS/VMLS — same-width multiply-accumulate.  Must be per-lane: a
  // single full-width INT_MULT/FLOAT_MULT multiplies the whole register and a
  // full-width add/sub carries across lane boundaries.  Supports the scalar
  // (`vmla.i32 q, q, d[idx]`) form where the multiplier is a broadcast.
  case ARM_INS_VMLA:
  case ARM_INS_VMLAS:
  case ARM_INS_VMLS: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSub = (Insn->id == ARM_INS_VMLS);
    bool IsFP = ARM.vector_data == ARM_VECTORDATA_F32 ||
                ARM.vector_data == ARM_VECTORDATA_F64;
    NdOp MulOp = IsFP ? NdOp::FLOAT_MULT : NdOp::INT_MULT;
    NdOp AccOp = IsFP ? (IsSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD)
                      : (IsSub ? NdOp::INT_SUB : NdOp::INT_ADD);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned LaneSz = LI.LaneSz;
    if (IsFP && LaneSz == 0)
      LaneSz = (ARM.vector_data == ARM_VECTORDATA_F64) ? 8 : 4;
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      // `vmla.iN/.fN qD, qN, dM[idx]` — the multiplier is ONE broadcast scalar
      // lane.  operandRead returns the whole Dm register, so detect the indexed
      // form via the operand's lane index and splat that lane; size-based
      // detection never fired (Dm reads back 8 bytes) and a per-lane SUBBYTES
      // walked dM[0],dM[1],… past the register.
      int BLane = ARM.operands[2].neon_lane >= 0 ? ARM.operands[2].neon_lane
                                                 : ARM.operands[2].vector_index;
      NdVar ScalarB;
      if (BLane >= 0) {
        ScalarB = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, ScalarB,
               {B, NdVar::cst(static_cast<uint64_t>(BLane) * LaneSz, 4)});
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        NdVar Lb = ScalarB;
        if (BLane < 0) {
          Lb = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        }
        NdVar Ld = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ld, {OldDst, NdVar::cst(I * LaneSz, 4)});
        NdVar P = S.makeTemp(LaneSz);
        S.emit(MulOp, P, {La, Lb});
        NdVar R = S.makeTemp(LaneSz);
        S.emit(AccOp, R, {Ld, P});
        if (I == 0)
          Acc = R;
        else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(MulOp, Prod, {A, B});
      S.emit(AccOp, Dst, {OldDst, Prod});
    }
    break;
  }
  // VMLAL/VMLSL — widening multiply-accumulate: Qd[i] +=
  // widen(Dn[i])*widen(Dm[i]). Source lanes are narrow (InSz), destination
  // lanes are double width (OutSz). A full-width multiply produces a single
  // 64x64->128 product, not per-lane.
  case ARM_INS_VMLAL:
  case ARM_INS_VMLSL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSub = (Insn->id == ARM_INS_VMLSL);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned InSz = LI.LaneSz;
    NdOp Ext = LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
    if (InSz > 0 && InSz <= 4) {
      unsigned OutSz = InSz * 2;
      unsigned NLanes = Dst.Size / OutSz;
      // By-scalar `vmlal.s16 q,d,d[idx]` broadcasts one Dm lane; operandRead
      // ignores vector_index (returns the whole 8-byte Dm) so the old
      // `B.Size<=InSz` test never fired — detect the lane index and splat it.
      int BLane = ARM.operands[2].neon_lane >= 0 ? ARM.operands[2].neon_lane
                                                 : ARM.operands[2].vector_index;
      NdVar ScalarB;
      if (BLane >= 0) {
        ScalarB = S.makeTemp(InSz);
        S.emit(NdOp::SUBBYTES, ScalarB,
               {B, NdVar::cst(static_cast<uint64_t>(BLane) * InSz, 4)});
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(InSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * InSz, 4)});
        NdVar Lb = ScalarB;
        if (BLane < 0) {
          Lb = S.makeTemp(InSz);
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * InSz, 4)});
        }
        NdVar Wa = S.makeTemp(OutSz);
        S.emit(Ext, Wa, {La});
        NdVar Wb = S.makeTemp(OutSz);
        S.emit(Ext, Wb, {Lb});
        NdVar P = S.makeTemp(OutSz);
        S.emit(NdOp::INT_MULT, P, {Wa, Wb});
        NdVar Ld = S.makeTemp(OutSz);
        S.emit(NdOp::SUBBYTES, Ld, {OldDst, NdVar::cst(I * OutSz, 4)});
        NdVar R = S.makeTemp(OutSz);
        S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, R, {Ld, P});
        if (I == 0)
          Acc = R;
        else {
          NdVar Next = S.makeTemp(Acc.Size + OutSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {A, B});
      S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Dst, {OldDst, Prod});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
