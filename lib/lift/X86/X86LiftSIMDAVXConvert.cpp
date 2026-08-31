//===- X86LiftSIMDAVXConvert.cpp - x86/x64 AVX/AVX-512 conversion lifter --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// VEX/EVEX VCVT* conversions between packed or scalar
/// integers and floats, between float widths, and the
/// half-precision (F16C) pack/unpack.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include <algorithm>
#include <vector>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

enum class PackedConvertDirection : uint8_t {
  IntegerToFloat,
  FloatToFloat,
  FloatToIntegerTruncate,
  FloatToIntegerRounded,
};

struct PackedConvertSpec {
  uint8_t Opcode = 0;
  uint8_t MandatoryPrefix = 0;
  uint8_t SourceElementSize = 0;
  uint8_t DestinationElementSize = 0;
  bool W = false;
  bool Unsigned = false;
  PackedConvertDirection Direction = PackedConvertDirection::IntegerToFloat;
  bool SupportsEmbeddedRounding = false;
  bool SupportsSAEOnly = false;
};

struct ScalarFloatConvertSpec {
  uint8_t SourceElementSize = 0;
  uint8_t DestinationElementSize = 0;
  bool W = false;
  uint8_t MandatoryPrefix = 0;
  bool SupportsEmbeddedRounding = false;
  bool SupportsSAEOnly = false;
};

struct ScalarFloatToIntegerSpec {
  uint8_t SourceElementSize = 0;
  uint8_t MandatoryPrefix = 0;
  uint8_t Opcode = 0;
  bool Unsigned = false;
  bool Truncate = false;
};

bool getScalarFloatConvertSpec(unsigned InsnId, ScalarFloatConvertSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VCVTSD2SS:
    Spec = {8, 4, true, 3, true, false};
    return true;
  case X86_INS_VCVTSS2SD:
    Spec = {4, 8, false, 2, false, true};
    return true;
  default:
    return false;
  }
}

bool getScalarFloatToIntegerSpec(unsigned InsnId,
                                 ScalarFloatToIntegerSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VCVTSS2SI:
    Spec = {4, 2, 0x2d, false, false};
    return true;
  case X86_INS_VCVTSD2SI:
    Spec = {8, 3, 0x2d, false, false};
    return true;
  case X86_INS_VCVTTSS2SI:
    Spec = {4, 2, 0x2c, false, true};
    return true;
  case X86_INS_VCVTTSD2SI:
    Spec = {8, 3, 0x2c, false, true};
    return true;
  case X86_INS_VCVTSS2USI:
    Spec = {4, 2, 0x79, true, false};
    return true;
  case X86_INS_VCVTSD2USI:
    Spec = {8, 3, 0x79, true, false};
    return true;
  case X86_INS_VCVTTSS2USI:
    Spec = {4, 2, 0x78, true, true};
    return true;
  case X86_INS_VCVTTSD2USI:
    Spec = {8, 3, 0x78, true, true};
    return true;
  default:
    return false;
  }
}

bool getPackedConvertSpec(unsigned InsnId, PackedConvertSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VCVTDQ2PS:
    Spec = {0x5b, 0, 4, 4, false, false, PackedConvertDirection::IntegerToFloat,
            true};
    return true;
  case X86_INS_VCVTDQ2PD:
    Spec = {
        0xe6, 2, 4, 8, false, false, PackedConvertDirection::IntegerToFloat};
    return true;
  case X86_INS_VCVTUDQ2PS:
    Spec = {0x7a, 3, 4, 4, false, true, PackedConvertDirection::IntegerToFloat,
            true};
    return true;
  case X86_INS_VCVTUDQ2PD:
    Spec = {0x7a, 2, 4, 8, false, true, PackedConvertDirection::IntegerToFloat};
    return true;
  case X86_INS_VCVTQQ2PS:
    Spec = {0x5b, 0, 8, 4, true, false, PackedConvertDirection::IntegerToFloat,
            true};
    return true;
  case X86_INS_VCVTQQ2PD:
    Spec = {0xe6, 2, 8, 8, true, false, PackedConvertDirection::IntegerToFloat,
            true};
    return true;
  case X86_INS_VCVTUQQ2PS:
    Spec = {0x7a, 3, 8, 4, true, true, PackedConvertDirection::IntegerToFloat,
            true};
    return true;
  case X86_INS_VCVTUQQ2PD:
    Spec = {0x7a, 2, 8, 8, true, true, PackedConvertDirection::IntegerToFloat,
            true};
    return true;

  case X86_INS_VCVTPD2PS:
    Spec = {0x5a, 1, 8, 4, true, false, PackedConvertDirection::FloatToFloat,
            true};
    return true;
  case X86_INS_VCVTPS2PD:
    Spec = {
        0x5a,  0,   4, 8, false, false, PackedConvertDirection::FloatToFloat,
        false, true};
    return true;

  case X86_INS_VCVTTPS2DQ:
    Spec = {0x5b,
            2,
            4,
            4,
            false,
            false,
            PackedConvertDirection::FloatToIntegerTruncate};
    return true;
  case X86_INS_VCVTTPD2DQ:
    Spec = {0xe6,
            1,
            8,
            4,
            true,
            false,
            PackedConvertDirection::FloatToIntegerTruncate};
    return true;
  case X86_INS_VCVTTPS2UDQ:
    Spec = {0x78,
            0,
            4,
            4,
            false,
            true,
            PackedConvertDirection::FloatToIntegerTruncate};
    return true;
  case X86_INS_VCVTTPD2UDQ:
    Spec = {0x78,
            0,
            8,
            4,
            true,
            true,
            PackedConvertDirection::FloatToIntegerTruncate};
    return true;
  case X86_INS_VCVTTPS2QQ:
    Spec = {0x7a,
            1,
            4,
            8,
            false,
            false,
            PackedConvertDirection::FloatToIntegerTruncate};
    return true;
  case X86_INS_VCVTTPD2QQ:
    Spec = {0x7a,
            1,
            8,
            8,
            true,
            false,
            PackedConvertDirection::FloatToIntegerTruncate};
    return true;
  case X86_INS_VCVTTPS2UQQ:
    Spec = {0x78,
            1,
            4,
            8,
            false,
            true,
            PackedConvertDirection::FloatToIntegerTruncate};
    return true;
  case X86_INS_VCVTTPD2UQQ:
    Spec = {0x78,
            1,
            8,
            8,
            true,
            true,
            PackedConvertDirection::FloatToIntegerTruncate};
    return true;

  case X86_INS_VCVTPS2DQ:
    Spec = {0x5b,
            1,
            4,
            4,
            false,
            false,
            PackedConvertDirection::FloatToIntegerRounded,
            true};
    return true;
  case X86_INS_VCVTPD2DQ:
    Spec = {0xe6,
            3,
            8,
            4,
            true,
            false,
            PackedConvertDirection::FloatToIntegerRounded,
            true};
    return true;
  case X86_INS_VCVTPS2UDQ:
    Spec = {0x79,
            0,
            4,
            4,
            false,
            true,
            PackedConvertDirection::FloatToIntegerRounded,
            true};
    return true;
  case X86_INS_VCVTPD2UDQ:
    Spec = {0x79,
            0,
            8,
            4,
            true,
            true,
            PackedConvertDirection::FloatToIntegerRounded,
            true};
    return true;
  case X86_INS_VCVTPS2QQ:
    Spec = {0x7b,
            1,
            4,
            8,
            false,
            false,
            PackedConvertDirection::FloatToIntegerRounded,
            true};
    return true;
  case X86_INS_VCVTPD2QQ:
    Spec = {0x7b,
            1,
            8,
            8,
            true,
            false,
            PackedConvertDirection::FloatToIntegerRounded,
            true};
    return true;
  case X86_INS_VCVTPS2UQQ:
    Spec = {0x79,
            1,
            4,
            8,
            false,
            true,
            PackedConvertDirection::FloatToIntegerRounded,
            true};
    return true;
  case X86_INS_VCVTPD2UQQ:
    Spec = {0x79,
            1,
            8,
            8,
            true,
            true,
            PackedConvertDirection::FloatToIntegerRounded,
            true};
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
  return Size == 64 && Operand.reg >= X86_REG_ZMM0 &&
         Operand.reg <= X86_REG_ZMM31;
}

