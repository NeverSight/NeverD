//===- ARMLiftSIMDNEONCompare.cpp - ARM32 NEON compare lifter ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-lane absolute difference (VABD, VABA and their widening and
/// reducing forms) and the mask-producing compares VCEQ, VCGE, VCGT,
/// VCLE, VCLT and VTST.
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

namespace {
// NEON vector compares (VCEQ/VCGT/VTST/...) set *each lane* to all-ones when
// the per-lane predicate holds and all-zeros otherwise.  Emitting a single
// full-width compare (the old behaviour) collapses the whole register to a
// 0/1 boolean, which is only accidentally correct when callers read lane 0 in
// scenarios where the wide result happens to coincide.  This helper performs
// the comparison lane-by-lane and writes an explicit all-ones/all-zeros mask.
void emitPerLaneCmpMask(ARMLifter::LiftState &S, NdVar Dst, NdVar A,
                        NdVar B, NdOp CmpOp, unsigned LaneSz) {
  if (LaneSz == 0 || Dst.Size <= LaneSz) {
    // Unknown lane size or scalar width: fall back to a full-width compare so
    // we never regress relative to the previous behaviour.
    S.emit(CmpOp, Dst, {A, B});
    return;
  }
  unsigned NLanes = Dst.Size / LaneSz;
  uint64_t AllOnes = (LaneSz >= 8) ? ~0ULL : ((1ULL << (LaneSz * 8)) - 1);
  NdVar Acc = S.makeTemp(0);
  for (unsigned I = 0; I < NLanes; ++I) {
    NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
    S.emit(NdOp::SUBBYTES, La,
           {A, NdVar::cst(static_cast<uint64_t>(I) * LaneSz, 4)});
    S.emit(NdOp::SUBBYTES, Lb,
           {B, NdVar::cst(static_cast<uint64_t>(I) * LaneSz, 4)});
    NdVar Cmp = S.makeTemp(1);
    S.emit(CmpOp, Cmp, {La, Lb});
    NdVar Mask = S.makeTemp(LaneSz);
    S.emit(NdOp::SELECT, Mask,
           {Cmp, NdVar::cst(AllOnes, LaneSz), NdVar::cst(0, LaneSz)});
    if (I == 0)
      Acc = Mask;
    else {
      NdVar Next = S.makeTemp(Acc.Size + LaneSz);
      S.emit(NdOp::CONCAT, Next, {Mask, Acc});
      Acc = Next;
    }
  }
  S.emit(NdOp::COPY, Dst, {Acc});
}
} // namespace

