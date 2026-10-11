//===- X86FPPackedConvertState.h - Packed conversion lifter -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticated SSE/VEX packed conversion, source access and MXCSR completion.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_LIFT_X86_X86FPPACKEDCONVERTSTATE_H
#define NEVERD_LIB_LIFT_X86_X86FPPACKEDCONVERTSTATE_H

#include "X86LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86FPState.h"
#include "neverd/lift/X86Lifter.h"

#include <array>
#include <optional>

namespace neverd {
inline std::optional<unsigned> x86PackedConvertKind(unsigned Id, bool Vex) {
#define PACKED_CONVERT(NAME, KIND)                                             \
  case X86_INS_##NAME:                                                         \
    return Vex ? std::nullopt : std::optional<unsigned>(KIND);                 \
  case X86_INS_V##NAME:                                                        \
    return Vex ? std::optional<unsigned>(KIND) : std::nullopt;
  switch (Id) {
    PACKED_CONVERT(CVTDQ2PS, 0)
    PACKED_CONVERT(CVTDQ2PD, 1)
    PACKED_CONVERT(CVTPS2PD, 2)
    PACKED_CONVERT(CVTPD2PS, 3)
    PACKED_CONVERT(CVTPS2DQ, 4)
    PACKED_CONVERT(CVTPD2DQ, 5)
    PACKED_CONVERT(CVTTPS2DQ, 6)
    PACKED_CONVERT(CVTTPD2DQ, 7)
  default:
    return std::nullopt;
  }
#undef PACKED_CONVERT
}
inline bool liftPackedConvertState(X86Lifter &L, X86Lifter::LiftState &S,
                                   const cs_insn *Insn, const cs_x86 &X86,
                                   unsigned Kind, bool Vex) {
  const auto Refuse = [&]() -> bool {
    throw UnliftedInstruction(Insn ? Insn->address : InvalidVA,
                              Insn ? Insn->mnemonic : nullptr,
                              Insn ? Insn->op_str : nullptr);
  };
  if (!Insn || !Insn->size || Insn->size > 15 || Kind > 7 ||
      (L.targetArch() != Arch::X86 && L.targetArch() != Arch::X64) ||
      X86.op_count != 2 || X86.operands[0].type != X86_OP_REG ||
      X86.encoding.imm_offset || X86.encoding.imm_size || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID || X86.operands[0].avx_zero_opmask ||
      X86.operands[1].avx_zero_opmask ||
      X86.operands[0].avx_bcast != X86_AVX_BCAST_INVALID ||
      X86.operands[1].avx_bcast != X86_AVX_BCAST_INVALID)
    return Refuse();
  constexpr std::array<uint8_t, 8> Opcodes{0x5b, 0xe6, 0x5a, 0x5a,
                                           0x5b, 0xe6, 0x5b, 0xe6};
  constexpr std::array<uint8_t, 8> Prefixes{0, 2, 0, 1, 1, 3, 2, 1};
  const bool Mode64 = L.targetArch() == Arch::X64;
  unsigned Offset = 0;
  uint8_t Segment = 0, Mandatory = 0, Rex = 0;
  bool Address = false;
  while (Offset < Insn->size) {
    const uint8_t Byte = Insn->bytes[Offset];
    if (Byte == 0x26 || Byte == 0x2e || Byte == 0x36 || Byte == 0x3e ||
        Byte == 0x64 || Byte == 0x65) {
      if (Segment || Rex)
        return Refuse();
      Segment = Byte;
    } else if (Byte == 0x67) {
      if (Address || Rex)
        return Refuse();
      Address = true;
    } else if (!Vex && (Byte == 0x66 || Byte == 0xf2 || Byte == 0xf3)) {
      if (Mandatory || Rex)
        return Refuse();
      Mandatory = Byte;
    } else if (!Vex && Mode64 && (Byte & 0xf0) == 0x40) {
      if (Rex)
        return Refuse();
      Rex = Byte;
    } else
      break;
    ++Offset;
  }
  bool R = (Rex & 4) != 0, B = (Rex & 1) != 0, X = (Rex & 2) != 0;
  bool Wide = false;
  if (Vex) {
    if (Offset + 3 >= Insn->size)
      return Refuse();
    uint8_t P1;
    if (Insn->bytes[Offset] == 0xc5) {
      P1 = Insn->bytes[Offset + 1];
      R = (P1 & 0x80) == 0;
      Offset += 2;
    } else if (Insn->bytes[Offset] == 0xc4) {
      if (Offset + 4 >= Insn->size || (Insn->bytes[Offset + 1] & 31) != 1)
        return Refuse();
      const auto P0 = Insn->bytes[Offset + 1];
      P1 = Insn->bytes[Offset + 2];
      R = !(P0 & 0x80);
      B = !(P0 & 0x20);
      X = !(P0 & 0x40);
      Offset += 3;
    } else
      return Refuse();
    if ((P1 & 0x78) != 0x78 || (P1 & 3) != Prefixes[Kind] ||
        (!Mode64 && (R || B || X)))
      return Refuse();
    Wide = (P1 & 4) != 0;
  } else {
    const uint8_t Expected = Prefixes[Kind] == 1   ? 0x66
                             : Prefixes[Kind] == 2 ? 0xf3
                             : Prefixes[Kind] == 3 ? 0xf2
                                                   : 0;
    if (Mandatory != Expected || Offset + 2 >= Insn->size ||
        Insn->bytes[Offset++] != 0x0f)
      return Refuse();
  }
  const unsigned Control = Kind | (Vex ? 8 : 0) | (Wide ? 16 : 0);
  const unsigned SourceBytes = x86FPPackedConvertSourceBytes(Control);
  const unsigned DestinationBytes = x86FPPackedConvertDestinationBytes(Control);
  if (Offset + 1 >= Insn->size || Insn->bytes[Offset] != Opcodes[Kind] ||
      X86.encoding.modrm_offset != Offset + 1 ||
      Insn->bytes[Offset + 1] != X86.modrm)
    return Refuse();
  const unsigned Destination = ((X86.modrm >> 3) & 7) | (R ? 8 : 0);
  if (X86.operands[0].size != DestinationBytes ||
      X86.operands[0].reg !=
          (DestinationBytes == 32 ? X86_REG_YMM0 : X86_REG_XMM0) + Destination)
    return Refuse();
  const auto &Operand = X86.operands[1];
  if ((X86.modrm >> 6) == 3) {
    const unsigned RegisterBytes = SourceBytes == 32 ? 32 : 16;
    const unsigned Register = (X86.modrm & 7) | (B ? 8 : 0);
    if (Operand.type != X86_OP_REG || Operand.size != RegisterBytes ||
        Operand.reg !=
            (RegisterBytes == 32 ? X86_REG_YMM0 : X86_REG_XMM0) + Register)
      return Refuse();
  } else if (Operand.type != X86_OP_MEM || Operand.size != SourceBytes)
    return Refuse();
  if (!validateCanonicalScalarConversionTail(
          Insn, X86, Offset + 2, Segment, Mode64,
          Mode64 ? (Address ? 4 : 8) : (Address ? 2 : 4), B ? 8 : 0, X ? 8 : 0,
          Operand))
    return Refuse();
  const bool Memory = Operand.type == X86_OP_MEM;
  auto Source = Memory ? S.computeEA(Operand) : L.operandRead(S, Operand);
  if (!Memory && Source.Size != SourceBytes) {
    auto Narrow = S.makeTemp(SourceBytes);
    S.emit(NdOp::SUBBYTES, Narrow, {Source, NdVar::cst(0, 4)});
    Source = Narrow;
  }
  auto Incoming = S.makeTemp(4), Completed = S.makeTemp(DestinationBytes + 4);
  S.emitIntrinsic(Intrinsic::X86ReadMXCSR, Incoming);
  S.emitIntrinsic(Memory ? Intrinsic::X86FPPackedConvertMemoryState
                         : Intrinsic::X86FPPackedConvertState,
                  Completed, {Source, NdVar::cst(Control, 1), Incoming},
                  NdMemoryOrdering::None,
                  Memory ? S.memoryAddressSpace(Operand)
                         : NdMemoryAddressSpace::Default);
  auto Number = S.makeTemp(DestinationBytes), Outgoing = S.makeTemp(4);
  S.emit(NdOp::SUBBYTES, Number, {Completed, NdVar::cst(0, 4)});
  S.emit(NdOp::SUBBYTES, Outgoing,
         {Completed, NdVar::cst(DestinationBytes, 4)});
  S.emitVoidIntrinsic(Intrinsic::X86WriteMXCSR, {Outgoing});
  S.emit(NdOp::COPY, L.operandWrite(X86.operands[0]), {Number});
  return true;
}
} // namespace neverd
#endif
