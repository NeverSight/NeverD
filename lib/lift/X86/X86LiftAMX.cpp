//===- X86LiftAMX.cpp - Architectural AMX bank lifting ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain complete tile-bank effects and native memory/feature distinctions.
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/X86/X86AmxState.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/support/X86/X86Addressing.h"

#include <cstdint>
#include <optional>

namespace neverd {

namespace {

std::optional<AMXTileComputeKind> computeKind(unsigned Instruction) {
  switch (Instruction) {
  case X86_INS_TDPBSSD:
    return AMXTileComputeKind::Int8SignedSigned;
  case X86_INS_TDPBSUD:
    return AMXTileComputeKind::Int8SignedUnsigned;
  case X86_INS_TDPBUSD:
    return AMXTileComputeKind::Int8UnsignedSigned;
  case X86_INS_TDPBUUD:
    return AMXTileComputeKind::Int8UnsignedUnsigned;
  case X86_INS_TDPBF16PS:
    return AMXTileComputeKind::BF16;
  case X86_INS_TDPFP16PS:
    return AMXTileComputeKind::FP16;
  case X86_INS_TCMMIMFP16PS:
    return AMXTileComputeKind::ComplexFP16Imaginary;
  case X86_INS_TCMMRLFP16PS:
    return AMXTileComputeKind::ComplexFP16Real;
  case X86_INS_TDPBF8PS:
    return AMXTileComputeKind::BF8BF8;
  case X86_INS_TDPBHF8PS:
    return AMXTileComputeKind::BF8HF8;
  case X86_INS_TDPHBF8PS:
    return AMXTileComputeKind::HF8BF8;
  case X86_INS_TDPHF8PS:
    return AMXTileComputeKind::HF8HF8;
  case X86_INS_TMMULTF32PS:
    return AMXTileComputeKind::TF32;
  default:
    return std::nullopt;
  }
}

std::optional<AMXTileRowKind> rowKind(unsigned Instruction) {
  switch (Instruction) {
  case X86_INS_TILEMOVROW:
    return AMXTileRowKind::Move;
  case X86_INS_TCVTROWD2PS:
    return AMXTileRowKind::Int32ToFP32;
  case X86_INS_TCVTROWPS2BF16H:
    return AMXTileRowKind::FP32ToBF16High;
  case X86_INS_TCVTROWPS2BF16L:
    return AMXTileRowKind::FP32ToBF16Low;
  case X86_INS_TCVTROWPS2PHH:
    return AMXTileRowKind::FP32ToFP16High;
  case X86_INS_TCVTROWPS2PHL:
    return AMXTileRowKind::FP32ToFP16Low;
  default:
    return std::nullopt;
  }
}

bool isTileRegister(const RegInfo &Register) {
  return Register.Size == x86reg::TileRegStride &&
         Register.Offset >= x86reg::TileBase &&
         Register.Offset < x86reg::tileReg(x86reg::TileRegCount) &&
         (Register.Offset - x86reg::TileBase) % x86reg::TileRegStride == 0;
}

} // namespace

bool liftAMX(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
             const cs_x86 &X86) {
  using K = X86AmxOp;
  std::optional<K> Kind;
  if (auto C = computeKind(Insn->id))
    Kind = K(unsigned(K::Int8SignedSigned) + unsigned(*C));
  else if (auto R = rowKind(Insn->id))
    Kind = K(unsigned(K::MoveRow) + unsigned(*R));
  else
    switch (Insn->id) {
    case X86_INS_LDTILECFG:
      Kind = K::LoadConfig;
      break;
    case X86_INS_STTILECFG:
      Kind = K::StoreConfig;
      break;
    case X86_INS_TILERELEASE:
      Kind = K::Release;
      break;
    case X86_INS_TILEZERO:
      Kind = K::Zero;
      break;
    case X86_INS_TILELOADD:
      Kind = K::Load;
      break;
    case X86_INS_TILELOADDT1:
      Kind = K::LoadT1;
      break;
    case X86_INS_TILELOADDRS:
      Kind = K::LoadRS;
      break;
    case X86_INS_TILELOADDRST1:
      Kind = K::LoadRST1;
      break;
    case X86_INS_TILESTORED:
      Kind = K::Store;
      break;
    default:
      return false;
    }
  if (L.targetArch() != Arch::X64 || !Insn->size || Insn->size > 15)
    return false;
  // The decoder owns encoding admission. Retain the encoding family because
  // APX-promoted memory instructions require APX even with low GPR operands.
  size_t Prefix = 0;
  while (Prefix < Insn->size) {
    const auto B = Insn->bytes[Prefix];
    if (B != 0x26 && B != 0x2e && B != 0x36 && B != 0x3e && B != 0x64 &&
        B != 0x65 && B != 0x67 && (B < 0x40 || B > 0x4f))
      break;
    ++Prefix;
  }
  if (Prefix == Insn->size ||
      (Insn->bytes[Prefix] != 0xc4 && Insn->bytes[Prefix] != 0x62))
    return false;
  uint32_t Descriptor = X86AmxStateVersion | unsigned(*Kind);
  auto Tile = [&](unsigned I) -> std::optional<unsigned> {
    if (I >= X86.op_count || X86.operands[I].type != X86_OP_REG)
      return {};
    auto R = mapCapstoneReg(static_cast<x86_reg>(X86.operands[I].reg));
    if (!isTileRegister(R))
      return {};
    return (R.Offset - x86reg::TileBase) / x86reg::TileRegStride;
  };
  NdVar Output, Base = NdVar::scalar(0, 8), Extra;
  NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default;
  if (x86AmxIsCompute(*Kind)) {
    if (X86.op_count != 3)
      return false;
    for (unsigned I = 0; I != 3; ++I) {
      auto T = Tile(I);
      if (!T)
        return false;
      Descriptor |= *T << (5 + 3 * I);
    }
  } else if (x86AmxIsRow(*Kind)) {
    if (X86.op_count != 3 || X86.operands[0].type != X86_OP_REG)
      return false;
    const auto R = mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
    auto T = Tile(1);
    if (!T || R.Size != x86reg::VectorRegStride ||
        R.Offset < x86reg::VectorBase ||
        R.Offset >= x86reg::vectorReg(x86reg::VectorRegCount) ||
        (R.Offset - x86reg::VectorBase) % x86reg::VectorRegStride)
      return false;
    Descriptor |= *T << 5;
    Output = NdVar::reg(R.Offset, x86reg::VectorRegStride);
    if (X86.operands[2].type == X86_OP_IMM) {
      const auto V = X86.operands[2].imm;
      if (V < 0 || V > UINT8_MAX)
        return false;
      Descriptor |= X86AmxImmediateRow;
      Extra = NdVar::scalar(V, 4);
    } else if (X86.operands[2].type == X86_OP_REG) {
      const auto Selector =
          mapCapstoneReg(static_cast<x86_reg>(X86.operands[2].reg));
      if (!x86reg::isGeneralRegOffset(Selector.Offset) || Selector.Size != 4)
        return false;
      if (Selector.Offset >= x86reg::ExtendedGPRBase)
        Descriptor |= X86AmxExtendedRow;
      Extra = NdVar::reg(Selector.Offset, 4);
    } else
      return false;
  } else if (*Kind == K::Release) {
    if (X86.op_count)
      return false;
  } else if (*Kind == K::Zero) {
    auto T = Tile(0);
    if (X86.op_count != 1 || !T)
      return false;
    Descriptor |= *T << 5;
  } else {
    const bool TileMemory = x86AmxIsTileMemory(*Kind);
    if (X86.op_count != (TileMemory ? 2 : 1) ||
        (S.AddressSize != 4 && S.AddressSize != 8))
      return false;
    const unsigned MemoryIndex = TileMemory && *Kind != K::Store ? 1 : 0;
    const auto &Memory = X86.operands[MemoryIndex];
    if (Memory.type != X86_OP_MEM)
      return false;
    Space = S.memoryAddressSpace(Memory);
    if (Memory.mem.segment == X86_REG_SS)
      Descriptor |= X86AmxStackSegment;
    if (S.AddressSize == 4)
      Descriptor |= X86AmxAddress32;
    else if (Space == NdMemoryAddressSpace::Default &&
             isStackDefaultBase(Memory.mem.base))
      Descriptor |= X86AmxStackReference;
    // Validate provenance before appending stride/EA effects. Native row-memory
    // keeps its complete base in a separate compiler-owned stack leaf.
    if (!x86AmxAddressIsValid(Descriptor, Space))
      throw UnliftedInstruction(Insn->address, Insn->mnemonic, Insn->op_str);
    cs_x86_op RowBase = Memory;
    if (TileMemory) {
      auto T = Tile(1 - MemoryIndex);
      if (!T)
        return false;
      Descriptor |= *T << 5;
      // SIB.index is a byte stride, not part of the first row's address.
      if (Memory.mem.scale != 1 && Memory.mem.scale != 2 &&
          Memory.mem.scale != 4 && Memory.mem.scale != 8)
        return false;
      Extra = NdVar::scalar(0, 8);
      if (Memory.mem.index != X86_REG_INVALID) {
        const auto R = mapCapstoneReg(static_cast<x86_reg>(Memory.mem.index));
        if (!x86reg::isGeneralRegOffset(R.Offset) || R.Size != S.AddressSize)
          return false;
        Extra = NdVar::reg(R.Offset, R.Size);
        if (Extra.Size == 4) {
          auto Wide = S.makeTemp(8);
          S.emit(NdOp::INT_ZEXT, Wide, {Extra});
          Extra = Wide;
        }
        if (Memory.mem.scale != 1) {
          auto Scaled = S.makeTemp(8);
          S.emit(NdOp::INT_MULT, Scaled,
                 {Extra, NdVar::scalar(Memory.mem.scale, 8)});
          Extra = Scaled;
        }
      }
      RowBase.mem.index = X86_REG_INVALID;
      RowBase.mem.scale = 1;
    }
    if (Insn->bytes[Prefix] == 0x62)
      Descriptor |= X86AmxEvexMemory;
    Base = S.computeEA(RowBase);
  }
  if (!x86AmxDescriptorIsValid(Descriptor))
    return false;
  if (Extra.Size)
    S.emitIntrinsic(Intrinsic::X86AMXState, Output,
                    {Base, NdVar::scalar(Descriptor, 4), Extra},
                    NdMemoryOrdering::None, Space);
  else
    S.emitIntrinsic(Intrinsic::X86AMXState, Output,
                    {Base, NdVar::scalar(Descriptor, 4)},
                    NdMemoryOrdering::None, Space);
  return true;
}

} // namespace neverd
