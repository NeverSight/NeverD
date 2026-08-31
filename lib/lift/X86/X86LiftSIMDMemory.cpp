//===- X86LiftSIMDMemory.cpp - x86 EVEX masked memory helpers ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include <algorithm>

namespace neverd {

namespace {

bool isSegmentPrefix(uint8_t Prefix) {
  return Prefix == 0x26 || Prefix == 0x2e || Prefix == 0x36 || Prefix == 0x3e ||
         Prefix == 0x64 || Prefix == 0x65;
}

x86_reg effectiveSegment(uint8_t Prefix, bool Is64Bit) {
  if (Is64Bit && Prefix != 0x64 && Prefix != 0x65)
    return X86_REG_INVALID;
  switch (Prefix) {
  case 0x26:
    return X86_REG_ES;
  case 0x2e:
    return X86_REG_CS;
  case 0x36:
    return X86_REG_SS;
  case 0x3e:
    return X86_REG_DS;
  case 0x64:
    return X86_REG_FS;
  case 0x65:
    return X86_REG_GS;
  default:
    return X86_REG_INVALID;
  }
}

bool matchesAddressRegister(x86_reg Register, unsigned Index,
                            uint16_t AddressSize) {
  if (Index >= 32)
    return false;
  const RegInfo Info = mapCapstoneReg(Register);
  const uint64_t ExpectedOffset = Index < 16
                                      ? static_cast<uint64_t>(Index) * 8
                                      : x86reg::extendedGeneralReg(Index - 16);
  return Info.Offset == ExpectedOffset && Info.Size == AddressSize;
}

bool isNoSibIndex(x86_reg Register, uint16_t AddressSize) {
  return Register == X86_REG_INVALID ||
         (AddressSize == 4 && Register == X86_REG_EIZ) ||
         (AddressSize == 8 && Register == X86_REG_RIZ);
}

bool isValidEvexMemoryOperand(const cs_x86_op &Operand, uint16_t AddressSize,
                              uint16_t TupleSize) {
  if (Operand.type != X86_OP_MEM || Operand.size != TupleSize ||
      (AddressSize != 4 && AddressSize != 8))
    return false;

  auto IsAddressRegister = [&](x86_reg Reg) {
    if (Reg == X86_REG_INVALID)
      return true;
    if (Reg == X86_REG_RIP)
      return AddressSize == 8;
    if (Reg == X86_REG_EIP)
      return AddressSize == 4;
    const RegInfo Info = mapCapstoneReg(Reg);
    return x86reg::isGeneralRegOffset(Info.Offset) && Info.Size == AddressSize;
  };
  if (!IsAddressRegister(static_cast<x86_reg>(Operand.mem.base)) ||
      !IsAddressRegister(static_cast<x86_reg>(Operand.mem.index)) ||
      (Operand.mem.index != X86_REG_INVALID &&
       (Operand.mem.base == X86_REG_RIP || Operand.mem.base == X86_REG_EIP)))
    return false;
  return Operand.mem.scale == 1 || Operand.mem.scale == 2 ||
         Operand.mem.scale == 4 || Operand.mem.scale == 8;
}

bool validateCanonicalVectorMemoryTail(
    const cs_insn *Insn, const cs_x86 &X86, size_t TailOffset, uint8_t ModRM,
    uint8_t SegmentPrefix, bool Is64Bit, uint16_t AddressSize,
    unsigned BaseExtension, unsigned IndexExtension, uint16_t Disp8Scale,
    const cs_x86_op &Operand, size_t TrailingBytes) {
  if (!Insn || (ModRM & 0xc0) == 0xc0 || Operand.type != X86_OP_MEM ||
      Disp8Scale == 0 ||
      Operand.mem.segment != effectiveSegment(SegmentPrefix, Is64Bit))
    return false;

  const unsigned Mod = ModRM >> 6;
  const unsigned RM = ModRM & 7;
  size_t Cursor = TailOffset;
  bool HasSIB = false;
  uint8_t SIB = 0;
  uint8_t DisplacementSize = 0;
  int ExpectedBase = -1;
  int ExpectedIndex = -1;
  x86_reg ExpectedSpecialBase = X86_REG_INVALID;
  int ExpectedScale = 1;

  if (!Is64Bit && (BaseExtension != 0 || IndexExtension != 0))
    return false;

  if (AddressSize == 2) {
    static constexpr int BaseByRM[8] = {3, 3, 5, 5, 6, 7, 5, 3};
    static constexpr int IndexByRM[8] = {6, 7, 6, 7, -1, -1, -1, -1};
    ExpectedBase = BaseByRM[RM];
    ExpectedIndex = IndexByRM[RM];
    if (Mod == 0 && RM == 6) {
      ExpectedBase = -1;
      DisplacementSize = 2;
    }
  } else if (RM == 4) {
    if (Cursor >= Insn->size)
      return false;
    HasSIB = true;
    SIB = Insn->bytes[Cursor++];
    ExpectedScale = 1 << (SIB >> 6);
    const unsigned IndexLow = (SIB >> 3) & 7;
    const unsigned BaseLow = SIB & 7;
    if (IndexLow != 4 || IndexExtension != 0)
      ExpectedIndex = static_cast<int>(IndexLow + IndexExtension);
    if (Mod == 0 && BaseLow == 5)
      DisplacementSize = 4;
    else
      ExpectedBase = static_cast<int>(BaseLow + BaseExtension);
  } else if (Mod == 0 && RM == 5) {
    if (Is64Bit)
      ExpectedSpecialBase = AddressSize == 4 ? X86_REG_EIP : X86_REG_RIP;
    DisplacementSize = 4;
  } else {
    ExpectedBase = static_cast<int>(RM + BaseExtension);
  }
  if (Mod == 1)
    DisplacementSize = 1;
  else if (Mod == 2)
    DisplacementSize = AddressSize == 2 ? 2 : 4;

  const size_t DisplacementOffset = Cursor;
  int64_t RawDisplacement = 0;
  if (DisplacementSize == 1) {
    if (Cursor >= Insn->size)
      return false;
    RawDisplacement = static_cast<int8_t>(Insn->bytes[Cursor++]);
  } else if (DisplacementSize == 2) {
    if (Insn->size - Cursor < 2)
      return false;
    const uint16_t Raw = static_cast<uint16_t>(Insn->bytes[Cursor]) |
                         (static_cast<uint16_t>(Insn->bytes[Cursor + 1]) << 8);
    RawDisplacement = static_cast<int16_t>(Raw);
    Cursor += 2;
  } else if (DisplacementSize == 4) {
    if (Insn->size - Cursor < 4)
      return false;
    uint32_t Raw = 0;
    for (unsigned Index = 0; Index < 4; ++Index)
      Raw |= static_cast<uint32_t>(Insn->bytes[Cursor + Index]) << (Index * 8);
    RawDisplacement = static_cast<int32_t>(Raw);
    Cursor += 4;
  }
  const int64_t Displacement =
      DisplacementSize == 1 ? RawDisplacement * Disp8Scale : RawDisplacement;
  if (Cursor + TrailingBytes != Insn->size ||
      X86.encoding.disp_size != DisplacementSize ||
      X86.encoding.disp_offset !=
          (DisplacementSize == 0 ? 0 : DisplacementOffset) ||
      X86.disp != Displacement || Operand.mem.disp != Displacement ||
      Operand.mem.scale != ExpectedScale || X86.sib != (HasSIB ? SIB : 0))
    return false;

  if (HasSIB) {
    if (X86.sib_base != Operand.mem.base ||
        X86.sib_index != Operand.mem.index || X86.sib_scale != ExpectedScale)
      return false;
  } else if (X86.sib_base != X86_REG_INVALID ||
             X86.sib_index != X86_REG_INVALID || X86.sib_scale != 0) {
    return false;
  }

  if (ExpectedSpecialBase != X86_REG_INVALID) {
    if (Operand.mem.base != ExpectedSpecialBase)
      return false;
  } else if (ExpectedBase < 0) {
    if (Operand.mem.base != X86_REG_INVALID)
      return false;
  } else if (!matchesAddressRegister(static_cast<x86_reg>(Operand.mem.base),
                                     static_cast<unsigned>(ExpectedBase),
                                     AddressSize)) {
    return false;
  }
  if (ExpectedIndex < 0)
    return isNoSibIndex(static_cast<x86_reg>(Operand.mem.index), AddressSize);
  return matchesAddressRegister(static_cast<x86_reg>(Operand.mem.index),
                                static_cast<unsigned>(ExpectedIndex),
                                AddressSize);
}

bool validateCanonicalVectorRegisterTail(const cs_insn *Insn, const cs_x86 &X86,
                                         uint8_t ModRM, size_t TailOffset,
                                         size_t TrailingBytes) {
  return Insn && (ModRM & 0xc0) == 0xc0 &&
         Insn->size == TailOffset + TrailingBytes &&
         X86.encoding.disp_offset == 0 && X86.encoding.disp_size == 0 &&
         X86.disp == 0 && X86.sib == 0 && X86.sib_base == X86_REG_INVALID &&
         X86.sib_index == X86_REG_INVALID && X86.sib_scale == 0;
}

NdVar repeatLowElement(X86Lifter::LiftState &S, NdVar Source,
                       uint16_t ElementSize, uint16_t ResultSize) {
  NdVar Result = S.makeTemp(ElementSize);
  S.emit(NdOp::SUBBYTES, Result, {Source, NdVar::cst(0, 4)});
  while (Result.Size < ResultSize) {
    const uint16_t NextSize = std::min<uint16_t>(ResultSize, Result.Size * 2);
    if (NextSize != Result.Size * 2)
      return {};
    NdVar Next = S.makeTemp(NextSize);
    S.emit(NdOp::CONCAT, Next, {Result, Result});
    Result = Next;
  }
  return Result;
}

} // namespace

bool parseCanonicalEvexEncodingInfo(const cs_insn *Insn, const cs_x86 &X86,
                                    Arch TargetArch,
                                    CanonicalEvexEncodingInfo &Encoding) {
  Encoding = {};
  if (!Insn || Insn->size == 0 || Insn->size > 15 ||
      (TargetArch != Arch::X64 && TargetArch != Arch::X86))
    return false;
  Encoding.Is64Bit = TargetArch == Arch::X64;
  while (Encoding.Offset < Insn->size && Insn->bytes[Encoding.Offset] != 0x62) {
    const uint8_t Prefix = Insn->bytes[Encoding.Offset++];
    if (isSegmentPrefix(Prefix)) {
      if (Encoding.SegmentPrefix != 0)
        return false;
      Encoding.SegmentPrefix = Prefix;
    } else if (Prefix == 0x67) {
      if (Encoding.AddressOverride)
        return false;
      Encoding.AddressOverride = true;
    } else {
      return false;
    }
  }
  if (Encoding.Offset + 6 > Insn->size || Insn->bytes[Encoding.Offset] != 0x62)
    return false;
  Encoding.P0 = Insn->bytes[Encoding.Offset + 1];
  Encoding.P1 = Insn->bytes[Encoding.Offset + 2];
  Encoding.P2 = Insn->bytes[Encoding.Offset + 3];
  Encoding.Opcode = Insn->bytes[Encoding.Offset + 4];
  Encoding.ModRM = Insn->bytes[Encoding.Offset + 5];
  if (!Encoding.Is64Bit &&
      ((Encoding.P0 & 0x08) != 0 || (Encoding.P1 & 0x04) == 0))
    return false;
  Encoding.AddressSize = Encoding.Is64Bit ? (Encoding.AddressOverride ? 4 : 8)
                                          : (Encoding.AddressOverride ? 2 : 4);
  if (X86.prefix[0] != 0 || X86.prefix[1] != Encoding.SegmentPrefix ||
      X86.prefix[2] != 0 ||
      X86.prefix[3] != (Encoding.AddressOverride ? 0x67 : 0) ||
      X86.addr_size != Encoding.AddressSize ||
      X86.encoding.modrm_offset != Encoding.Offset + 5 ||
      X86.modrm != Encoding.ModRM)
    return false;
  for (unsigned Index = 0; Index < 4; ++Index)
    if (X86.opcode[Index] != Insn->bytes[Encoding.Offset + Index])
      return false;
  return true;
}

bool parseCanonicalVex3EncodingInfo(const cs_insn *Insn, const cs_x86 &X86,
                                    Arch TargetArch,
                                    CanonicalVex3EncodingInfo &Encoding) {
  Encoding = {};
  if (!Insn || Insn->size == 0 || Insn->size > 15 ||
      (TargetArch != Arch::X64 && TargetArch != Arch::X86))
    return false;
  Encoding.Is64Bit = TargetArch == Arch::X64;
  while (Encoding.Offset < Insn->size && Insn->bytes[Encoding.Offset] != 0xc4) {
    const uint8_t Prefix = Insn->bytes[Encoding.Offset++];
    if (isSegmentPrefix(Prefix)) {
      if (Encoding.SegmentPrefix != 0)
        return false;
      Encoding.SegmentPrefix = Prefix;
    } else if (Prefix == 0x67) {
      if (Encoding.AddressOverride)
        return false;
      Encoding.AddressOverride = true;
    } else {
      return false;
    }
  }
  if (Encoding.Offset + 5 > Insn->size || Insn->bytes[Encoding.Offset] != 0xc4)
    return false;
  Encoding.P0 = Insn->bytes[Encoding.Offset + 1];
  Encoding.P1 = Insn->bytes[Encoding.Offset + 2];
  Encoding.Opcode = Insn->bytes[Encoding.Offset + 3];
  Encoding.ModRM = Insn->bytes[Encoding.Offset + 4];
  if (!Encoding.Is64Bit && (Encoding.P0 & 0xe0) != 0xe0)
    return false;
  Encoding.AddressSize = Encoding.Is64Bit ? (Encoding.AddressOverride ? 4 : 8)
                                          : (Encoding.AddressOverride ? 2 : 4);
  const uint8_t ExpectedRex =
      static_cast<uint8_t>(0x40 | ((Encoding.P1 & 0x80) != 0 ? 0x08 : 0) |
                           ((Encoding.P0 & 0x80) == 0 ? 0x04 : 0) |
                           ((Encoding.P0 & 0x40) == 0 ? 0x02 : 0) |
                           ((Encoding.P0 & 0x20) == 0 ? 0x01 : 0));
  if (X86.prefix[0] != 0 || X86.prefix[1] != Encoding.SegmentPrefix ||
      X86.prefix[2] != 0 ||
      X86.prefix[3] != (Encoding.AddressOverride ? 0x67 : 0) ||
      X86.rex != ExpectedRex || X86.addr_size != Encoding.AddressSize ||
      X86.encoding.modrm_offset != Encoding.Offset + 4 ||
      X86.modrm != Encoding.ModRM || X86.opcode[0] != 0xc4 ||
      X86.opcode[1] != Encoding.P0 || X86.opcode[2] != Encoding.P1 ||
      X86.opcode[3] != 0)
    return false;
  return true;
}

bool validateCanonicalEvexMemoryTail(const cs_insn *Insn, const cs_x86 &X86,
                                     const CanonicalEvexEncodingInfo &Encoding,
                                     const cs_x86_op &Operand,
                                     uint16_t TupleScale,
                                     size_t TrailingBytes) {
  const unsigned BaseExtension = ((Encoding.P0 & 0x20) == 0 ? 8 : 0) |
                                 ((Encoding.P0 & 0x08) != 0 ? 16 : 0);
  const unsigned IndexExtension = ((Encoding.P0 & 0x40) == 0 ? 8 : 0) |
                                  ((Encoding.P1 & 0x04) == 0 ? 16 : 0);
  return validateCanonicalVectorMemoryTail(
      Insn, X86, Encoding.Offset + 6, Encoding.ModRM, Encoding.SegmentPrefix,
      Encoding.Is64Bit, Encoding.AddressSize, BaseExtension, IndexExtension,
      TupleScale, Operand, TrailingBytes);
}

bool validateCanonicalEvexRegisterTail(
    const cs_insn *Insn, const cs_x86 &X86,
    const CanonicalEvexEncodingInfo &Encoding, size_t TrailingBytes) {
  return validateCanonicalVectorRegisterTail(
      Insn, X86, Encoding.ModRM, Encoding.Offset + 6, TrailingBytes);
}

bool validateCanonicalVex3MemoryTail(const cs_insn *Insn, const cs_x86 &X86,
                                     const CanonicalVex3EncodingInfo &Encoding,
                                     const cs_x86_op &Operand,
                                     size_t TrailingBytes) {
  const unsigned BaseExtension = (Encoding.P0 & 0x20) == 0 ? 8 : 0;
  const unsigned IndexExtension = (Encoding.P0 & 0x40) == 0 ? 8 : 0;
  return validateCanonicalVectorMemoryTail(
      Insn, X86, Encoding.Offset + 5, Encoding.ModRM, Encoding.SegmentPrefix,
      Encoding.Is64Bit, Encoding.AddressSize, BaseExtension, IndexExtension, 1,
      Operand, TrailingBytes);
}

bool validateCanonicalVex3RegisterTail(
    const cs_insn *Insn, const cs_x86 &X86,
    const CanonicalVex3EncodingInfo &Encoding, size_t TrailingBytes) {
  return validateCanonicalVectorRegisterTail(
      Insn, X86, Encoding.ModRM, Encoding.Offset + 5, TrailingBytes);
}

NdVar emitEvexMaskedMemoryLoad(X86Lifter::LiftState &S,
                               const cs_x86_op &MemoryOperand,
                               NdVar CompactMask, uint16_t ResultSize,
                               uint16_t ElementSize, uint16_t MemoryTupleSize,
                               bool Broadcast) {
  if ((ResultSize != 16 && ResultSize != 32 && ResultSize != 64) ||
      ElementSize == 0 || ElementSize > 8 || ResultSize % ElementSize != 0 ||
      MemoryTupleSize < ElementSize || MemoryTupleSize > ResultSize ||
      MemoryTupleSize % ElementSize != 0 ||
      (Broadcast && MemoryTupleSize != ElementSize) || CompactMask.Size == 0 ||
      CompactMask.Size > 8 || ResultSize / ElementSize > CompactMask.Size * 8 ||
      !isValidEvexMemoryOperand(MemoryOperand, S.AddressSize, MemoryTupleSize))
    return {};

  const Intrinsic LoadId = maskedVectorLoadIntrinsic(ElementSize);
  if (LoadId == Intrinsic::None)
    return {};
  const NdVar Address = S.computeEA(MemoryOperand);
  const NdMemoryAddressSpace AddressSpace =
      X86Lifter::LiftState::memoryAddressSpace(MemoryOperand);

  if (!Broadcast) {
    const NdVar ExpandedMask =
        expandCompactLaneMask(S, CompactMask, ResultSize, ElementSize);
    if (ExpandedMask.Size != ResultSize)
      return {};
    NdVar Result = S.makeTemp(ResultSize);
    S.emitIntrinsic(LoadId, Result, {Address, ExpandedMask},
                    NdMemoryOrdering::None, AddressSpace);
    return Result;
  }

  const unsigned LaneCount = ResultSize / ElementSize;
  const uint64_t RelevantBits =
      LaneCount == 64 ? UINT64_MAX : ((UINT64_C(1) << LaneCount) - 1);
  NdVar RelevantMask = S.makeTemp(CompactMask.Size);
  S.emit(NdOp::INT_AND, RelevantMask,
         {CompactMask, NdVar::cst(RelevantBits, CompactMask.Size)});
  NdVar LoadGuard = S.makeTemp(1);
  S.emit(NdOp::INT_NOTEQUAL, LoadGuard,
         {RelevantMask, NdVar::cst(0, CompactMask.Size)});

  const uint16_t GuardSize =
      static_cast<uint16_t>(std::max(1u, (16u / ElementSize + 7u) / 8u));
  NdVar WideGuard = LoadGuard;
  if (GuardSize != 1) {
    WideGuard = S.makeTemp(GuardSize);
    S.emit(NdOp::INT_ZEXT, WideGuard, {LoadGuard});
  }
  const NdVar ExpandedLoadGuard =
      expandCompactLaneMask(S, WideGuard, 16, ElementSize);
  if (ExpandedLoadGuard.Size != 16)
    return {};
  NdVar LoadedScalar = S.makeTemp(16);
  S.emitIntrinsic(LoadId, LoadedScalar, {Address, ExpandedLoadGuard},
                  NdMemoryOrdering::None, AddressSpace);
  return repeatLowElement(S, LoadedScalar, ElementSize, ResultSize);
}

} // namespace neverd
