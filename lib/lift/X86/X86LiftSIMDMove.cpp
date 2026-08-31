//===- X86LiftSIMDMove.cpp - x86/x64 SIMD data movement lifter ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSE/AVX vector data movement: aligned and unaligned register
/// and memory moves, lane extract/insert, byte-align, high/low
/// quadword moves, scalar and 128-bit broadcast, 128-bit lane
/// insert/extract, byte-mask extraction, and the vector-state
/// ops (VZEROUPPER/VZEROALL, EMMS/FEMMS).
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include <algorithm>
#include <optional>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

bool hasEvexEncoding(const cs_insn *Insn) {
  if (!Insn)
    return false;
  size_t Offset = 0;
  while (Offset < Insn->size &&
         (Insn->bytes[Offset] == 0x64 || Insn->bytes[Offset] == 0x65 ||
          Insn->bytes[Offset] == 0x67))
    ++Offset;
  return Offset < Insn->size && Insn->size - Offset >= 6 &&
         Insn->bytes[Offset] == 0x62;
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

bool isValidMaskedMoveMemoryOperand(const cs_x86_op &Operand,
                                    uint16_t AddressSize, uint16_t VectorSize) {
  if (Operand.type != X86_OP_MEM || Operand.size != VectorSize ||
      (AddressSize != 4 && AddressSize != 8))
    return false;

  auto IsAddressRegister = [&](x86_reg Reg) {
    if (Reg == X86_REG_INVALID)
      return true;
    if (Reg == X86_REG_RIP)
      return AddressSize == 8;
    if (Reg == X86_REG_EIP)
      return AddressSize == 4;
    const RegInfo RI = mapCapstoneReg(Reg);
    return x86reg::isGeneralRegOffset(RI.Offset) && RI.Size == AddressSize;
  };
  if (!IsAddressRegister(static_cast<x86_reg>(Operand.mem.base)) ||
      !IsAddressRegister(static_cast<x86_reg>(Operand.mem.index)))
    return false;
  if (Operand.mem.index != X86_REG_INVALID &&
      (Operand.mem.base == X86_REG_RIP || Operand.mem.base == X86_REG_EIP))
    return false;
  return Operand.mem.scale == 1 || Operand.mem.scale == 2 ||
         Operand.mem.scale == 4 || Operand.mem.scale == 8;
}

std::optional<NdVar> emitAMXStride(X86Lifter::LiftState &S,
                                   const cs_x86_op &Memory) {
  if (Memory.type != X86_OP_MEM)
    return std::nullopt;
  if (Memory.mem.index == X86_REG_INVALID)
    return NdVar::scalar(0, 8);
  if (Memory.mem.scale != 1 && Memory.mem.scale != 2 &&
      Memory.mem.scale != 4 && Memory.mem.scale != 8)
    return std::nullopt;

  const RegInfo Index =
      mapCapstoneReg(static_cast<x86_reg>(Memory.mem.index));
  if (!x86reg::isGeneralRegOffset(Index.Offset) ||
      (Index.Size != 4 && Index.Size != 8) || Index.Size != S.AddressSize)
    return std::nullopt;

  NdVar Value = NdVar::reg(Index.Offset, Index.Size);
  if (Value.Size == 4) {
    NdVar Extended = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, Extended, {Value});
    Value = Extended;
  }
  if (Memory.mem.scale != 1) {
    unsigned Shift = Memory.mem.scale == 2 ? 1 : Memory.mem.scale == 4 ? 2 : 3;
    NdVar Scaled = S.makeTemp(8);
    S.emit(NdOp::INT_LEFT, Scaled, {Value, NdVar::scalar(Shift, 8)});
    Value = Scaled;
  }
  return Value;
}

Intrinsic maskedStoreIntrinsic(uint16_t ElementSize) {
  switch (ElementSize) {
  case 1:
    return Intrinsic::MaskedStoreB;
  case 2:
    return Intrinsic::MaskedStoreW;
  case 4:
    return Intrinsic::MaskedStoreD;
  case 8:
    return Intrinsic::MaskedStoreQ;
  default:
    return Intrinsic::None;
  }
}

struct EvexFullVectorMoveSpec {
  uint8_t CanonicalP1;
  uint8_t LoadOpcode;
  uint8_t StoreOpcode;
  uint16_t ElementSize;
  bool RequiresAlignment;
};

std::optional<EvexFullVectorMoveSpec>
evexFullVectorMoveSpec(unsigned InsnId) {
  switch (InsnId) {
  case X86_INS_VMOVDQU8:
    return EvexFullVectorMoveSpec{0x7f, 0x6f, 0x7f, 1, false};
  case X86_INS_VMOVDQU16:
    return EvexFullVectorMoveSpec{0xff, 0x6f, 0x7f, 2, false};
  case X86_INS_VMOVDQU32:
    return EvexFullVectorMoveSpec{0x7e, 0x6f, 0x7f, 4, false};
  case X86_INS_VMOVDQU64:
    return EvexFullVectorMoveSpec{0xfe, 0x6f, 0x7f, 8, false};
  case X86_INS_VMOVDQA32:
    return EvexFullVectorMoveSpec{0x7d, 0x6f, 0x7f, 4, true};
  case X86_INS_VMOVDQA64:
    return EvexFullVectorMoveSpec{0xfd, 0x6f, 0x7f, 8, true};
  case X86_INS_VMOVAPS:
    return EvexFullVectorMoveSpec{0x7c, 0x28, 0x29, 4, true};
  case X86_INS_VMOVAPD:
    return EvexFullVectorMoveSpec{0xfd, 0x28, 0x29, 8, true};
  case X86_INS_VMOVUPS:
    return EvexFullVectorMoveSpec{0x7c, 0x10, 0x11, 4, false};
  case X86_INS_VMOVUPD:
    return EvexFullVectorMoveSpec{0xfd, 0x10, 0x11, 8, false};
  default:
    return std::nullopt;
  }
}

