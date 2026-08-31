//===- X86LiftSIMDConvert.cpp - x86/x64 SIMD pack, unpack and convert lifter
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSE/AVX width-changing instructions: integer/float packed
/// conversions, lane interleaving (unpack), saturating pack to a
/// narrower lane, and the zero/sign-extending lane widenings.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include <algorithm>
#include <optional>
#include <vector>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

struct EvexEncoding {
  uint8_t P0 = 0;
  uint8_t P1 = 0;
  uint8_t P2 = 0;
  uint8_t Opcode = 0;
  uint8_t ModRM = 0;
};

bool isLegacyPrefix(uint8_t Byte) {
  switch (Byte) {
  case 0xf0:
  case 0xf2:
  case 0xf3:
  case 0x2e:
  case 0x36:
  case 0x3e:
  case 0x26:
  case 0x64:
  case 0x65:
  case 0x66:
  case 0x67:
    return true;
  default:
    return false;
  }
}

std::optional<EvexEncoding> getEvexEncoding(const cs_insn *Insn) {
  if (!Insn)
    return std::nullopt;
  size_t Offset = 0;
  while (Offset < Insn->size && isLegacyPrefix(Insn->bytes[Offset]))
    ++Offset;
  if (Offset >= Insn->size || Insn->size - Offset < 6 ||
      Insn->bytes[Offset] != 0x62)
    return std::nullopt;
  return EvexEncoding{Insn->bytes[Offset + 1], Insn->bytes[Offset + 2],
                      Insn->bytes[Offset + 3], Insn->bytes[Offset + 4],
                      Insn->bytes[Offset + 5]};
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

Intrinsic packedMaskedLoadIntrinsic(uint16_t ElementSize) {
  switch (ElementSize) {
  case 1:
    return Intrinsic::MaskedLoadB;
  case 2:
    return Intrinsic::MaskedLoadW;
  case 4:
    return Intrinsic::MaskedLoadD;
  case 8:
    return Intrinsic::MaskedLoadQ;
  default:
    return Intrinsic::None;
  }
}

NdVar concatenateLowToHigh(X86Lifter::LiftState &S,
                           const std::vector<NdVar> &Elements) {
  if (Elements.empty())
    return {};
  NdVar Result = Elements.front();
  for (size_t Index = 1; Index < Elements.size(); ++Index) {
    if (Elements[Index].Size == 0)
      return {};
    NdVar Joined = S.makeTemp(Result.Size + Elements[Index].Size);
    S.emit(NdOp::CONCAT, Joined, {Elements[Index], Result});
    Result = Joined;
  }
  return Result;
}

NdVar extractOpmaskBit(X86Lifter::LiftState &S, NdVar Mask, unsigned BitIndex) {
  NdVar Shifted = Mask;
  if (BitIndex != 0) {
    Shifted = S.makeTemp(Mask.Size);
    S.emit(NdOp::INT_RIGHT, Shifted, {Mask, NdVar::cst(BitIndex, Mask.Size)});
  }
  NdVar BitWide = S.makeTemp(Mask.Size);
  S.emit(NdOp::INT_AND, BitWide, {Shifted, NdVar::cst(1, Mask.Size)});
  if (Mask.Size == 1)
    return BitWide;
  NdVar Bit = S.makeTemp(1);
  S.emit(NdOp::SUBBYTES, Bit, {BitWide, NdVar::cst(0, 4)});
  return Bit;
}

NdVar emitPackedUnpackMemoryMaskImpl(X86Lifter::LiftState &S,
                                     const cs_x86_op *MaskOperand,
                                     uint16_t VectorSize,
                                     uint16_t ElementSize, bool HighHalf) {
  if ((VectorSize != 16 && VectorSize != 32 && VectorSize != 64) ||
      ElementSize == 0 || VectorSize % ElementSize != 0 ||
      16 % (ElementSize * 2) != 0)
    return {};

  const unsigned ElementCount = VectorSize / ElementSize;
  NdVar Mask;
  if (MaskOperand) {
    const uint16_t MaskSize =
        static_cast<uint16_t>(std::max(1u, (ElementCount + 7u) / 8u));
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(MaskOperand->reg));
    if (!isX86OpmaskOperand(*MaskOperand) || MaskInfo.Size < MaskSize)
      return {};
    Mask = NdVar::reg(MaskInfo.Offset, MaskSize);
  }

  const unsigned ElementsPerLane = 16 / ElementSize;
  const unsigned HalfElementCount = ElementsPerLane / 2;
  const unsigned FirstElement = HighHalf ? HalfElementCount : 0;
  const uint64_t SignBit = UINT64_C(1) << (ElementSize * 8 - 1);
  std::vector<NdVar> SourceMask;
  SourceMask.reserve(ElementCount);
  for (unsigned SourceElement = 0; SourceElement < ElementCount;
       ++SourceElement) {
    const unsigned ElementWithinLane = SourceElement % ElementsPerLane;
    if (ElementWithinLane < FirstElement ||
        ElementWithinLane >= FirstElement + HalfElementCount) {
      SourceMask.push_back(NdVar::cst(0, ElementSize));
      continue;
    }

    if (!MaskOperand) {
      SourceMask.push_back(NdVar::cst(SignBit, ElementSize));
      continue;
    }

    const unsigned LaneBaseElement = SourceElement - ElementWithinLane;
    const unsigned ResultElement =
        LaneBaseElement + (ElementWithinLane - FirstElement) * 2 + 1;
    NdVar LaneMask = S.makeTemp(ElementSize);
    S.emit(NdOp::SELECT, LaneMask,
           {extractOpmaskBit(S, Mask, ResultElement),
            NdVar::cst(SignBit, ElementSize), NdVar::cst(0, ElementSize)});
    SourceMask.push_back(LaneMask);
  }
  return concatenateLowToHigh(S, SourceMask);
}

