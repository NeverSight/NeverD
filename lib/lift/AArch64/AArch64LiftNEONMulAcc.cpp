//===- AArch64LiftNEONMulAcc.cpp - NEON multiply-accumulate ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-lane integer multiply-accumulate (MLA/MLS) and fused
/// floating-point multiply-add (FMLA/FMLS/FNMADD/FNMSUB),
/// including the by-element indexed forms.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftNEONMulAcc(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                    const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // SMIN/UMIN handled above in the combined SMAX/SMIN/UMAX/UMIN case
  // NEON integer multiply-accumulate.  Must be per-lane: a single full-width
  // INT_MULT multiplies the whole 128-bit register (cross-lane carry) and a
  // full-width INT_ADD then carries across lanes.  Supports the indexed form
  // (`mla v.4s, v.4s, v.s[idx]`) where the multiplier is a scalar broadcast.
  case AARCH64_INS_MLA:
  case AARCH64_INS_MLS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar DstR = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSub = (Insn->id == AARCH64_INS_MLS);
    int BLane = ARM64.operands[2].vector_index;
    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      bool AScalar = (A.Size <= LaneSz);
      bool BScalar = (B.Size <= LaneSz);
      // By-element `mla/mls v.T, v.T, vN.<ty>[idx]` broadcasts one source lane.
      if (BLane >= 0 && !BScalar) {
        NdVar BFull = L.operandWrite(ARM64.operands[2]);
        NdVar BElem = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, BElem,
               {BFull, NdVar::cst(static_cast<uint64_t>(BLane) * LaneSz, 4)});
        B = BElem;
        BScalar = true;
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = AScalar ? A : S.makeTemp(LaneSz);
        if (!AScalar)
          S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        NdVar Lb = BScalar ? B : S.makeTemp(LaneSz);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Ld = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ld, {DstR, NdVar::cst(I * LaneSz, 4)});
        NdVar P = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_MULT, P, {La, Lb});
        NdVar R = S.makeTemp(LaneSz);
        S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, R, {Ld, P});
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
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {A, B});
      S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Dst, {DstR, Prod});
    }
    break;
  }
  case AARCH64_INS_FMLA: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar DstR = NdVar::reg(Dst.Offset, Dst.Size);

    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;

    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      bool AScalar = (A.Size <= LaneSz);
      bool BScalar = (B.Size <= LaneSz);
      // By-element `fmla v.T, v.T, vN.<ty>[idx]` broadcasts one source lane.
      if (ARM64.operands[2].vector_index >= 0 && !BScalar) {
        NdVar BFull = L.operandWrite(ARM64.operands[2]);
        NdVar BElem = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, BElem,
               {BFull, NdVar::cst(static_cast<uint64_t>(
                                      ARM64.operands[2].vector_index) *
                                      LaneSz,
                                  4)});
        B = BElem;
        BScalar = true;
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = AScalar ? A : S.makeTemp(LaneSz);
        if (!AScalar)
          S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        NdVar Lb = BScalar ? B : S.makeTemp(LaneSz);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Ld = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ld, {DstR, NdVar::cst(I * LaneSz, 4)});
        // FMLA is fused (single rounding): Ld + La*Lb = fma(La,Lb,Ld).
        NdVar R = S.makeTemp(LaneSz);
        S.emit(NdOp::FLOAT_FMA, R, {La, Lb, Ld});
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
      S.emit(NdOp::FLOAT_FMA, Dst, {A, B, DstR});
    }
    break;
  }
  case AARCH64_INS_FMLS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar DstR = NdVar::reg(Dst.Offset, Dst.Size);

    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;

    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      bool AScalar = (A.Size <= LaneSz);
      bool BScalar = (B.Size <= LaneSz);
      // By-element `fmls v.T, v.T, vN.<ty>[idx]` broadcasts one source lane.
      if (ARM64.operands[2].vector_index >= 0 && !BScalar) {
        NdVar BFull = L.operandWrite(ARM64.operands[2]);
        NdVar BElem = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, BElem,
               {BFull, NdVar::cst(static_cast<uint64_t>(
                                      ARM64.operands[2].vector_index) *
                                      LaneSz,
                                  4)});
        B = BElem;
        BScalar = true;
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = AScalar ? A : S.makeTemp(LaneSz);
        if (!AScalar)
          S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        NdVar Lb = BScalar ? B : S.makeTemp(LaneSz);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Ld = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ld, {DstR, NdVar::cst(I * LaneSz, 4)});
        // FMLS is fused (single rounding): Ld - La*Lb = fma(-La,Lb,Ld).
        NdVar NLa = S.makeTemp(LaneSz);
        S.emit(NdOp::FLOAT_NEG, NLa, {La});
        NdVar R = S.makeTemp(LaneSz);
        S.emit(NdOp::FLOAT_FMA, R, {NLa, Lb, Ld});
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
      NdVar NA = S.makeTemp(Dst.Size);
      S.emit(NdOp::FLOAT_NEG, NA, {A});
      S.emit(NdOp::FLOAT_FMA, Dst, {NA, B, DstR});
    }
    break;
  }
  // NEON FNMADD / FNMSUB
  case AARCH64_INS_FNMADD: {
    // FNMADD Dd, Dn, Dm, Da: Dd = -(Da + Dn*Dm)
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar C = L.operandRead(S, ARM64.operands[3]);
    // Keep the negation inside the fused operation.  Negating an already
    // rounded fma(A, B, C) gives the wrong result under directed rounding.
    NdVar NA = S.makeTemp(Dst.Size);
    NdVar NC = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_NEG, NA, {A});
    S.emit(NdOp::FLOAT_NEG, NC, {C});
    S.emit(NdOp::FLOAT_FMA, Dst, {NA, B, NC});
    break;
  }
  case AARCH64_INS_FNMSUB: {
    // FNMSUB Dd, Dn, Dm, Da: Dd = Dn*Dm - Da
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar C = L.operandRead(S, ARM64.operands[3]);
    // Fused: A*B - C = fma(A,B,-C).
    NdVar NC = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_NEG, NC, {C});
    S.emit(NdOp::FLOAT_FMA, Dst, {A, B, NC});
    break;
  }
  default:
    return false;
  }
  return true;
}

} // namespace neverd