unsigned vectorRegisterIndex(const cs_x86_op &Operand) {
  if (Operand.size == 16)
    return static_cast<unsigned>(Operand.reg - X86_REG_XMM0);
  if (Operand.size == 32)
    return static_cast<unsigned>(Operand.reg - X86_REG_YMM0);
  return static_cast<unsigned>(Operand.reg - X86_REG_ZMM0);
}

bool hasPotentialEvexPrefix(const cs_insn *Insn) {
  if (!Insn)
    return false;
  // Claim malformed EVEX encodings too, so a forbidden legacy prefix cannot
  // bypass the strict EVEX validator and fall through to the legacy lifter.
  size_t Offset = 0;
  while (Offset < Insn->size) {
    const uint8_t Byte = Insn->bytes[Offset];
    if (Byte == 0x62)
      return true;
    if (Byte != 0x26 && Byte != 0x2e && Byte != 0x36 && Byte != 0x3e &&
        Byte != 0x64 && Byte != 0x65 && Byte != 0x66 && Byte != 0x67 &&
        Byte != 0xf0 && Byte != 0xf2 && Byte != 0xf3)
      return false;
    ++Offset;
  }
  return false;
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

bool isScalarIntegerToFloat(unsigned InsnId) {
  return InsnId == X86_INS_VCVTSI2SS || InsnId == X86_INS_VCVTSI2SD ||
         InsnId == X86_INS_VCVTUSI2SS || InsnId == X86_INS_VCVTUSI2SD;
}

bool liftEvexScalarIntegerToFloat(X86Lifter &L, X86Lifter::LiftState &S,
                                  const cs_insn *Insn, const cs_x86 &X86,
                                  unsigned InsnId) {
  if (!isScalarIntegerToFloat(InsnId))
    return false;

  const bool ToDouble =
      InsnId == X86_INS_VCVTSI2SD || InsnId == X86_INS_VCVTUSI2SD;
  const bool Unsigned =
      InsnId == X86_INS_VCVTUSI2SS || InsnId == X86_INS_VCVTUSI2SD;
  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != 0x01 ||
      ((Encoding.P1 | 0x04) & 0x07) !=
          static_cast<uint8_t>(0x04 | (ToDouble ? 0x03 : 0x02)) ||
      Encoding.Opcode != (Unsigned ? 0x7b : 0x2a) ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      X86.op_count != 3)
    return false;

  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &MergeSource = X86.operands[1];
  const cs_x86_op &IntegerSource = X86.operands[2];
  const uint16_t IntegerSize = (Encoding.P1 & 0x80) != 0 ? 8 : 4;
  if (!isVectorRegisterOfSize(Destination, 16) ||
      !isVectorRegisterOfSize(MergeSource, 16) ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(Destination) ||
      decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
          vectorRegisterIndex(MergeSource))
    return false;

  const bool MemoryForm = (Encoding.ModRM & 0xc0) != 0xc0;
  const bool EncodedB = (Encoding.P2 & 0x10) != 0;
  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  const bool EmbeddedRounding = !MemoryForm && EncodedB;
  const x86_avx_rm ExpectedRounding =
      EmbeddedRounding
          ? static_cast<x86_avx_rm>(X86_AVX_RM_RN + (EncodedLength >> 5))
          : X86_AVX_RM_INVALID;
  if ((MemoryForm && EncodedB) || (!EmbeddedRounding && EncodedLength != 0) ||
      (Encoding.P2 & 0x87) != 0 || X86.avx_sae != EmbeddedRounding ||
      X86.avx_rm != ExpectedRounding)
    return false;

  for (unsigned Index = 0; Index < X86.op_count; ++Index)
    if (X86.operands[Index].avx_zero_opmask ||
        X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID)
      return false;

  if (MemoryForm) {
    if (IntegerSource.type != X86_OP_MEM || IntegerSource.size != IntegerSize ||
        !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, IntegerSource,
                                         IntegerSize))
      return false;
  } else {
    if (IntegerSource.type != X86_OP_REG || IntegerSource.size != IntegerSize ||
        !validateCanonicalEvexRegisterTail(Insn, X86, Encoding))
      return false;

    const unsigned EncodedSource = (Encoding.ModRM & 7) |
                                   ((Encoding.P0 & 0x20) == 0 ? 8 : 0) |
                                   ((Encoding.P0 & 0x08) != 0 ? 16 : 0);
    const RegInfo SourceInfo =
        mapCapstoneReg(static_cast<x86_reg>(IntegerSource.reg));
    const uint64_t ExpectedOffset =
        EncodedSource < 16 ? static_cast<uint64_t>(EncodedSource) * 8
                           : x86reg::extendedGeneralReg(EncodedSource - 16);
    if (EncodedSource >= 32 || !x86reg::isGeneralRegOffset(SourceInfo.Offset) ||
        SourceInfo.Offset != ExpectedOffset || SourceInfo.Size != IntegerSize)
      return false;
  }

  NdVar ScalarSource = L.operandRead(S, IntegerSource);
  const NdVar Merge = L.operandRead(S, MergeSource);
  const NdVar DestinationVar = L.operandWrite(Destination);
  if (ScalarSource.Size != IntegerSize || Merge.Size != 16 ||
      DestinationVar.Size != 16)
    return false;
  NdVar Source = S.makeTemp(16);
  S.emit(NdOp::INT_ZEXT, Source, {ScalarSource});

  const X86FPRounding Rounding =
      EmbeddedRounding ? static_cast<X86FPRounding>(EncodedLength >> 5)
                       : X86FPRounding::MXCSR;
  const X86FPConvertKind Kind = Unsigned
                                    ? X86FPConvertKind::UnsignedIntegerToFloat
                                    : X86FPConvertKind::SignedIntegerToFloat;
  const uint16_t Control = makeX86FPConvertControl(
      Kind, IntegerSize == 8, ToDouble, false, EmbeddedRounding, Rounding, 1);
  NdVar Converted = S.makeTemp(16);
  S.emitIntrinsic(Intrinsic::X86FPConvert, Converted,
                  {NdVar::cst(Control, 2), Source, NdVar::cst(1, 1)});

  const uint16_t DestinationElementSize = ToDouble ? 8 : 4;
  NdVar Low = S.makeTemp(DestinationElementSize);
  S.emit(NdOp::SUBBYTES, Low, {Converted, NdVar::cst(0, 4)});
  NdVar Upper = S.makeTemp(16 - DestinationElementSize);
  S.emit(NdOp::SUBBYTES, Upper, {Merge, NdVar::cst(DestinationElementSize, 4)});
  S.emit(NdOp::CONCAT, DestinationVar, {Upper, Low});
  return true;
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