bool validateEvexPackedUnpack(const cs_insn *Insn, const cs_x86 &X86,
                              Arch TargetArch, uint8_t ExpectedOpcode,
                              uint16_t ElementSize, uint16_t VectorSize,
                              bool HasWriteMask, const cs_x86_op *MaskOperand,
                              const cs_x86_op &DestinationOperand,
                              const cs_x86_op &LeftOperand,
                              const cs_x86_op &RightOperand,
                              CanonicalEvexEncodingInfo &Encoding,
                              bool &Broadcast) {
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, TargetArch, Encoding) ||
      (Encoding.P0 & 0x07) != 0x01 ||
      ((Encoding.P1 | 0x04) & 0x07) != 0x05 ||
      Encoding.Opcode != ExpectedOpcode || X86.encoding.imm_offset != 0 ||
      X86.encoding.imm_size != 0 || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const bool W = (Encoding.P1 & 0x80) != 0;
  if ((ElementSize == 4 && W) || (ElementSize == 8 && !W))
    return false;

  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  const uint8_t ExpectedLength = VectorSize == 16   ? 0
                                 : VectorSize == 32 ? 0x20
                                                    : 0x40;
  if (EncodedLength == 0x60 || EncodedLength != ExpectedLength ||
      !isVectorRegisterOfSize(DestinationOperand, VectorSize) ||
      !isVectorRegisterOfSize(LeftOperand, VectorSize) ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(DestinationOperand) ||
      decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
          vectorRegisterIndex(LeftOperand))
    return false;

  Broadcast = (Encoding.P2 & 0x10) != 0;
  const bool MemoryForm = RightOperand.type == X86_OP_MEM;
  if (MemoryForm) {
    if ((Encoding.ModRM & 0xc0) == 0xc0 || (Broadcast && ElementSize < 4) ||
        RightOperand.size != (Broadcast ? ElementSize : VectorSize) ||
        !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, RightOperand,
                                         Broadcast ? ElementSize : VectorSize))
      return false;
  } else if (!isVectorRegisterOfSize(RightOperand, VectorSize) || Broadcast ||
             decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                 vectorRegisterIndex(RightOperand) ||
             !validateCanonicalEvexRegisterTail(Insn, X86, Encoding)) {
    return false;
  }

  const unsigned ElementCount = VectorSize / ElementSize;
  const uint16_t RequiredMaskSize =
      static_cast<uint16_t>(std::max(1u, (ElementCount + 7u) / 8u));
  const uint8_t EncodedMask = Encoding.P2 & 0x07;
  const bool EncodedZero = (Encoding.P2 & 0x80) != 0;
  if (HasWriteMask) {
    if (!MaskOperand || !isX86OpmaskOperand(*MaskOperand) ||
        MaskOperand->reg == X86_REG_K0 ||
        MaskOperand->size != RequiredMaskSize ||
        EncodedMask != static_cast<uint8_t>(MaskOperand->reg - X86_REG_K0) ||
        EncodedZero != static_cast<bool>(MaskOperand->avx_zero_opmask))
      return false;
  } else if (MaskOperand || EncodedMask != 0 || EncodedZero) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast =
      Broadcast ? broadcastForLaneCount(ElementCount) : X86_AVX_BCAST_INVALID;
  if (RightOperand.avx_bcast != ExpectedBroadcast)
    return false;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    if (&Operand != &RightOperand && Operand.avx_bcast != X86_AVX_BCAST_INVALID)
      return false;
    if (Operand.avx_zero_opmask && (!MaskOperand || &Operand != MaskOperand))
      return false;
  }
  return true;
}

} // namespace

