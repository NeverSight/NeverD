//===- X86FPRoundState.h - ROUND numerical/state completion -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86FPROUNDSTATE_H
#define NEVERD_LIB_LIFT_X86_X86FPROUNDSTATE_H

#include "X86LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86FPState.h"
#include "neverd/lift/X86Lifter.h"

namespace neverd {
inline bool liftFPRoundState(X86Lifter &L, X86Lifter::LiftState &S,
                             const cs_insn *Insn, const cs_x86 &X86,
                             bool Double, bool Scalar, bool Vex) {
  const auto Refuse = [&]() -> bool {
    throw UnliftedInstruction(Insn ? Insn->address : InvalidVA,
                              Insn ? Insn->mnemonic : nullptr,
                              Insn ? Insn->op_str : nullptr);
  };
  const unsigned SourceIndex = Vex && Scalar ? 2 : 1;
  const unsigned ImmediateIndex = SourceIndex + 1;
  if (!Insn || Insn->size == 0 || Insn->size > 15 ||
      (L.targetArch() != Arch::X64 && L.targetArch() != Arch::X86) ||
      X86.op_count != ImmediateIndex + 1 || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID || X86.operands[0].type != X86_OP_REG ||
      X86.operands[ImmediateIndex].type != X86_OP_IMM ||
      X86.encoding.imm_size != 1 || X86.encoding.imm_offset != Insn->size - 1 ||
      X86.operands[ImmediateIndex].imm != Insn->bytes[Insn->size - 1])
    return Refuse();
  const bool Mode64 = L.targetArch() == Arch::X64;
  unsigned Offset = 0;
  uint8_t Segment = 0, Rex = 0;
  bool Address = false, Mandatory = false;
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
    } else if (!Vex && Byte == 0x66) {
      if (Mandatory || Rex)
        return Refuse();
      Mandatory = true;
    } else if (!Vex && Mode64 && (Byte & 0xf0) == 0x40) {
      if (Rex)
        return Refuse();
      Rex = Byte;
    } else
      break;
    ++Offset;
  }
  bool R = (Rex & 4) != 0, B = (Rex & 1) != 0, X = (Rex & 2) != 0;
  unsigned Width = 16, UpperRegister = 0;
  if (Vex) {
    if (Offset + 5 >= Insn->size || Insn->bytes[Offset] != 0xc4)
      return Refuse();
    const uint8_t P0 = Insn->bytes[Offset + 1];
    const uint8_t P1 = Insn->bytes[Offset + 2];
    if ((P0 & 0x1f) != 3 || (P1 & 3) != 1 || (!Mode64 && (P0 & 0xe0) != 0xe0) ||
        (Scalar ? (P1 & 4) != 0 : (P1 & 0x78) != 0x78))
      return Refuse();
    R = (P0 & 0x80) == 0;
    B = (P0 & 0x20) == 0;
    X = (P0 & 0x40) == 0;
    Width = (P1 & 4) ? 32 : 16;
    UpperRegister = (~P1 >> 3) & 15;
    if (Scalar &&
        ((!Mode64 && UpperRegister >= 8) ||
         X86.operands[1].type != X86_OP_REG || X86.operands[1].size != 16 ||
         X86.operands[1].reg != X86_REG_XMM0 + UpperRegister))
      return Refuse();
    Offset += 3;
  } else {
    if (!Mandatory || Offset + 3 >= Insn->size || Insn->bytes[Offset] != 0x0f ||
        Insn->bytes[Offset + 1] != 0x3a)
      return Refuse();
    Offset += 2;
  }
  const unsigned Opcode = (Scalar ? 0x0a : 0x08) + unsigned(Double);
  if (Offset + 2 >= Insn->size || Insn->bytes[Offset] != Opcode ||
      X86.encoding.modrm_offset != Offset + 1 ||
      Insn->bytes[Offset + 1] != X86.modrm)
    return Refuse();
  const unsigned RegisterBase = Width == 32 ? X86_REG_YMM0 : X86_REG_XMM0;
  if (X86.operands[0].size != Width ||
      X86.operands[0].reg !=
          RegisterBase + ((X86.modrm >> 3) & 7) + (R ? 8 : 0))
    return Refuse();
  const unsigned NumericalBytes = Scalar ? (Double ? 8 : 4) : Width;
  const auto &SourceOperand = X86.operands[SourceIndex];
  if ((X86.modrm >> 6) == 3) {
    if (SourceOperand.type != X86_OP_REG || SourceOperand.size != Width ||
        SourceOperand.reg != RegisterBase + (X86.modrm & 7) + (B ? 8 : 0))
      return Refuse();
  } else if (SourceOperand.type != X86_OP_MEM ||
             SourceOperand.size != NumericalBytes)
    return Refuse();
  const unsigned AddressSize = Mode64 ? (Address ? 4 : 8) : (Address ? 2 : 4);
  if (!validateCanonicalScalarConversionTail(Insn, X86, Offset + 2, Segment,
                                             Mode64, AddressSize, B ? 8 : 0,
                                             X ? 8 : 0, SourceOperand, 1))
    return Refuse();

  const bool Memory = SourceOperand.type == X86_OP_MEM;
  auto Source =
      Memory ? S.computeEA(SourceOperand) : L.operandRead(S, SourceOperand);
  if (!Memory && Source.Size != NumericalBytes) {
    const auto Narrow = S.makeTemp(NumericalBytes);
    S.emit(NdOp::SUBBYTES, Narrow, {Source, NdVar::cst(0, 4)});
    Source = Narrow;
  }
  const auto Incoming = S.makeTemp(4);
  S.emitIntrinsic(Intrinsic::X86ReadMXCSR, Incoming, {});
  const auto Completed = S.makeTemp(NumericalBytes + 4);
  const unsigned Control =
      makeX86FPRoundTransformControl(X86FPRoundTransformKind::RoundScale,
                                     Double, Scalar, false) |
      (Memory && Vex && !Scalar ? 8 : 0);
  if (Memory)
    S.emitIntrinsic(Intrinsic::X86FPRoundMemoryState, Completed,
                    {Source, NdVar::cst(Control, 1),
                     NdVar::cst(Insn->bytes[Insn->size - 1], 1), Incoming},
                    NdMemoryOrdering::None,
                    S.memoryAddressSpace(SourceOperand));
  else
    S.emitIntrinsic(Intrinsic::X86FPRoundState, Completed,
                    {NdVar::cst(Control, 1), Source,
                     NdVar::cst(Insn->bytes[Insn->size - 1], 1), Incoming});
  const auto Number = S.makeTemp(NumericalBytes);
  const auto Outgoing = S.makeTemp(4);
  S.emit(NdOp::SUBBYTES, Number, {Completed, NdVar::cst(0, 4)});
  S.emit(NdOp::SUBBYTES, Outgoing, {Completed, NdVar::cst(NumericalBytes, 4)});
  const auto Destination = L.operandWrite(X86.operands[0]);
  if (Scalar) {
    const auto UpperSource = L.operandRead(S, X86.operands[Vex ? 1 : 0]);
    const auto Upper = S.makeTemp(Width - NumericalBytes);
    S.emit(NdOp::SUBBYTES, Upper, {UpperSource, NdVar::cst(NumericalBytes, 4)});
    S.emit(NdOp::CONCAT, Destination, {Upper, Number});
  } else
    S.emit(NdOp::COPY, Destination, {Number});
  S.emitIntrinsic(Intrinsic::X86WriteMXCSR, {}, {Outgoing});
  return true;
}
} // namespace neverd
#endif