bool isEvexOnlyFullVectorMove(unsigned InsnId) {
  return InsnId == X86_INS_VMOVDQU8 || InsnId == X86_INS_VMOVDQU16 ||
         InsnId == X86_INS_VMOVDQU32 || InsnId == X86_INS_VMOVDQU64 ||
         InsnId == X86_INS_VMOVDQA32 || InsnId == X86_INS_VMOVDQA64;
}

std::optional<unsigned> vectorRegisterIndex(const cs_x86_op &Operand) {
  if (!isVectorRegisterOfSize(Operand, Operand.size))
    return std::nullopt;
  if (Operand.size == 16)
    return static_cast<unsigned>(Operand.reg - X86_REG_XMM0);
  if (Operand.size == 32)
    return static_cast<unsigned>(Operand.reg - X86_REG_YMM0);
  if (Operand.size == 64)
    return static_cast<unsigned>(Operand.reg - X86_REG_ZMM0);
  return std::nullopt;
}

bool liftEvexFullVectorMove(X86Lifter &L, X86Lifter::LiftState &S,
                            const cs_insn *Insn, const cs_x86 &X86,
                            const EvexFullVectorMoveSpec &Spec) {
  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != 0x01 ||
      (Encoding.P1 | 0x04) != Spec.CanonicalP1 ||
      (Encoding.Opcode != Spec.LoadOpcode &&
       Encoding.Opcode != Spec.StoreOpcode) ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      hasUnsupportedEvexValueModifier(X86) ||
      (X86.op_count != 2 && X86.op_count != 3))
    return false;

  const bool HasMask = X86.op_count == 3;
  if (HasMask && !isX86OpmaskOperand(X86.operands[1]))
    return false;
  const unsigned SourceIndex = HasMask ? 2 : 1;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &Source = X86.operands[SourceIndex];
  const bool DestinationIsMemory = Destination.type == X86_OP_MEM;
  const bool SourceIsMemory = Source.type == X86_OP_MEM;
  if (DestinationIsMemory && SourceIsMemory)
    return false;

  const bool IsStoreEncoding = Encoding.Opcode == Spec.StoreOpcode;
  if ((DestinationIsMemory && !IsStoreEncoding) ||
      (SourceIsMemory && IsStoreEncoding))
    return false;
  const cs_x86_op &RawRegOperand = IsStoreEncoding ? Source : Destination;
  const cs_x86_op &RawRMOperand = IsStoreEncoding ? Destination : Source;
  const auto RawRegIndex = vectorRegisterIndex(RawRegOperand);
  if (!RawRegIndex ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) != *RawRegIndex)
    return false;

  const uint16_t VectorSize = RawRegOperand.size;
  if ((VectorSize != 16 && VectorSize != 32 && VectorSize != 64) ||
      VectorSize % Spec.ElementSize != 0)
    return false;
  if (RawRMOperand.type == X86_OP_MEM) {
    if (RawRMOperand.size != VectorSize ||
        Encoding.AddressSize != S.AddressSize ||
        !isValidMaskedMoveMemoryOperand(RawRMOperand, S.AddressSize,
                                        VectorSize) ||
        !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, RawRMOperand,
                                         VectorSize))
      return false;
  } else {
    const auto RawRMIndex = vectorRegisterIndex(RawRMOperand);
    if (!RawRMIndex || RawRMOperand.size != VectorSize ||
        decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) != *RawRMIndex ||
        !validateCanonicalEvexRegisterTail(Insn, X86, Encoding))
      return false;
  }

  const unsigned LaneCount = VectorSize / Spec.ElementSize;
  const uint16_t RequiredMaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
  const uint8_t ExpectedLength =
      VectorSize == 16 ? 0 : (VectorSize == 32 ? 0x20 : 0x40);
  NdVar CompactMask;
  uint8_t ExpectedP2 = static_cast<uint8_t>(ExpectedLength | 0x08);
  const cs_x86_op *MaskOperand = nullptr;
  if (HasMask) {
    MaskOperand = &X86.operands[1];
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(MaskOperand->reg));
    if (MaskOperand->reg == X86_REG_K0 ||
        MaskOperand->size != RequiredMaskSize ||
        MaskInfo.Size < RequiredMaskSize ||
        (DestinationIsMemory && MaskOperand->avx_zero_opmask))
      return false;
    ExpectedP2 |= static_cast<uint8_t>(MaskOperand->reg - X86_REG_K0);
    if (MaskOperand->avx_zero_opmask)
      ExpectedP2 |= 0x80;
    CompactMask = NdVar::reg(MaskInfo.Offset, RequiredMaskSize);
  } else {
    const uint64_t AllLanes = LaneCount == 64
                                  ? UINT64_MAX
                                  : (UINT64_C(1) << LaneCount) - 1;
    CompactMask = NdVar::cst(AllLanes, RequiredMaskSize);
  }
  if (Encoding.P2 != ExpectedP2)
    return false;
  for (unsigned Index = 0; Index < X86.op_count; ++Index)
    if (X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID ||
        (X86.operands[Index].avx_zero_opmask &&
         (!MaskOperand || &X86.operands[Index] != MaskOperand)))
      return false;

  if (!DestinationIsMemory && !SourceIsMemory) {
    NdVar Raw = L.operandRead(S, Source);
    if (Raw.Size != VectorSize)
      return false;
    if (MaskOperand)
      return emitMaskedVectorResult(L, S, Destination, *MaskOperand, Raw,
                                    Spec.ElementSize);
    S.emit(NdOp::COPY, L.operandWrite(Destination), {Raw});
    return true;
  }

  const cs_x86_op &MemoryOperand =
      DestinationIsMemory ? Destination : Source;
  const NdVar Address = S.computeEA(MemoryOperand);
  const NdMemoryAddressSpace AddressSpace =
      X86Lifter::LiftState::memoryAddressSpace(MemoryOperand);
  if (Spec.RequiresAlignment) {
    if (MaskOperand) {
      const uint64_t RelevantBits =
          LaneCount == 64 ? UINT64_MAX : (UINT64_C(1) << LaneCount) - 1;
      NdVar RelevantMask = S.makeTemp(RequiredMaskSize);
      S.emit(NdOp::INT_AND, RelevantMask,
             {CompactMask, NdVar::cst(RelevantBits, RequiredMaskSize)});
      NdVar AnyLane = S.makeTemp(1);
      S.emit(NdOp::INT_NOTEQUAL, AnyLane,
             {RelevantMask, NdVar::cst(0, RequiredMaskSize)});
      S.emitIntrinsic(Intrinsic::RequireAligned, {},
                      {Address, NdVar::scalar(VectorSize, 8), AnyLane},
                      NdMemoryOrdering::None, AddressSpace);
    } else {
      S.emitIntrinsic(Intrinsic::RequireAligned, {},
                      {Address, NdVar::scalar(VectorSize, 8)},
                      NdMemoryOrdering::None, AddressSpace);
    }
  }

  if (DestinationIsMemory) {
    const NdVar Value = L.operandRead(S, Source);
    if (Value.Size != VectorSize)
      return false;
    if (!MaskOperand) {
      S.emit(NdOp::STORE, {}, {Address, Value}, NdMemoryOrdering::None,
             AddressSpace);
      return true;
    }
    const NdVar ExpandedMask =
        expandCompactLaneMask(S, CompactMask, VectorSize, Spec.ElementSize);
    const Intrinsic StoreId = maskedStoreIntrinsic(Spec.ElementSize);
    if (ExpandedMask.Size != VectorSize || StoreId == Intrinsic::None)
      return false;
    S.emitIntrinsic(StoreId, {}, {Address, ExpandedMask, Value},
                    NdMemoryOrdering::None, AddressSpace);
    return true;
  }

  NdVar Raw;
  if (MaskOperand) {
    const NdVar ExpandedMask =
        expandCompactLaneMask(S, CompactMask, VectorSize, Spec.ElementSize);
    const Intrinsic LoadId = maskedVectorLoadIntrinsic(Spec.ElementSize);
    if (ExpandedMask.Size != VectorSize || LoadId == Intrinsic::None)
      return false;
    Raw = S.makeTemp(VectorSize);
    S.emitIntrinsic(LoadId, Raw, {Address, ExpandedMask},
                    NdMemoryOrdering::None, AddressSpace);
  } else {
    Raw = S.makeTemp(VectorSize);
    S.emit(NdOp::LOAD, Raw, {Address}, NdMemoryOrdering::None, AddressSpace);
  }
  if (MaskOperand)
    return emitMaskedVectorResult(L, S, Destination, *MaskOperand, Raw,
                                  Spec.ElementSize);
  S.emit(NdOp::COPY, L.operandWrite(Destination), {Raw});
  return true;
}

