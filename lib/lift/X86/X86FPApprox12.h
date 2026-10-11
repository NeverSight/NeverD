//===- X86FPApprox12.h - Legacy/VEX reciprocal approximation -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86FPAPPROX12_H
#define NEVERD_LIB_LIFT_X86_X86FPAPPROX12_H

#include "X86LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86FPState.h"
#include "neverd/lift/X86Lifter.h"

namespace neverd {
inline bool liftFPApprox12(X86Lifter &L, X86Lifter::LiftState &S,
                           const cs_insn *Insn, const cs_x86 &X86, bool Rsqrt,
                           bool Scalar, bool Vex) {
  const auto Refuse = [&]() -> bool {
    throw UnliftedInstruction(Insn ? Insn->address : InvalidVA,
                              Insn ? Insn->mnemonic : nullptr,
                              Insn ? Insn->op_str : nullptr);
  };
  const unsigned SourceIndex = Vex && Scalar ? 2 : 1;
  if (!Insn || !Insn->size || Insn->size > 15 ||
      (L.targetArch() != Arch::X86 && L.targetArch() != Arch::X64) ||
      X86.op_count != SourceIndex + 1 || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID || X86.operands[0].type != X86_OP_REG)
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
    } else if (!Vex && Byte == 0xf3) {
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
  unsigned Width = 16;
  if (Vex) {
    if (Offset + 3 >= Insn->size)
      return Refuse();
    uint8_t P1 = 0;
    if (Insn->bytes[Offset] == 0xc5) {
      P1 = Insn->bytes[Offset + 1];
      R = (P1 & 0x80) == 0;
      B = X = false;
      Offset += 2;
    } else if (Insn->bytes[Offset] == 0xc4) {
      if (Offset + 4 >= Insn->size)
        return Refuse();
      const uint8_t P0 = Insn->bytes[Offset + 1];
      if ((P0 & 0x1f) != 1 || (!Mode64 && (P0 & 0xe0) != 0xe0))
        return Refuse();
      R = (P0 & 0x80) == 0;
      B = (P0 & 0x20) == 0;
      X = (P0 & 0x40) == 0;
      P1 = Insn->bytes[Offset + 2];
      Offset += 3;
    } else
      return Refuse();
    if ((P1 & 3) != (Scalar ? 2 : 0) ||
        (Scalar ? (P1 & 4) != 0 : (P1 & 0x78) != 0x78) || (!Mode64 && R))
      return Refuse();
    Width = (P1 & 4) ? 32 : 16;
    const unsigned Upper = (~P1 >> 3) & 15;
    if (Scalar &&
        ((!Mode64 && Upper >= 8) || X86.operands[1].type != X86_OP_REG ||
         X86.operands[1].size != 16 ||
         X86.operands[1].reg != X86_REG_XMM0 + Upper))
      return Refuse();
  } else {
    if (Mandatory != Scalar || Offset + 2 >= Insn->size ||
        Insn->bytes[Offset] != 0x0f)
      return Refuse();
    ++Offset;
  }
  if (Offset + 1 >= Insn->size ||
      Insn->bytes[Offset] != (Rsqrt ? 0x52 : 0x53) ||
      X86.encoding.modrm_offset != Offset + 1 ||
      Insn->bytes[Offset + 1] != X86.modrm)
    return Refuse();
  const unsigned RegisterBase = Width == 32 ? X86_REG_YMM0 : X86_REG_XMM0;
  if (X86.operands[0].size != Width ||
      X86.operands[0].reg !=
          RegisterBase + ((X86.modrm >> 3) & 7) + (R ? 8 : 0))
    return Refuse();
  const unsigned Bytes = Scalar ? 4 : Width;
  const auto &Operand = X86.operands[SourceIndex];
  if ((X86.modrm >> 6) == 3) {
    if (Operand.type != X86_OP_REG || Operand.size != Width ||
        Operand.reg != RegisterBase + (X86.modrm & 7) + (B ? 8 : 0))
      return Refuse();
  } else if (Operand.type != X86_OP_MEM || Operand.size != Bytes)
    return Refuse();
  const unsigned AddressSize = Mode64 ? (Address ? 4 : 8) : (Address ? 2 : 4);
  if (!validateCanonicalScalarConversionTail(Insn, X86, Offset + 2, Segment,
                                             Mode64, AddressSize, B ? 8 : 0,
                                             X ? 8 : 0, Operand))
    return Refuse();
  const bool Memory = Operand.type == X86_OP_MEM;
  auto Source = Memory ? S.computeEA(Operand) : L.operandRead(S, Operand);
  if (!Memory && Source.Size != Bytes) {
    const auto Low = S.makeTemp(Bytes);
    S.emit(NdOp::SUBBYTES, Low, {Source, NdVar::cst(0, 4)});
    Source = Low;
  }
  const unsigned Control =
      unsigned(Rsqrt) | (Scalar ? 2 : 0) | (Memory && Vex && !Scalar ? 4 : 0);
  const auto Number = S.makeTemp(Bytes);
  if (Memory)
    S.emitIntrinsic(Intrinsic::X86FPApprox12MemoryState, Number,
                    {Source, NdVar::cst(Control, 1)}, NdMemoryOrdering::None,
                    S.memoryAddressSpace(Operand));
  else
    S.emitIntrinsic(Intrinsic::X86FPApprox12State, Number,
                    {NdVar::cst(Control, 1), Source});
  const auto Destination = L.operandWrite(X86.operands[0]);
  if (Scalar) {
    const auto UpperSource = L.operandRead(S, X86.operands[Vex ? 1 : 0]);
    const auto Upper = S.makeTemp(12);
    S.emit(NdOp::SUBBYTES, Upper, {UpperSource, NdVar::cst(4, 4)});
    S.emit(NdOp::CONCAT, Destination, {Upper, Number});
  } else
    S.emit(NdOp::COPY, Destination, {Number});
  return true;
}
} // namespace neverd
#endif
