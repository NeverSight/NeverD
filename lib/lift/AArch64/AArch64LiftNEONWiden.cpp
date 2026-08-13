//===- AArch64LiftNEONWiden.cpp - NEON widening and narrowing -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Widening add/subtract (SADDL/UADDL/SADDW/UADDW/SSUBL/USUBL/
/// SSUBW/USUBW), add/subtract returning the high narrow half
/// (ADDHN/RADDHN/SUBHN/RSUBHN), widening shift-left-long
/// (SSHLL/USHLL) and the narrowing extracts XTN/SQXTN/UQXTN/
/// SQXTUN.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftNEONWiden(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                   const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // NEON widening add/sub
  case AARCH64_INS_SADDL:
  case AARCH64_INS_SADDL2:
  case AARCH64_INS_UADDL:
  case AARCH64_INS_UADDL2:
  case AARCH64_INS_SADDW:
  case AARCH64_INS_SADDW2:
  case AARCH64_INS_UADDW:
  case AARCH64_INS_UADDW2: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);

    auto DstVas = ARM64.operands[0].vas;
    unsigned DstLane = 0;
    if (DstVas == AARCH64LAYOUT_VL_4S)
      DstLane = 4;
    else if (DstVas == AARCH64LAYOUT_VL_8H)
      DstLane = 2;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      DstLane = 8;

    bool IsSigned =
        (Insn->id == AARCH64_INS_SADDL || Insn->id == AARCH64_INS_SADDL2 ||
         Insn->id == AARCH64_INS_SADDW || Insn->id == AARCH64_INS_SADDW2);
    bool IsWideningBoth =
        (Insn->id == AARCH64_INS_SADDL || Insn->id == AARCH64_INS_SADDL2 ||
         Insn->id == AARCH64_INS_UADDL || Insn->id == AARCH64_INS_UADDL2);
    // The "2" variants read the *upper* 64-bit half of the 128-bit source
    // register(s) for their narrow operands.
    bool IsUpper =
        (Insn->id == AARCH64_INS_SADDL2 || Insn->id == AARCH64_INS_UADDL2 ||
         Insn->id == AARCH64_INS_SADDW2 || Insn->id == AARCH64_INS_UADDW2);
    if (DstLane > 0 && Dst.Size > DstLane) {
      unsigned NLanes = Dst.Size / DstLane;
      unsigned NarrowLane = DstLane / 2;
      unsigned NarrowBase = IsUpper ? 8 : 0; // high 64 bits for "2" variants
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La, Lb;
        if (IsWideningBoth) {
          NdVar NarrA = S.makeTemp(NarrowLane);
          S.emit(NdOp::SUBBYTES, NarrA,
                 {A, NdVar::cst(NarrowBase + I * NarrowLane, 4)});
          La = S.makeTemp(DstLane);
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, La, {NarrA});
          NdVar NarrB = S.makeTemp(NarrowLane);
          S.emit(NdOp::SUBBYTES, NarrB,
                 {B, NdVar::cst(NarrowBase + I * NarrowLane, 4)});
          Lb = S.makeTemp(DstLane);
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Lb, {NarrB});
        } else {
          // SADDW/UADDW: A is already wide (read full lane), B is narrow and
          // taken from the upper half for the "2" variant.
          La = S.makeTemp(DstLane);
          S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * DstLane, 4)});
          NdVar NarrB = S.makeTemp(NarrowLane);
          S.emit(NdOp::SUBBYTES, NarrB,
                 {B, NdVar::cst(NarrowBase + I * NarrowLane, 4)});
          Lb = S.makeTemp(DstLane);
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Lb, {NarrB});
        }
        NdVar Lr = S.makeTemp(DstLane);
        S.emit(NdOp::INT_ADD, Lr, {La, Lb});
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
      S.emit(NdOp::INT_ADD, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_SSUBL:
  case AARCH64_INS_SSUBL2:
  case AARCH64_INS_USUBL:
  case AARCH64_INS_USUBL2:
  case AARCH64_INS_SSUBW:
  case AARCH64_INS_SSUBW2:
  case AARCH64_INS_USUBW:
  case AARCH64_INS_USUBW2: {
    // Per-lane widening subtract (mirror of the SADDL family).  A plain
    // full-width INT_SUB ignores the sign/zero extension and propagates borrows
    // across the lane boundaries.
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    auto DstVas = ARM64.operands[0].vas;
    unsigned DstLane = 0;
    if (DstVas == AARCH64LAYOUT_VL_4S)
      DstLane = 4;
    else if (DstVas == AARCH64LAYOUT_VL_8H)
      DstLane = 2;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      DstLane = 8;
    bool IsSigned =
        (Insn->id == AARCH64_INS_SSUBL || Insn->id == AARCH64_INS_SSUBL2 ||
         Insn->id == AARCH64_INS_SSUBW || Insn->id == AARCH64_INS_SSUBW2);
    bool IsWideningBoth =
        (Insn->id == AARCH64_INS_SSUBL || Insn->id == AARCH64_INS_SSUBL2 ||
         Insn->id == AARCH64_INS_USUBL || Insn->id == AARCH64_INS_USUBL2);
    bool IsUpper =
        (Insn->id == AARCH64_INS_SSUBL2 || Insn->id == AARCH64_INS_USUBL2 ||
         Insn->id == AARCH64_INS_SSUBW2 || Insn->id == AARCH64_INS_USUBW2);
    if (DstLane > 0 && Dst.Size > DstLane) {
      unsigned NLanes = Dst.Size / DstLane;
      unsigned NarrowLane = DstLane / 2;
      unsigned NarrowBase = IsUpper ? 8 : 0;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La, Lb;
        if (IsWideningBoth) {
          NdVar NarrA = S.makeTemp(NarrowLane);
          S.emit(NdOp::SUBBYTES, NarrA,
                 {A, NdVar::cst(NarrowBase + I * NarrowLane, 4)});
          La = S.makeTemp(DstLane);
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, La, {NarrA});
        } else {
          La = S.makeTemp(DstLane);
          S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * DstLane, 4)});
        }
        NdVar NarrB = S.makeTemp(NarrowLane);
        S.emit(NdOp::SUBBYTES, NarrB,
               {B, NdVar::cst(NarrowBase + I * NarrowLane, 4)});
        Lb = S.makeTemp(DstLane);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Lb, {NarrB});
        NdVar Lr = S.makeTemp(DstLane);
        S.emit(NdOp::INT_SUB, Lr, {La, Lb});
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
      S.emit(NdOp::INT_SUB, Dst, {A, B});
    }
    break;
  }
  // NEON {ADD,SUB}HN{2} / R{ADD,SUB}HN{2} — add/subtract returning high narrow:
  // each wide lane is added/subtracted at full width, then the HIGH half of the
  // result is taken and packed into the narrow destination.  The "2" variants
  // write the narrowed lanes into the UPPER half (preserving the low half); the
  // rounding variants add 1<<(narrowBits-1) before taking the high half.  The
  // old placeholder did a full-width INT_ADD/INT_SUB with no narrowing.
  case AARCH64_INS_ADDHN:
  case AARCH64_INS_ADDHN2:
  case AARCH64_INS_RADDHN:
  case AARCH64_INS_RADDHN2:
  case AARCH64_INS_SUBHN:
  case AARCH64_INS_SUBHN2:
  case AARCH64_INS_RSUBHN:
  case AARCH64_INS_RSUBHN2: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    bool IsSub =
        (Insn->id == AARCH64_INS_SUBHN || Insn->id == AARCH64_INS_SUBHN2 ||
         Insn->id == AARCH64_INS_RSUBHN || Insn->id == AARCH64_INS_RSUBHN2);
    bool IsRound =
        (Insn->id == AARCH64_INS_RADDHN || Insn->id == AARCH64_INS_RADDHN2 ||
         Insn->id == AARCH64_INS_RSUBHN || Insn->id == AARCH64_INS_RSUBHN2);
    bool IsUpper =
        (Insn->id == AARCH64_INS_ADDHN2 || Insn->id == AARCH64_INS_RADDHN2 ||
         Insn->id == AARCH64_INS_SUBHN2 || Insn->id == AARCH64_INS_RSUBHN2);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned WideSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_2D)
      WideSz = 8;
    else if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      WideSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      WideSz = 2;
    if (WideSz < 2) {
      S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Dst, {A, B});
      break;
    }
    unsigned NarrowSz = WideSz / 2;
    unsigned NLanes = A.Size / WideSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar AL = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, AL, {A, NdVar::cst(I * WideSz, 4)});
      NdVar BL = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, BL, {B, NdVar::cst(I * WideSz, 4)});
      NdVar Sum = S.makeTemp(WideSz);
      S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Sum, {AL, BL});
      if (IsRound) {
        NdVar Rounded = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ADD, Rounded,
               {Sum, NdVar::cst(1ULL << (NarrowSz * 8 - 1), WideSz)});
        Sum = Rounded;
      }
      NdVar Narrow = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Sum, NdVar::cst(NarrowSz, 4)});
      if (I == 0) {
        Acc = Narrow;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + NarrowSz);
        S.emit(NdOp::CONCAT, Next, {Narrow, Acc});
        Acc = Next;
      }
    }
    if (IsUpper && Dst.Size > Acc.Size) {
      unsigned LoSz = Dst.Size - Acc.Size;
      NdVar Lo = S.makeTemp(LoSz);
      S.emit(NdOp::SUBBYTES, Lo,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {Acc, Lo});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }
  // NEON extend
  // {S,U}SHLL{,2} — widening shift-left-long: each narrow lane is sign/zero
  // extended to the wide element and shifted left by an immediate.  A single
  // full-width INT_SEXT/INT_ZEXT (the old behaviour) is not per-lane and drops
  // the shift.  The "2" variants take the upper 64 bits of the source.
  case AARCH64_INS_SSHLL:
  case AARCH64_INS_SSHLL2:
  case AARCH64_INS_USHLL:
  case AARCH64_INS_USHLL2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    bool IsSigned =
        (Insn->id == AARCH64_INS_SSHLL || Insn->id == AARCH64_INS_SSHLL2);
    bool IsUpper =
        (Insn->id == AARCH64_INS_SSHLL2 || Insn->id == AARCH64_INS_USHLL2);
    uint64_t Shift = 0;
    if (ARM64.op_count >= 3 && ARM64.operands[2].type == AARCH64_OP_IMM)
      Shift = static_cast<uint64_t>(ARM64.operands[2].imm);
    unsigned SrcLaneSz = 0;
    switch (ARM64.operands[1].vas) {
    case AARCH64LAYOUT_VL_8B:
    case AARCH64LAYOUT_VL_16B:
      SrcLaneSz = 1;
      break;
    case AARCH64LAYOUT_VL_4H:
    case AARCH64LAYOUT_VL_8H:
      SrcLaneSz = 2;
      break;
    case AARCH64LAYOUT_VL_2S:
    case AARCH64LAYOUT_VL_4S:
      SrcLaneSz = 4;
      break;
    default:
      break;
    }
    if (SrcLaneSz > 0 && SrcLaneSz < 8) {
      unsigned DstLaneSz = SrcLaneSz * 2;
      unsigned NLanes = Dst.Size / DstLaneSz;
      unsigned Base = IsUpper ? 8 : 0;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar SLane = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, SLane,
               {Src, NdVar::cst(Base + I * SrcLaneSz, 4)});
        NdVar Wide = S.makeTemp(DstLaneSz);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Wide, {SLane});
        NdVar Shifted = Wide;
        if (Shift > 0) {
          Shifted = S.makeTemp(DstLaneSz);
          S.emit(NdOp::INT_LEFT, Shifted, {Wide, NdVar::cst(Shift, DstLaneSz)});
        }
        if (I == 0) {
          Acc = Shifted;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {Shifted, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Dst, {Src});
    }
    break;
  }
  // XTN — extract narrow: truncate each wide lane to half width (no
  // saturation). XTN2 writes the narrowed lanes into the UPPER half, preserving
  // the low half. (Was an intrinsic with no backend handler ->
  // silently returned 0.)
  case AARCH64_INS_XTN:
  case AARCH64_INS_XTN2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned WideSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      WideSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      WideSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_2D)
      WideSz = 8;
    if (WideSz == 0) {
      S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
      break;
    }
    unsigned NarrowSz = WideSz / 2;
    unsigned NLanes = Src.Size / WideSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * WideSz, 4)});
      NdVar Narrow = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Lane, NdVar::cst(0, 4)});
      if (I == 0) {
        Acc = Narrow;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + NarrowSz);
        S.emit(NdOp::CONCAT, Next, {Narrow, Acc});
        Acc = Next;
      }
    }
    if (Insn->id == AARCH64_INS_XTN2 && Dst.Size > Acc.Size) {
      unsigned LoSz = Dst.Size - Acc.Size;
      NdVar Lo = S.makeTemp(LoSz);
      S.emit(NdOp::SUBBYTES, Lo,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {Acc, Lo});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }
  // SQXTN — signed saturating narrow: each wide lane clamped to signed narrow
  // range.
  case AARCH64_INS_SQXTN:
  case AARCH64_INS_SQXTN2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned WideSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      WideSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      WideSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_2D)
      WideSz = 8;
    if (WideSz == 0) {
      // Scalar form (sqxtn s,d / h,s / b,h): the source's full width is the
      // single wide lane; saturate it instead of plain truncation.
      if (Src.Size == 2 || Src.Size == 4 || Src.Size == 8)
        WideSz = Src.Size;
      else {
        S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
        break;
      }
    }
    unsigned NarrowSz = WideSz / 2;
    unsigned NLanes = Src.Size / WideSz;
    int64_t SMax = (1LL << (NarrowSz * 8 - 1)) - 1;
    int64_t SMin = -(1LL << (NarrowSz * 8 - 1));
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * WideSz, 4)});
      // Narrowing saturate via trunc+sext overflow detect (avoids fork's
      // InstCombine mis-fold on INT_SLESS+SELECT clamp chains).
      NdVar Narrow = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Lane, NdVar::cst(0, 4)});
      NdVar BackWide = S.makeTemp(WideSz);
      S.emit(NdOp::INT_SEXT, BackWide, {Narrow});
      NdVar Fits = S.makeTemp(1);
      S.emit(NdOp::INT_EQUAL, Fits, {Lane, BackWide});
      NdVar IsPos = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, IsPos, {NdVar::cst(0, WideSz), Lane});
      NdVar OvfVal = S.makeTemp(NarrowSz);
      S.emit(NdOp::SELECT, OvfVal,
             {IsPos, NdVar::cst(static_cast<uint64_t>(SMax), NarrowSz),
              NdVar::cst(static_cast<uint64_t>(SMin), NarrowSz)});
      NdVar Result = S.makeTemp(NarrowSz);
      S.emit(NdOp::SELECT, Result, {Fits, Narrow, OvfVal});
      if (I == 0) {
        Acc = Result;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + NarrowSz);
        S.emit(NdOp::CONCAT, Next, {Result, Acc});
        Acc = Next;
      }
    }
    // The "2" form writes the narrowed lanes into the UPPER half of Dst and
    // preserves the lower half (the prior narrow result); the base form
    // replaces the whole register.  Without this SQXTN2 dropped the SQXTN low
    // half.
    if (Insn->id == AARCH64_INS_SQXTN2 && Dst.Size > Acc.Size) {
      unsigned LoSz = Dst.Size - Acc.Size;
      NdVar Lo = S.makeTemp(LoSz);
      S.emit(NdOp::SUBBYTES, Lo,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {Acc, Lo});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }
  // UQXTN — unsigned saturating narrow.
  case AARCH64_INS_UQXTN:
  case AARCH64_INS_UQXTN2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned WideSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      WideSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      WideSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_2D)
      WideSz = 8;
    if (WideSz == 0) {
      // Scalar form (uqxtn s,d / h,s / b,h): saturate the single wide lane.
      if (Src.Size == 2 || Src.Size == 4 || Src.Size == 8)
        WideSz = Src.Size;
      else {
        S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
        break;
      }
    }
    unsigned NarrowSz = WideSz / 2;
    unsigned NLanes = Src.Size / WideSz;
    uint64_t UMax = (1ULL << (NarrowSz * 8)) - 1;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * WideSz, 4)});
      NdVar GtMax = S.makeTemp(1);
      S.emit(NdOp::INT_LESS, GtMax, {NdVar::cst(UMax, WideSz), Lane});
      NdVar Clamped = S.makeTemp(WideSz);
      S.emit(NdOp::SELECT, Clamped, {GtMax, NdVar::cst(UMax, WideSz), Lane});
      NdVar Narrow = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Clamped, NdVar::cst(0, 4)});
      if (I == 0) {
        Acc = Narrow;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + NarrowSz);
        S.emit(NdOp::CONCAT, Next, {Narrow, Acc});
        Acc = Next;
      }
    }
    // The "2" form writes the narrowed lanes into Dst's UPPER half, preserving
    // the lower half (the prior narrow result).
    if (Insn->id == AARCH64_INS_UQXTN2 && Dst.Size > Acc.Size) {
      unsigned LoSz = Dst.Size - Acc.Size;
      NdVar Lo = S.makeTemp(LoSz);
      S.emit(NdOp::SUBBYTES, Lo,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {Acc, Lo});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }
  // SQXTUN — signed-to-unsigned saturating narrow.
  case AARCH64_INS_SQXTUN:
  case AARCH64_INS_SQXTUN2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned WideSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      WideSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      WideSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_2D)
      WideSz = 8;
    if (WideSz == 0) {
      // Scalar form (sqxtun s,d / h,s / b,h): saturate the single wide lane.
      if (Src.Size == 2 || Src.Size == 4 || Src.Size == 8)
        WideSz = Src.Size;
      else {
        S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
        break;
      }
    }
    unsigned NarrowSz = WideSz / 2;
    unsigned NLanes = Src.Size / WideSz;
    uint64_t UMax = (1ULL << (NarrowSz * 8)) - 1;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * WideSz, 4)});
      NdVar IsNeg = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, IsNeg, {Lane, NdVar::cst(0, WideSz)});
      NdVar Clamped1 = S.makeTemp(WideSz);
      S.emit(NdOp::SELECT, Clamped1, {IsNeg, NdVar::cst(0, WideSz), Lane});
      NdVar GtMax = S.makeTemp(1);
      S.emit(NdOp::INT_LESS, GtMax, {NdVar::cst(UMax, WideSz), Clamped1});
      NdVar Clamped2 = S.makeTemp(WideSz);
      S.emit(NdOp::SELECT, Clamped2,
             {GtMax, NdVar::cst(UMax, WideSz), Clamped1});
      NdVar Narrow = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Clamped2, NdVar::cst(0, 4)});
      if (I == 0) {
        Acc = Narrow;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + NarrowSz);
        S.emit(NdOp::CONCAT, Next, {Narrow, Acc});
        Acc = Next;
      }
    }
    // The "2" form writes the narrowed lanes into Dst's UPPER half, preserving
    // the lower half (the prior narrow result).
    if (Insn->id == AARCH64_INS_SQXTUN2 && Dst.Size > Acc.Size) {
      unsigned LoSz = Dst.Size - Acc.Size;
      NdVar Lo = S.makeTemp(LoSz);
      S.emit(NdOp::SUBBYTES, Lo,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {Acc, Lo});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }
  default:
    return false;
  }
  return true;
}

} // namespace neverd