struct EvexScalarBroadcastSpec {
  uint8_t Opcode;
  uint8_t GprOpcode;
  uint16_t ElementSize;
  bool W;
  bool Supports128;
};

std::optional<EvexScalarBroadcastSpec>
evexScalarBroadcastSpec(unsigned InsnId) {
  switch (InsnId) {
  default:
    return std::nullopt;
  case X86_INS_VBROADCASTSS:
    return EvexScalarBroadcastSpec{0x18, 0, 4, false, true};
  case X86_INS_VBROADCASTSD:
    return EvexScalarBroadcastSpec{0x19, 0, 8, true, false};
  case X86_INS_VPBROADCASTB:
    return EvexScalarBroadcastSpec{0x78, 0x7a, 1, false, true};
  case X86_INS_VPBROADCASTW:
    return EvexScalarBroadcastSpec{0x79, 0x7b, 2, false, true};
  case X86_INS_VPBROADCASTD:
    return EvexScalarBroadcastSpec{0x58, 0x7c, 4, false, true};
  case X86_INS_VPBROADCASTQ:
    return EvexScalarBroadcastSpec{0x59, 0x7c, 8, true, true};
  }
}

NdVar repeatLowElement(X86Lifter::LiftState &S, NdVar Source,
                       uint16_t ElementSize, uint16_t ResultSize) {
  if (ElementSize == 0 || Source.Size < ElementSize ||
      ResultSize < ElementSize || ResultSize % ElementSize != 0)
    return {};
  NdVar Result = Source;
  if (Source.Size != ElementSize) {
    Result = S.makeTemp(ElementSize);
    S.emit(NdOp::SUBBYTES, Result, {Source, NdVar::cst(0, 4)});
  }
  while (Result.Size < ResultSize) {
    if (Result.Size > ResultSize / 2)
      return {};
    NdVar Next = S.makeTemp(Result.Size * 2);
    S.emit(NdOp::CONCAT, Next, {Result, Result});
    Result = Next;
  }
  return Result;
}