NdVar maskF16CSourceLanes(X86Lifter::LiftState &S, NdVar Source,
                          NdVar CompactMask, unsigned ActiveLanes,
                          uint16_t ElementSize) {
  if (Source.Size == 0 || Source.Size % ElementSize != 0 ||
      ActiveLanes > Source.Size / ElementSize || CompactMask.Size == 0)
    return {};
  NdVar Result;
  const unsigned StoredLanes = Source.Size / ElementSize;
  for (unsigned Lane = 0; Lane < StoredLanes; ++Lane) {
    NdVar SourceLane = S.makeTemp(ElementSize);
    S.emit(NdOp::SUBBYTES, SourceLane,
           {Source, NdVar::cst(static_cast<uint64_t>(Lane) * ElementSize, 4)});
    NdVar Selected = NdVar::cst(0, ElementSize);
    if (Lane < ActiveLanes) {
      const NdVar MaskBit = extractCompactMaskBit(S, CompactMask, Lane);
      Selected = S.makeTemp(ElementSize);
      S.emit(NdOp::SELECT, Selected,
             {MaskBit, SourceLane, NdVar::cst(0, ElementSize)});
    }
    if (Lane == 0) {
      Result = Selected;
      continue;
    }
    NdVar Next = S.makeTemp(Result.Size + ElementSize);
    S.emit(NdOp::CONCAT, Next, {Selected, Result});
    Result = Next;
  }
  return Result;
}

bool emitMaskedF16CNarrowResult(X86Lifter &L, X86Lifter::LiftState &S,
                                const cs_x86_op &DestinationOperand,
                                const cs_x86_op &MaskOperand, NdVar Converted,
                                unsigned ActiveLanes) {
  const NdVar Destination = L.operandWrite(DestinationOperand);
  if (Destination.Size != Converted.Size || ActiveLanes == 0 ||
      ActiveLanes * 2 > Destination.Size)
    return false;
  const RegInfo MaskInfo =
      mapCapstoneReg(static_cast<x86_reg>(MaskOperand.reg));
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (ActiveLanes + 7u) / 8u));
  if (!isX86OpmaskOperand(MaskOperand) || MaskInfo.Size < MaskSize)
    return false;
  const NdVar Mask = NdVar::reg(MaskInfo.Offset, MaskSize);
  const NdVar OldDestination = L.operandRead(S, DestinationOperand);

  NdVar Result;
  for (unsigned Lane = 0; Lane < ActiveLanes; ++Lane) {
    NdVar NewLane = S.makeTemp(2);
    S.emit(NdOp::SUBBYTES, NewLane,
           {Converted, NdVar::cst(static_cast<uint64_t>(Lane) * 2, 4)});
    const NdVar MaskBit = extractCompactMaskBit(S, Mask, Lane);
    NdVar Inactive = NdVar::cst(0, 2);
    if (!MaskOperand.avx_zero_opmask) {
      Inactive = S.makeTemp(2);
      S.emit(NdOp::SUBBYTES, Inactive,
             {OldDestination, NdVar::cst(static_cast<uint64_t>(Lane) * 2, 4)});
    }
    NdVar Selected = S.makeTemp(2);
    S.emit(NdOp::SELECT, Selected, {MaskBit, NewLane, Inactive});
    if (Lane == 0) {
      Result = Selected;
      continue;
    }
    NdVar Next = S.makeTemp(Result.Size + 2);
    S.emit(NdOp::CONCAT, Next, {Selected, Result});
    Result = Next;
  }
  if (Result.Size < Destination.Size) {
    NdVar Padded = S.makeTemp(Destination.Size);
    S.emit(NdOp::CONCAT, Padded,
           {NdVar::cst(0, Destination.Size - Result.Size), Result});
    Result = Padded;
  }
  S.emit(NdOp::COPY, Destination, {Result});
  return true;
}

