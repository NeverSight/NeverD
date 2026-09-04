//===- X86LiftSIMDShuffle.cpp - x86/x64 SIMD shuffle, permute and blend lifter
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSE/AVX lane-selection instructions: immediate and variable
/// blends, byte/word/dword shuffles, in-lane and cross-lane
/// permutes, 128-bit lane permutes, and the masked vector
/// load/store forms.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

struct TablePermuteSpec {
  uint8_t Opcode = 0;
  uint16_t ElementSize = 0;
  bool IndexInDestination = false;
  bool W = false;
};

bool getTablePermuteSpec(unsigned InsnId, TablePermuteSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VPERMI2B:
    Spec = {0x75, 1, true, false};
    return true;
  case X86_INS_VPERMI2W:
    Spec = {0x75, 2, true, true};
    return true;
  case X86_INS_VPERMI2D:
    Spec = {0x76, 4, true, false};
    return true;
  case X86_INS_VPERMI2Q:
    Spec = {0x76, 8, true, true};
    return true;
  case X86_INS_VPERMI2PS:
    Spec = {0x77, 4, true, false};
    return true;
  case X86_INS_VPERMI2PD:
    Spec = {0x77, 8, true, true};
    return true;
  case X86_INS_VPERMT2B:
    Spec = {0x7d, 1, false, false};
    return true;
  case X86_INS_VPERMT2W:
    Spec = {0x7d, 2, false, true};
    return true;
  case X86_INS_VPERMT2D:
    Spec = {0x7e, 4, false, false};
    return true;
  case X86_INS_VPERMT2Q:
    Spec = {0x7e, 8, false, true};
    return true;
  case X86_INS_VPERMT2PS:
    Spec = {0x7f, 4, false, false};
    return true;
  case X86_INS_VPERMT2PD:
    Spec = {0x7f, 8, false, true};
    return true;
  default:
    return false;
  }
}

bool isVectorRegisterOfSize(const cs_x86_op &Operand, uint16_t Size) {
  if (Operand.type != X86_OP_REG || Operand.size != Size)
    return false;
  if (Size == 16)
    return Operand.reg >= X86_REG_XMM0 && Operand.reg <= X86_REG_XMM31;
  if (Size == 32)
    return Operand.reg >= X86_REG_YMM0 && Operand.reg <= X86_REG_YMM31;
  if (Size == 64)
    return Operand.reg >= X86_REG_ZMM0 && Operand.reg <= X86_REG_ZMM31;
  return false;
}

unsigned vectorRegisterIndex(const cs_x86_op &Operand) {
  if (Operand.size == 16)
    return static_cast<unsigned>(Operand.reg - X86_REG_XMM0);
  if (Operand.size == 32)
    return static_cast<unsigned>(Operand.reg - X86_REG_YMM0);
  return static_cast<unsigned>(Operand.reg - X86_REG_ZMM0);
}

x86_avx_bcast broadcastForLaneCount(unsigned LaneCount) {
  switch (LaneCount) {
  case 2:
    return X86_AVX_BCAST_2;
  case 4:
    return X86_AVX_BCAST_4;
  case 8:
    return X86_AVX_BCAST_8;
  case 16:
    return X86_AVX_BCAST_16;
  default:
    return X86_AVX_BCAST_INVALID;
  }
}

