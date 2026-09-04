//===- X86LiftLegacySSEInt.cpp - x86 baseline SSE integer lifter ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Baseline (non-VEX) SSE/SSE2 integer instructions modelled
/// as bulk 128-bit operations: bitwise logic, packed
/// add/subtract, whole-register and per-lane shifts, and the
/// shuffle/unpack family.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftLegacySSEInt(X86Lifter &L, X86Lifter::LiftState &S,
                      const cs_insn *Insn, const cs_x86 &X86) {
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

  // ========================================================================
  // Legacy SSE element-wise integer ops (bulk 128-bit nd-var operations).
  // Loses per-lane semantics but preserves dataflow shape.
  // ========================================================================
  case X86_INS_XORPS:
  case X86_INS_XORPD:
  case X86_INS_PXOR:
  case X86_INS_ORPS:
  case X86_INS_ORPD:
  case X86_INS_POR:
  case X86_INS_ANDPS:
  case X86_INS_ANDPD:
  case X86_INS_PAND:
  case X86_INS_ANDNPS:
  case X86_INS_ANDNPD:
  case X86_INS_PANDN:
  case X86_INS_PADDB:
  case X86_INS_PADDW:
  case X86_INS_PADDD:
  case X86_INS_PADDQ:
  case X86_INS_PSUBB:
  case X86_INS_PSUBW:
  case X86_INS_PSUBD:
  case X86_INS_PSUBQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdOp Opc = NdOp::COPY;
    switch (InsnId) {
    case X86_INS_XORPS:
    case X86_INS_XORPD:
    case X86_INS_PXOR:
      Opc = NdOp::INT_XOR;
      break;
    case X86_INS_ORPS:
    case X86_INS_ORPD:
    case X86_INS_POR:
      Opc = NdOp::INT_OR;
      break;
    case X86_INS_ANDPS:
    case X86_INS_ANDPD:
    case X86_INS_PAND:
      Opc = NdOp::INT_AND;
      break;
    case X86_INS_ANDNPS:
    case X86_INS_ANDNPD:
    case X86_INS_PANDN: {
      NdVar Neg = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, Neg, {Dst});
      S.emit(NdOp::INT_AND, Dst, {Neg, Src});
      break;
    }
    case X86_INS_PADDB:
    case X86_INS_PADDW:
    case X86_INS_PADDD:
    case X86_INS_PADDQ:
    case X86_INS_PSUBB:
    case X86_INS_PSUBW:
    case X86_INS_PSUBD:
    case X86_INS_PSUBQ: {
      unsigned LaneSz = 0;
      switch (InsnId) {
      case X86_INS_PADDB:
      case X86_INS_PSUBB:
        LaneSz = 1;
        break;
      case X86_INS_PADDW:
      case X86_INS_PSUBW:
        LaneSz = 2;
        break;
      case X86_INS_PADDD:
      case X86_INS_PSUBD:
        LaneSz = 4;
        break;
      case X86_INS_PADDQ:
      case X86_INS_PSUBQ:
        LaneSz = 8;
        break;
      default:
        break;
      }
      NdOp LaneOpc = (InsnId == X86_INS_PADDB || InsnId == X86_INS_PADDW ||
                      InsnId == X86_INS_PADDD || InsnId == X86_INS_PADDQ)
                         ? NdOp::INT_ADD
                         : NdOp::INT_SUB;
      // Split into low/high halves to avoid non-power-of-2 intermediate types.
      unsigned HalfSz = Dst.Size / 2;
      unsigned LanesPerHalf = HalfSz / LaneSz;
      auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < LanesPerHalf; ++I) {
          unsigned Off = BaseOff + I * LaneSz;
          NdVar La = S.makeTemp(LaneSz);
          NdVar Lb = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {Dst, NdVar::cst(Off, 4)});
          S.emit(NdOp::SUBBYTES, Lb, {Src, NdVar::cst(Off, 4)});
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
    default:
      Opc = NdOp::COPY;
    }
    if (InsnId != X86_INS_ANDNPS && InsnId != X86_INS_ANDNPD &&
        InsnId != X86_INS_PANDN && InsnId != X86_INS_PADDB &&
        InsnId != X86_INS_PADDW && InsnId != X86_INS_PADDD &&
        InsnId != X86_INS_PADDQ && InsnId != X86_INS_PSUBB &&
        InsnId != X86_INS_PSUBW && InsnId != X86_INS_PSUBD &&
        InsnId != X86_INS_PSUBQ) {
      S.emit(Opc, Dst, {Dst, Src});
    }
    break;
  }

  // ========================================================================
  // PSLLDQ / PSRLDQ (byte-shift) and packed element shifts
  // ========================================================================
  case X86_INS_PSLLDQ:
  case X86_INS_PSRLDQ: {
    if (X86.op_count < 2 || X86.operands[0].type != X86_OP_REG ||
        X86.operands[1].type != X86_OP_IMM)
      return false;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[0]);
    if (Dst.Size != 16 || Src.Size != 16)
      return false;

    const int Shift = static_cast<uint8_t>(X86.operands[1].imm);
    NdVar Bytes[16];
    for (unsigned I = 0; I < 16; ++I) {
      const int Byte = static_cast<int>(I);
      const int SourceByte =
          InsnId == X86_INS_PSLLDQ ? Byte - Shift : Byte + Shift;
      if (SourceByte < 0 || SourceByte >= 16) {
        Bytes[I] = NdVar::cst(0, 1);
        continue;
      }
      Bytes[I] = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, Bytes[I],
             {Src, NdVar::cst(static_cast<unsigned>(SourceByte), 4)});
    }

    // Assemble low-to-high through power-of-two widths accepted by LowIR.
    unsigned Count = 16;
    unsigned Width = 1;
    while (Count > 1) {
      for (unsigned I = 0; I < Count / 2; ++I) {
        NdVar Pair = S.makeTemp(Width * 2);
        S.emit(NdOp::CONCAT, Pair, {Bytes[I * 2 + 1], Bytes[I * 2]});
        Bytes[I] = Pair;
      }
      Count /= 2;
      Width *= 2;
    }
    S.emit(NdOp::COPY, Dst, {Bytes[0]});
    break;
  }
  case X86_INS_PSLLD:
  case X86_INS_PSLLW:
  case X86_INS_PSLLQ:
  case X86_INS_PSRLD:
  case X86_INS_PSRLW:
  case X86_INS_PSRLQ:
  case X86_INS_PSRAW:
  case X86_INS_PSRAD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    unsigned LaneSz = 0;
    NdOp ShiftOp = NdOp::INT_LEFT;
    switch (InsnId) {
    case X86_INS_PSLLW:
      LaneSz = 2;
      ShiftOp = NdOp::INT_LEFT;
      break;
    case X86_INS_PSLLD:
      LaneSz = 4;
      ShiftOp = NdOp::INT_LEFT;
      break;
    case X86_INS_PSLLQ:
      LaneSz = 8;
      ShiftOp = NdOp::INT_LEFT;
      break;
    case X86_INS_PSRLW:
      LaneSz = 2;
      ShiftOp = NdOp::INT_RIGHT;
      break;
    case X86_INS_PSRLD:
      LaneSz = 4;
      ShiftOp = NdOp::INT_RIGHT;
      break;
    case X86_INS_PSRLQ:
      LaneSz = 8;
      ShiftOp = NdOp::INT_RIGHT;
      break;
    case X86_INS_PSRAW:
      LaneSz = 2;
      ShiftOp = NdOp::INT_ASHR;
      break;
    default:
      LaneSz = 4;
      ShiftOp = NdOp::INT_ASHR;
      break;
    }
    unsigned LaneBits = LaneSz * 8;
    bool Arith = (ShiftOp == NdOp::INT_ASHR);

    // The shift count is the scalar in the low 64 bits of the source (or imm8),
    // shared by all lanes.  x86 does NOT mask it: a logical shift yields 0 and
    // an arithmetic shift yields a sign fill when count >= lane bit width.
    NdVar RawCnt;
    if (X86.operands[1].type == X86_OP_IMM) {
      RawCnt = NdVar::cst((uint64_t)X86.operands[1].imm, 8);
    } else {
      NdVar SrcFull = L.operandRead(S, X86.operands[1]);
      RawCnt = S.makeTemp(8);
      if (SrcFull.Size >= 8)
        S.emit(NdOp::SUBBYTES, RawCnt, {SrcFull, NdVar::cst(0, 4)});
      else
        S.emit(NdOp::INT_ZEXT, RawCnt, {SrcFull});
    }
    NdVar InRange = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, InRange, {RawCnt, NdVar::cst(LaneBits, 8)});
    NdVar CntL = S.makeTemp(LaneSz);
    S.emit(NdOp::SUBBYTES, CntL, {RawCnt, NdVar::cst(0, 4)});
    if (Arith) {
      NdVar Clamped = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, Clamped,
             {InRange, CntL, NdVar::cst(LaneBits - 1, LaneSz)});
      CntL = Clamped;
    }

    unsigned NLanes = Dst.Size / LaneSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, Lane, {Dst, NdVar::cst(I * LaneSz, 4)});
      NdVar Raw = S.makeTemp(LaneSz);
      S.emit(ShiftOp, Raw, {Lane, CntL});
      NdVar Shifted = Raw;
      if (!Arith) {
        Shifted = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, Shifted, {InRange, Raw, NdVar::cst(0, LaneSz)});
      }
      if (I == 0) {
        Acc = Shifted;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Shifted, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  // ========================================================================
  // SIMD shuffles / unpacks
  // ========================================================================
  case X86_INS_PSHUFD:
  case X86_INS_PSHUFLW:
  case X86_INS_PSHUFHW: {
    if (X86.op_count < 3 || X86.operands[2].type != X86_OP_IMM)
      return false;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    const uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    const unsigned ElementSize = InsnId == X86_INS_PSHUFD ? 4 : 2;
    const unsigned FirstShuffledElement = InsnId == X86_INS_PSHUFHW ? 4 : 0;
    if (!EmitImmediateLaneShuffle(Dst, Src, Imm, ElementSize,
                                  FirstShuffledElement))
      return false;
    break;
  }
  case X86_INS_PSHUFB: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Pshufb, Dst, {Dst, Src});
    break;
  }
  case X86_INS_SHUFPS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Shufps, Dst, {Dst, Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_SHUFPD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Shufpd, Dst, {Dst, Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_PUNPCKLBW:
  case X86_INS_PUNPCKHBW:
  case X86_INS_PUNPCKLWD:
  case X86_INS_PUNPCKHWD:
  case X86_INS_PUNPCKLDQ:
  case X86_INS_PUNPCKHDQ:
  case X86_INS_PUNPCKLQDQ:
  case X86_INS_PUNPCKHQDQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint16_t ElementSize = 0;
    bool HighHalf = false;
    switch (InsnId) {
    case X86_INS_PUNPCKLBW:
      ElementSize = 1;
      break;
    case X86_INS_PUNPCKHBW:
      ElementSize = 1;
      HighHalf = true;
      break;
    case X86_INS_PUNPCKLWD:
      ElementSize = 2;
      break;
    case X86_INS_PUNPCKHWD:
      ElementSize = 2;
      HighHalf = true;
      break;
    case X86_INS_PUNPCKLDQ:
      ElementSize = 4;
      break;
    case X86_INS_PUNPCKHDQ:
      ElementSize = 4;
      HighHalf = true;
      break;
    case X86_INS_PUNPCKLQDQ:
      ElementSize = 8;
      break;
    default:
      ElementSize = 8;
      HighHalf = true;
      break;
    }
    if (!emitPackedUnpack(S, Dst, Dst, Src, ElementSize, HighHalf))
      return false;
    break;
  }
  case X86_INS_UNPCKLPS:
  case X86_INS_UNPCKHPS:
  case X86_INS_UNPCKLPD:
  case X86_INS_UNPCKHPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_UNPCKLPS:
      Id = Intrinsic::Unpcklps;
      break;
    case X86_INS_UNPCKHPS:
      Id = Intrinsic::Unpckhps;
      break;
    case X86_INS_UNPCKLPD:
      Id = Intrinsic::Unpcklpd;
      break;
    default:
      Id = Intrinsic::Unpckhpd;
      break;
    }
    S.emitIntrinsic(Id, Dst, {Dst, Src});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