NdVar emitPackedUnpackMemoryMask(X86Lifter::LiftState &S,
                                 const cs_x86_op *MaskOperand,
                                 uint16_t VectorSize, uint16_t ElementSize,
                                 bool HighHalf) {
  return emitPackedUnpackMemoryMaskImpl(S, MaskOperand, VectorSize,
                                        ElementSize, HighHalf);
}

bool emitPackedUnpack(X86Lifter::LiftState &S, NdVar Destination, NdVar Left,
                      NdVar Right, uint16_t ElementSize, bool HighHalf) {
  if (Destination.Size == 0 || Destination.Size != Left.Size ||
      Destination.Size != Right.Size ||
      (Destination.Size != 8 && Destination.Size != 16 &&
       Destination.Size != 32 && Destination.Size != 64) ||
      ElementSize == 0)
    return false;

  const unsigned LaneSize = Destination.Size == 8 ? 8 : 16;
  if (Destination.Size % LaneSize != 0 || LaneSize % (ElementSize * 2) != 0)
    return false;
  const unsigned ResultElementCount = Destination.Size / ElementSize;
  if (ResultElementCount > 64 ||
      (ResultElementCount & (ResultElementCount - 1)) != 0)
    return false;

  NdVar Elements[64];
  unsigned OutputIndex = 0;
  const unsigned HalfElementCount = LaneSize / (ElementSize * 2);
  const unsigned FirstElement = HighHalf ? HalfElementCount : 0;
  for (unsigned LaneBase = 0; LaneBase < Destination.Size;
       LaneBase += LaneSize) {
    for (unsigned I = 0; I < HalfElementCount; ++I) {
      const unsigned ByteOffset = LaneBase + (FirstElement + I) * ElementSize;
      Elements[OutputIndex] = S.makeTemp(ElementSize);
      S.emit(NdOp::SUBBYTES, Elements[OutputIndex++],
             {Left, NdVar::cst(ByteOffset, 4)});
      Elements[OutputIndex] = S.makeTemp(ElementSize);
      S.emit(NdOp::SUBBYTES, Elements[OutputIndex++],
             {Right, NdVar::cst(ByteOffset, 4)});
    }
  }
  if (OutputIndex != ResultElementCount)
    return false;

  unsigned Count = ResultElementCount;
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
  S.emit(NdOp::COPY, Destination, {Elements[0]});
  return true;
}

