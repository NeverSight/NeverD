//===- AArch64LiftMulLong.cpp - Long multiply and leading sign bits -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The widening 32x32->64 multiplies SMULL/UMULL and
/// SMADDL/UMADDL, the 64x64 high-half UMULH/SMULH, and CLS.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftMulLong(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                 const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // --- SMULL / UMULL ---
  case AARCH64_INS_SMULL:
  case AARCH64_INS_UMULL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    bool IsSigned = (Insn->id == AARCH64_INS_SMULL);

    auto DstVas = ARM64.operands[0].vas;
    unsigned DstLane = 0;
    if (DstVas == AARCH64LAYOUT_VL_4S)
      DstLane = 4;
    else if (DstVas == AARCH64LAYOUT_VL_8H)
      DstLane = 2;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      DstLane = 8;

    if (DstLane > 0 && Dst.Size > DstLane) {
      unsigned NLanes = Dst.Size / DstLane;
      unsigned NarrowLane = DstLane / 2;
      // By-element `smull v.Ts, v.Th, vN.<ty>[idx]`: operandRead returns just
      // the selected element, so broadcast it to every lane instead of walking
      // B (a per-lane SUBBYTES would read past the element and zero
      // lanes 1..N).
      bool BScalar = (B.Size <= NarrowLane);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar NarrA = S.makeTemp(NarrowLane);
        S.emit(NdOp::SUBBYTES, NarrA, {A, NdVar::cst(I * NarrowLane, 4)});
        NdVar WA = S.makeTemp(DstLane);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WA, {NarrA});
        NdVar NarrB = BScalar ? B : S.makeTemp(NarrowLane);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, NarrB, {B, NdVar::cst(I * NarrowLane, 4)});
        NdVar WB = S.makeTemp(DstLane);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WB, {NarrB});
        NdVar Lr = S.makeTemp(DstLane);
        S.emit(NdOp::INT_MULT, Lr, {WA, WB});
        if (I == 0) {
          Acc = Lr;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLane);
          S.emit(NdOp::CONCAT, Next, {Lr, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar ExtA = S.makeTemp(8);
      NdVar ExtB = S.makeTemp(8);
      if (IsSigned) {
        S.emit(NdOp::INT_SEXT, ExtA, {A});
        S.emit(NdOp::INT_SEXT, ExtB, {B});
      } else {
        S.emit(NdOp::INT_ZEXT, ExtA, {A});
        S.emit(NdOp::INT_ZEXT, ExtB, {B});
      }
      S.emit(NdOp::INT_MULT, Dst, {ExtA, ExtB});
    }
    break;
  }

  // --- SMADDL / UMADDL (multiply-add long) ---
  // Capstone 6 alias: SMULL/UMULL Xd, Wn, Wm → id=SMADDL/UMADDL, op_count=3
  case AARCH64_INS_SMADDL:
  case AARCH64_INS_UMADDL: {
    if (Insn->is_alias && ARM64.op_count == 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar N = L.operandRead(S, ARM64.operands[1]);
      NdVar M = L.operandRead(S, ARM64.operands[2]);
      NdVar ExtN = S.makeTemp(8), ExtM = S.makeTemp(8);
      if (Insn->id == AARCH64_INS_SMADDL) {
        S.emit(NdOp::INT_SEXT, ExtN, {N});
        S.emit(NdOp::INT_SEXT, ExtM, {M});
      } else {
        S.emit(NdOp::INT_ZEXT, ExtN, {N});
        S.emit(NdOp::INT_ZEXT, ExtM, {M});
      }
      S.emit(NdOp::INT_MULT, Dst, {ExtN, ExtM});
      break;
    }
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar N = L.operandRead(S, ARM64.operands[1]);
    NdVar M = L.operandRead(S, ARM64.operands[2]);
    NdVar A = L.operandRead(S, ARM64.operands[3]);
    NdVar ExtN = S.makeTemp(8);
    NdVar ExtM = S.makeTemp(8);
    NdVar Prod = S.makeTemp(8);
    if (Insn->id == AARCH64_INS_SMADDL) {
      S.emit(NdOp::INT_SEXT, ExtN, {N});
      S.emit(NdOp::INT_SEXT, ExtM, {M});
    } else {
      S.emit(NdOp::INT_ZEXT, ExtN, {N});
      S.emit(NdOp::INT_ZEXT, ExtM, {M});
    }
    S.emit(NdOp::INT_MULT, Prod, {ExtN, ExtM});
    S.emit(NdOp::INT_ADD, Dst, {A, Prod});
    break;
  }

  // --- UMULH / SMULH (upper Half of 64×64 → 128 multiply) ---
  case AARCH64_INS_UMULH:
  case AARCH64_INS_SMULH: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar AExt = S.makeTemp(16);
    NdVar BExt = S.makeTemp(16);
    if (Insn->id == AARCH64_INS_SMULH) {
      S.emit(NdOp::INT_SEXT, AExt, {A});
      S.emit(NdOp::INT_SEXT, BExt, {B});
    } else {
      S.emit(NdOp::INT_ZEXT, AExt, {A});
      S.emit(NdOp::INT_ZEXT, BExt, {B});
    }
    NdVar Prod = S.makeTemp(16);
    S.emit(NdOp::INT_MULT, Prod, {AExt, BExt});
    S.emit(NdOp::SUBBYTES, Dst, {Prod, NdVar::cst(8, 4)});
    break;
  }

  // --- CLS (count leading sign Bits) ---
  // CLS(x) = CLZ(x ^ (x ASR 1)) - 1
  case AARCH64_INS_CLS: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    NdVar Shifted = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_ASHR, Shifted, {Src, NdVar::cst(1, Src.Size)});
    NdVar Xored = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_XOR, Xored, {Src, Shifted});
    NdVar Clz = S.makeTemp(Src.Size);
    S.emit(NdOp::LZCOUNT, Clz, {Xored});
    S.emit(NdOp::INT_SUB, Dst, {Clz, NdVar::cst(1, Src.Size)});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