bool liftEvexScalarBroadcast(X86Lifter &L, X86Lifter::LiftState &S,
                             const cs_insn *Insn, const cs_x86 &X86,
                             unsigned InsnId) {
  const auto Spec = evexScalarBroadcastSpec(InsnId);
  CanonicalEvexEncodingInfo Encoding;
  if (!Spec ||
      !parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != 0x02 ||
      (Encoding.P1 | 0x04) !=
          static_cast<uint8_t>((Spec->W ? 0x80 : 0) | 0x7d) ||
      (Encoding.Opcode != Spec->Opcode &&
       (Spec->GprOpcode == 0 || Encoding.Opcode != Spec->GprOpcode)) ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      hasUnsupportedEvexValueModifier(X86) ||
      (X86.op_count != 2 && X86.op_count != 3))
    return false;
  const bool GprForm = Encoding.Opcode == Spec->GprOpcode;
  const bool HasMask = X86.op_count == 3;
  if (HasMask && !isX86OpmaskOperand(X86.operands[1]))
    return false;
  const unsigned SourceIndex = HasMask ? 2 : 1;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &Source = X86.operands[SourceIndex];
  if ((Destination.size != 16 && Destination.size != 32 &&
       Destination.size != 64) ||
      (!Spec->Supports128 && Destination.size == 16) ||
      !isVectorRegisterOfSize(Destination, Destination.size) ||
      Destination.size % Spec->ElementSize != 0 ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          static_cast<unsigned>(Destination.reg -
                                (Destination.size == 16   ? X86_REG_XMM0
                                 : Destination.size == 32 ? X86_REG_YMM0
                                                          : X86_REG_ZMM0)))
    return false;

  const uint8_t ExpectedLength = Destination.size == 16
                                     ? 0
                                     : (Destination.size == 32 ? 0x20 : 0x40);
  if ((Encoding.P2 & 0x78) != static_cast<uint8_t>(ExpectedLength | 0x08))
    return false;

  const unsigned LaneCount = Destination.size / Spec->ElementSize;
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
  NdVar CompactMask = NdVar::cst(
      LaneCount == 64 ? UINT64_MAX : ((UINT64_C(1) << LaneCount) - 1),
      MaskSize);
  if (HasMask) {
    const cs_x86_op &Mask = X86.operands[1];
    const RegInfo MaskInfo = mapCapstoneReg(static_cast<x86_reg>(Mask.reg));
    if (Mask.reg == X86_REG_K0 || Mask.size != MaskSize ||
        MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < MaskSize ||
        (Encoding.P2 & 7) != Mask.reg - X86_REG_K0 ||
        ((Encoding.P2 & 0x80) != 0) != Mask.avx_zero_opmask)
      return false;
    CompactMask = NdVar::reg(MaskInfo.Offset, MaskSize);
  } else if ((Encoding.P2 & 0x87) != 0) {
    return false;
  }
  for (unsigned Index = 0; Index < X86.op_count; ++Index)
    if (X86.operands[Index].avx_zero_opmask && (!HasMask || Index != 1))
      return false;

  NdVar Raw;
  if (!GprForm && Source.type == X86_OP_MEM) {
    if (Source.size != Spec->ElementSize ||
        !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, Source,
                                         Spec->ElementSize))
      return false;
    Raw = emitEvexMaskedMemoryLoad(S, Source, CompactMask, Destination.size,
                                  Spec->ElementSize, Spec->ElementSize, true);
  } else if (!GprForm && Source.type == X86_OP_REG && Source.size == 16 &&
             Source.reg >= X86_REG_XMM0 && Source.reg <= X86_REG_XMM31) {
    if (!validateCanonicalEvexRegisterTail(Insn, X86, Encoding) ||
        decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
            static_cast<unsigned>(Source.reg - X86_REG_XMM0))
      return false;
    Raw = repeatLowElement(S, L.operandRead(S, Source), Spec->ElementSize,
                           Destination.size);
  } else if (GprForm && Source.type == X86_OP_REG &&
             Source.size == (Spec->ElementSize == 8 ? 8 : 4)) {
    const RegInfo SourceInfo =
        mapCapstoneReg(static_cast<x86_reg>(Source.reg));
    const unsigned EncodedSource =
        (Encoding.ModRM & 7) | ((Encoding.P0 & 0x20) == 0 ? 8 : 0) |
        ((Encoding.P0 & 0x08) != 0 ? 16 : 0);
    const uint64_t ExpectedSourceOffset =
        EncodedSource < 16
            ? static_cast<uint64_t>(EncodedSource) * 8
            : x86reg::extendedGeneralReg(EncodedSource - 16);
    if (!validateCanonicalEvexRegisterTail(Insn, X86, Encoding) ||
        !x86reg::isGeneralRegOffset(SourceInfo.Offset) ||
        SourceInfo.Size != Source.size || EncodedSource >= 32 ||
        SourceInfo.Offset != ExpectedSourceOffset)
      return false;
    Raw = repeatLowElement(S, L.operandRead(S, Source), Spec->ElementSize,
                           Destination.size);
  } else {
    return false;
  }
  if (Raw.Size != Destination.size)
    return false;

  if (HasMask)
    return emitMaskedVectorResult(L, S, Destination, X86.operands[1], Raw,
                                  Spec->ElementSize);
  S.emit(NdOp::COPY, L.operandWrite(Destination), {Raw});
  return true;
}

} // namespace

