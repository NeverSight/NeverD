//===- X86LiftSIMDIntArith.cpp - x86/x64 SIMD integer add/sub/min/max lifter
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSE/AVX packed integer arithmetic: wrapping and saturating
/// add/subtract, rounded average, and signed/unsigned
/// minimum/maximum.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftSIMDIntArith(X86Lifter &L, X86Lifter::LiftState &S,
                      const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // SIMD saturating add/sub — per-lane with saturation.
  // Uses LLVM saturating intrinsics (@llvm.{s,u}{add,sub}.sat) to avoid
  // a constant-folding bug in the fork's optimizer that mis-computes manual
  // sext→add→icmp→select→trunc clamp chains for signed saturation.
  case X86_INS_PADDSB:
  case X86_INS_PADDSW:
  case X86_INS_PADDUSB:
  case X86_INS_PADDUSW:
  case X86_INS_PSUBSB:
  case X86_INS_PSUBSW:
  case X86_INS_PSUBUSB:
  case X86_INS_PSUBUSW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    unsigned LaneSz = 1;
    bool IsSigned = false;
    bool IsSub = false;
    switch (InsnId) {
    case X86_INS_PADDSW:
    case X86_INS_PSUBSW:
    case X86_INS_PADDUSW:
    case X86_INS_PSUBUSW:
      LaneSz = 2;
      break;
    default:
      break;
    }
    switch (InsnId) {
    case X86_INS_PADDSB:
    case X86_INS_PADDSW:
    case X86_INS_PSUBSB:
    case X86_INS_PSUBSW:
      IsSigned = true;
      break;
    default:
      break;
    }
    switch (InsnId) {
    case X86_INS_PSUBSB:
    case X86_INS_PSUBSW:
    case X86_INS_PSUBUSB:
    case X86_INS_PSUBUSW:
      IsSub = true;
      break;
    default:
      break;
    }
    Intrinsic IC;
    if (IsSigned)
      IC = IsSub ? Intrinsic::X86_SsubSat : Intrinsic::X86_SaddSat;
    else
      IC = IsSub ? Intrinsic::X86_UsubSat : Intrinsic::X86_UaddSat;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {DstR, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {Src, NdVar::cst(Off, 4)});
        NdVar Lr = S.makeTemp(LaneSz);
        S.emitIntrinsic(IC, Lr, {La, Lb});
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
    NdVar LoHalf = BuildHalf(0);
    NdVar HiHalf = BuildHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // PAVG{B,W} — packed average bytes/words.
  case X86_INS_PAVGB:
  case X86_INS_PAVGW:
  case X86_INS_VPAVGB:
  case X86_INS_VPAVGW: {
    // Packed unsigned rounding average per lane: dst[i] = (a[i]+b[i]+1) >> 1.
    // The old code emitted a single full-width INT_ADD — no divide, no rounding
    // and not lane-isolated (carries crossed byte/word boundaries).
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz =
        (InsnId == X86_INS_PAVGB || InsnId == X86_INS_VPAVGB) ? 1 : 2;
    unsigned WideSz = LaneSz * 2;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar Al = S.makeTemp(LaneSz), Bl = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Al, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Bl, {B, NdVar::cst(Off, 4)});
        NdVar Aw = S.makeTemp(WideSz), Bw = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ZEXT, Aw, {Al});
        S.emit(NdOp::INT_ZEXT, Bw, {Bl});
        NdVar Sum = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ADD, Sum, {Aw, Bw});
        NdVar Sum1 = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ADD, Sum1, {Sum, NdVar::cst(1, WideSz)});
        NdVar Sh = S.makeTemp(WideSz);
        S.emit(NdOp::INT_RIGHT, Sh, {Sum1, NdVar::cst(1, WideSz)});
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
      return Acc;
    };
    NdVar LoHalf = BuildHalf(0);
    NdVar HiHalf = BuildHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // Packed min/max — per-lane comparison + select.
  case X86_INS_PMINSB:
  case X86_INS_PMINSW:
  case X86_INS_PMINSD:
  case X86_INS_PMINUB:
  case X86_INS_PMINUW:
  case X86_INS_PMINUD:
  case X86_INS_PMAXSB:
  case X86_INS_PMAXSW:
  case X86_INS_PMAXSD:
  case X86_INS_PMAXUB:
  case X86_INS_PMAXUW:
  case X86_INS_PMAXUD:
  case X86_INS_VPMINSB:
  case X86_INS_VPMINSW:
  case X86_INS_VPMINSD:
  case X86_INS_VPMINSQ:
  case X86_INS_VPMINUB:
  case X86_INS_VPMINUW:
  case X86_INS_VPMINUD:
  case X86_INS_VPMINUQ:
  case X86_INS_VPMAXSB:
  case X86_INS_VPMAXSW:
  case X86_INS_VPMAXSD:
  case X86_INS_VPMAXSQ:
  case X86_INS_VPMAXUB:
  case X86_INS_VPMAXUW:
  case X86_INS_VPMAXUD:
  case X86_INS_VPMAXUQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? L.operandRead(S, X86.operands[1])
                    : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 1;
    switch (InsnId) {
    case X86_INS_PMINSW:
    case X86_INS_PMAXSW:
    case X86_INS_PMINUW:
    case X86_INS_PMAXUW:
    case X86_INS_VPMINSW:
    case X86_INS_VPMAXSW:
    case X86_INS_VPMINUW:
    case X86_INS_VPMAXUW:
      LaneSz = 2;
      break;
    case X86_INS_PMINSD:
    case X86_INS_PMAXSD:
    case X86_INS_PMINUD:
    case X86_INS_PMAXUD:
    case X86_INS_VPMINSD:
    case X86_INS_VPMAXSD:
    case X86_INS_VPMINUD:
    case X86_INS_VPMAXUD:
      LaneSz = 4;
      break;
    case X86_INS_VPMINSQ:
    case X86_INS_VPMAXSQ:
    case X86_INS_VPMINUQ:
    case X86_INS_VPMAXUQ:
      LaneSz = 8;
      break;
    default:
      break;
    }
    bool IsSigned = false;
    switch (InsnId) {
    case X86_INS_PMINSB:
    case X86_INS_PMINSW:
    case X86_INS_PMINSD:
    case X86_INS_PMAXSB:
    case X86_INS_PMAXSW:
    case X86_INS_PMAXSD:
    case X86_INS_VPMINSB:
    case X86_INS_VPMINSW:
    case X86_INS_VPMINSD:
    case X86_INS_VPMINSQ:
    case X86_INS_VPMAXSB:
    case X86_INS_VPMAXSW:
    case X86_INS_VPMAXSD:
    case X86_INS_VPMAXSQ:
      IsSigned = true;
      break;
    default:
      break;
    }
    bool IsMax = false;
    switch (InsnId) {
    case X86_INS_PMAXSB:
    case X86_INS_PMAXSW:
    case X86_INS_PMAXSD:
    case X86_INS_PMAXUB:
    case X86_INS_PMAXUW:
    case X86_INS_PMAXUD:
    case X86_INS_VPMAXSB:
    case X86_INS_VPMAXSW:
    case X86_INS_VPMAXSD:
    case X86_INS_VPMAXSQ:
    case X86_INS_VPMAXUB:
    case X86_INS_VPMAXUW:
    case X86_INS_VPMAXUD:
    case X86_INS_VPMAXUQ:
      IsMax = true;
      break;
    default:
      break;
    }
    NdOp CmpOp = IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        NdVar Cond = S.makeTemp(1);
        S.emit(CmpOp, Cond, {La, Lb});
        NdVar Lr = S.makeTemp(LaneSz);
        if (IsMax)
          S.emit(NdOp::SELECT, Lr, {Cond, Lb, La});
        else
          S.emit(NdOp::SELECT, Lr, {Cond, La, Lb});
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
    NdVar LoHalf = BuildHalf(0);
    NdVar HiHalf = BuildHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // AVX packed integer add/sub — per-lane decomposition.
  case X86_INS_VPADDB:
  case X86_INS_VPADDW:
  case X86_INS_VPADDD:
  case X86_INS_VPADDQ:
  case X86_INS_VPSUBB:
  case X86_INS_VPSUBW:
  case X86_INS_VPSUBD:
  case X86_INS_VPSUBQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 0;
    switch (InsnId) {
    case X86_INS_VPADDB:
    case X86_INS_VPSUBB:
      LaneSz = 1;
      break;
    case X86_INS_VPADDW:
    case X86_INS_VPSUBW:
      LaneSz = 2;
      break;
    case X86_INS_VPADDD:
    case X86_INS_VPSUBD:
      LaneSz = 4;
      break;
    case X86_INS_VPADDQ:
    case X86_INS_VPSUBQ:
      LaneSz = 8;
      break;
    default:
      break;
    }
    // NOTE: even qword (LaneSz==8) lanes must be added/subtracted
    // independently — a full-width INT_ADD/INT_SUB would propagate the carry
    // from lane 0 into lane 1 (wrong for VPADDQ/VPSUBQ).
    bool IsSub = (InsnId == X86_INS_VPSUBB || InsnId == X86_INS_VPSUBW ||
                  InsnId == X86_INS_VPSUBD || InsnId == X86_INS_VPSUBQ);
    NdOp LaneOpc = IsSub ? NdOp::INT_SUB : NdOp::INT_ADD;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        NdVar Lr = S.makeTemp(LaneSz);
        S.emit(LaneOpc, Lr, {La, Lb});
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
    NdVar LoHalf = BuildHalf(0);
    NdVar HiHalf = BuildHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }
  // AVX saturating add/sub — per-lane with saturation.
  case X86_INS_VPADDSB:
  case X86_INS_VPADDSW:
  case X86_INS_VPADDUSB:
  case X86_INS_VPADDUSW:
  case X86_INS_VPSUBSB:
  case X86_INS_VPSUBSW:
  case X86_INS_VPSUBUSB:
  case X86_INS_VPSUBUSW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 1;
    bool IsSigned = false, IsSub = false;
    switch (InsnId) {
    case X86_INS_VPADDSW:
    case X86_INS_VPSUBSW:
    case X86_INS_VPADDUSW:
    case X86_INS_VPSUBUSW:
      LaneSz = 2;
      break;
    default:
      break;
    }
    switch (InsnId) {
    case X86_INS_VPADDSB:
    case X86_INS_VPADDSW:
    case X86_INS_VPSUBSB:
    case X86_INS_VPSUBSW:
      IsSigned = true;
      break;
    default:
      break;
    }
    switch (InsnId) {
    case X86_INS_VPSUBSB:
    case X86_INS_VPSUBSW:
    case X86_INS_VPSUBUSB:
    case X86_INS_VPSUBUSW:
      IsSub = true;
      break;
    default:
      break;
    }
    NdOp ArithOp = IsSub ? NdOp::INT_SUB : NdOp::INT_ADD;
    unsigned WiderSz = LaneSz * 2;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        NdVar Ax = S.makeTemp(WiderSz), Bx = S.makeTemp(WiderSz);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Ax, {La});
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Bx, {Lb});
        NdVar Wide = S.makeTemp(WiderSz);
        S.emit(ArithOp, Wide, {Ax, Bx});
        int64_t MaxV, MinV;
        if (IsSigned) {
          MaxV = (1LL << (LaneSz * 8 - 1)) - 1;
          MinV = -(1LL << (LaneSz * 8 - 1));
        } else {
          MaxV = (1LL << (LaneSz * 8)) - 1;
          MinV = 0;
        }
        NdVar HiClamp = S.makeTemp(1);
        if (IsSigned)
          S.emit(NdOp::INT_SLESS, HiClamp, {NdVar::cst(MaxV, WiderSz), Wide});
        else
          S.emit(NdOp::INT_LESS, HiClamp, {NdVar::cst(MaxV, WiderSz), Wide});
        NdVar Clamped1 = S.makeTemp(WiderSz);
        S.emit(NdOp::SELECT, Clamped1,
               {HiClamp, NdVar::cst(MaxV, WiderSz), Wide});
        NdVar LoClamp = S.makeTemp(1);
        if (IsSigned)
          S.emit(NdOp::INT_SLESS, LoClamp,
                 {Clamped1, NdVar::cst(MinV, WiderSz)});
        else
          S.emit(NdOp::INT_SLESS, LoClamp, {Wide, NdVar::cst(0, WiderSz)});
        NdVar Clamped2 = S.makeTemp(WiderSz);
        S.emit(NdOp::SELECT, Clamped2,
               {LoClamp, NdVar::cst(MinV, WiderSz), Clamped1});
        NdVar Lr = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lr, {Clamped2, NdVar::cst(0, 4)});
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
    NdVar LoHalf = BuildHalf(0);
    NdVar HiHalf = BuildHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
