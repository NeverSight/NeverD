//===- X86LiftAMX.cpp - x86 AMX tile compute lifting --------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

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
  if (const auto RowKind = rowKind(Insn->id)) {
    if (L.targetArch() != Arch::X64 || X86.op_count != 3 ||
        X86.operands[0].type != X86_OP_REG ||
        X86.operands[1].type != X86_OP_REG ||
        (X86.operands[2].type != X86_OP_REG &&
         X86.operands[2].type != X86_OP_IMM))
      return false;

    const RegInfo DestinationRegister =
        mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
    const RegInfo SourceRegister =
        mapCapstoneReg(static_cast<x86_reg>(X86.operands[1].reg));
    if (DestinationRegister.Size != x86reg::VectorRegStride ||
        DestinationRegister.Offset < x86reg::VectorBase ||
        DestinationRegister.Offset >=
            x86reg::vectorReg(x86reg::VectorRegCount) ||
        !isTileRegister(SourceRegister))
      return false;

    NdVar Selector;
    if (X86.operands[2].type == X86_OP_IMM) {
      const int64_t Immediate = X86.operands[2].imm;
      if (Immediate < 0 || Immediate > UINT8_MAX)
        return false;
      const uint32_t Row = static_cast<uint32_t>(Immediate) & 0x3f;
      const uint32_t Chunk = static_cast<uint32_t>(Immediate) >> 6;
      Selector = NdVar::cst((Chunk << 16) | Row, 4);
    } else {
      const RegInfo SelectorRegister =
          mapCapstoneReg(static_cast<x86_reg>(X86.operands[2].reg));
      if (!x86reg::isGeneralRegOffset(SelectorRegister.Offset) ||
          SelectorRegister.Size != 4)
        return false;
      Selector = NdVar::reg(SelectorRegister.Offset, 4);
    }

    const NdVar Destination =
        NdVar::reg(DestinationRegister.Offset, x86reg::VectorRegStride);
    const NdVar Source =
        NdVar::reg(SourceRegister.Offset, x86reg::TileRegStride);
    const NdVar Config =
        NdVar::reg(x86reg::TileConfig, x86reg::TileConfigSize);
    S.emitIntrinsic(
        Intrinsic::AMXTileRow, Destination,
        {NdVar::cst(static_cast<uint8_t>(*RowKind), 1), Config, Source,
         Selector});
    S.emitIntrinsic(Intrinsic::AMXClearStartRow, Config, {Config});
    return true;
  }

  const auto Kind = computeKind(Insn->id);
  if (!Kind)
    return false;
  if (X86.op_count != 3 || X86.operands[0].type != X86_OP_REG ||
      X86.operands[1].type != X86_OP_REG ||
      X86.operands[2].type != X86_OP_REG)
    return false;

  const RegInfo Destination =
      mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
  const RegInfo Source1 =
      mapCapstoneReg(static_cast<x86_reg>(X86.operands[1].reg));
  const RegInfo Source2 =
      mapCapstoneReg(static_cast<x86_reg>(X86.operands[2].reg));
  if (!isTileRegister(Destination) || !isTileRegister(Source1) ||
      !isTileRegister(Source2) || Destination.Offset == Source1.Offset ||
      Destination.Offset == Source2.Offset || Source1.Offset == Source2.Offset)
    return false;

  const NdVar DestinationValue =
      NdVar::reg(Destination.Offset, x86reg::TileRegStride);
  const NdVar Source1Value =
      NdVar::reg(Source1.Offset, x86reg::TileRegStride);
  const NdVar Source2Value =
      NdVar::reg(Source2.Offset, x86reg::TileRegStride);
  const NdVar Config =
      NdVar::reg(x86reg::TileConfig, x86reg::TileConfigSize);
  S.emitIntrinsic(
      Intrinsic::AMXTileCompute, DestinationValue,
      {NdVar::cst(static_cast<uint8_t>(*Kind), 1), Config, DestinationValue,
       Source1Value, Source2Value});
  S.emitIntrinsic(Intrinsic::AMXClearStartRow, Config, {Config});
  return true;
}

} // namespace neverd
