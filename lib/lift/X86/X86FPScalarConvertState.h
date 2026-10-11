//===- X86FPScalarConvertState.h - Scalar FP conversion completion -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticated scalar FP conversion, memory and MXCSR completion.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_LIFT_X86_X86FPSCALARCONVERTSTATE_H
#define NEVERD_LIB_LIFT_X86_X86FPSCALARCONVERTSTATE_H

#include "X86LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86FPState.h"
#include "neverd/lift/X86Lifter.h"

#include <array>

namespace neverd {
inline bool liftScalarConvertToFPState(X86Lifter &L, X86Lifter::LiftState &S,
                                       const cs_insn *Insn, const cs_x86 &X86,
                                       bool Integer, unsigned DestinationBytes,
                                       bool Vex) {
  const auto Refuse = [&]() -> bool {
    throw UnliftedInstruction(Insn ? Insn->address : InvalidVA,
                              Insn ? Insn->mnemonic : nullptr,
                              Insn ? Insn->op_str : nullptr);
  };
  if (!Insn || Insn->size == 0 || Insn->size > 15 ||
      (L.targetArch() != Arch::X64 && L.targetArch() != Arch::X86) ||
      (DestinationBytes != 4 && DestinationBytes != 8) ||
      X86.op_count != (Vex ? 3 : 2) || X86.operands[0].type != X86_OP_REG ||
      X86.operands[0].size != 16 || X86.encoding.imm_offset ||
      X86.encoding.imm_size || X86.avx_sae || X86.avx_rm != X86_AVX_RM_INVALID)
    return Refuse();
  const bool Mode64 = L.targetArch() == Arch::X64;
  unsigned Offset = 0;
  uint8_t Rex = 0, Mandatory = 0, Segment = 0;
  bool Address = false, Operand = false;
  while (Offset < Insn->size) {
    const auto Byte = Insn->bytes[Offset];
    if (Byte == 0x26 || Byte == 0x2e || Byte == 0x36 || Byte == 0x3e ||
        Byte == 0x64 || Byte == 0x65) {
      if (Segment || Rex)
        return Refuse();
      Segment = Byte;
    } else if (Byte == 0x67) {
      if (Address || Rex)
        return Refuse();
      Address = true;
    } else if (!Vex && Byte == 0x66) {
      if (Operand || Rex)
        return Refuse();
      Operand = true;
    } else if (!Vex && (Byte == 0xf2 || Byte == 0xf3)) {
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
  bool W = (Rex & 8) != 0, R = (Rex & 4) != 0;
  bool B = (Rex & 1) != 0, X = (Rex & 2) != 0;
  const bool PrefixDouble =
      Integer ? DestinationBytes == 8 : DestinationBytes == 4;
  if (Vex) {
    if (Offset + 3 >= Insn->size)
      return Refuse();
    uint8_t P1;
    if (Insn->bytes[Offset] == 0xc5) {
      P1 = Insn->bytes[Offset + 1];
      R = (P1 & 0x80) == 0;
      Offset += 2;
    } else if (Insn->bytes[Offset] == 0xc4) {
      if (Offset + 4 >= Insn->size || (Insn->bytes[Offset + 1] & 0x1f) != 1)
        return Refuse();
      const auto P0 = Insn->bytes[Offset + 1];
      P1 = Insn->bytes[Offset + 2];
      R = (P0 & 0x80) == 0;
      B = (P0 & 0x20) == 0;
      X = (P0 & 0x40) == 0;
      W = (P1 & 0x80) != 0;
      Offset += 3;
    } else
      return Refuse();
    const unsigned Merge = (~(P1 >> 3)) & 15;
    if ((P1 & 4) || (P1 & 3) != (PrefixDouble ? 3 : 2) ||
        (!Mode64 && (R || B || X || Merge >= 8)) ||
        X86.operands[1].type != X86_OP_REG || X86.operands[1].size != 16 ||
        X86.operands[1].reg != X86_REG_XMM0 + Merge)
      return Refuse();
  } else if (Mandatory != (PrefixDouble ? 0xf2 : 0xf3) ||
             Offset + 2 >= Insn->size || Insn->bytes[Offset++] != 0x0f)
    return Refuse();
  if (Offset + 1 >= Insn->size ||
      Insn->bytes[Offset] != (Integer ? 0x2a : 0x5a) ||
      X86.encoding.modrm_offset != Offset + 1 ||
      Insn->bytes[Offset + 1] != X86.modrm)
    return Refuse();
  const unsigned DestinationReg = ((X86.modrm >> 3) & 7) | (R ? 8 : 0);
  if (X86.operands[0].reg != X86_REG_XMM0 + DestinationReg)
    return Refuse();
  const unsigned SourceBytes =
      Integer ? ((Mode64 && W) ? 8 : 4) : (DestinationBytes == 8 ? 4 : 8);
  const auto &SourceOperand = X86.operands[Vex ? 2 : 1];
  constexpr std::array<unsigned, 16> GPR32 = {
      X86_REG_EAX,  X86_REG_ECX,  X86_REG_EDX,  X86_REG_EBX,
      X86_REG_ESP,  X86_REG_EBP,  X86_REG_ESI,  X86_REG_EDI,
      X86_REG_R8D,  X86_REG_R9D,  X86_REG_R10D, X86_REG_R11D,
      X86_REG_R12D, X86_REG_R13D, X86_REG_R14D, X86_REG_R15D};
  constexpr std::array<unsigned, 16> GPR64 = {
      X86_REG_RAX, X86_REG_RCX, X86_REG_RDX, X86_REG_RBX,
      X86_REG_RSP, X86_REG_RBP, X86_REG_RSI, X86_REG_RDI,
      X86_REG_R8,  X86_REG_R9,  X86_REG_R10, X86_REG_R11,
      X86_REG_R12, X86_REG_R13, X86_REG_R14, X86_REG_R15};
  if ((X86.modrm >> 6) == 3) {
    const unsigned Reg = (X86.modrm & 7) | (B ? 8 : 0);
    if (SourceOperand.type != X86_OP_REG ||
        SourceOperand.reg != (Integer
                                  ? (SourceBytes == 8 ? GPR64[Reg] : GPR32[Reg])
                                  : X86_REG_XMM0 + Reg) ||
        SourceOperand.size != (Integer ? SourceBytes : 16))
      return Refuse();
  } else if (SourceOperand.type != X86_OP_MEM ||
             SourceOperand.size != SourceBytes)
    return Refuse();
  if (!validateCanonicalScalarConversionTail(
          Insn, X86, Offset + 2, Segment, Mode64,
          Mode64 ? (Address ? 4 : 8) : (Address ? 2 : 4), B ? 8 : 0, X ? 8 : 0,
          SourceOperand))
    return Refuse();
  const bool Memory = SourceOperand.type == X86_OP_MEM;
  auto Source =
      Memory ? S.computeEA(SourceOperand) : L.operandRead(S, SourceOperand);
  if (!Memory && Source.Size != SourceBytes) {
    const auto Narrow = S.makeTemp(SourceBytes);
    S.emit(NdOp::SUBBYTES, Narrow, {Source, NdVar::cst(0, 4)});
    Source = Narrow;
  }
  const auto Incoming = S.makeTemp(4);
  S.emitIntrinsic(Intrinsic::X86ReadMXCSR, Incoming);
  const unsigned Control = (Integer ? 1 : 0) | (SourceBytes == 8 ? 2 : 0) |
                           (DestinationBytes == 8 ? 4 : 0) | (Vex ? 8 : 0);
  const auto Completed = S.makeTemp(DestinationBytes + 4);
  S.emitIntrinsic(Memory ? Intrinsic::X86FPScalarConvertMemoryState
                         : Intrinsic::X86FPScalarConvertState,
                  Completed, {Source, NdVar::cst(Control, 1), Incoming},
                  NdMemoryOrdering::None,
                  Memory ? S.memoryAddressSpace(SourceOperand)
                         : NdMemoryAddressSpace::Default);
  const auto Number = S.makeTemp(DestinationBytes), Outgoing = S.makeTemp(4);
  S.emit(NdOp::SUBBYTES, Number, {Completed, NdVar::cst(0, 4)});
  S.emit(NdOp::SUBBYTES, Outgoing,
         {Completed, NdVar::cst(DestinationBytes, 4)});
  S.emitVoidIntrinsic(Intrinsic::X86WriteMXCSR, {Outgoing});
  const auto Destination = L.operandWrite(X86.operands[0]);
  const auto Upper = S.makeTemp(16 - DestinationBytes);
  S.emit(NdOp::SUBBYTES, Upper,
         {L.operandRead(S, X86.operands[Vex ? 1 : 0]),
          NdVar::cst(DestinationBytes, 4)});
  S.emit(NdOp::CONCAT, Destination, {Upper, Number});
  return true;
}
} // namespace neverd
#endif