bool hasCanonicalTablePermuteEncoding(const cs_insn *Insn, const cs_x86 &X86,
                                      Arch TargetArch,
                                      const TablePermuteSpec &Spec,
                                      const cs_x86_op &Destination,
                                      const cs_x86_op &Source1,
                                      const cs_x86_op &Source2,
                                      const cs_x86_op *Mask, bool &Broadcast) {
  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, TargetArch, Encoding) ||
      (Encoding.P0 & 0x07) != 0x02 ||
      ((Encoding.P1 | 0x04) & 0x87) != (Spec.W ? 0x85 : 0x05) ||
      Encoding.Opcode != Spec.Opcode || X86.encoding.imm_offset != 0 ||
      X86.encoding.imm_size != 0 || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  if (EncodedLength == 0x60)
    return false;
  const uint16_t VectorSize =
      EncodedLength == 0 ? 16 : (EncodedLength == 0x20 ? 32 : 64);
  if (!isVectorRegisterOfSize(Destination, VectorSize) ||
      !isVectorRegisterOfSize(Source1, VectorSize))
    return false;

  const uint8_t EncodedMask = Encoding.P2 & 0x07;
  const bool ZeroMask = (Encoding.P2 & 0x80) != 0;
  if (Mask) {
    const unsigned LaneCount = VectorSize / Spec.ElementSize;
    const uint16_t MaskSize =
        static_cast<uint16_t>(std::max(1u, (LaneCount + 7) / 8));
    if (!isX86OpmaskOperand(*Mask) || Mask->reg == X86_REG_K0 ||
        Mask->size != MaskSize ||
        EncodedMask != static_cast<uint8_t>(Mask->reg - X86_REG_K0) ||
        ZeroMask != Mask->avx_zero_opmask)
      return false;
  } else if (EncodedMask != 0 || ZeroMask) {
    return false;
  }

  const unsigned EncodedDestination =
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM);
  const unsigned EncodedSource1 =
      decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2);
  if (EncodedDestination != vectorRegisterIndex(Destination) ||
      EncodedSource1 != vectorRegisterIndex(Source1))
    return false;

  Broadcast = (Encoding.P2 & 0x10) != 0;
  const bool MemoryForm = Source2.type == X86_OP_MEM;
  if (MemoryForm) {
    const unsigned LaneCount = VectorSize / Spec.ElementSize;
    const x86_avx_bcast ExpectedBroadcast =
        Broadcast ? broadcastForLaneCount(LaneCount) : X86_AVX_BCAST_INVALID;
    if ((Encoding.ModRM & 0xc0) == 0xc0 ||
        (Broadcast && Spec.ElementSize < 4) ||
        Source2.size != (Broadcast ? Spec.ElementSize : VectorSize) ||
        Source2.avx_bcast != ExpectedBroadcast ||
        !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, Source2,
                                         Broadcast ? Spec.ElementSize
                                                   : VectorSize))
      return false;
  } else if (!isVectorRegisterOfSize(Source2, VectorSize) || Broadcast ||
             Source2.avx_bcast != X86_AVX_BCAST_INVALID ||
             decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                 vectorRegisterIndex(Source2) ||
             !validateCanonicalEvexRegisterTail(Insn, X86, Encoding)) {
    return false;
  }

  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    if (&Operand != &Source2 && Operand.avx_bcast != X86_AVX_BCAST_INVALID)
      return false;
    if (Operand.avx_zero_opmask && (!Mask || &Operand != Mask))
      return false;
  }
  return true;
}

std::vector<NdVar> extractTableLanes(X86Lifter::LiftState &S, NdVar Table,
                                     unsigned LaneCount, uint16_t ElementSize) {
  std::vector<NdVar> Lanes;
  Lanes.reserve(LaneCount);
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    NdVar Value = S.makeTemp(ElementSize);
    S.emit(NdOp::SUBBYTES, Value,
           {Table, NdVar::cst(static_cast<uint64_t>(Lane) * ElementSize, 4)});
    Lanes.push_back(Value);
  }
  return Lanes;
}

NdVar selectTableLane(X86Lifter::LiftState &S,
                      const std::vector<NdVar> &TableLanes, NdVar Index,
                      uint16_t ElementSize) {
  std::vector<NdVar> Candidates = TableLanes;
  const unsigned LaneCount = static_cast<unsigned>(Candidates.size());
  for (unsigned Bit = 1, Count = LaneCount; Count > 1; Bit <<= 1, Count >>= 1) {
    NdVar MaskedBit = S.makeTemp(ElementSize);
    S.emit(NdOp::INT_AND, MaskedBit, {Index, NdVar::cst(Bit, ElementSize)});
    NdVar BitSet = S.makeTemp(1);
    S.emit(NdOp::INT_NOTEQUAL, BitSet, {MaskedBit, NdVar::cst(0, ElementSize)});
    for (unsigned Pair = 0; Pair < Count / 2; ++Pair) {
      NdVar Selected = S.makeTemp(ElementSize);
      S.emit(NdOp::SELECT, Selected,
             {BitSet, Candidates[Pair * 2 + 1], Candidates[Pair * 2]});
      Candidates[Pair] = Selected;
    }
  }
  return Candidates.front();
}