bool liftEvexF16C(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                  const cs_x86 &X86, unsigned InsnId) {
  const bool HalfToSingle = InsnId == X86_INS_VCVTPH2PS;
  if (!HalfToSingle && InsnId != X86_INS_VCVTPS2PH)
    return false;

  const bool HasMask = HalfToSingle ? X86.op_count == 3 : X86.op_count == 4;
  if (X86.op_count != (HalfToSingle ? (HasMask ? 3 : 2) : (HasMask ? 4 : 3)) ||
      (HasMask && !isX86OpmaskOperand(X86.operands[1])))
    return false;
  const unsigned SourceIndex = HasMask ? 2 : 1;
  const unsigned ImmediateIndex = HasMask ? 3 : 2;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &Source = X86.operands[SourceIndex];
  const cs_x86_op &RawRegOperand = HalfToSingle ? Destination : Source;
  const cs_x86_op &RawRMOperand = HalfToSingle ? Source : Destination;
  if (!isVectorRegisterOfSize(RawRegOperand, RawRegOperand.size))
    return false;
  const uint16_t WideSize = RawRegOperand.size;
  if (WideSize != 16 && WideSize != 32 && WideSize != 64)
    return false;
  const uint16_t NarrowSize = WideSize / 2;
  const uint16_t NarrowRegisterSize = std::max<uint16_t>(16, NarrowSize);
  if (RawRMOperand.type == X86_OP_REG) {
    if (!isVectorRegisterOfSize(RawRMOperand, NarrowRegisterSize))
      return false;
  } else if (RawRMOperand.type != X86_OP_MEM ||
             RawRMOperand.size != NarrowSize) {
    return false;
  }

  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != (HalfToSingle ? 0x02 : 0x03) ||
      (Encoding.P1 | 0x04) != 0x7d ||
      Encoding.Opcode != (HalfToSingle ? 0x13 : 0x1d))
    return false;
  const bool SAE = (Encoding.P2 & 0x10) != 0;
  if (X86.avx_sae != SAE || X86.avx_rm != X86_AVX_RM_INVALID ||
      (SAE && (WideSize != 64 || RawRMOperand.type == X86_OP_MEM)))
    return false;

  const unsigned LaneCount = WideSize / 4;
  const uint16_t RequiredMaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
  const cs_x86_op *MaskOperand = nullptr;
  NdVar CompactMask;
  uint8_t EncodedMask = 0;
  bool ZeroMask = false;
  if (HasMask) {
    MaskOperand = &X86.operands[1];
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(MaskOperand->reg));
    if (MaskOperand->reg == X86_REG_K0 ||
        MaskOperand->size != RequiredMaskSize ||
        MaskInfo.Size < RequiredMaskSize ||
        (Destination.type == X86_OP_MEM && MaskOperand->avx_zero_opmask))
      return false;
    EncodedMask = static_cast<uint8_t>(MaskOperand->reg - X86_REG_K0);
    ZeroMask = MaskOperand->avx_zero_opmask;
    CompactMask = NdVar::reg(MaskInfo.Offset, RequiredMaskSize);
  } else {
    CompactMask = NdVar::cst((UINT64_C(1) << LaneCount) - 1, RequiredMaskSize);
  }
  const uint8_t EncodedLength =
      SAE ? 0 : (WideSize == 16 ? 0 : (WideSize == 32 ? 0x20 : 0x40));
  const uint8_t ExpectedP2 =
      static_cast<uint8_t>(EncodedLength | 0x08 | (SAE ? 0x10 : 0) |
                           EncodedMask | (ZeroMask ? 0x80 : 0));
  if (Encoding.P2 != ExpectedP2 ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(RawRegOperand))
    return false;
  for (unsigned Index = 0; Index < X86.op_count; ++Index)
    if (X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID ||
        (X86.operands[Index].avx_zero_opmask &&
         (!MaskOperand || &X86.operands[Index] != MaskOperand)))
      return false;

  const size_t TrailingBytes = HalfToSingle ? 0 : 1;
  if (RawRMOperand.type == X86_OP_MEM) {
    if (!validateCanonicalEvexMemoryTail(Insn, X86, Encoding, RawRMOperand,
                                         NarrowSize, TrailingBytes))
      return false;
  } else if (decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                 vectorRegisterIndex(RawRMOperand) ||
             !validateCanonicalEvexRegisterTail(Insn, X86, Encoding,
                                                TrailingBytes)) {
    return false;
  }

  uint8_t Immediate = 0;
  if (HalfToSingle) {
    if (X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0)
      return false;
  } else {
    const cs_x86_op &ImmediateOperand = X86.operands[ImmediateIndex];
    if (ImmediateOperand.type != X86_OP_IMM || ImmediateOperand.size != 1 ||
        X86.encoding.imm_size != 1 ||
        X86.encoding.imm_offset + 1 != Insn->size ||
        Insn->bytes[X86.encoding.imm_offset] !=
            static_cast<uint8_t>(ImmediateOperand.imm))
      return false;
    Immediate = static_cast<uint8_t>(ImmediateOperand.imm);
  }

  const auto ConvertKind =
      HalfToSingle ? (SAE ? F16CConvertKind::HalfToSingleSuppressExceptions
                          : F16CConvertKind::HalfToSingle)
                   : (SAE ? F16CConvertKind::SingleToHalfSuppressExceptions
                          : F16CConvertKind::SingleToHalf);
  if (HalfToSingle) {
    NdVar SourceValue;
    if (Source.type == X86_OP_MEM && MaskOperand) {
      const uint16_t SourceStorageSize = std::max<uint16_t>(16, NarrowSize);
      const uint64_t RelevantBits = (UINT64_C(1) << LaneCount) - 1;
      NdVar RelevantMask = S.makeTemp(RequiredMaskSize);
      S.emit(NdOp::INT_AND, RelevantMask,
             {CompactMask, NdVar::cst(RelevantBits, RequiredMaskSize)});
      const NdVar ExpandedMask =
          expandCompactLaneMask(S, RelevantMask, SourceStorageSize, 2);
      if (ExpandedMask.Size != SourceStorageSize)
        return false;
      const NdVar Address = S.computeEA(Source);
      SourceValue = S.makeTemp(SourceStorageSize);
      S.emitIntrinsic(Intrinsic::MaskedLoadW, SourceValue,
                      {Address, ExpandedMask}, NdMemoryOrdering::None,
                      X86Lifter::LiftState::memoryAddressSpace(Source));
    } else {
      SourceValue = L.operandRead(S, Source);
      if (MaskOperand)
        SourceValue =
            maskF16CSourceLanes(S, SourceValue, CompactMask, LaneCount, 2);
    }
    if (SourceValue.Size < NarrowSize || SourceValue.Size > 32)
      return false;
    NdVar Converted = S.makeTemp(WideSize);
    S.emitIntrinsic(Intrinsic::F16CConvert, Converted,
                    {NdVar::cst(static_cast<uint8_t>(ConvertKind), 1),
                     SourceValue, NdVar::cst(0, 1)});
    if (MaskOperand)
      return emitMaskedVectorResult(L, S, Destination, *MaskOperand, Converted,
                                    4);
    S.emit(NdOp::COPY, L.operandWrite(Destination), {Converted});
    return true;
  }

  NdVar SourceValue = L.operandRead(S, Source);
  if (SourceValue.Size != WideSize)
    return false;
  if (MaskOperand)
    SourceValue =
        maskF16CSourceLanes(S, SourceValue, CompactMask, LaneCount, 4);
  if (SourceValue.Size != WideSize)
    return false;
  NdVar Converted = S.makeTemp(NarrowRegisterSize);
  S.emitIntrinsic(Intrinsic::F16CConvert, Converted,
                  {NdVar::cst(static_cast<uint8_t>(ConvertKind), 1),
                   SourceValue, NdVar::cst(Immediate, 1)});
  if (Destination.type == X86_OP_MEM) {
    if (!MaskOperand) {
      NdVar Stored = Converted;
      if (Stored.Size != NarrowSize) {
        Stored = S.makeTemp(NarrowSize);
        S.emit(NdOp::SUBBYTES, Stored, {Converted, NdVar::cst(0, 4)});
      }
      S.storeToMem(Destination, Stored);
      return true;
    }
    const uint64_t RelevantBits = (UINT64_C(1) << LaneCount) - 1;
    NdVar RelevantMask = S.makeTemp(RequiredMaskSize);
    S.emit(NdOp::INT_AND, RelevantMask,
           {CompactMask, NdVar::cst(RelevantBits, RequiredMaskSize)});
    const NdVar ExpandedMask =
        expandCompactLaneMask(S, RelevantMask, NarrowRegisterSize, 2);
    if (ExpandedMask.Size != NarrowRegisterSize)
      return false;
    const NdVar Address = S.computeEA(Destination);
    S.emitIntrinsic(Intrinsic::MaskedStoreW, {},
                    {Address, ExpandedMask, Converted}, NdMemoryOrdering::None,
                    X86Lifter::LiftState::memoryAddressSpace(Destination));
    return true;
  }
  if (MaskOperand)
    return emitMaskedF16CNarrowResult(L, S, Destination, *MaskOperand,
                                      Converted, LaneCount);
  S.emit(NdOp::COPY, L.operandWrite(Destination), {Converted});
  return true;
}

