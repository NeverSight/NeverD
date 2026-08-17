//===- AArch64LiftNEONCompare.cpp - NEON vector compare -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-lane integer and floating-point compares (CMEQ/CMGE/CMGT/
/// CMHI/CMHS/CMLE/CMLT/CMTST, FCMEQ/FCMGE/FCMGT/FCMLE/FCMLT and
/// the absolute compares FACGE/FACGT), producing an all-ones or
/// all-zeros lane mask.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftNEONCompare(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // NEON vector compare — output is all-ones or all-zeros Mask
  case AARCH64_INS_CMEQ:
  case AARCH64_INS_CMGE:
  case AARCH64_INS_CMGT:
  case AARCH64_INS_CMHI:
  case AARCH64_INS_CMHS:
  case AARCH64_INS_CMLE:
  case AARCH64_INS_CMLT:
  case AARCH64_INS_CMTST:
  case AARCH64_INS_FACGE:
  case AARCH64_INS_FACGT:
  case AARCH64_INS_FCMEQ:
  case AARCH64_INS_FCMGE:
  case AARCH64_INS_FCMGT:
  case AARCH64_INS_FCMLE:
  case AARCH64_INS_FCMLT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A, B;
    if (ARM64.op_count >= 3) {
      A = L.operandRead(S, ARM64.operands[1]);
      B = L.operandRead(S, ARM64.operands[2]);
    } else {
      A = L.operandRead(S, ARM64.operands[1]);
      B = NdVar::cst(0, A.Size);
    }
    NdOp CmpOp = NdOp::INT_EQUAL;
    // FP compares (FCM*/FAC*) must use FLOAT_* ops; comparing the float bit
    // patterns as signed integers is only right for non-negative values (the
    // sign bit makes negatives look "large").  FACGE/FACGT compare absolute
    // values.
    bool IsAbs = false;
    switch (Insn->id) {
    case AARCH64_INS_CMEQ:
      CmpOp = NdOp::INT_EQUAL;
      break;
    case AARCH64_INS_FCMEQ:
      CmpOp = NdOp::FLOAT_EQUAL;
      break;
    case AARCH64_INS_CMGE:
      CmpOp = NdOp::INT_SLESSEQUAL;
      std::swap(A, B);
      break;
    case AARCH64_INS_FACGE:
      IsAbs = true;
      [[fallthrough]];
    case AARCH64_INS_FCMGE:
      CmpOp = NdOp::FLOAT_LESSEQUAL;
      std::swap(A, B);
      break;
    case AARCH64_INS_CMGT:
      CmpOp = NdOp::INT_SLESS;
      std::swap(A, B);
      break;
    case AARCH64_INS_FACGT:
      IsAbs = true;
      [[fallthrough]];
    case AARCH64_INS_FCMGT:
      CmpOp = NdOp::FLOAT_LESS;
      std::swap(A, B);
      break;
    case AARCH64_INS_CMHI:
      CmpOp = NdOp::INT_LESS;
      std::swap(A, B);
      break;
    case AARCH64_INS_CMHS:
      CmpOp = NdOp::INT_LESSEQUAL;
      std::swap(A, B);
      break;
    case AARCH64_INS_CMLE:
      CmpOp = NdOp::INT_SLESSEQUAL;
      break;
    case AARCH64_INS_FCMLE:
      CmpOp = NdOp::FLOAT_LESSEQUAL;
      break;
    case AARCH64_INS_CMLT:
      CmpOp = NdOp::INT_SLESS;
      break;
    case AARCH64_INS_FCMLT:
      CmpOp = NdOp::FLOAT_LESS;
      break;
    case AARCH64_INS_CMTST: {
      // Set each lane to all-ones when (a & b) != 0 in that lane.  The bitwise
      // AND is the same full-width or per-lane; rewrite the operands so the
      // shared per-lane mask logic below produces the correct per-lane result
      // (the old code emitted a single full-width 0/1 boolean).
      NdVar Anded = S.makeTemp(A.Size);
      S.emit(NdOp::INT_AND, Anded, {A, B});
      A = Anded;
      B = NdVar::cst(0, Anded.Size);
      CmpOp = NdOp::INT_NOTEQUAL;
      break;
    }
    default:
      break;
    }
    {
      auto Vas = ARM64.operands[0].vas;
      unsigned LaneSz = neonElemSize(Vas);
      // Capstone leaves the arrangement unset on Advanced SIMD scalar compare
      // forms (for example, `cmeq d0, d0, #0`).  The destination register then
      // names the single lane and supplies its width.
      bool InputsFitScalar = A.Size > 0 && B.Size > 0 &&
                             (A.isConst() || A.Size == Dst.Size) &&
                             (B.isConst() || B.Size == Dst.Size);
      if (LaneSz == 0 && Dst.Size > 0 && Dst.Size <= 8 && InputsFitScalar)
        LaneSz = Dst.Size;
      if (LaneSz > 0 && LaneSz <= 8 && Dst.Size >= LaneSz &&
          Dst.Size % LaneSz == 0) {
        unsigned NLanes = Dst.Size / LaneSz;
        NdVar Acc = S.makeTemp(0);
        auto ReadLane = [&](NdVar V, unsigned I) {
          // An immediate compare operand is broadcast to every lane.  Keep a
          // constant as a lane-sized value instead of trying to slice the
          // scalar encoding as though it were a packed vector.
          if (V.isConst())
            return NdVar::cst(V.Offset, LaneSz);
          NdVar Lane = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Lane,
                 {V, NdVar::cst(static_cast<uint64_t>(I) * LaneSz, 4)});
          return Lane;
        };
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = ReadLane(A, I);
          NdVar Lb = ReadLane(B, I);
          if (IsAbs) {
            NdVar AbsA = S.makeTemp(LaneSz), AbsB = S.makeTemp(LaneSz);
            S.emit(NdOp::FLOAT_ABS, AbsA, {La});
            S.emit(NdOp::FLOAT_ABS, AbsB, {Lb});
            La = AbsA;
            Lb = AbsB;
          }
          NdVar CmpRes = S.makeTemp(1);
          S.emit(CmpOp, CmpRes, {La, Lb});
          uint64_t AllOnes = (LaneSz == 1)   ? 0xFF
                             : (LaneSz == 2) ? 0xFFFF
                             : (LaneSz == 4) ? 0xFFFFFFFFULL
                                             : 0xFFFFFFFFFFFFFFFFULL;
          NdVar Mask = S.makeTemp(LaneSz);
          S.emit(NdOp::SELECT, Mask,
                 {CmpRes, NdVar::cst(AllOnes, LaneSz), NdVar::cst(0, LaneSz)});
          if (I == 0) {
            Acc = Mask;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Mask, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        // Unknown layouts retain the whole-value fallback.  Match a scalar
        // immediate to the other operand so comparisons never depend on the
        // generic immediate-width heuristic.
        if (A.isConst() && !B.isConst())
          A = NdVar::cst(A.Offset, B.Size);
        else if (B.isConst() && !A.isConst())
          B = NdVar::cst(B.Offset, A.Size);
        if (IsAbs) {
          NdVar AbsA = S.makeTemp(A.Size), AbsB = S.makeTemp(B.Size);
          S.emit(NdOp::FLOAT_ABS, AbsA, {A});
          S.emit(NdOp::FLOAT_ABS, AbsB, {B});
          A = AbsA;
          B = AbsB;
        }
        S.emit(CmpOp, Dst, {A, B});
      }
    }
    break;
  }
  default:
    return false;
  }
  return true;
}

} // namespace neverd