bool liftSIMDMove(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                  const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // Additional SSE / AVX (VEX) packed instructions — treat as their SSE
  // counterparts. Loses per-Lane semantics but preserves dataflow.
  // ========================================================================
  case X86_INS_VMOVDQA:
  case X86_INS_VMOVDQU:
  case X86_INS_VMOVDQA32:
  case X86_INS_VMOVDQA64:
  case X86_INS_VMOVDQU8:
  case X86_INS_VMOVDQU16:
  case X86_INS_VMOVDQU32:
  case X86_INS_VMOVDQU64:
  case X86_INS_VMOVAPS:
  case X86_INS_VMOVAPD:
  case X86_INS_VMOVUPS:
  case X86_INS_VMOVUPD:
  case X86_INS_VMOVSS:
  case X86_INS_VMOVSD:
  case X86_INS_VMOVD:
  case X86_INS_VMOVQ: {
    if (X86.op_count < 2)
      break;

    if (const auto Spec = evexFullVectorMoveSpec(InsnId)) {
      if (hasEvexEncoding(Insn))
        return liftEvexFullVectorMove(L, S, Insn, X86, *Spec);
      if (isEvexOnlyFullVectorMove(InsnId))
        return false;
    }

    const bool HasWriteMask =
        X86.op_count >= 3 && X86.operands[1].type == X86_OP_REG &&
        X86.operands[1].reg >= X86_REG_K0 && X86.operands[1].reg <= X86_REG_K7;
    if (HasWriteMask) {
      // A masked memory operation suppresses faults for inactive elements and
      // therefore cannot be represented by an eager full-width LOAD/STORE.
      // Keep those forms fail-closed until the memory IR carries lane guards.
      if (X86.operands[0].type != X86_OP_REG || X86.op_count != 3 ||
          X86.operands[2].type != X86_OP_REG)
        return false;

      uint16_t ElementSize = 0;
      switch (InsnId) {
      case X86_INS_VMOVDQU8:
        ElementSize = 1;
        break;
      case X86_INS_VMOVDQU16:
        ElementSize = 2;
        break;
      case X86_INS_VMOVDQA32:
      case X86_INS_VMOVDQU32:
      case X86_INS_VMOVAPS:
      case X86_INS_VMOVUPS:
        ElementSize = 4;
        break;
      case X86_INS_VMOVDQA64:
      case X86_INS_VMOVDQU64:
      case X86_INS_VMOVAPD:
      case X86_INS_VMOVUPD:
        ElementSize = 8;
        break;
      default:
        return false;
      }

      NdVar Dst = L.operandWrite(X86.operands[0]);
      NdVar Src = L.operandRead(S, X86.operands[2]);
      if (Dst.Size == 0 || Dst.Size != Src.Size || Dst.Size % ElementSize != 0)
        return false;
      if (!emitMaskedVectorResult(L, S, X86.operands[0], X86.operands[1], Src,
                                  ElementSize))
        return false;
      break;
    }

    NdVar Src = L.operandRead(S, X86.operands[1]);
    if (X86.operands[0].type == X86_OP_MEM) {
      // Memory destination (store form, e.g. `vmovdqa [mem], xmm`).  As with
      // the SSE MOV* path, L.operandWrite() on a memory operand yields a
      // discarded ram(0) placeholder, so the store must be emitted explicitly
      // or it is silently dropped (the value never reaches memory).  Scalar
      // forms write only the low element width.
      unsigned StoreSz = 0;
      if (InsnId == X86_INS_VMOVSS || InsnId == X86_INS_VMOVD)
        StoreSz = 4;
      else if (InsnId == X86_INS_VMOVSD || InsnId == X86_INS_VMOVQ)
        StoreSz = 8;
      if (StoreSz && Src.Size > StoreSz) {
        NdVar Lo = S.makeTemp(StoreSz);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        Src = Lo;
      }
      S.storeToMem(X86.operands[0], Src);
    } else {
      NdVar Dst = L.operandWrite(X86.operands[0]);
      if (Src.Size > Dst.Size) {
        NdVar Lo = S.makeTemp(Dst.Size);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        Src = Lo;
      }
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  case X86_INS_LDTILECFG: {
    if (L.targetArch() != Arch::X64 || X86.op_count != 1 ||
        X86.operands[0].type != X86_OP_MEM)
      return false;
    const NdVar Config =
        NdVar::reg(x86reg::TileConfig, x86reg::TileConfigSize);
    if (!S.emitMemoryIntrinsic(Intrinsic::AMXLoadConfig, X86.operands[0],
                               {NdVar::scalar(S.AddressSize, 1)}, Config))
      return false;
    // A successful LDTILECFG enters either INIT or palette 1 and clears the
    // entire physical tile-data component.  These copies are deliberately
    // sequenced after validation/load so a fault leaves old state untouched.
    for (unsigned Tile = 0; Tile < x86reg::TileRegCount; ++Tile)
      S.emit(NdOp::COPY,
             NdVar::reg(x86reg::tileReg(Tile), x86reg::TileRegStride),
             {NdVar::cst(0, x86reg::TileRegStride)});
    break;
  }

  case X86_INS_STTILECFG: {
    if (L.targetArch() != Arch::X64 || X86.op_count != 1 ||
        X86.operands[0].type != X86_OP_MEM)
      return false;
    const NdVar Config =
        NdVar::reg(x86reg::TileConfig, x86reg::TileConfigSize);
    if (!S.emitMemoryIntrinsic(
            Intrinsic::AMXStoreConfig, X86.operands[0],
            {Config, NdVar::scalar(S.AddressSize, 1)}))
      return false;
    break;
  }

  case X86_INS_TILERELEASE: {
    if (L.targetArch() != Arch::X64 || X86.op_count != 0)
      return false;
    S.emit(NdOp::COPY,
           NdVar::reg(x86reg::TileConfig, x86reg::TileConfigSize),
           {NdVar::cst(0, x86reg::TileConfigSize)});
    for (unsigned Tile = 0; Tile < x86reg::TileRegCount; ++Tile)
      S.emit(NdOp::COPY,
             NdVar::reg(x86reg::tileReg(Tile), x86reg::TileRegStride),
             {NdVar::cst(0, x86reg::TileRegStride)});
    break;
  }

  case X86_INS_TILELOADD:
  case X86_INS_TILELOADDT1:
  case X86_INS_TILELOADDRS:
  case X86_INS_TILELOADDRST1: {
    if (L.targetArch() != Arch::X64 || X86.op_count != 2 ||
        X86.operands[0].type != X86_OP_REG ||
        X86.operands[1].type != X86_OP_MEM ||
        (S.AddressSize != 4 && S.AddressSize != 8))
      return false;
    NdVar Destination = L.operandWrite(X86.operands[0]);
    if (Destination.Size != x86reg::TileRegStride)
      return false;
    const auto Stride = emitAMXStride(S, X86.operands[1]);
    if (!Stride)
      return false;
    cs_x86_op RowBase = X86.operands[1];
    RowBase.mem.index = X86_REG_INVALID;
    RowBase.mem.scale = 1;
    const NdVar Config =
        NdVar::reg(x86reg::TileConfig, x86reg::TileConfigSize);
    if (!S.emitMemoryIntrinsic(
            Intrinsic::AMXTileLoad, RowBase,
            {*Stride, Config, Destination, NdVar::scalar(S.AddressSize, 1)},
            Destination))
      return false;
    S.emitIntrinsic(Intrinsic::AMXClearStartRow, Config, {Config});
    break;
  }

  case X86_INS_TILESTORED: {
    if (L.targetArch() != Arch::X64 || X86.op_count != 2 ||
        X86.operands[0].type != X86_OP_MEM ||
        X86.operands[1].type != X86_OP_REG ||
        (S.AddressSize != 4 && S.AddressSize != 8))
      return false;
    NdVar Source = L.operandRead(S, X86.operands[1]);
    if (Source.Size != x86reg::TileRegStride)
      return false;
    const auto Stride = emitAMXStride(S, X86.operands[0]);
    if (!Stride)
      return false;
    cs_x86_op RowBase = X86.operands[0];
    RowBase.mem.index = X86_REG_INVALID;
    RowBase.mem.scale = 1;
    const NdVar Config =
        NdVar::reg(x86reg::TileConfig, x86reg::TileConfigSize);
    if (!S.emitMemoryIntrinsic(
            Intrinsic::AMXTileStore, RowBase,
            {*Stride, Config, Source, NdVar::scalar(S.AddressSize, 1)},
            Config))
      return false;
    break;
  }

  case X86_INS_TILEZERO: {
    if (X86.op_count != 1 || X86.operands[0].type != X86_OP_REG)
      return false;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    if (Dst.Size != x86reg::TileRegStride)
      return false;
    const NdVar Config =
        NdVar::reg(x86reg::TileConfig, x86reg::TileConfigSize);
    S.emitIntrinsic(Intrinsic::AMXTileZero, Dst, {Config});
    S.emitIntrinsic(Intrinsic::AMXClearStartRow, Config, {Config});
    break;
  }

  case X86_INS_VZEROUPPER:
  case X86_INS_VZEROALL: {
    const unsigned RegisterCount = L.targetArch() == Arch::X64 ? 16 : 8;
    for (unsigned Index = 0; Index < RegisterCount; ++Index) {
      const uint64_t Offset = x86reg::vectorReg(Index);
      NdVar Zmm = NdVar::reg(Offset, 64);
      if (InsnId == X86_INS_VZEROALL) {
        S.emit(NdOp::COPY, Zmm, {NdVar::cst(0, 64)});
      } else {
        S.emit(NdOp::INT_ZEXT, Zmm, {NdVar::reg(Offset, 16)});
      }
    }
    break;
  }

  // PMOVMSKB / VPMOVMSKB — extract MSBs of each byte → GPR bitmask.
  case X86_INS_PMOVMSKB:
  case X86_INS_VPMOVMSKB: {
    if (X86.op_count != 2 || X86.operands[0].type != X86_OP_REG ||
        X86.operands[1].type != X86_OP_REG)
      return false;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    if (Dst.Size != 4 ||
        (Src.Size != 8 && Src.Size != 16 && Src.Size != 32))
      return false;

    NdVar Mask = NdVar::cst(0, Dst.Size);
    for (unsigned I = 0; I < Src.Size; ++I) {
      NdVar Byte = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, Byte, {Src, NdVar::cst(I, 4)});
      NdVar Bit = S.makeTemp(1);
      S.emit(NdOp::INT_RIGHT, Bit, {Byte, NdVar::cst(7, 1)});
      NdVar WideBit = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, WideBit, {Bit});
      if (I != 0) {
        NdVar Shifted = S.makeTemp(Dst.Size);
        S.emit(NdOp::INT_LEFT, Shifted,
               {WideBit, NdVar::cst(I, Dst.Size)});
        WideBit = Shifted;
      }
      if (I == 0) {
        Mask = WideBit;
      } else {
        NdVar Combined = S.makeTemp(Dst.Size);
        S.emit(NdOp::INT_OR, Combined, {Mask, WideBit});
        Mask = Combined;
      }
    }
    S.emit(NdOp::COPY, Dst, {Mask});
    break;
  }

  // PEXTRB/W/D/Q: extract element from XMM to GPR/mem.
  // Extract ElemSz bytes at the given lane index, then zero-extend to Dst.
  case X86_INS_PEXTRB:
  case X86_INS_PEXTRW:
  case X86_INS_PEXTRD:
  case X86_INS_PEXTRQ:
  case X86_INS_VPEXTRB:
  case X86_INS_VPEXTRW:
  case X86_INS_VPEXTRD:
  case X86_INS_VPEXTRQ: {
    if (X86.op_count < 3 ||
        (X86.operands[0].type != X86_OP_REG &&
         X86.operands[0].type != X86_OP_MEM) ||
        X86.operands[1].type != X86_OP_REG ||
        X86.operands[2].type != X86_OP_IMM)
      return false;
    uint16_t ElemSz = 1;
    if (InsnId == X86_INS_PEXTRW || InsnId == X86_INS_VPEXTRW)
      ElemSz = 2;
    else if (InsnId == X86_INS_PEXTRD || InsnId == X86_INS_VPEXTRD)
      ElemSz = 4;
    else if (InsnId == X86_INS_PEXTRQ || InsnId == X86_INS_VPEXTRQ)
      ElemSz = 8;
    // A MEMORY destination must be written with an explicit STORE:
    // L.operandWrite() returns a discarded ram(0) placeholder, so writing the
    // element into it silently dropped the store (memory left unchanged).
    // Extract into an element-sized temp and store it back; the register form
    // is unchanged (it zero-extends the element into the GPR via the ElemSz <
    // Dst.Size path).
    bool IsMem = (X86.operands[0].type == X86_OP_MEM);
    NdVar Dst = IsMem ? S.makeTemp(ElemSz) : L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    if (Src.Size != 16 || Dst.Size < ElemSz)
      return false;

    // The instruction only consumes the low log2(lane-count) immediate bits.
    // Extract the selected lane directly so execution never relies on a
    // scalar implementation pretending to perform an i128 right shift.
    const uint64_t LaneMask = (16 / ElemSz) - 1;
    const uint64_t Lane = static_cast<uint8_t>(X86.operands[2].imm) & LaneMask;
    NdVar Elem = S.makeTemp(ElemSz);
    S.emit(NdOp::SUBBYTES, Elem,
           {Src, NdVar::cst(Lane * ElemSz, 4)});
    if (ElemSz < Dst.Size) {
      S.emit(NdOp::INT_ZEXT, Dst, {Elem});
    } else {
      S.emit(NdOp::COPY, Dst, {Elem});
    }
    if (IsMem)
      S.storeToMem(X86.operands[0], Dst);
    break;
  }
  // PINSRB/W/D/Q: insert element into XMM at Lane index.
  // Use per-lane SUBBYTES+CONCAT to avoid uint64_t mask overflow for i128.
  case X86_INS_PINSRB:
  case X86_INS_PINSRW:
  case X86_INS_PINSRD:
  case X86_INS_PINSRQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint16_t ElemSz = 1;
    if (InsnId == X86_INS_PINSRW)
      ElemSz = 2;
    else if (InsnId == X86_INS_PINSRD)
      ElemSz = 4;
    else if (InsnId == X86_INS_PINSRQ)
      ElemSz = 8;
    if (X86.op_count >= 3 && X86.operands[2].type == X86_OP_IMM) {
      uint64_t Idx = X86.operands[2].imm;
      unsigned NLanes = Dst.Size / ElemSz;
      // A degenerate (size-0) destination would make NLanes 0 and the modulo a
      // division by zero; guard defensively.
      Idx = NLanes ? (Idx % NLanes) : 0;
      NdVar ElemVal = Src;
      if (Src.Size > ElemSz) {
        ElemVal = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, ElemVal, {Src, NdVar::cst(0, 4)});
      } else if (Src.Size < ElemSz) {
        ElemVal = S.makeTemp(ElemSz);
        S.emit(NdOp::INT_ZEXT, ElemVal, {Src});
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane;
        if (I == Idx) {
          Lane = ElemVal;
        } else {
          Lane = S.makeTemp(ElemSz);
          S.emit(NdOp::SUBBYTES, Lane, {DstR, NdVar::cst(I * ElemSz, 4)});
        }
        if (I == 0) {
          Acc = Lane;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + ElemSz);
          S.emit(NdOp::CONCAT, Next, {Lane, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_OR, Dst, {DstR, Src});
    }
    break;
  }
  case X86_INS_PALIGNR: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Palignr, Dst, {Dst, Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_MOVLHPS: {
    // dst[63:0] preserved, dst[127:64] = src[63:0].  Built with
    // SUBBYTES/CONCAT: a 64-bit mask constant widened to 16 bytes becomes
    // all-ones (not a low mask), which left the old high half OR'd into the new
    // one.
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Lo = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, Lo, {DstR, NdVar::cst(0, 4)});
    NdVar SrcLo = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, SrcLo, {Src, NdVar::cst(0, 4)});
    S.emit(NdOp::CONCAT, Dst, {SrcLo, Lo});
    break;
  }
  case X86_INS_MOVHLPS: {
    // dst[63:0] = src[127:64], dst[127:64] preserved.  The old SUBBYTES-to-reg
    // wrote only the low 8 bytes and dropped (zeroed) the preserved high half.
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar SrcHi = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, SrcHi, {Src, NdVar::cst(8, 4)});
    NdVar Hi = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, Hi, {DstR, NdVar::cst(8, 4)});
    S.emit(NdOp::CONCAT, Dst, {Hi, SrcHi});
    break;
  }

  case X86_INS_VBROADCASTSS:
  case X86_INS_VBROADCASTSD:
  case X86_INS_VPBROADCASTB:
  case X86_INS_VPBROADCASTW:
  case X86_INS_VPBROADCASTD:
  case X86_INS_VPBROADCASTQ: {
    if (hasEvexEncoding(Insn))
      return liftEvexScalarBroadcast(L, S, Insn, X86, InsnId);
    if (X86.op_count != 2)
      return false;
    const auto Spec = evexScalarBroadcastSpec(InsnId);
    if (!Spec)
      return false;
    NdVar Source = L.operandRead(S, X86.operands[1]);
    NdVar Result = repeatLowElement(S, Source, Spec->ElementSize,
                                    X86.operands[0].size);
    if (Result.Size != X86.operands[0].size)
      return false;
    S.emit(NdOp::COPY, L.operandWrite(X86.operands[0]), {Result});
    break;
  }

  case X86_INS_VINSERTF128:
  case X86_INS_VINSERTI128: {
    // VINSERTF128 ymm1, ymm2, xmm3/m128, imm8
    // inserts 128-bit Src into the Lane selected by imm8[0]
    if (X86.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar YmmSrc = L.operandRead(S, X86.operands[1]);
    NdVar XmmSrc = L.operandRead(S, X86.operands[2]);
    uint8_t Lane = static_cast<uint8_t>(X86.operands[3].imm) & 1;
    if (Lane == 0) {
      // Replace low 128 Bits: build from XmmSrc (low) + high half of YmmSrc
      NdVar Hi = S.makeTemp(16);
      S.emit(NdOp::SUBBYTES, Hi, {YmmSrc, NdVar::cst(16, 1)});
      S.emit(NdOp::CONCAT, Dst, {Hi, XmmSrc});
    } else {
      // Replace high 128 Bits: build from low half of YmmSrc + XmmSrc (high)
      NdVar Lo = S.makeTemp(16);
      S.emit(NdOp::SUBBYTES, Lo, {YmmSrc, NdVar::cst(0, 1)});
      S.emit(NdOp::CONCAT, Dst, {XmmSrc, Lo});
    }
    break;
  }
  case X86_INS_VEXTRACTF128:
  case X86_INS_VEXTRACTI128: {
    // VEXTRACTF128 xmm1/m128, ymm2, imm8
    // extracts the 128-bit Lane selected by imm8[0]
    if (X86.op_count < 3)
      break;
    // A MEMORY destination must be written with an explicit STORE:
    // L.operandWrite() of a mem operand yields a discarded ram(0) placeholder,
    // so emitting the extracted lane straight into it silently dropped the
    // write-back for `vextractf128/vextracti128 [mem],ymm,imm` (lane computed,
    // memory left unchanged) — the same class of bug already fixed for
    // PEXTR*/EXTRACTPS and the MOVHPS/MOVLPS partial stores.  Extract into a
    // temp and store it.
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    NdVar Dst = MemDst ? S.makeTemp(16) : L.operandWrite(X86.operands[0]);
    NdVar YmmSrc = L.operandRead(S, X86.operands[1]);
    uint8_t Lane = static_cast<uint8_t>(X86.operands[2].imm) & 1;
    S.emit(NdOp::SUBBYTES, Dst, {YmmSrc, NdVar::cst(Lane * 16, 1)});
    if (MemDst)
      S.storeToMem(X86.operands[0], Dst);
    break;
  }

  // VPINSRB/W/D/Q: VEX insert element — per-lane SUBBYTES+CONCAT.
  case X86_INS_VPINSRB:
  case X86_INS_VPINSRW:
  case X86_INS_VPINSRD:
  case X86_INS_VPINSRQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    bool IsVEX = (X86.op_count >= 4);
    NdVar Base = IsVEX ? L.operandRead(S, X86.operands[1])
                       : L.operandRead(S, X86.operands[0]);
    NdVar Src = IsVEX ? L.operandRead(S, X86.operands[2])
                      : L.operandRead(S, X86.operands[1]);
    int ImmIdx = IsVEX ? 3 : 2;
    uint16_t ElemSz = 1;
    if (InsnId == X86_INS_VPINSRW)
      ElemSz = 2;
    else if (InsnId == X86_INS_VPINSRD)
      ElemSz = 4;
    else if (InsnId == X86_INS_VPINSRQ)
      ElemSz = 8;
    if (ImmIdx < X86.op_count && X86.operands[ImmIdx].type == X86_OP_IMM) {
      uint64_t Idx = X86.operands[ImmIdx].imm;
      unsigned NLanes = Dst.Size / ElemSz;
      // A degenerate (size-0) destination would make NLanes 0 and the modulo a
      // division by zero; guard defensively.
      Idx = NLanes ? (Idx % NLanes) : 0;
      NdVar ElemVal = Src;
      if (Src.Size > ElemSz) {
        ElemVal = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, ElemVal, {Src, NdVar::cst(0, 4)});
      } else if (Src.Size < ElemSz) {
        ElemVal = S.makeTemp(ElemSz);
        S.emit(NdOp::INT_ZEXT, ElemVal, {Src});
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane;
        if (I == Idx) {
          Lane = ElemVal;
        } else {
          Lane = S.makeTemp(ElemSz);
          S.emit(NdOp::SUBBYTES, Lane, {Base, NdVar::cst(I * ElemSz, 4)});
        }
        if (I == 0) {
          Acc = Lane;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + ElemSz);
          S.emit(NdOp::CONCAT, Next, {Lane, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::COPY, Dst, {Base});
    }
    break;
  }

  case X86_INS_VBROADCASTF128:
  case X86_INS_VBROADCASTI128:
  case X86_INS_VBLENDMPS:
  case X86_INS_VBLENDMPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // MOVMSKPS/MOVMSKPD/VMOVMSKPS/VMOVMSKPD — extract sign bits per lane.
  case X86_INS_MOVMSKPS:
  case X86_INS_MOVMSKPD:
  case X86_INS_VMOVMSKPS:
  case X86_INS_VMOVMSKPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    bool IsPS = (InsnId == X86_INS_MOVMSKPS || InsnId == X86_INS_VMOVMSKPS);
    unsigned LaneSz = IsPS ? 4 : 8;
    unsigned NLanes = Src.Size / LaneSz;
    unsigned SignBit = LaneSz * 8 - 1;
    NdVar Accum = NdVar::cst(0, Dst.Size);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
      NdVar SignWide = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_RIGHT, SignWide, {Lane, NdVar::cst(SignBit, LaneSz)});
      NdVar Sign = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, Sign, {SignWide, NdVar::cst(0, 4)});
      NdVar SignExt = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, SignExt, {Sign});
      NdVar Shifted = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_LEFT, Shifted, {SignExt, NdVar::cst(I, Dst.Size)});
      NdVar NewAccum = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_OR, NewAccum, {Accum, Shifted});
      Accum = NewAccum;
    }
    S.emit(NdOp::COPY, Dst, {Accum});
    break;
  }

  // FEMMS / EMMS — exit MMX State (no-Opc for our purposes).
  case X86_INS_EMMS:
  case X86_INS_FEMMS:
    S.emit(NdOp::NOP, {}, {});
    break;

  default:
    return false;
  }
  return true;
}

} // namespace neverd