bool liftSIMDNEONCompare(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                         const cs_arm &ARM) {
  switch (Insn->id) {
  case ARM_INS_VABD:
  case ARM_INS_VABDL:
  case ARM_INS_VABA:
  case ARM_INS_VABAL: {
    // VABD: Vd = |Vn - Vm|;  VABA: Vd += |Vn - Vm|  (the "L" forms widen the
    // narrow input lanes to double width).  Both must be per-lane: the previous
    // implementation did a single full-width compare+subtract+select over the
    // whole register, collapsing all lanes to one predicate and propagating
    // borrows across lane boundaries (broke SAD: `vaba.u32`/`vabd.u32`).
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    bool IsAccum = (Insn->id == ARM_INS_VABA || Insn->id == ARM_INS_VABAL);
    bool IsWiden = (Insn->id == ARM_INS_VABDL || Insn->id == ARM_INS_VABAL);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned InSz = LI.LaneSz; // narrow (source) lane width
    bool IsSigned = LI.IsSigned;
    if (InSz > 0) {
      unsigned OutSz = IsWiden ? InSz * 2 : InSz;
      unsigned NLanes = Dst.Size / OutSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(InSz);
        S.emit(NdOp::SUBBYTES, La,
               {A, NdVar::cst(static_cast<uint64_t>(I) * InSz, 4)});
        NdVar Lb = S.makeTemp(InSz);
        S.emit(NdOp::SUBBYTES, Lb,
               {B, NdVar::cst(static_cast<uint64_t>(I) * InSz, 4)});
        // Compute |a-b| in the (possibly widened) output lane width.
        NdVar Wa = La, Wb = Lb;
        if (OutSz != InSz) {
          Wa = S.makeTemp(OutSz);
          Wb = S.makeTemp(OutSz);
          NdOp Ext = IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
          S.emit(Ext, Wa, {La});
          S.emit(Ext, Wb, {Lb});
        }
        NdVar Lt = S.makeTemp(1);
        S.emit(IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS, Lt, {Wa, Wb});
        NdVar DiffPos = S.makeTemp(OutSz);
        S.emit(NdOp::INT_SUB, DiffPos, {Wa, Wb});
        NdVar DiffNeg = S.makeTemp(OutSz);
        S.emit(NdOp::INT_SUB, DiffNeg, {Wb, Wa});
        NdVar Abs = S.makeTemp(OutSz);
        S.emit(NdOp::SELECT, Abs, {Lt, DiffNeg, DiffPos});
        if (IsAccum) {
          NdVar Old = S.makeTemp(OutSz);
          S.emit(
              NdOp::SUBBYTES, Old,
              {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(I * OutSz, 4)});
          NdVar Sum = S.makeTemp(OutSz);
          S.emit(NdOp::INT_ADD, Sum, {Old, Abs});
          Abs = Sum;
        }
        if (I == 0)
          Acc = Abs;
        else {
          NdVar Next = S.makeTemp(Acc.Size + OutSz);
          S.emit(NdOp::CONCAT, Next, {Abs, Acc});
          Acc = Next;
        }
      }
      if (Acc.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Acc});
      else
        S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      // Unknown lane width: fall back to the (scalar) absolute difference.
      NdVar GE = S.makeTemp(1);
      S.emit(NdOp::INT_LESSEQUAL, GE, {B, A});
      NdVar DiffPos = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SUB, DiffPos, {A, B});
      NdVar DiffNeg = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SUB, DiffNeg, {B, A});
      NdVar AbsDiff = S.makeTemp(Dst.Size);
      S.emit(NdOp::SELECT, AbsDiff, {GE, DiffPos, DiffNeg});
      if (IsAccum)
        S.emit(NdOp::INT_ADD, Dst,
               {NdVar::reg(Dst.Offset, Dst.Size), AbsDiff});
      else
        S.emit(NdOp::COPY, Dst, {AbsDiff});
    }
    break;
  }
  case ARM_INS_VABAV: {
    // VABAV: scalar += sum of |Vn - Vm| across all lanes (reduce).
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned InSz = LI.LaneSz;
    bool IsSigned = LI.IsSigned;
    if (InSz > 0 && A.Size >= InSz) {
      unsigned NLanes = A.Size / InSz;
      NdVar Sum = NdVar::reg(Dst.Offset, Dst.Size);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(InSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * InSz, 4)});
        NdVar Lb = S.makeTemp(InSz);
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * InSz, 4)});
        NdVar Lt = S.makeTemp(1);
        S.emit(IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS, Lt, {La, Lb});
        NdVar DiffPos = S.makeTemp(InSz);
        S.emit(NdOp::INT_SUB, DiffPos, {La, Lb});
        NdVar DiffNeg = S.makeTemp(InSz);
        S.emit(NdOp::INT_SUB, DiffNeg, {Lb, La});
        NdVar Abs = S.makeTemp(InSz);
        S.emit(NdOp::SELECT, Abs, {Lt, DiffNeg, DiffPos});
        NdVar AbsW = Abs;
        if (Dst.Size != InSz) {
          AbsW = S.makeTemp(Dst.Size);
          S.emit(NdOp::INT_ZEXT, AbsW, {Abs});
        }
        NdVar NewSum = S.makeTemp(Dst.Size);
        S.emit(NdOp::INT_ADD, NewSum, {Sum, AbsW});
        Sum = NewSum;
      }
      S.emit(NdOp::COPY, Dst, {Sum});
    }
    break;
  }

  // NEON compare
  case ARM_INS_VCEQ: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp Op = LI.IsFloat ? NdOp::FLOAT_EQUAL : NdOp::INT_EQUAL;
    emitPerLaneCmpMask(S, Dst, A, B, Op, LI.LaneSz);
    break;
  }
  case ARM_INS_VCGE:
  case ARM_INS_VACGE: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    // a >= b  <=>  b <= a (operands swapped).
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp Op = LI.IsFloat    ? NdOp::FLOAT_LESSEQUAL
              : LI.IsSigned ? NdOp::INT_SLESSEQUAL
                            : NdOp::INT_LESSEQUAL;
    emitPerLaneCmpMask(S, Dst, B, A, Op, LI.LaneSz);
    break;
  }
  case ARM_INS_VCGT:
  case ARM_INS_VACGT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    // a > b  <=>  b < a (operands swapped).
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp Op = LI.IsFloat    ? NdOp::FLOAT_LESS
              : LI.IsSigned ? NdOp::INT_SLESS
                            : NdOp::INT_LESS;
    emitPerLaneCmpMask(S, Dst, B, A, Op, LI.LaneSz);
    break;
  }
  case ARM_INS_VCLE: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp Op = LI.IsFloat    ? NdOp::FLOAT_LESSEQUAL
              : LI.IsSigned ? NdOp::INT_SLESSEQUAL
                            : NdOp::INT_LESSEQUAL;
    emitPerLaneCmpMask(S, Dst, A, B, Op, LI.LaneSz);
    break;
  }
  case ARM_INS_VCLT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp Op = LI.IsFloat    ? NdOp::FLOAT_LESS
              : LI.IsSigned ? NdOp::INT_SLESS
                            : NdOp::INT_LESS;
    emitPerLaneCmpMask(S, Dst, A, B, Op, LI.LaneSz);
    break;
  }
  case ARM_INS_VTST: {
    // VTST sets each lane to all-ones when (a & b) != 0 in that lane, else 0.
    // The bitwise AND is identical full-width or per-lane, but the != 0 test
    // and the all-ones result must be computed *per lane*.
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar Anded = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, Anded, {A, B});
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    emitPerLaneCmpMask(S, Dst, Anded, NdVar::cst(0, A.Size),
                       NdOp::INT_NOTEQUAL, LI.LaneSz);
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