bool liftSIMDConvert(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                     const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // SSE int<->float conversions, additional variants.
  case X86_INS_CVTDQ2PS:
  case X86_INS_CVTDQ2PD:
  case X86_INS_CVTPI2PS:
  case X86_INS_CVTPI2PD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    bool IsPD = (InsnId == X86_INS_CVTDQ2PD || InsnId == X86_INS_CVTPI2PD);
    unsigned IntSz = 4;
    unsigned FPSz = IsPD ? 8 : 4;
    unsigned NLanes = Src.Size / IntSz;
    if (NLanes < 2 || NLanes > 4) {
      S.emit(NdOp::FLOAT_INT2FLOAT, Dst, {Src});
      break;
    }
    if (IsPD && NLanes > 2)
      NLanes = 2;
    std::vector<NdVar> Lanes(NLanes);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Elem = S.makeTemp(IntSz);
      S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(I * IntSz, 4)});
      Lanes[I] = S.makeTemp(FPSz);
      S.emit(NdOp::FLOAT_INT2FLOAT, Lanes[I], {Elem});
    }
    if (NLanes == 2) {
      unsigned PairSz = FPSz * 2;
      NdVar Pair = S.makeTemp(PairSz);
      S.emit(NdOp::CONCAT, Pair, {Lanes[1], Lanes[0]});
      if (Dst.Size == PairSz) {
        S.emit(NdOp::COPY, Dst, {Pair});
      } else {
        NdVar ZHi = S.makeTemp(Dst.Size - PairSz);
        S.emit(NdOp::COPY, ZHi, {NdVar::cst(0, (uint16_t)(Dst.Size - PairSz))});
        S.emit(NdOp::CONCAT, Dst, {ZHi, Pair});
      }
    } else {
      NdVar Lo = S.makeTemp(FPSz * 2);
      S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
      NdVar Hi = S.makeTemp(FPSz * 2);
      S.emit(NdOp::CONCAT, Hi, {Lanes[3], Lanes[2]});
      S.emit(NdOp::CONCAT, Dst, {Hi, Lo});
    }
    break;
  }
  case X86_INS_CVTPS2DQ:
  case X86_INS_CVTPD2DQ:
  case X86_INS_CVTPS2PI:
  case X86_INS_CVTPD2PI:
  case X86_INS_CVTTPS2DQ:
  case X86_INS_CVTTPD2DQ:
  case X86_INS_CVTTPS2PI:
  case X86_INS_CVTTPD2PI: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    bool IsPD = (InsnId == X86_INS_CVTPD2DQ || InsnId == X86_INS_CVTTPD2DQ ||
                 InsnId == X86_INS_CVTPD2PI || InsnId == X86_INS_CVTTPD2PI);
    // The non-T variants round using MXCSR (default: nearest, ties to even);
    // only the CVTT* variants truncate toward zero.
    bool IsTrunc =
        (InsnId == X86_INS_CVTTPS2DQ || InsnId == X86_INS_CVTTPD2DQ ||
         InsnId == X86_INS_CVTTPS2PI || InsnId == X86_INS_CVTTPD2PI);
    unsigned FPSz = IsPD ? 8 : 4;
    unsigned DstElemSz = 4;
    unsigned NLanes = Src.Size / FPSz;
    unsigned DstLanes = Dst.Size / DstElemSz;
    if (NLanes < 2 || NLanes > 4) {
      S.emit(NdOp::FLOAT_TRUNC, Dst, {Src});
      break;
    }
    if (NLanes > DstLanes)
      NLanes = DstLanes;
    std::vector<NdVar> Lanes(NLanes);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Elem = S.makeTemp(FPSz);
      S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(I * FPSz, 4)});
      Lanes[I] = S.makeTemp(DstElemSz);
      if (IsTrunc) {
        S.emit(NdOp::FLOAT_TRUNC, Lanes[I], {Elem});
      } else {
        NdVar Rnd = S.makeTemp(FPSz);
        S.emit(NdOp::FLOAT_ROUNDEVEN, Rnd, {Elem});
        S.emit(NdOp::FLOAT_FLOAT2INT, Lanes[I], {Rnd});
      }
    }
    if (NLanes == 2) {
      if (Dst.Size == 8) {
        S.emit(NdOp::CONCAT, Dst, {Lanes[1], Lanes[0]});
      } else {
        NdVar Lo = S.makeTemp(8);
        S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
        NdVar ZHi = S.makeTemp(8);
        S.emit(NdOp::COPY, ZHi, {NdVar::cst(0, 8)});
        S.emit(NdOp::CONCAT, Dst, {ZHi, Lo});
      }
    } else {
      NdVar Lo = S.makeTemp(8);
      S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
      NdVar Hi = S.makeTemp(8);
      S.emit(NdOp::CONCAT, Hi, {Lanes[3], Lanes[2]});
      S.emit(NdOp::CONCAT, Dst, {Hi, Lo});
    }
    break;
  }
  // Packed FP width conversions (scalar CVTSS2SD/CVTSD2SS are handled in
  // liftCore, which runs first).  Each lane must carry its true FP width so
  // the emitter picks fpext/fptrunc correctly.
  case X86_INS_CVTPS2PD: {
    // Low two single-precision lanes -> two double-precision lanes.
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    std::vector<NdVar> Lanes(2);
    for (unsigned I = 0; I < 2; ++I) {
      NdVar Elem = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(I * 4, 4)});
      Lanes[I] = S.makeTemp(8);
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Lanes[I], {Elem});
    }
    S.emit(NdOp::CONCAT, Dst, {Lanes[1], Lanes[0]});
    break;
  }
  case X86_INS_CVTPD2PS: {
    // Two double-precision lanes -> two single-precision lanes (low 64 bits);
    // the upper 64 bits of the destination are zeroed.
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    std::vector<NdVar> Lanes(2);
    for (unsigned I = 0; I < 2; ++I) {
      NdVar Elem = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(I * 8, 4)});
      Lanes[I] = S.makeTemp(4);
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Lanes[I], {Elem});
    }
    NdVar Lo = S.makeTemp(8);
    S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
    if (Dst.Size > 8) {
      NdVar ZHi = S.makeTemp(Dst.Size - 8);
      S.emit(NdOp::COPY, ZHi, {NdVar::cst(0, (uint16_t)(Dst.Size - 8))});
      S.emit(NdOp::CONCAT, Dst, {ZHi, Lo});
    } else {
      S.emit(NdOp::COPY, Dst, {Lo});
    }
    break;
  }

  // VEX and EVEX packed unpacks. Wider vectors still interleave independently
  // inside each architectural 128-bit lane.
  case X86_INS_VPUNPCKLBW:
  case X86_INS_VPUNPCKHBW:
  case X86_INS_VPUNPCKLWD:
  case X86_INS_VPUNPCKHWD:
  case X86_INS_VPUNPCKLDQ:
  case X86_INS_VPUNPCKHDQ:
  case X86_INS_VPUNPCKLQDQ:
  case X86_INS_VPUNPCKHQDQ: {
    if (X86.op_count < 3)
      break;
    uint16_t ElementSize = 0;
    bool HighHalf = false;
    uint8_t ExpectedOpcode = 0;
    switch (InsnId) {
    case X86_INS_VPUNPCKLBW:
      ElementSize = 1;
      ExpectedOpcode = 0x60;
      break;
    case X86_INS_VPUNPCKHBW:
      ElementSize = 1;
      HighHalf = true;
      ExpectedOpcode = 0x68;
      break;
    case X86_INS_VPUNPCKLWD:
      ElementSize = 2;
      ExpectedOpcode = 0x61;
      break;
    case X86_INS_VPUNPCKHWD:
      ElementSize = 2;
      HighHalf = true;
      ExpectedOpcode = 0x69;
      break;
    case X86_INS_VPUNPCKLDQ:
      ElementSize = 4;
      ExpectedOpcode = 0x62;
      break;
    case X86_INS_VPUNPCKHDQ:
      ElementSize = 4;
      HighHalf = true;
      ExpectedOpcode = 0x6a;
      break;
    case X86_INS_VPUNPCKLQDQ:
      ElementSize = 8;
      ExpectedOpcode = 0x6c;
      break;
    default:
      ElementSize = 8;
      HighHalf = true;
      ExpectedOpcode = 0x6d;
      break;
    }

    if (getEvexEncoding(Insn)) {
      const bool HasWriteMask =
          X86.op_count == 4 && isX86OpmaskOperand(X86.operands[1]);
      if ((!HasWriteMask && X86.op_count != 3) ||
          (HasWriteMask && X86.op_count != 4))
        return false;
      const unsigned LeftIndex = HasWriteMask ? 2 : 1;
      const unsigned RightIndex = HasWriteMask ? 3 : 2;
      const cs_x86_op &DestinationOperand = X86.operands[0];
      const cs_x86_op *MaskOperand = HasWriteMask ? &X86.operands[1] : nullptr;
      const cs_x86_op &LeftOperand = X86.operands[LeftIndex];
      const cs_x86_op &RightOperand = X86.operands[RightIndex];
      const uint16_t VectorSize = DestinationOperand.size;
      CanonicalEvexEncodingInfo Encoding;
      bool Broadcast = false;

      // Validate every decoded and raw EVEX property before emitting LowIR.
      if ((VectorSize != 16 && VectorSize != 32 && VectorSize != 64) ||
          !isVectorRegisterOfSize(DestinationOperand, VectorSize) ||
          !isVectorRegisterOfSize(LeftOperand, VectorSize) ||
          DestinationOperand.avx_zero_opmask || LeftOperand.avx_zero_opmask ||
          RightOperand.avx_zero_opmask ||
          !validateEvexPackedUnpack(
              Insn, X86, L.targetArch(), ExpectedOpcode, ElementSize,
              VectorSize, HasWriteMask, MaskOperand, DestinationOperand,
              LeftOperand, RightOperand, Encoding, Broadcast))
        return false;

      if (MaskOperand) {
        const unsigned ElementCount = VectorSize / ElementSize;
        const uint16_t RequiredMaskSize =
            static_cast<uint16_t>(std::max(1u, (ElementCount + 7u) / 8u));
        const RegInfo MaskInfo =
            mapCapstoneReg(static_cast<x86_reg>(MaskOperand->reg));
        if (MaskInfo.Size < RequiredMaskSize)
          return false;
      }

      NdVar Left = L.operandRead(S, LeftOperand);
      if (Left.Size != VectorSize)
        return false;
      NdVar Right;
      if (RightOperand.type == X86_OP_REG) {
        Right = L.operandRead(S, RightOperand);
        if (Right.Size != VectorSize)
          return false;
      } else if (Broadcast) {
        const unsigned ElementCount = VectorSize / ElementSize;
        const uint16_t MaskSize =
            static_cast<uint16_t>(std::max(1u, (ElementCount + 7u) / 8u));
        uint64_t MemoryResultLanes = 0;
        for (unsigned Lane = 1; Lane < ElementCount; Lane += 2)
          MemoryResultLanes |= UINT64_C(1) << Lane;
        NdVar MemoryMask = NdVar::cst(MemoryResultLanes, MaskSize);
        if (MaskOperand) {
          const RegInfo MaskInfo =
              mapCapstoneReg(static_cast<x86_reg>(MaskOperand->reg));
          NdVar ActiveMask = NdVar::reg(MaskInfo.Offset, MaskSize);
          MemoryMask = S.makeTemp(MaskSize);
          S.emit(NdOp::INT_AND, MemoryMask,
                 {ActiveMask, NdVar::cst(MemoryResultLanes, MaskSize)});
        }
        Right =
            emitEvexMaskedMemoryLoad(S, RightOperand, MemoryMask, VectorSize,
                                     ElementSize, ElementSize, true);
        if (Right.Size != VectorSize)
          return false;
      } else {
        NdVar SourceMask = emitPackedUnpackMemoryMask(
            S, MaskOperand, VectorSize, ElementSize, HighHalf);
        const Intrinsic Load = packedMaskedLoadIntrinsic(ElementSize);
        if (SourceMask.Size != VectorSize || Load == Intrinsic::None)
          return false;
        const NdVar Address = S.computeEA(RightOperand);
        Right = S.makeTemp(VectorSize);
        S.emitIntrinsic(Load, Right, {Address, SourceMask},
                        NdMemoryOrdering::None,
                        X86Lifter::LiftState::memoryAddressSpace(RightOperand));
      }

      NdVar Result = MaskOperand ? S.makeTemp(VectorSize)
                                 : L.operandWrite(DestinationOperand);
      if (!emitPackedUnpack(S, Result, Left, Right, ElementSize, HighHalf))
        return false;
      if (MaskOperand &&
          !emitMaskedVectorResult(L, S, DestinationOperand, *MaskOperand,
                                  Result, ElementSize))
        return false;
      break;
    }

    if (X86.op_count != 3 ||
        !isVectorRegisterOfSize(X86.operands[0], X86.operands[0].size) ||
        !isVectorRegisterOfSize(X86.operands[1], X86.operands[0].size) ||
        (X86.operands[0].size != 16 && X86.operands[0].size != 32))
      return false;
    NdVar Destination = L.operandWrite(X86.operands[0]);
    NdVar Left = L.operandRead(S, X86.operands[1]);
    NdVar Right = L.operandRead(S, X86.operands[2]);
    if (!emitPackedUnpack(S, Destination, Left, Right, ElementSize, HighHalf))
      return false;
    break;
  }

  // PMOVZX/PMOVSX — packed move with per-element zero/sign extension.
  // Each of the N low source elements (byte/word/dword) is independently
  // extended to the wider destination element.  A single whole-register
  // INT_ZEXT/INT_SEXT is wrong: it would treat the packed source as one
  // scalar and only fill lane 0 (and corrupt the rest).
  case X86_INS_PMOVZXBW:
  case X86_INS_PMOVZXBD:
  case X86_INS_PMOVZXBQ:
  case X86_INS_PMOVZXWD:
  case X86_INS_PMOVZXWQ:
  case X86_INS_PMOVZXDQ:
  case X86_INS_VPMOVZXBW:
  case X86_INS_VPMOVZXBD:
  case X86_INS_VPMOVZXBQ:
  case X86_INS_VPMOVZXWD:
  case X86_INS_VPMOVZXWQ:
  case X86_INS_VPMOVZXDQ:
  case X86_INS_PMOVSXBW:
  case X86_INS_PMOVSXBD:
  case X86_INS_PMOVSXBQ:
  case X86_INS_PMOVSXWD:
  case X86_INS_PMOVSXWQ:
  case X86_INS_PMOVSXDQ:
  case X86_INS_VPMOVSXBW:
  case X86_INS_VPMOVSXBD:
  case X86_INS_VPMOVSXBQ:
  case X86_INS_VPMOVSXWD:
  case X86_INS_VPMOVSXWQ:
  case X86_INS_VPMOVSXDQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint16_t SrcElemSz = 1, DstElemSz = 2;
    bool IsSigned = false;
    switch (InsnId) {
    case X86_INS_PMOVZXBW:
    case X86_INS_VPMOVZXBW:
      SrcElemSz = 1;
      DstElemSz = 2;
      break;
    case X86_INS_PMOVZXBD:
    case X86_INS_VPMOVZXBD:
      SrcElemSz = 1;
      DstElemSz = 4;
      break;
    case X86_INS_PMOVZXBQ:
    case X86_INS_VPMOVZXBQ:
      SrcElemSz = 1;
      DstElemSz = 8;
      break;
    case X86_INS_PMOVZXWD:
    case X86_INS_VPMOVZXWD:
      SrcElemSz = 2;
      DstElemSz = 4;
      break;
    case X86_INS_PMOVZXWQ:
    case X86_INS_VPMOVZXWQ:
      SrcElemSz = 2;
      DstElemSz = 8;
      break;
    case X86_INS_PMOVZXDQ:
    case X86_INS_VPMOVZXDQ:
      SrcElemSz = 4;
      DstElemSz = 8;
      break;
    case X86_INS_PMOVSXBW:
    case X86_INS_VPMOVSXBW:
      SrcElemSz = 1;
      DstElemSz = 2;
      IsSigned = true;
      break;
    case X86_INS_PMOVSXBD:
    case X86_INS_VPMOVSXBD:
      SrcElemSz = 1;
      DstElemSz = 4;
      IsSigned = true;
      break;
    case X86_INS_PMOVSXBQ:
    case X86_INS_VPMOVSXBQ:
      SrcElemSz = 1;
      DstElemSz = 8;
      IsSigned = true;
      break;
    case X86_INS_PMOVSXWD:
    case X86_INS_VPMOVSXWD:
      SrcElemSz = 2;
      DstElemSz = 4;
      IsSigned = true;
      break;
    case X86_INS_PMOVSXWQ:
    case X86_INS_VPMOVSXWQ:
      SrcElemSz = 2;
      DstElemSz = 8;
      IsSigned = true;
      break;
    case X86_INS_PMOVSXDQ:
    case X86_INS_VPMOVSXDQ:
      SrcElemSz = 4;
      DstElemSz = 8;
      IsSigned = true;
      break;
    default:
      break;
    }
    unsigned NLanes = DstElemSz ? (Dst.Size / DstElemSz) : 0;
    if (NLanes == 0) {
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Dst, {Src});
      break;
    }
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar El = S.makeTemp(SrcElemSz);
      S.emit(NdOp::SUBBYTES, El, {Src, NdVar::cst(I * SrcElemSz, 4)});
      NdVar Ext = S.makeTemp(DstElemSz);
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Ext, {El});
      if (I == 0) {
        Acc = Ext;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + DstElemSz);
        S.emit(NdOp::CONCAT, Next, {Ext, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  // Packed convert/pack — per-lane saturating narrow.
  case X86_INS_PACKUSWB:
  case X86_INS_PACKUSDW:
  case X86_INS_PACKSSWB:
  case X86_INS_PACKSSDW:
  case X86_INS_VPACKUSWB:
  case X86_INS_VPACKUSDW:
  case X86_INS_VPACKSSWB:
  case X86_INS_VPACKSSDW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar DstR = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                     : L.operandRead(S, X86.operands[0]);
    NdVar Src2 = L.operandRead(S, X86.operands[X86.op_count - 1]);

    bool IsSigned =
        (InsnId == X86_INS_PACKSSWB || InsnId == X86_INS_PACKSSDW ||
         InsnId == X86_INS_VPACKSSWB || InsnId == X86_INS_VPACKSSDW);
    bool IsWord = (InsnId == X86_INS_PACKSSWB || InsnId == X86_INS_PACKUSWB ||
                   InsnId == X86_INS_VPACKSSWB || InsnId == X86_INS_VPACKUSWB);
    unsigned SrcLaneSz = IsWord ? 2 : 4;
    unsigned DstLaneSz = IsWord ? 1 : 2;
    // PACK operates within each 128-bit lane: the lane's low half comes from
    // src1's lane, the high half from src2's lane.  The 256-bit (VEX.256) form
    // therefore interleaves the two operands PER 128-bit LANE
    // (dst = [pack(s1.lane0), pack(s2.lane0), pack(s1.lane1), pack(s2.lane1)]),
    // NOT as one 256-bit-wide pack — emitting all of src1 then all of src2 (the
    // naive extension) mislays the high lane.  MMX (64-bit dst) is a single
    // 8-byte lane; XMM one 16-byte lane; YMM two 16-byte lanes.
    unsigned LaneBytes = (Dst.Size >= 16) ? 16u : Dst.Size;
    unsigned NLanes128 = Dst.Size / LaneBytes; // MMX:1, XMM:1, YMM:2
    unsigned SrcElemsPerLane = LaneBytes / SrcLaneSz;

    auto ClampLane = [&](NdVar Src, unsigned Off) -> NdVar {
      NdVar Lane = S.makeTemp(SrcLaneSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(Off, 4)});

      // Narrowing saturate: trunc to DstLaneSz, sext back, compare with
      // original.  If equal the value fits; otherwise pick 127 / -128 (signed)
      // or 255 / 0 (unsigned) based on the sign of the original.  This avoids
      // both the fork's InstCombine crash on @llvm.smax/@llvm.smin chains AND
      // the constant-folding mis-compute on INT_SLESS+SELECT clamp patterns.
      NdVar Narrow = S.makeTemp(DstLaneSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Lane, NdVar::cst(0, 4)});
      NdVar BackWide = S.makeTemp(SrcLaneSz);
      if (IsSigned)
        S.emit(NdOp::INT_SEXT, BackWide, {Narrow});
      else
        S.emit(NdOp::INT_ZEXT, BackWide, {Narrow});
      NdVar Fits = S.makeTemp(1);
      S.emit(NdOp::INT_EQUAL, Fits, {Lane, BackWide});
      NdVar IsPos = S.makeTemp(1);
      int64_t Hi = IsSigned ? (1LL << (DstLaneSz * 8 - 1)) - 1
                            : (1LL << (DstLaneSz * 8)) - 1;
      int64_t Lo = IsSigned ? -(1LL << (DstLaneSz * 8 - 1)) : 0;
      if (IsSigned) {
        S.emit(NdOp::INT_SLESS, IsPos, {NdVar::cst(0, SrcLaneSz), Lane});
      } else {
        S.emit(NdOp::INT_SLESS, IsPos,
               {NdVar::cst(static_cast<uint64_t>(Hi), SrcLaneSz), Lane});
      }
      NdVar OverflowVal = S.makeTemp(DstLaneSz);
      NdVar HiNarrow = NdVar::cst(static_cast<uint64_t>(Hi), DstLaneSz);
      NdVar LoNarrow = NdVar::cst(static_cast<uint64_t>(Lo), DstLaneSz);
      S.emit(NdOp::SELECT, OverflowVal, {IsPos, HiNarrow, LoNarrow});
      NdVar Result = S.makeTemp(DstLaneSz);
      S.emit(NdOp::SELECT, Result, {Fits, Narrow, OverflowVal});
      return Result;
    };

    // Build the result low-to-high.  For each 128-bit lane L, first the
    // SrcElemsPerLane clamped elements of src1's lane L, then src2's lane L.
    // CONCAT(hi, lo) prepends `hi` above the accumulator, so iterating elements
    // in increasing significance yields the correct little-endian layout.
    bool First = true;
    NdVar Acc = S.makeTemp(0);
    auto AppendLane = [&](NdVar Src, unsigned Lane) {
      unsigned Base = Lane * LaneBytes; // byte offset of this lane in Src
      for (unsigned E = 0; E < SrcElemsPerLane; ++E) {
        NdVar B = ClampLane(Src, Base + E * SrcLaneSz);
        if (First) {
          Acc = B;
          First = false;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {B, Acc});
          Acc = Next;
        }
      }
    };
    for (unsigned L = 0; L < NLanes128; ++L) {
      AppendLane(DstR, L); // src1 lane L -> low half of dst lane L
      AppendLane(Src2, L); // src2 lane L -> high half of dst lane L
    }
    if (Acc.Size < Dst.Size) {
      NdVar Wide = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, Wide, {Acc});
      S.emit(NdOp::COPY, Dst, {Wide});
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