bool liftEvexScalarFloatConvert(X86Lifter &L, X86Lifter::LiftState &S,
                                const cs_insn *Insn, const cs_x86 &X86,
                                const ScalarFloatConvertSpec &Spec) {
  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != 0x01 ||
      ((Encoding.P1 | 0x04) & 0x87) !=
          static_cast<uint8_t>((Spec.W ? 0x80 : 0) | 0x04 |
                               Spec.MandatoryPrefix) ||
      Encoding.Opcode != 0x5a || X86.encoding.imm_offset != 0 ||
      X86.encoding.imm_size != 0 || (X86.op_count != 3 && X86.op_count != 4))
    return false;

  const bool HasMask = X86.op_count == 4;
  const unsigned MergeIndex = HasMask ? 2 : 1;
  const unsigned SourceIndex = HasMask ? 3 : 2;
  const cs_x86_op &DestinationOperand = X86.operands[0];
  const cs_x86_op &MergeOperand = X86.operands[MergeIndex];
  const cs_x86_op &SourceOperand = X86.operands[SourceIndex];
  const bool MemoryForm = SourceOperand.type == X86_OP_MEM;
  if (!isVectorRegisterOfSize(DestinationOperand, 16) ||
      !isVectorRegisterOfSize(MergeOperand, 16) ||
      (!MemoryForm && !isVectorRegisterOfSize(SourceOperand, 16)) ||
      (MemoryForm && SourceOperand.size != Spec.SourceElementSize) ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(DestinationOperand) ||
      decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
          vectorRegisterIndex(MergeOperand) ||
      (((Encoding.ModRM & 0xc0) != 0xc0) != MemoryForm))
    return false;

  const bool EncodedB = (Encoding.P2 & 0x10) != 0;
  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  const bool RegisterControl = !MemoryForm && EncodedB;
  const bool EmbeddedRounding =
      RegisterControl && Spec.SupportsEmbeddedRounding;
  const bool SAEOnly = RegisterControl && Spec.SupportsSAEOnly;
  if ((MemoryForm && EncodedB) ||
      (RegisterControl && !EmbeddedRounding && !SAEOnly) ||
      (!RegisterControl && EncodedLength != 0) ||
      (SAEOnly && EncodedLength != 0))
    return false;
  const x86_avx_rm ExpectedRounding =
      EmbeddedRounding
          ? static_cast<x86_avx_rm>(X86_AVX_RM_RN + (EncodedLength >> 5))
          : X86_AVX_RM_INVALID;
  if (X86.avx_sae != RegisterControl || X86.avx_rm != ExpectedRounding)
    return false;

  const uint8_t EncodedMask = Encoding.P2 & 7;
  const bool EncodedZero = (Encoding.P2 & 0x80) != 0;
  const cs_x86_op *MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  if (HasMask) {
    if (!isX86OpmaskOperand(*MaskOperand) || MaskOperand->reg == X86_REG_K0 ||
        MaskOperand->size != 1 ||
        EncodedMask != static_cast<uint8_t>(MaskOperand->reg - X86_REG_K0) ||
        EncodedZero != static_cast<bool>(MaskOperand->avx_zero_opmask))
      return false;
  } else if (EncodedMask != 0 || EncodedZero) {
    return false;
  }
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    if (Operand.avx_bcast != X86_AVX_BCAST_INVALID ||
        (Operand.avx_zero_opmask && (!MaskOperand || &Operand != MaskOperand)))
      return false;
  }

  if (MemoryForm) {
    if (!validateCanonicalEvexMemoryTail(Insn, X86, Encoding, SourceOperand,
                                         Spec.SourceElementSize))
      return false;
  } else if (decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                 vectorRegisterIndex(SourceOperand) ||
             !validateCanonicalEvexRegisterTail(Insn, X86, Encoding)) {
    return false;
  }

  NdVar ActiveMask = NdVar::cst(1, 1);
  if (MaskOperand) {
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(MaskOperand->reg));
    if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < 1)
      return false;
    ActiveMask = S.makeTemp(1);
    S.emit(NdOp::INT_AND, ActiveMask,
           {NdVar::reg(MaskInfo.Offset, 1), NdVar::cst(1, 1)});
  }

  NdVar Source;
  if (MemoryForm) {
    Source = emitEvexMaskedMemoryLoad(S, SourceOperand, ActiveMask, 16,
                                      Spec.SourceElementSize,
                                      Spec.SourceElementSize, false);
  } else {
    Source = L.operandRead(S, SourceOperand);
  }
  const NdVar Merge = L.operandRead(S, MergeOperand);
  const NdVar Destination = L.operandWrite(DestinationOperand);
  if (Source.Size != 16 || Merge.Size != 16 || Destination.Size != 16)
    return false;

  const X86FPRounding Rounding =
      EmbeddedRounding ? static_cast<X86FPRounding>(EncodedLength >> 5)
                       : X86FPRounding::MXCSR;
  const uint16_t Control = makeX86FPConvertControl(
      X86FPConvertKind::FloatToFloat, Spec.SourceElementSize == 8,
      Spec.DestinationElementSize == 8, false, RegisterControl, Rounding, 1);
  NdVar Converted = S.makeTemp(16);
  S.emitIntrinsic(Intrinsic::X86FPConvert, Converted,
                  {NdVar::cst(Control, 2), Source, ActiveMask});

  NdVar NewLow = S.makeTemp(Spec.DestinationElementSize);
  S.emit(NdOp::SUBBYTES, NewLow, {Converted, NdVar::cst(0, 4)});
  NdVar Low = NewLow;
  if (MaskOperand) {
    NdVar Inactive = NdVar::cst(0, Spec.DestinationElementSize);
    if (!MaskOperand->avx_zero_opmask) {
      const NdVar OldDestination = L.operandRead(S, DestinationOperand);
      Inactive = S.makeTemp(Spec.DestinationElementSize);
      S.emit(NdOp::SUBBYTES, Inactive, {OldDestination, NdVar::cst(0, 4)});
    }
    Low = S.makeTemp(Spec.DestinationElementSize);
    S.emit(NdOp::SELECT, Low, {ActiveMask, NewLow, Inactive});
  }
  NdVar Upper = S.makeTemp(16 - Spec.DestinationElementSize);
  S.emit(NdOp::SUBBYTES, Upper,
         {Merge, NdVar::cst(Spec.DestinationElementSize, 4)});
  S.emit(NdOp::CONCAT, Destination, {Upper, Low});
  return true;
}

bool liftEvexScalarFloatToInteger(X86Lifter &L, X86Lifter::LiftState &S,
                                  const cs_insn *Insn, const cs_x86 &X86,
                                  const ScalarFloatToIntegerSpec &Spec) {
  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x0f) != 0x01 || Encoding.Opcode != Spec.Opcode ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      X86.op_count != 2)
    return false;

  const cs_x86_op &DestinationOperand = X86.operands[0];
  const cs_x86_op &SourceOperand = X86.operands[1];
  const bool Destination64 = (Encoding.P1 & 0x80) != 0;
  const uint16_t DestinationSize = Destination64 ? 8 : 4;
  const bool MemoryForm = SourceOperand.type == X86_OP_MEM;
  if (((Encoding.P1 | 0x04) & 0x87) !=
          static_cast<uint8_t>((Destination64 ? 0x80 : 0) | 0x04 |
                               Spec.MandatoryPrefix) ||
      (Encoding.P1 & 0x78) != 0x78 || (Encoding.P2 & 0x08) == 0 ||
      DestinationOperand.type != X86_OP_REG ||
      DestinationOperand.size != DestinationSize ||
      (!MemoryForm && !isVectorRegisterOfSize(SourceOperand, 16)) ||
      (MemoryForm && SourceOperand.size != Spec.SourceElementSize) ||
      (((Encoding.ModRM & 0xc0) != 0xc0) != MemoryForm))
    return false;

  const bool EncodedB = (Encoding.P2 & 0x10) != 0;
  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  const bool RegisterControl = !MemoryForm && EncodedB;
  const bool EmbeddedRounding = RegisterControl && !Spec.Truncate;
  const bool SAEOnly = RegisterControl && Spec.Truncate;
  if ((MemoryForm && EncodedB) || (!RegisterControl && EncodedLength != 0) ||
      (SAEOnly && EncodedLength != 0) || (Encoding.P2 & 0x87) != 0)
    return false;
  const x86_avx_rm ExpectedRounding =
      EmbeddedRounding
          ? static_cast<x86_avx_rm>(X86_AVX_RM_RN + (EncodedLength >> 5))
          : X86_AVX_RM_INVALID;
  if (X86.avx_sae != RegisterControl || X86.avx_rm != ExpectedRounding)
    return false;

  for (unsigned Index = 0; Index < X86.op_count; ++Index)
    if (X86.operands[Index].avx_zero_opmask ||
        X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID)
      return false;

  const unsigned EncodedDestination =
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM);
  const RegInfo DestinationInfo =
      mapCapstoneReg(static_cast<x86_reg>(DestinationOperand.reg));
  const uint64_t ExpectedDestinationOffset =
      EncodedDestination < 16
          ? static_cast<uint64_t>(EncodedDestination) * 8
          : x86reg::extendedGeneralReg(EncodedDestination - 16);
  if (EncodedDestination >= 32 ||
      !x86reg::isGeneralRegOffset(DestinationInfo.Offset) ||
      DestinationInfo.Offset != ExpectedDestinationOffset ||
      DestinationInfo.Size != DestinationSize)
    return false;

  if (MemoryForm) {
    if (!validateCanonicalEvexMemoryTail(Insn, X86, Encoding, SourceOperand,
                                         Spec.SourceElementSize))
      return false;
  } else if (decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                 vectorRegisterIndex(SourceOperand) ||
             !validateCanonicalEvexRegisterTail(Insn, X86, Encoding)) {
    return false;
  }

  NdVar Source = L.operandRead(S, SourceOperand);
  if (Source.Size == 0 || Source.Size > 16)
    return false;
  if (Source.Size != 16) {
    NdVar Padded = S.makeTemp(16);
    S.emit(NdOp::INT_ZEXT, Padded, {Source});
    Source = Padded;
  }

  const X86FPRounding Rounding =
      Spec.Truncate      ? X86FPRounding::TowardZero
      : EmbeddedRounding ? static_cast<X86FPRounding>(EncodedLength >> 5)
                         : X86FPRounding::MXCSR;
  const X86FPConvertKind Kind = Spec.Unsigned
                                    ? X86FPConvertKind::FloatToUnsignedInteger
                                    : X86FPConvertKind::FloatToSignedInteger;
  const uint16_t Control =
      makeX86FPConvertControl(Kind, Spec.SourceElementSize == 8, Destination64,
                              Spec.Truncate, RegisterControl, Rounding, 1);
  NdVar Converted = S.makeTemp(16);
  S.emitIntrinsic(Intrinsic::X86FPConvert, Converted,
                  {NdVar::cst(Control, 2), Source, NdVar::cst(1, 1)});
  NdVar Result = S.makeTemp(DestinationSize);
  S.emit(NdOp::SUBBYTES, Result, {Converted, NdVar::cst(0, 4)});
  S.emit(NdOp::COPY, L.operandWrite(DestinationOperand), {Result});
  return true;
}

