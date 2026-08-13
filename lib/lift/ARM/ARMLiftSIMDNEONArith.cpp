//===- ARMLiftSIMDNEONArith.cpp - ARM32 NEON add/subtract lifter ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-lane NEON add and subtract: the widening, narrowing, pairwise,
/// halving and rotated-complex forms.
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

bool liftSIMDNEONArith(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                       const cs_arm &ARM) {
  switch (Insn->id) {
  // NEON widening add: per-lane sign/zero extend then add
  case ARM_INS_VADDL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    if (LI.LaneSz > 0 && A.Size >= LI.LaneSz && Dst.Size > A.Size) {
      unsigned NLanes = A.Size / LI.LaneSz;
      unsigned WLaneSz = LI.LaneSz * 2;
      auto ExtOp = LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      NdVar Acc = NdVar::cst(0, 0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar LA = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, LA, {A, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar LB = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, LB, {B, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar WA = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WA, {LA});
        NdVar WB = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WB, {LB});
        NdVar Sum = S.makeTemp(WLaneSz);
        S.emit(NdOp::INT_ADD, Sum, {WA, WB});
        if (I == 0) {
          Acc = Sum;
        } else {
          NdVar P = S.makeTemp(Acc.Size + WLaneSz);
          S.emit(NdOp::CONCAT, P, {Sum, Acc});
          Acc = P;
        }
      }
      if (Acc.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Acc});
      else
        S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_ADD, Dst, {A, B});
    }
    break;
  }

  // NEON pairwise add: d[i] = a[2i] + a[2i+1], d[N/2+i] = b[2i] + b[2i+1]
  case ARM_INS_VPADD: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    if (LI.LaneSz > 0 && A.Size >= 2 * LI.LaneSz) {
      unsigned NPairs = A.Size / LI.LaneSz / 2;
      NdVar Acc = NdVar::cst(0, 0);
      auto doAdd = [&](const NdVar &Src, unsigned PairIdx) {
        for (unsigned P = 0; P < NPairs; ++P) {
          NdVar Lo = S.makeTemp(LI.LaneSz);
          S.emit(
              NdOp::SUBBYTES, Lo,
              {Src, NdVar::cst(static_cast<uint64_t>(P) * 2 * LI.LaneSz, 4)});
          NdVar Hi = S.makeTemp(LI.LaneSz);
          S.emit(NdOp::SUBBYTES, Hi,
                 {Src, NdVar::cst(
                           (static_cast<uint64_t>(P) * 2 + 1) * LI.LaneSz, 4)});
          NdVar Sum = S.makeTemp(LI.LaneSz);
          if (LI.IsFloat)
            S.emit(NdOp::FLOAT_ADD, Sum, {Lo, Hi});
          else
            S.emit(NdOp::INT_ADD, Sum, {Lo, Hi});
          if (PairIdx == 0 && P == 0) {
            Acc = Sum;
          } else {
            NdVar Prev = S.makeTemp(Acc.Size + LI.LaneSz);
            S.emit(NdOp::CONCAT, Prev, {Sum, Acc});
            Acc = Prev;
          }
        }
      };
      doAdd(A, 0);
      doAdd(B, 1);
      if (Acc.Size == Dst.Size)
        S.emit(NdOp::COPY, Dst, {Acc});
      else if (Acc.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Acc});
      else
        S.emit(NdOp::SUBBYTES, Dst, {Acc, NdVar::cst(0, 4)});
    } else {
      S.emit(NdOp::INT_ADD, Dst, {A, B});
    }
    break;
  }

  // VADDW: wide += widen(narrow), per lane.  operand[1] is already wide (Q),
  // operand[2] is the narrow source (D) which is sign/zero extended.
  case ARM_INS_VADDW: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned SrcLaneSz = LI.LaneSz;
    if (SrcLaneSz > 0 && SrcLaneSz < 8) {
      unsigned DstLaneSz = SrcLaneSz * 2;
      unsigned NLanes = Dst.Size / DstLaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar AW = S.makeTemp(DstLaneSz);
        S.emit(NdOp::SUBBYTES, AW, {A, NdVar::cst(I * DstLaneSz, 4)});
        NdVar BN = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, BN, {B, NdVar::cst(I * SrcLaneSz, 4)});
        NdVar BW = S.makeTemp(DstLaneSz);
        S.emit(LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, BW, {BN});
        NdVar R = S.makeTemp(DstLaneSz);
        S.emit(NdOp::INT_ADD, R, {AW, BW});
        if (I == 0) {
          Acc = R;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_ADD, Dst, {A, B});
    }
    break;
  }
  // VADDHN/VRADDHN: add two wide vectors, return the high half of each lane
  // (rounding variant adds half an ULP first).
  case ARM_INS_VADDHN:
  case ARM_INS_VRADDHN: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    bool IsRound = (Insn->id == ARM_INS_VRADDHN);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned WideLaneSz = LI.LaneSz;
    if (WideLaneSz >= 2 && A.Size >= WideLaneSz) {
      unsigned NarrowLaneSz = WideLaneSz / 2;
      unsigned NLanes = A.Size / WideLaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar AL = S.makeTemp(WideLaneSz), BL = S.makeTemp(WideLaneSz);
        S.emit(NdOp::SUBBYTES, AL, {A, NdVar::cst(I * WideLaneSz, 4)});
        S.emit(NdOp::SUBBYTES, BL, {B, NdVar::cst(I * WideLaneSz, 4)});
        NdVar Sum = S.makeTemp(WideLaneSz);
        S.emit(NdOp::INT_ADD, Sum, {AL, BL});
        if (IsRound) {
          NdVar Sum1 = S.makeTemp(WideLaneSz);
          S.emit(
              NdOp::INT_ADD, Sum1,
              {Sum, NdVar::cst(1ULL << (NarrowLaneSz * 8 - 1), WideLaneSz)});
          Sum = Sum1;
        }
        NdVar Hi = S.makeTemp(NarrowLaneSz);
        S.emit(NdOp::SUBBYTES, Hi, {Sum, NdVar::cst(NarrowLaneSz, 4)});
        if (I == 0) {
          Acc = Hi;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + NarrowLaneSz);
          S.emit(NdOp::CONCAT, Next, {Hi, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_ADD, Dst, {A, B});
    }
    break;
  }
  // VSUBHN/VRSUBHN: subtract two wide vectors, return the high half of each
  // lane (rounding variant adds half an ULP first).  Mirror of VADDHN; was a
  // full-width INT_SUB placeholder (no narrowing, no per-lane).
  case ARM_INS_VSUBHN:
  case ARM_INS_VRSUBHN: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    bool IsRound = (Insn->id == ARM_INS_VRSUBHN);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned WideLaneSz = LI.LaneSz;
    if (WideLaneSz >= 2 && A.Size >= WideLaneSz) {
      unsigned NarrowLaneSz = WideLaneSz / 2;
      unsigned NLanes = A.Size / WideLaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar AL = S.makeTemp(WideLaneSz), BL = S.makeTemp(WideLaneSz);
        S.emit(NdOp::SUBBYTES, AL, {A, NdVar::cst(I * WideLaneSz, 4)});
        S.emit(NdOp::SUBBYTES, BL, {B, NdVar::cst(I * WideLaneSz, 4)});
        NdVar Diff = S.makeTemp(WideLaneSz);
        S.emit(NdOp::INT_SUB, Diff, {AL, BL});
        if (IsRound) {
          NdVar Diff1 = S.makeTemp(WideLaneSz);
          S.emit(
              NdOp::INT_ADD, Diff1,
              {Diff, NdVar::cst(1ULL << (NarrowLaneSz * 8 - 1), WideLaneSz)});
          Diff = Diff1;
        }
        NdVar Hi = S.makeTemp(NarrowLaneSz);
        S.emit(NdOp::SUBBYTES, Hi, {Diff, NdVar::cst(NarrowLaneSz, 4)});
        if (I == 0) {
          Acc = Hi;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + NarrowLaneSz);
          S.emit(NdOp::CONCAT, Next, {Hi, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_SUB, Dst, {A, B});
    }
    break;
  }
  // Per-lane halving add: dst[i] = (a[i]+b[i](+1)) >> 1.  VHADD truncates,
  // VRHADD rounds; sign/zero from the data type.  The old code did a single
  // full-width INT_ADD (no halving, no per-lane).
  case ARM_INS_VHADD:
  case ARM_INS_VRHADD: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    bool IsRound = (Insn->id == ARM_INS_VRHADD);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned LaneSz = LI.LaneSz;
    if (LaneSz > 0 && LaneSz <= 4 && Dst.Size >= LaneSz) {
      unsigned WideSz = LaneSz * 2;
      unsigned NLanes = Dst.Size / LaneSz;
      NdOp ExtOp = LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      NdOp ShOp = LI.IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Al = S.makeTemp(LaneSz), Bl = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Al, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Bl, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Aw = S.makeTemp(WideSz), Bw = S.makeTemp(WideSz);
        S.emit(ExtOp, Aw, {Al});
        S.emit(ExtOp, Bw, {Bl});
        NdVar Sum = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ADD, Sum, {Aw, Bw});
        if (IsRound) {
          NdVar Sum1 = S.makeTemp(WideSz);
          S.emit(NdOp::INT_ADD, Sum1, {Sum, NdVar::cst(1, WideSz)});
          Sum = Sum1;
        }
        NdVar Sh = S.makeTemp(WideSz);
        S.emit(ShOp, Sh, {Sum, NdVar::cst(1, WideSz)});
        NdVar Res = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Res, {Sh, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = Res;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Res, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_ADD, Dst, {A, B});
    }
    break;
  }
  // Per-lane halving subtract: dst[i] = (a[i]-b[i]) >> 1 (no rounding form).
  // Was grouped with VSUBHN/VCADD as a full-width INT_SUB placeholder (no
  // halving, cross-lane borrow).
  case ARM_INS_VHSUB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned LaneSz = LI.LaneSz;
    if (LaneSz > 0 && LaneSz <= 4 && Dst.Size >= LaneSz) {
      unsigned WideSz = LaneSz * 2;
      unsigned NLanes = Dst.Size / LaneSz;
      NdOp ExtOp = LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      NdOp ShOp = LI.IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Al = S.makeTemp(LaneSz), Bl = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Al, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Bl, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Aw = S.makeTemp(WideSz), Bw = S.makeTemp(WideSz);
        S.emit(ExtOp, Aw, {Al});
        S.emit(ExtOp, Bw, {Bl});
        NdVar Diff = S.makeTemp(WideSz);
        S.emit(NdOp::INT_SUB, Diff, {Aw, Bw});
        NdVar Sh = S.makeTemp(WideSz);
        S.emit(ShOp, Sh, {Diff, NdVar::cst(1, WideSz)});
        NdVar Res = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Res, {Sh, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = Res;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Res, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_SUB, Dst, {A, B});
    }
    break;
  }
  // VPADDL: pairwise add adjacent lanes of one source and widen to 2x lane
  // (Dst[i] = widen(Src[2i]) + widen(Src[2i+1])).  VPADAL additionally adds
  // the prior Dst lane.  The old placeholder just COPYed the source, dropping
  // both the pairwise sum and the widening — breaking every vcnt+vpaddl
  // popcount reduction clang emits.
  case ARM_INS_VPADDL:
  case ARM_INS_VPADAL: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    bool IsAccum = (Insn->id == ARM_INS_VPADAL);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned LaneSz = LI.LaneSz;
    if (LaneSz > 0 && LaneSz < 8 && Dst.Size >= LaneSz * 2) {
      unsigned WLaneSz = LaneSz * 2;
      unsigned NDst = Dst.Size / WLaneSz;
      auto ExtOp = LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      NdVar OldDst;
      if (IsAccum)
        OldDst = L.operandRead(S, ARM.operands[0]);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NDst; ++I) {
        NdVar A = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, A, {Src, NdVar::cst((2 * I) * LaneSz, 4)});
        NdVar B = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, B, {Src, NdVar::cst((2 * I + 1) * LaneSz, 4)});
        NdVar WA = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WA, {A});
        NdVar WB = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WB, {B});
        NdVar Sum = S.makeTemp(WLaneSz);
        S.emit(NdOp::INT_ADD, Sum, {WA, WB});
        if (IsAccum) {
          NdVar Old = S.makeTemp(WLaneSz);
          S.emit(NdOp::SUBBYTES, Old, {OldDst, NdVar::cst(I * WLaneSz, 4)});
          NdVar Sum2 = S.makeTemp(WLaneSz);
          S.emit(NdOp::INT_ADD, Sum2, {Sum, Old});
          Sum = Sum2;
        }
        if (I == 0) {
          Acc = Sum;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + WLaneSz);
          S.emit(NdOp::CONCAT, Next, {Sum, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VSUBL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    if (LI.LaneSz > 0 && A.Size >= LI.LaneSz && Dst.Size > A.Size) {
      unsigned NLanes = A.Size / LI.LaneSz;
      unsigned WLaneSz = LI.LaneSz * 2;
      auto ExtOp = LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      NdVar Acc = NdVar::cst(0, 0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar LA = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, LA, {A, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar LB = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, LB, {B, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar WA = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WA, {LA});
        NdVar WB = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WB, {LB});
        NdVar Diff = S.makeTemp(WLaneSz);
        S.emit(NdOp::INT_SUB, Diff, {WA, WB});
        if (I == 0) {
          Acc = Diff;
        } else {
          NdVar P = S.makeTemp(Acc.Size + WLaneSz);
          S.emit(NdOp::CONCAT, P, {Diff, Acc});
          Acc = P;
        }
      }
      if (Acc.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Acc});
      else
        S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_SUB, Dst, {A, B});
    }
    break;
  }
  // VSUBW: wide -= widen(narrow), per lane (mirror of VADDW).  The generic
  // full-width INT_SUB both skipped per-lane widening of the narrow D source
  // and propagated the borrow across the 64-bit lanes.
  case ARM_INS_VSUBW: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned SrcLaneSz = LI.LaneSz;
    if (SrcLaneSz > 0 && SrcLaneSz < 8) {
      unsigned DstLaneSz = SrcLaneSz * 2;
      unsigned NLanes = Dst.Size / DstLaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar AW = S.makeTemp(DstLaneSz);
        S.emit(NdOp::SUBBYTES, AW, {A, NdVar::cst(I * DstLaneSz, 4)});
        NdVar BN = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, BN, {B, NdVar::cst(I * SrcLaneSz, 4)});
        NdVar BW = S.makeTemp(DstLaneSz);
        S.emit(LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, BW, {BN});
        NdVar R = S.makeTemp(DstLaneSz);
        S.emit(NdOp::INT_SUB, R, {AW, BW});
        if (I == 0) {
          Acc = R;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_SUB, Dst, {A, B});
    }
    break;
  }
  case ARM_INS_VHCADD:
  // VCADD — rotated complex floating-point add (AArch32 FEAT_FCMA).  Per pair:
  //   rot 90:  re = Vn.re - Vm.im;  im = Vn.im + Vm.re
  //   rot 270: re = Vn.re + Vm.im;  im = Vn.im - Vm.re
  // The old code was a whole-register INT_SUB (integer op on FP, no rotation).
  case ARM_INS_VCADD: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Vn = L.operandRead(S, ARM.operands[1]);
    NdVar Vm = L.operandRead(S, ARM.operands[2]);
    int64_t Rot = ARM.operands[3].imm;
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned ES = LI.LaneSz;
    if (!LI.IsFloat || (ES != 4 && ES != 8) || Dst.Size < 2 * ES) {
      S.emit(NdOp::INT_SUB, Dst, {Vn, Vm});
      break;
    }
    auto lane = [&](NdVar V, unsigned Idx) {
      NdVar T = S.makeTemp(ES);
      S.emit(NdOp::SUBBYTES, T,
             {V, NdVar::cst(static_cast<uint64_t>(Idx) * ES, 4)});
      return T;
    };
    NdVar Acc = NdVar::cst(0, 0);
    bool First = true;
    auto append = [&](NdVar L) {
      if (First) {
        Acc = L;
        First = false;
        return;
      }
      NdVar N = S.makeTemp(Acc.Size + ES);
      S.emit(NdOp::CONCAT, N, {L, Acc});
      Acc = N;
    };
    unsigned NLanes = Dst.Size / ES;
    for (unsigned K = 0; K < NLanes / 2; ++K) {
      NdVar VnRe = lane(Vn, 2 * K), VnIm = lane(Vn, 2 * K + 1);
      NdVar VmRe = lane(Vm, 2 * K), VmIm = lane(Vm, 2 * K + 1);
      NdVar OutRe = S.makeTemp(ES), OutIm = S.makeTemp(ES);
      if (Rot == 270) {
        S.emit(NdOp::FLOAT_ADD, OutRe, {VnRe, VmIm});
        S.emit(NdOp::FLOAT_SUB, OutIm, {VnIm, VmRe});
      } else {
        S.emit(NdOp::FLOAT_SUB, OutRe, {VnRe, VmIm});
        S.emit(NdOp::FLOAT_ADD, OutIm, {VnIm, VmRe});
      }
      append(OutRe);
      append(OutIm);
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