NdVar extractCompactMaskBit(X86Lifter::LiftState &S, NdVar Mask,
                            unsigned Lane) {
  NdVar Shifted = Mask;
  if (Lane != 0) {
    Shifted = S.makeTemp(Mask.Size);
    S.emit(NdOp::INT_RIGHT, Shifted, {Mask, NdVar::cst(Lane, Mask.Size)});
  }
  NdVar WideBit = S.makeTemp(Mask.Size);
  S.emit(NdOp::INT_AND, WideBit, {Shifted, NdVar::cst(1, Mask.Size)});
  if (Mask.Size == 1)
    return WideBit;
  NdVar Bit = S.makeTemp(1);
  S.emit(NdOp::SUBBYTES, Bit, {WideBit, NdVar::cst(0, 4)});
  return Bit;
}

NdVar resizeUnsigned(X86Lifter::LiftState &S, NdVar Value,
                     uint16_t ResultSize) {
  if (Value.Size == ResultSize)
    return Value;
  NdVar Result = S.makeTemp(ResultSize);
  if (Value.Size < ResultSize)
    S.emit(NdOp::INT_ZEXT, Result, {Value});
  else
    S.emit(NdOp::SUBBYTES, Result, {Value, NdVar::cst(0, 4)});
  return Result;
}

NdVar buildTable2MemoryMask(X86Lifter::LiftState &S,
                            const std::vector<NdVar> &Indices,
                            const cs_x86_op *MaskOperand, unsigned LaneCount,
                            uint16_t ElementSize) {
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
  NdVar OutputMask;
  if (MaskOperand) {
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(MaskOperand->reg));
    if (!isX86OpmaskOperand(*MaskOperand) || MaskInfo.Size < MaskSize)
      return {};
    OutputMask = NdVar::reg(MaskInfo.Offset, MaskSize);
  }

  NdVar MemoryMask = NdVar::cst(0, MaskSize);
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    NdVar TableBit = S.makeTemp(ElementSize);
    S.emit(NdOp::INT_AND, TableBit,
           {Indices[Lane], NdVar::cst(LaneCount, ElementSize)});
    NdVar SelectsTable2 = S.makeTemp(1);
    S.emit(NdOp::INT_NOTEQUAL, SelectsTable2,
           {TableBit, NdVar::cst(0, ElementSize)});
    NdVar NeedsMemory = SelectsTable2;
    if (MaskOperand) {
      NeedsMemory = S.makeTemp(1);
      S.emit(NdOp::BOOL_AND, NeedsMemory,
             {SelectsTable2, extractCompactMaskBit(S, OutputMask, Lane)});
    }

    NdVar SourceLane = S.makeTemp(ElementSize);
    S.emit(NdOp::INT_AND, SourceLane,
           {Indices[Lane], NdVar::cst(LaneCount - 1, ElementSize)});
    SourceLane = resizeUnsigned(S, SourceLane, MaskSize);
    NdVar OneHot = S.makeTemp(MaskSize);
    S.emit(NdOp::INT_LEFT, OneHot, {NdVar::cst(1, MaskSize), SourceLane});
    NdVar Gated = S.makeTemp(MaskSize);
    S.emit(NdOp::SELECT, Gated, {NeedsMemory, OneHot, NdVar::cst(0, MaskSize)});
    NdVar NextMask = S.makeTemp(MaskSize);
    S.emit(NdOp::INT_OR, NextMask, {MemoryMask, Gated});
    MemoryMask = NextMask;
  }
  return MemoryMask;
}

} // namespace