bool liftEvexPackedConvert(X86Lifter &L, X86Lifter::LiftState &S,
                           const cs_insn *Insn, const cs_x86 &X86,
                           const PackedConvertSpec &Spec) {
  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != 0x01 || Encoding.Opcode != Spec.Opcode ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      (X86.op_count != 2 && X86.op_count != 3))
    return false;

  const bool HasMask = X86.op_count == 3;
  const unsigned SourceIndex = HasMask ? 2 : 1;
  const cs_x86_op &DestinationOperand = X86.operands[0];
  const cs_x86_op &SourceOperand = X86.operands[SourceIndex];
  const bool MemoryForm = SourceOperand.type == X86_OP_MEM;
  if (DestinationOperand.type != X86_OP_REG ||
      (!MemoryForm && SourceOperand.type != X86_OP_REG))
    return false;

  const uint8_t RawVectorLength = Encoding.P2 & 0x60;
  if (MemoryForm && RawVectorLength == 0x60)
    return false;
  const uint16_t EncodedWideSize =
      RawVectorLength == 0 ? 16 : (RawVectorLength == 0x20 ? 32 : 64);
  const uint16_t WideSize =
      MemoryForm ? EncodedWideSize
                 : (Spec.SourceElementSize > Spec.DestinationElementSize
                        ? SourceOperand.size
                        : DestinationOperand.size);
  if (WideSize != 16 && WideSize != 32 && WideSize != 64)
    return false;
  const uint16_t WideElementSize =
      std::max(Spec.SourceElementSize, Spec.DestinationElementSize);
  const unsigned LaneCount = WideSize / WideElementSize;
  const uint16_t ExpectedSourceSize = static_cast<uint16_t>(
      std::max<unsigned>(16, LaneCount * Spec.SourceElementSize));
  const uint16_t ExpectedDestinationSize = static_cast<uint16_t>(
      std::max<unsigned>(16, LaneCount * Spec.DestinationElementSize));
  if (!isVectorRegisterOfSize(DestinationOperand, ExpectedDestinationSize) ||
      (!MemoryForm &&
       !isVectorRegisterOfSize(SourceOperand, ExpectedSourceSize)))
    return false;

  const bool EncodedB = (Encoding.P2 & 0x10) != 0;
  const bool RegisterControl = !MemoryForm && EncodedB;
  const bool SAEOnly =
      RegisterControl &&
      (Spec.Direction == PackedConvertDirection::FloatToIntegerTruncate ||
       Spec.SupportsSAEOnly);
  const bool EmbeddedRounding =
      RegisterControl && !SAEOnly && Spec.SupportsEmbeddedRounding;
  if ((RegisterControl && !SAEOnly && !EmbeddedRounding) ||
      (RegisterControl && WideSize != 64))
    return false;
  const uint8_t EmbeddedRoundingBits = Encoding.P2 & 0x60;
  const x86_avx_rm ExpectedRounding =
      EmbeddedRounding
          ? static_cast<x86_avx_rm>(X86_AVX_RM_RN + (EmbeddedRoundingBits >> 5))
          : X86_AVX_RM_INVALID;
  if (X86.avx_sae != RegisterControl || X86.avx_rm != ExpectedRounding)
    return false;

  const uint8_t ExpectedP1 =
      static_cast<uint8_t>(0x7c | Spec.MandatoryPrefix | (Spec.W ? 0x80 : 0));
  const uint8_t VectorLength =
      WideSize == 16 ? 0 : (WideSize == 32 ? 0x20 : 0x40);
  if ((Encoding.P1 | 0x04) != ExpectedP1 ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(DestinationOperand) ||
      (((Encoding.ModRM & 0xc0) != 0xc0) != MemoryForm))
    return false;

  const uint16_t RequiredMaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
  uint8_t EncodedMask = 0;
  bool ZeroMask = false;
  NdVar Mask;
  if (HasMask) {
    const cs_x86_op &MaskOperand = X86.operands[1];
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(MaskOperand.reg));
    if (!isX86OpmaskOperand(MaskOperand) || MaskOperand.reg == X86_REG_K0 ||
        MaskOperand.size != RequiredMaskSize ||
        MaskInfo.Size < RequiredMaskSize)
      return false;
    EncodedMask = static_cast<uint8_t>(MaskOperand.reg - X86_REG_K0);
    ZeroMask = MaskOperand.avx_zero_opmask;
    Mask = NdVar::reg(MaskInfo.Offset, RequiredMaskSize);
  } else {
    Mask =
        NdVar::cst((UINT64_C(1) << LaneCount) - UINT64_C(1), RequiredMaskSize);
  }
  const bool Broadcast = MemoryForm && EncodedB;
  const uint8_t EncodedLength =
      EmbeddedRounding ? EmbeddedRoundingBits : (SAEOnly ? 0 : VectorLength);
  const uint8_t ExpectedP2 =
      static_cast<uint8_t>(EncodedLength | 0x08 | (EncodedB ? 0x10 : 0) |
                           EncodedMask | (ZeroMask ? 0x80 : 0));
  if (Encoding.P2 != ExpectedP2)
    return false;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    if (X86.operands[Index].avx_zero_opmask && (!HasMask || Index != 1))
      return false;
    if (Index != SourceIndex &&
        X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID)
      return false;
  }

  if (MemoryForm) {
    const uint16_t FullTupleSize =
        static_cast<uint16_t>(LaneCount * Spec.SourceElementSize);
    const uint16_t TupleSize =
        Broadcast ? Spec.SourceElementSize : FullTupleSize;
    const x86_avx_bcast ExpectedBroadcast =
        Broadcast ? broadcastForLaneCount(LaneCount) : X86_AVX_BCAST_INVALID;
    if (SourceOperand.size != TupleSize ||
        SourceOperand.avx_bcast != ExpectedBroadcast ||
        !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, SourceOperand,
                                         TupleSize))
      return false;
  } else if (Broadcast || SourceOperand.avx_bcast != X86_AVX_BCAST_INVALID ||
             decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                 vectorRegisterIndex(SourceOperand) ||
             !validateCanonicalEvexRegisterTail(Insn, X86, Encoding)) {
    return false;
  }

  NdVar Source;
  if (MemoryForm) {
    NdVar MemoryMask = S.makeTemp(RequiredMaskSize);
    S.emit(NdOp::INT_AND, MemoryMask,
           {Mask, NdVar::cst((UINT64_C(1) << LaneCount) - UINT64_C(1),
                             RequiredMaskSize)});
    const uint16_t FullTupleSize =
        static_cast<uint16_t>(LaneCount * Spec.SourceElementSize);
    Source = emitEvexMaskedMemoryLoad(
        S, SourceOperand, MemoryMask, ExpectedSourceSize,
        Spec.SourceElementSize,
        Broadcast ? Spec.SourceElementSize : FullTupleSize, Broadcast);
    if (Source.Size != ExpectedSourceSize)
      return false;
  } else {
    Source = L.operandRead(S, SourceOperand);
    if (Source.Size != ExpectedSourceSize)
      return false;
  }
  const NdVar Destination = L.operandWrite(DestinationOperand);
  NdVar OldDestination;
  if (HasMask && !ZeroMask)
    OldDestination = L.operandRead(S, DestinationOperand);

  X86FPConvertKind ConvertKind;
  if (Spec.Direction == PackedConvertDirection::IntegerToFloat)
    ConvertKind = Spec.Unsigned ? X86FPConvertKind::UnsignedIntegerToFloat
                                : X86FPConvertKind::SignedIntegerToFloat;
  else if (Spec.Direction == PackedConvertDirection::FloatToFloat)
    ConvertKind = X86FPConvertKind::FloatToFloat;
  else
    ConvertKind = Spec.Unsigned ? X86FPConvertKind::FloatToUnsignedInteger
                                : X86FPConvertKind::FloatToSignedInteger;
  const bool Truncate =
      Spec.Direction == PackedConvertDirection::FloatToIntegerTruncate;
  const X86FPRounding Rounding =
      Truncate           ? X86FPRounding::TowardZero
      : EmbeddedRounding ? static_cast<X86FPRounding>(EmbeddedRoundingBits >> 5)
                         : X86FPRounding::MXCSR;
  const uint16_t Control =
      makeX86FPConvertControl(ConvertKind, Spec.SourceElementSize == 8,
                              Spec.DestinationElementSize == 8, Truncate,
                              RegisterControl, Rounding, LaneCount);
  NdVar ConvertedVector = S.makeTemp(ExpectedDestinationSize);
  S.emitIntrinsic(Intrinsic::X86FPConvert, ConvertedVector,
                  {NdVar::cst(Control, 2), Source, Mask});

  std::vector<NdVar> ResultLanes;
  ResultLanes.reserve(LaneCount);
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    NdVar Converted = S.makeTemp(Spec.DestinationElementSize);
    S.emit(NdOp::SUBBYTES, Converted,
           {ConvertedVector,
            NdVar::cst(
                static_cast<uint64_t>(Lane) * Spec.DestinationElementSize, 4)});

    NdVar MaskBit;
    if (HasMask) {
      NdVar Shifted = Mask;
      if (Lane != 0) {
        Shifted = S.makeTemp(RequiredMaskSize);
        S.emit(NdOp::INT_RIGHT, Shifted,
               {Mask, NdVar::cst(Lane, RequiredMaskSize)});
      }
      NdVar BitWide = S.makeTemp(RequiredMaskSize);
      S.emit(NdOp::INT_AND, BitWide,
             {Shifted, NdVar::cst(1, RequiredMaskSize)});
      MaskBit = BitWide;
      if (RequiredMaskSize != 1) {
        MaskBit = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, MaskBit, {BitWide, NdVar::cst(0, 4)});
      }
      NdVar Inactive = NdVar::cst(0, Spec.DestinationElementSize);
      if (!ZeroMask) {
        Inactive = S.makeTemp(Spec.DestinationElementSize);
        S.emit(NdOp::SUBBYTES, Inactive,
               {OldDestination, NdVar::cst(static_cast<uint64_t>(Lane) *
                                               Spec.DestinationElementSize,
                                           4)});
      }
      NdVar Selected = S.makeTemp(Spec.DestinationElementSize);
      S.emit(NdOp::SELECT, Selected, {MaskBit, Converted, Inactive});
      Converted = Selected;
    }
    ResultLanes.push_back(Converted);
  }

  NdVar Result = ResultLanes.front();
  for (unsigned Lane = 1; Lane < LaneCount; ++Lane) {
    NdVar Next = S.makeTemp(Result.Size + Spec.DestinationElementSize);
    S.emit(NdOp::CONCAT, Next, {ResultLanes[Lane], Result});
    Result = Next;
  }
  if (Result.Size < Destination.Size) {
    NdVar Padded = S.makeTemp(Destination.Size);
    S.emit(NdOp::CONCAT, Padded,
           {NdVar::cst(0, Destination.Size - Result.Size), Result});
    Result = Padded;
  }
  S.emit(NdOp::COPY, Destination, {Result});
  return true;
}

} // namespace

bool liftSIMDAVXConvert(X86Lifter &L, X86Lifter::LiftState &S,
                        const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;

  PackedConvertSpec PackedSpec;
  ScalarFloatConvertSpec ScalarFloatSpec;
  ScalarFloatToIntegerSpec ScalarIntegerSpec;
  if (hasPotentialEvexPrefix(Insn)) {
    if (getScalarFloatConvertSpec(InsnId, ScalarFloatSpec))
      return liftEvexScalarFloatConvert(L, S, Insn, X86, ScalarFloatSpec);
    if (getScalarFloatToIntegerSpec(InsnId, ScalarIntegerSpec))
      return liftEvexScalarFloatToInteger(L, S, Insn, X86, ScalarIntegerSpec);
    if (InsnId == X86_INS_VCVTPH2PS || InsnId == X86_INS_VCVTPS2PH)
      return liftEvexF16C(L, S, Insn, X86, InsnId);
    if (getPackedConvertSpec(InsnId, PackedSpec))
      return liftEvexPackedConvert(L, S, Insn, X86, PackedSpec);
    if (isScalarIntegerToFloat(InsnId))
      return liftEvexScalarIntegerToFloat(L, S, Insn, X86, InsnId);
  }
  switch (InsnId) {

  // ========================================================================
  // P1: AVX/AVX-512 other V* instructions — VCVT*, VRANGE*, VSCALEF*,
  //     VGETEXP*, VGETMANT*, VREDUCE*, VRNDSCALE*, VFIXUPIMM*, VFPCLASS*,
  //     VBROADCAST (EVEX), VINSERT/VEXTRACT (EVEX), VCOMPRESS/VEXPAND (float).
  // ========================================================================

  // VCVT — integer ↔ float conversions (AVX/AVX-512).
  // Packed int->FP `vcvtdq2ps/pd ymm/xmm` (128- or 256-bit): convert each i32
  // lane independently.  PS keeps lane count == dword count; PD widens the low
  // dwords to f64 (dst f64-lane count).  Lifting this as a single bulk
  // FLOAT_INT2FLOAT (the old scalar-shared path) treats the whole i128/i256 as
  // one integer and is wrong.
  case X86_INS_VCVTDQ2PS:
  case X86_INS_VCVTDQ2PD: {
    if (X86.op_count < 2)
      break;
    bool IsPD = (Insn->id == X86_INS_VCVTDQ2PD);
    unsigned FPSz = IsPD ? 8 : 4;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned NLanes = Dst.Size / FPSz;
    if (NLanes < 1) {
      S.emit(NdOp::FLOAT_INT2FLOAT, Dst, {Src});
      break;
    }
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Elem = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(I * 4, 4)});
      NdVar Lane = S.makeTemp(FPSz);
      S.emit(NdOp::FLOAT_INT2FLOAT, Lane, {Elem});
      if (I == 0) {
        Acc = Lane;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + FPSz);
        S.emit(NdOp::CONCAT, Next, {Lane, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // Scalar VEX int->FP `vcvtsi2ss/sd xmm1, xmm2, r/m`: the converted scalar
  // goes in the low element (float or double) and the upper bits come from
  // xmm2.  The result temp must be the real FP width, otherwise the emitter
  // infers the type from the wide destination and produces a double for a
  // single-precision convert.
  case X86_INS_VCVTSI2SS:
  case X86_INS_VCVTSI2SD:
  case X86_INS_VCVTUSI2SS:
  case X86_INS_VCVTUSI2SD: {
    if (X86.op_count < 2)
      break;
    bool ToDouble =
        (Insn->id == X86_INS_VCVTSI2SD || Insn->id == X86_INS_VCVTUSI2SD);
    bool Unsigned =
        (Insn->id == X86_INS_VCVTUSI2SS || Insn->id == X86_INS_VCVTUSI2SD);
    unsigned DstFPSz = ToDouble ? 8 : 4;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Upper = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1]) : Dst;
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    NdVar Tmp = S.makeTemp(DstFPSz);
    S.emit(Unsigned ? NdOp::FLOAT_UINT2FLOAT : NdOp::FLOAT_INT2FLOAT, Tmp,
           {Src});
    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], Tmp);
    } else if (Dst.Size > DstFPSz) {
      NdVar Hi = S.makeTemp(Dst.Size - DstFPSz);
      S.emit(NdOp::SUBBYTES, Hi, {Upper, NdVar::cst(DstFPSz, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Tmp});
    } else {
      S.emit(NdOp::COPY, Dst, {Tmp});
    }
    break;
  }
  // Packed int->FP (AVX-512 VL); kept as a bulk convert pending per-lane
  // support (these EVEX forms are not modeled by Unicorn for roundtrip).
  case X86_INS_VCVTUDQ2PS:
  case X86_INS_VCVTUDQ2PD:
  case X86_INS_VCVTQQ2PS:
  case X86_INS_VCVTQQ2PD:
  case X86_INS_VCVTUQQ2PS:
  case X86_INS_VCVTUQQ2PD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::FLOAT_INT2FLOAT, Dst, {Src});
    break;
  }

  case X86_INS_VCVTPS2DQ:
  case X86_INS_VCVTPD2DQ:
  case X86_INS_VCVTTPS2DQ:
  case X86_INS_VCVTTPD2DQ:
  case X86_INS_VCVTSS2SI:
  case X86_INS_VCVTSD2SI:
  case X86_INS_VCVTTSS2SI:
  case X86_INS_VCVTTSD2SI:
  case X86_INS_VCVTPS2UDQ:
  case X86_INS_VCVTPD2UDQ:
  case X86_INS_VCVTTPS2UDQ:
  case X86_INS_VCVTTPD2UDQ:
  case X86_INS_VCVTSS2USI:
  case X86_INS_VCVTSD2USI:
  case X86_INS_VCVTTSS2USI:
  case X86_INS_VCVTTSD2USI:
  case X86_INS_VCVTPS2QQ:
  case X86_INS_VCVTPD2QQ:
  case X86_INS_VCVTTPS2QQ:
  case X86_INS_VCVTTPD2QQ:
  case X86_INS_VCVTPS2UQQ:
  case X86_INS_VCVTPD2UQQ:
  case X86_INS_VCVTTPS2UQQ:
  case X86_INS_VCVTTPD2UQQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::FLOAT_TRUNC, Dst, {Src});
    break;
  }

  // Scalar VEX precision convert `vcvtXX2YY xmm1, xmm2, xmm3/m`: the converted
  // scalar goes in the low element and the upper bits come from xmm2.  Narrow
  // the convert source to its real FP width so the emitter does not mis-infer
  // the type from the full vector and pick the wrong direction.
  case X86_INS_VCVTSS2SD:
  case X86_INS_VCVTSD2SS: {
    if (X86.op_count < 2)
      break;
    bool ToDouble = (Insn->id == X86_INS_VCVTSS2SD);
    unsigned SrcFPSz = ToDouble ? 4 : 8;
    unsigned DstFPSz = ToDouble ? 8 : 4;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    NdVar Upper = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1]) : Dst;
    if (Src.Size > SrcFPSz) {
      NdVar N = S.makeTemp(SrcFPSz);
      S.emit(NdOp::SUBBYTES, N, {Src, NdVar::cst(0, 4)});
      Src = N;
    }
    NdVar Tmp = S.makeTemp(DstFPSz);
    S.emit(NdOp::FLOAT_FLOAT2FLOAT, Tmp, {Src});
    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], Tmp);
    } else if (Dst.Size > DstFPSz) {
      NdVar Hi = S.makeTemp(Dst.Size - DstFPSz);
      S.emit(NdOp::SUBBYTES, Hi, {Upper, NdVar::cst(DstFPSz, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Tmp});
    } else {
      S.emit(NdOp::COPY, Dst, {Tmp});
    }
    break;
  }
  // Packed widen single->double (per dst lane); reads the low single lanes.
  case X86_INS_VCVTPS2PD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned NLanes = Dst.Size / 8;
    if (NLanes < 1) {
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Dst, {Src});
      break;
    }
    NdVar Cur;
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar E = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, E, {Src, NdVar::cst(I * 4, 4)});
      NdVar L = S.makeTemp(8);
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, L, {E});
      if (I == 0) {
        Cur = L;
      } else {
        NdVar Next = S.makeTemp((I + 1) * 8);
        S.emit(NdOp::CONCAT, Next, {L, Cur});
        Cur = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Cur});
    break;
  }
  // Packed narrow double->single (per src lane); upper dst lanes are zeroed.
  case X86_INS_VCVTPD2PS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned NLanes = Src.Size / 8;
    if (NLanes < 1) {
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Dst, {Src});
      break;
    }
    NdVar Cur;
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar E = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, E, {Src, NdVar::cst(I * 8, 4)});
      NdVar L = S.makeTemp(4);
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, L, {E});
      if (I == 0) {
        Cur = L;
      } else {
        NdVar Next = S.makeTemp((I + 1) * 4);
        S.emit(NdOp::CONCAT, Next, {L, Cur});
        Cur = Next;
      }
    }
    if (Dst.Size > NLanes * 4) {
      NdVar ZHi = S.makeTemp(Dst.Size - NLanes * 4);
      S.emit(NdOp::COPY, ZHi,
             {NdVar::cst(0, (uint16_t)(Dst.Size - NLanes * 4))});
      S.emit(NdOp::CONCAT, Dst, {ZHi, Cur});
    } else {
      S.emit(NdOp::COPY, Dst, {Cur});
    }
    break;
  }
  // F16C has instruction-specific rounding and MXCSR exception semantics that
  // cannot be represented by the generic float-width conversion opcode.
  // Keep EVEX masked/SAE forms fail-closed until their merge/zero contracts are
  // carried explicitly; the VEX register destinations below are exact.
  case X86_INS_VCVTPH2PS: {
    if (Insn->size < 3 || Insn->bytes[0] != 0xc4 ||
        (Insn->bytes[2] & 0xf8) != 0x78 || X86.op_count != 2 ||
        X86.operands[0].type != X86_OP_REG ||
        (X86.operands[1].type != X86_OP_REG &&
         X86.operands[1].type != X86_OP_MEM))
      return false;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    const uint16_t RequiredSource = Dst.Size / 2;
    if ((Dst.Size != 16 && Dst.Size != 32) || Src.Size < RequiredSource ||
        Src.Size > 16)
      return false;
    S.emitIntrinsic(
        Intrinsic::F16CConvert, Dst,
        {NdVar::cst(static_cast<uint8_t>(F16CConvertKind::HalfToSingle), 1),
         Src, NdVar::cst(0, 1)});
    break;
  }
  case X86_INS_VCVTPS2PH: {
    if (Insn->size < 3 || Insn->bytes[0] != 0xc4 ||
        (Insn->bytes[2] & 0xf8) != 0x78 || X86.op_count != 3 ||
        X86.operands[0].type != X86_OP_REG ||
        X86.operands[1].type != X86_OP_REG ||
        X86.operands[2].type != X86_OP_IMM)
      return false;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Immediate = L.operandRead(S, X86.operands[2]);
    if (Dst.Size != 16 || (Src.Size != 16 && Src.Size != 32) ||
        Immediate.Size != 1 || !Immediate.isConst())
      return false;
    S.emitIntrinsic(
        Intrinsic::F16CConvert, Dst,
        {NdVar::cst(static_cast<uint8_t>(F16CConvertKind::SingleToHalf), 1),
         Src, Immediate});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