bool liftSIMDShuffle(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                     const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  auto EmitImmediateLaneShuffle = [&](NdVar Dst, NdVar Src, uint8_t Imm,
                                      unsigned ElementSize,
                                      unsigned FirstShuffledElement) {
    if (Dst.Size == 0 || Dst.Size != Src.Size || Dst.Size % 16 != 0)
      return false;
    const unsigned ElementsPerLane = 16 / ElementSize;
    const unsigned NumElements = Dst.Size / ElementSize;
    if (NumElements > 32)
      return false;

    NdVar Elements[32];
    for (unsigned I = 0; I < NumElements; ++I) {
      const unsigned LaneBase = (I / ElementsPerLane) * ElementsPerLane;
      const unsigned InLane = I % ElementsPerLane;
      unsigned Selected = InLane;
      if (InLane >= FirstShuffledElement && InLane < FirstShuffledElement + 4) {
        const unsigned ControlIndex = InLane - FirstShuffledElement;
        Selected = FirstShuffledElement + ((Imm >> (ControlIndex * 2)) & 3);
      }
      Elements[I] = S.makeTemp(ElementSize);
      S.emit(NdOp::SUBBYTES, Elements[I],
             {Src, NdVar::cst(static_cast<uint64_t>(LaneBase + Selected) *
                                  ElementSize,
                              4)});
    }
    unsigned Count = NumElements;
    unsigned Width = ElementSize;
    while (Count > 1) {
      for (unsigned I = 0; I < Count / 2; ++I) {
        NdVar Pair = S.makeTemp(Width * 2);
        S.emit(NdOp::CONCAT, Pair, {Elements[I * 2 + 1], Elements[I * 2]});
        Elements[I] = Pair;
      }
      Count /= 2;
      Width *= 2;
    }
    S.emit(NdOp::COPY, Dst, {Elements[0]});
    return true;
  };
  switch (InsnId) {

  case X86_INS_PBLENDW:
  case X86_INS_BLENDPS:
  case X86_INS_BLENDPD:
  case X86_INS_VPBLENDW:
  case X86_INS_VBLENDPS:
  case X86_INS_VBLENDPD: {
    // Immediate blend: each imm bit selects the matching lane from src2 (set)
    // or src1 (clear).  SSE forms reuse dst as src1 (3 operands); VEX forms are
    // non-destructive with an explicit src1 (4 operands: dst, src1, src2, imm).
    if (X86.op_count < 3)
      break;
    unsigned Base = (X86.op_count >= 4) ? 1 : 0;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar DstR = L.operandRead(S, X86.operands[Base]);
    NdVar Src = L.operandRead(S, X86.operands[Base + 1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[Base + 2].imm);
    unsigned LaneSz =
        (InsnId == X86_INS_BLENDPD || InsnId == X86_INS_VBLENDPD)   ? 8
        : (InsnId == X86_INS_BLENDPS || InsnId == X86_INS_VBLENDPS) ? 4
                                                                    : 2;
    unsigned NLanes = Dst.Size / LaneSz;
    std::vector<NdVar> Lanes;
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(LaneSz);
      if (Imm & (1u << I))
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
      else
        S.emit(NdOp::SUBBYTES, Lane, {DstR, NdVar::cst(I * LaneSz, 4)});
      Lanes.push_back(Lane);
    }
    NdVar Acc = Lanes[0];
    for (unsigned I = 1; I < NLanes; ++I) {
      NdVar W = S.makeTemp((I + 1) * LaneSz);
      S.emit(NdOp::CONCAT, W, {Lanes[I], Acc});
      Acc = W;
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case X86_INS_PBLENDVB:
  case X86_INS_BLENDVPS:
  case X86_INS_BLENDVPD: {
    // SSE4.1 variable blend: per-lane, if the high (sign) bit of the
    // corresponding element of the implicit XMM0 mask is set, take the lane
    // from the source operand, otherwise keep the destination's lane.
    //   PBLENDVB: 8-bit lanes (mask bit 7), BLENDVPS: 32-bit (bit 31),
    //   BLENDVPD: 64-bit (bit 63).
    // Capstone exposes XMM0 as operands[2]; fall back to the register if not.
    if (X86.op_count < 2)
      break;
    unsigned LaneSz = (InsnId == X86_INS_BLENDVPS)   ? 4
                      : (InsnId == X86_INS_BLENDVPD) ? 8
                                                     : 1;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar FalseV = L.operandRead(S, X86.operands[0]); // dst keeps its lane
    NdVar TrueV = L.operandRead(S, X86.operands[1]);  // src provides its lane
    NdVar Mask = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[2])
                                     : NdVar::reg(x86reg::XMM0, Dst.Size);
    unsigned NLanes = Dst.Size / LaneSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      unsigned Off = I * LaneSz;
      NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz),
            Lm = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, La, {FalseV, NdVar::cst(Off, 4)});
      S.emit(NdOp::SUBBYTES, Lb, {TrueV, NdVar::cst(Off, 4)});
      S.emit(NdOp::SUBBYTES, Lm, {Mask, NdVar::cst(Off, 4)});
      NdVar Cond = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, Cond, {Lm, NdVar::cst(0, LaneSz)});
      NdVar Lr = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, Lr, {Cond, Lb, La});
      if (I == 0) {
        Acc = Lr;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Lr, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  // VPERMILPS / VPERMILPD — IN-LANE permute of 32/64-bit elements.  The control
  // (imm8, or a per-element register) selects an element WITHIN each 128-bit
  // lane; the same imm8 applies to every lane.  The old code COPYed the source
  // and dropped the permutation entirely (control ignored) — wrong for every
  // non-identity control.  imm forms use constant lane indices; the variable
  // form selects per element from the low control bits (PS: bits[1:0], PD:
  // bit[1]).  Assemble the result low->high with a power-of-two CONCAT tree.
  case X86_INS_VPERMILPS:
  case X86_INS_VPERMILPD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint16_t ES = (InsnId == X86_INS_VPERMILPD) ? 8 : 4; // element size
    uint16_t EPL = 16 / ES;                              // elems per 128 lane
    uint16_t N = Dst.Size / ES;                          // total elements
    if (N < 2 || N > 8) {
      S.emit(NdOp::COPY, Dst, {Src});
      break;
    }
    bool IsImm = (X86.operands[2].type == X86_OP_IMM);
    uint64_t Imm = IsImm ? static_cast<uint64_t>(X86.operands[2].imm) : 0;
    NdVar Ctrl = IsImm ? NdVar() : L.operandRead(S, X86.operands[2]);
    NdVar Elems[8];
    for (uint16_t I = 0; I < N; ++I) {
      uint16_t Base = (I / EPL) * EPL; // first src element of this 128 lane
      if (IsImm) {
        uint16_t Sel =
            (ES == 4) ? ((Imm >> (2 * (I % EPL))) & 3) : ((Imm >> I) & 1);
        Elems[I] = S.makeTemp(ES);
        S.emit(NdOp::SUBBYTES, Elems[I],
               {Src, NdVar::cst(static_cast<uint64_t>(Base + Sel) * ES, 4)});
      } else {
        NdVar CtrlI = S.makeTemp(ES);
        S.emit(NdOp::SUBBYTES, CtrlI,
               {Ctrl, NdVar::cst(static_cast<uint64_t>(I) * ES, 4)});
        NdVar Idx = S.makeTemp(ES);
        if (ES == 4) {
          S.emit(NdOp::INT_AND, Idx, {CtrlI, NdVar::cst(3, ES)});
        } else {
          NdVar Sh = S.makeTemp(ES);
          S.emit(NdOp::INT_RIGHT, Sh, {CtrlI, NdVar::cst(1, ES)});
          S.emit(NdOp::INT_AND, Idx, {Sh, NdVar::cst(1, ES)});
        }
        // Dynamic in-lane gather: select among the EPL candidates by Idx.
        NdVar Acc = S.makeTemp(ES);
        S.emit(
            NdOp::SUBBYTES, Acc,
            {Src, NdVar::cst(static_cast<uint64_t>(Base + EPL - 1) * ES, 4)});
        for (int K = static_cast<int>(EPL) - 2; K >= 0; --K) {
          NdVar Cand = S.makeTemp(ES);
          S.emit(NdOp::SUBBYTES, Cand,
                 {Src, NdVar::cst(static_cast<uint64_t>(Base + K) * ES, 4)});
          NdVar Eq = S.makeTemp(1);
          S.emit(NdOp::INT_EQUAL, Eq,
                 {Idx, NdVar::cst(static_cast<uint64_t>(K), ES)});
          NdVar NewAcc = S.makeTemp(ES);
          S.emit(NdOp::SELECT, NewAcc, {Eq, Cand, Acc});
          Acc = NewAcc;
        }
        Elems[I] = Acc;
      }
    }
    // Power-of-two CONCAT tree (only 8/16/32-byte temps); final merge -> Dst.
    uint16_t Count = N, Sz = ES;
    while (Count > 1) {
      uint16_t Half = Count / 2;
      uint16_t NewSz = static_cast<uint16_t>(Sz * 2);
      for (uint16_t K = 0; K < Half; ++K) {
        NdVar Out = (Half == 1) ? Dst : S.makeTemp(NewSz);
        S.emit(NdOp::CONCAT, Out, {Elems[2 * K + 1], Elems[2 * K]});
        Elems[K] = Out;
      }
      Count = Half;
      Sz = NewSz;
    }
    break;
  }

  // VPERMPD / VPERMQ — CROSS-LANE permute of 4 qwords by imm8 (each 2-bit field
  // selects any of the 4 source qwords).  Bit-identical ops (fp vs int label).
  case X86_INS_VPERMPD:
  case X86_INS_VPERMQ: {
    if (X86.op_count < 3 || X86.operands[2].type != X86_OP_IMM) {
      if (X86.op_count >= 2) {
        NdVar Dst = L.operandWrite(X86.operands[0]);
        NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
        S.emit(NdOp::COPY, Dst, {Src});
      }
      break;
    }
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint64_t Imm = static_cast<uint64_t>(X86.operands[2].imm);
    NdVar E[4];
    for (int I = 0; I < 4; ++I) {
      uint16_t Sel = (Imm >> (2 * I)) & 3;
      E[I] = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, E[I],
             {Src, NdVar::cst(static_cast<uint64_t>(Sel) * 8, 4)});
    }
    NdVar Lo = S.makeTemp(16), Hi = S.makeTemp(16);
    S.emit(NdOp::CONCAT, Lo, {E[1], E[0]});
    S.emit(NdOp::CONCAT, Hi, {E[3], E[2]});
    S.emit(NdOp::CONCAT, Dst, {Hi, Lo});
    break;
  }

  // VPERMPS — CROSS-LANE permute of 8 single-precision elements by a
  // per-element dword index (operands[1]).  Bit-identical to VPERMD, so reuse
  // that intrinsic (operands order: dst, indices, source).  Old code dropped
  // the index.
  case X86_INS_VPERMPS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Idx = L.operandRead(S, X86.operands[1]);
    NdVar Src = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Permd, Dst, {Idx, Src});
    break;
  }

  // VPERMI2*/VPERMT2* — full-width two-table permutation. VPERMI2 uses the old
  // destination as its index vector, while VPERMT2 uses it as table 1.
  case X86_INS_VPERMI2D:
  case X86_INS_VPERMI2Q:
  case X86_INS_VPERMI2PS:
  case X86_INS_VPERMI2PD:
  case X86_INS_VPERMI2W:
  case X86_INS_VPERMI2B:
  case X86_INS_VPERMT2D:
  case X86_INS_VPERMT2Q:
  case X86_INS_VPERMT2PS:
  case X86_INS_VPERMT2PD:
  case X86_INS_VPERMT2W:
  case X86_INS_VPERMT2B: {
    TablePermuteSpec Spec;
    if (!getTablePermuteSpec(InsnId, Spec) ||
        (X86.op_count != 3 && X86.op_count != 4) || X86.avx_sae ||
        X86.avx_rm != X86_AVX_RM_INVALID)
      return false;
    const bool HasMask = X86.op_count == 4;
    const cs_x86_op &DestinationOperand = X86.operands[0];
    const cs_x86_op *MaskOperand = HasMask ? &X86.operands[1] : nullptr;
    const cs_x86_op &Source1Operand = X86.operands[HasMask ? 2 : 1];
    const cs_x86_op &Source2Operand = X86.operands[HasMask ? 3 : 2];
    bool Broadcast = false;
    if (!hasCanonicalTablePermuteEncoding(
            Insn, X86, L.targetArch(), Spec, DestinationOperand, Source1Operand,
            Source2Operand, MaskOperand, Broadcast))
      return false;

    const uint16_t VectorSize = DestinationOperand.size;
    const unsigned LaneCount = VectorSize / Spec.ElementSize;
    NdVar OldDestination = L.operandRead(S, DestinationOperand);
    NdVar Source1 = L.operandRead(S, Source1Operand);
    NdVar Indices = Spec.IndexInDestination ? OldDestination : Source1;
    NdVar Table1 = Spec.IndexInDestination ? Source1 : OldDestination;
    const std::vector<NdVar> IndexLanes =
        extractTableLanes(S, Indices, LaneCount, Spec.ElementSize);
    NdVar Source2;
    if (Source2Operand.type == X86_OP_MEM) {
      const NdVar MemoryMask = buildTable2MemoryMask(
          S, IndexLanes, MaskOperand, LaneCount, Spec.ElementSize);
      if (MemoryMask.Size == 0)
        return false;
      Source2 = emitEvexMaskedMemoryLoad(
          S, Source2Operand, MemoryMask, VectorSize, Spec.ElementSize,
          Broadcast ? Spec.ElementSize : VectorSize, Broadcast);
    } else {
      Source2 = L.operandRead(S, Source2Operand);
    }
    if (Source2.Size != VectorSize)
      return false;
    const std::vector<NdVar> Table1Lanes =
        extractTableLanes(S, Table1, LaneCount, Spec.ElementSize);
    const std::vector<NdVar> Table2Lanes =
        extractTableLanes(S, Source2, LaneCount, Spec.ElementSize);

    std::vector<NdVar> ResultLanes;
    ResultLanes.reserve(LaneCount);
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
      const NdVar Index = IndexLanes[Lane];
      NdVar FromTable1 =
          selectTableLane(S, Table1Lanes, Index, Spec.ElementSize);
      NdVar FromTable2 =
          selectTableLane(S, Table2Lanes, Index, Spec.ElementSize);
      NdVar TableBit = S.makeTemp(Spec.ElementSize);
      S.emit(NdOp::INT_AND, TableBit,
             {Index, NdVar::cst(LaneCount, Spec.ElementSize)});
      NdVar SelectTable2 = S.makeTemp(1);
      S.emit(NdOp::INT_NOTEQUAL, SelectTable2,
             {TableBit, NdVar::cst(0, Spec.ElementSize)});
      NdVar ResultLane = S.makeTemp(Spec.ElementSize);
      S.emit(NdOp::SELECT, ResultLane, {SelectTable2, FromTable2, FromTable1});
      ResultLanes.push_back(ResultLane);
    }

    for (unsigned Count = LaneCount, Width = Spec.ElementSize; Count > 1;
         Count >>= 1, Width <<= 1) {
      for (unsigned Pair = 0; Pair < Count / 2; ++Pair) {
        NdVar Joined = S.makeTemp(Width * 2);
        S.emit(NdOp::CONCAT, Joined,
               {ResultLanes[Pair * 2 + 1], ResultLanes[Pair * 2]});
        ResultLanes[Pair] = Joined;
      }
    }

    NdVar RawResult = ResultLanes.front();
    if (HasMask) {
      if (!emitMaskedVectorResult(L, S, DestinationOperand, *MaskOperand,
                                  RawResult, Spec.ElementSize))
        return false;
    } else {
      S.emit(NdOp::COPY, L.operandWrite(DestinationOperand), {RawResult});
    }
    break;
  }

  // VEX 3-operand shuffles: VPSHUFB xmm1, xmm2, xmm3 → pshufb(xmm2, xmm3)
  case X86_INS_VPSHUFB: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Pshufb, Dst, {A, B});
    break;
  }
  case X86_INS_VPSHUFD:
  case X86_INS_VPSHUFLW:
  case X86_INS_VPSHUFHW: {
    if (X86.op_count < 3 || X86.operands[2].type != X86_OP_IMM)
      return false;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    const uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    const unsigned ElementSize = InsnId == X86_INS_VPSHUFD ? 4 : 2;
    const unsigned FirstShuffledElement = InsnId == X86_INS_VPSHUFHW ? 4 : 0;
    if (!EmitImmediateLaneShuffle(Dst, Src, Imm, ElementSize,
                                  FirstShuffledElement))
      return false;
    break;
  }
  case X86_INS_VSHUFPS: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[3].imm);
    S.emitIntrinsic(Intrinsic::Shufps, Dst, {A, B, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_VSHUFPD: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[3].imm);
    S.emitIntrinsic(Intrinsic::Shufpd, Dst, {A, B, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_VPALIGNR: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[3].imm);
    S.emitIntrinsic(Intrinsic::Palignr, Dst, {A, B, NdVar::cst(Imm, 1)});
    break;
  }

  // VPBLENDD ymm1, ymm2, ymm3/m, imm8 — per-dword select: imm8[i] picks src2
  // (operands[2]) when set, else src1 (operands[1]).  Lift natively per dword
  // (SUBBYTES + CONCAT) so the 256-bit form roundtrips without an opaque
  // INTRINSIC; 128-bit uses the low 4 imm8 bits.
  case X86_INS_VPBLENDD: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[3].imm);
    int N = static_cast<int>(Dst.Size) / 4;
    NdVar E[8];
    for (int I = 0; I < N; ++I) {
      NdVar Src = ((Imm >> I) & 1) ? B : A;
      E[I] = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, E[I],
             {Src, NdVar::cst(static_cast<uint64_t>(I) * 4, 4)});
    }
    NdVar Acc = E[0];
    for (int I = 1; I < N; ++I) {
      NdVar T = (I == N - 1) ? Dst : S.makeTemp(4 * (I + 1));
      S.emit(NdOp::CONCAT, T, {E[I], Acc});
      Acc = T;
    }
    break;
  }
  case X86_INS_VPERM2F128:
  case X86_INS_VPERM2I128: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[3].imm);
    S.emitIntrinsic(Intrinsic::Perm2f128, Dst, {A, B, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_VPERMD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Idx = L.operandRead(S, X86.operands[1]);
    NdVar Src = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Permd, Dst, {Idx, Src});
    break;
  }

  case X86_INS_VPMASKMOVD:
  case X86_INS_VPMASKMOVQ:
  case X86_INS_VMASKMOVPS:
  case X86_INS_VMASKMOVPD: {
    if (X86.op_count < 3)
      break;
    bool IsQword =
        (InsnId == X86_INS_VPMASKMOVQ || InsnId == X86_INS_VMASKMOVPD);
    bool IsStore = (X86.operands[0].type == X86_OP_MEM);
    if (IsStore) {
      // The emitter consumes input[1] as the destination ADDRESS (IntToPtr).
      // L.operandRead() on a MEM operand would emit a LOAD and return the
      // loaded value, so the masked store wrote to the value-at-the-destination
      // instead of the destination itself.  Use computeEA for the address.
      NdVar AddrVn = S.computeEA(X86.operands[0]);
      NdVar MaskVn = L.operandRead(S, X86.operands[1]);
      NdVar DataVn = L.operandRead(S, X86.operands[2]);
      Intrinsic IId =
          IsQword ? Intrinsic::MaskedStoreQ : Intrinsic::MaskedStoreD;
      S.emitIntrinsic(
          IId, NdVar::reg(x86reg::RAX, 0), {AddrVn, MaskVn, DataVn},
          NdMemoryOrdering::None,
          X86Lifter::LiftState::memoryAddressSpace(X86.operands[0]));
    } else {
      NdVar Dst = L.operandWrite(X86.operands[0]);
      NdVar MaskVn = L.operandRead(S, X86.operands[1]);
      // Likewise the emitter loads through input[1] as an ADDRESS; passing
      // L.operandRead() (the loaded value) caused a double dereference.
      NdVar AddrVn = S.computeEA(X86.operands[2]);
      Intrinsic IId = IsQword ? Intrinsic::MaskedLoadQ : Intrinsic::MaskedLoadD;
      S.emitIntrinsic(
          IId, Dst, {AddrVn, MaskVn}, NdMemoryOrdering::None,
          X86Lifter::LiftState::memoryAddressSpace(X86.operands[2]));
    }
    break;
  }

  // VEX variable blend: VBLENDVPS/VBLENDVPD/VPBLENDVB take an explicit mask
  // register as the last operand (operands[3]); per lane, the high (sign) bit
  // of the mask element selects src2 (operands[2]) over src1 (operands[1]).
  case X86_INS_VPBLENDVB:
  case X86_INS_VBLENDVPS:
  case X86_INS_VBLENDVPD: {
    if (X86.op_count < 4)
      break;
    unsigned LaneSz = (InsnId == X86_INS_VBLENDVPS)   ? 4
                      : (InsnId == X86_INS_VBLENDVPD) ? 8
                                                      : 1;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar FalseV = L.operandRead(S, X86.operands[1]); // src1
    NdVar TrueV = L.operandRead(S, X86.operands[2]);  // src2
    NdVar Mask = L.operandRead(S, X86.operands[3]);
    unsigned NLanes = Dst.Size / LaneSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      unsigned Off = I * LaneSz;
      NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz),
            Lm = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, La, {FalseV, NdVar::cst(Off, 4)});
      S.emit(NdOp::SUBBYTES, Lb, {TrueV, NdVar::cst(Off, 4)});
      S.emit(NdOp::SUBBYTES, Lm, {Mask, NdVar::cst(Off, 4)});
      NdVar Cond = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, Cond, {Lm, NdVar::cst(0, LaneSz)});
      NdVar Lr = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, Lr, {Cond, Lb, La});
      if (I == 0) {
        Acc = Lr;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Lr, Acc});
        Acc = Next;
      }
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
