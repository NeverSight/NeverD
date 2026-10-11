//===- X86FPArithState.h - Legacy/VEX FP instruction completion -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86FPARITHSTATE_H
#define NEVERD_LIB_LIFT_X86_X86FPARITHSTATE_H

#include "X86LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86FPState.h"
#include "neverd/lift/X86Lifter.h"

namespace neverd {
struct FPArithStateSpec {
  X86FPArithKind Kind;
  uint8_t Opcode;
  bool Double;
  bool Scalar;
  bool Vex;
  unsigned Topology = 0;
};

inline bool getFPArithStateSpec(unsigned Id, FPArithStateSpec &Spec) {
  switch (Id) {
#define FP_FAMILY(Name, Opcode, Kind)                                          \
  case X86_INS_##Name##PS:                                                     \
    Spec = {X86FPArithKind::Kind, Opcode, false, false, false};                \
    return true;                                                               \
  case X86_INS_##Name##PD:                                                     \
    Spec = {X86FPArithKind::Kind, Opcode, true, false, false};                 \
    return true;                                                               \
  case X86_INS_##Name##SS:                                                     \
    Spec = {X86FPArithKind::Kind, Opcode, false, true, false};                 \
    return true;                                                               \
  case X86_INS_##Name##SD:                                                     \
    Spec = {X86FPArithKind::Kind, Opcode, true, true, false};                  \
    return true;                                                               \
  case X86_INS_V##Name##PS:                                                    \
    Spec = {X86FPArithKind::Kind, Opcode, false, false, true};                 \
    return true;                                                               \
  case X86_INS_V##Name##PD:                                                    \
    Spec = {X86FPArithKind::Kind, Opcode, true, false, true};                  \
    return true;                                                               \
  case X86_INS_V##Name##SS:                                                    \
    Spec = {X86FPArithKind::Kind, Opcode, false, true, true};                  \
    return true;                                                               \
  case X86_INS_V##Name##SD:                                                    \
    Spec = {X86FPArithKind::Kind, Opcode, true, true, true};                   \
    return true;
    FP_FAMILY(ADD, 0x58, Add)
    FP_FAMILY(SUB, 0x5c, Subtract)
    FP_FAMILY(MUL, 0x59, Multiply)
    FP_FAMILY(DIV, 0x5e, Divide)
    FP_FAMILY(SQRT, 0x51, SquareRoot)
    FP_FAMILY(MIN, 0x5d, Minimum)
    FP_FAMILY(MAX, 0x5f, Maximum)
#undef FP_FAMILY
#define FP_PACKED(Name, Opcode, Kind, Topology)                                \
  case X86_INS_##Name##PS:                                                     \
    Spec = {X86FPArithKind::Kind, Opcode, false, false, false, Topology};      \
    return true;                                                               \
  case X86_INS_##Name##PD:                                                     \
    Spec = {X86FPArithKind::Kind, Opcode, true, false, false, Topology};       \
    return true;                                                               \
  case X86_INS_V##Name##PS:                                                    \
    Spec = {X86FPArithKind::Kind, Opcode, false, false, true, Topology};       \
    return true;                                                               \
  case X86_INS_V##Name##PD:                                                    \
    Spec = {X86FPArithKind::Kind, Opcode, true, false, true, Topology};        \
    return true;
    FP_PACKED(HADD, 0x7c, Add, 64)
    FP_PACKED(HSUB, 0x7d, Subtract, 64)
    FP_PACKED(ADDSUB, 0xd0, Add, 128)
#undef FP_PACKED
  default:
    return false;
  }
}

inline bool liftFPArithState(X86Lifter &L, X86Lifter::LiftState &S,
                             const cs_insn *Insn, const cs_x86 &X86) {
  const auto Refuse = [&]() -> bool {
    throw UnliftedInstruction(Insn ? Insn->address : InvalidVA,
                              Insn ? Insn->mnemonic : nullptr,
                              Insn ? Insn->op_str : nullptr);
  };
  FPArithStateSpec Spec{};
  if (!Insn || !getFPArithStateSpec(Insn->id, Spec) || !Insn->size ||
      Insn->size > 15 ||
      (L.targetArch() != Arch::X86 && L.targetArch() != Arch::X64) ||
      X86.avx_sae || X86.avx_rm != X86_AVX_RM_INVALID)
    return Refuse();
  const bool Unary = Spec.Kind == X86FPArithKind::SquareRoot;
  const bool HasVvvv = Spec.Vex && (!Unary || Spec.Scalar);
  const unsigned SourceIndex = HasVvvv ? 2 : 1;
  if (X86.op_count != SourceIndex + 1 || X86.operands[0].type != X86_OP_REG)
    return Refuse();
  for (unsigned Index = 0; Index < X86.op_count; ++Index)
    if (X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID ||
        X86.operands[Index].avx_zero_opmask)
      return Refuse();
  const bool Mode64 = L.targetArch() == Arch::X64;
  const unsigned PP = Spec.Topology ? (Spec.Double ? 1 : 3)
                      : Spec.Scalar ? (Spec.Double ? 3 : 2)
                                    : (Spec.Double ? 1 : 0);
  const uint8_t MandatoryByte = PP == 1   ? 0x66
                                : PP == 2 ? 0xf3
                                : PP == 3 ? 0xf2
                                          : 0;
  unsigned Offset = 0;
  uint8_t Segment = 0, Rex = 0;
  bool Address = false, Mandatory = false;
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
    } else if (!Spec.Vex && MandatoryByte && Byte == MandatoryByte) {
      if (Mandatory || Rex)
        return Refuse();
      Mandatory = true;
    } else if (!Spec.Vex && Mode64 && (Byte & 0xf0) == 0x40) {
      if (Rex)
        return Refuse();
      Rex = Byte;
    } else
      break;
    ++Offset;
  }
  bool R = (Rex & 4) != 0, B = (Rex & 1) != 0, X = (Rex & 2) != 0;
  unsigned Width = 16;
  if (Spec.Vex) {
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
      const auto P0 = Insn->bytes[Offset + 1];
      if ((P0 & 0x1f) != 1 || (!Mode64 && (P0 & 0xe0) != 0xe0))
        return Refuse();
      R = (P0 & 0x80) == 0;
      B = (P0 & 0x20) == 0;
      X = (P0 & 0x40) == 0;
      P1 = Insn->bytes[Offset + 2];
      Offset += 3;
    } else
      return Refuse();
    if ((P1 & 3) != PP || (Spec.Scalar && (P1 & 4)) ||
        (!HasVvvv && (P1 & 0x78) != 0x78) || (!Mode64 && R))
      return Refuse();
    Width = (P1 & 4) ? 32 : 16;
    const unsigned Left = (~P1 >> 3) & 15;
    if (HasVvvv &&
        ((!Mode64 && Left >= 8) || X86.operands[1].type != X86_OP_REG ||
         X86.operands[1].size != Width ||
         X86.operands[1].reg !=
             (Width == 32 ? X86_REG_YMM0 : X86_REG_XMM0) + Left))
      return Refuse();
  } else {
    if (Mandatory != (MandatoryByte != 0) || Offset + 2 >= Insn->size ||
        Insn->bytes[Offset] != 0x0f)
      return Refuse();
    ++Offset;
  }
  if (Offset + 1 >= Insn->size || Insn->bytes[Offset] != Spec.Opcode ||
      X86.encoding.modrm_offset != Offset + 1 ||
      Insn->bytes[Offset + 1] != X86.modrm)
    return Refuse();
  const unsigned Base = Width == 32 ? X86_REG_YMM0 : X86_REG_XMM0;
  if (X86.operands[0].size != Width ||
      X86.operands[0].reg != Base + ((X86.modrm >> 3) & 7) + (R ? 8 : 0))
    return Refuse();
  const unsigned Element = Spec.Double ? 8 : 4;
  const unsigned Bytes = Spec.Scalar ? Element : Width;
  const auto &RHSOperand = X86.operands[SourceIndex];
  if ((X86.modrm >> 6) == 3) {
    if (RHSOperand.type != X86_OP_REG || RHSOperand.size != Width ||
        RHSOperand.reg != Base + (X86.modrm & 7) + (B ? 8 : 0))
      return Refuse();
  } else if (RHSOperand.type != X86_OP_MEM || RHSOperand.size != Bytes)
    return Refuse();
  if (!validateCanonicalScalarConversionTail(
          Insn, X86, Offset + 2, Segment, Mode64,
          Mode64 ? (Address ? 4 : 8) : (Address ? 2 : 4), B ? 8 : 0, X ? 8 : 0,
          RHSOperand))
    return Refuse();
  const bool Memory = RHSOperand.type == X86_OP_MEM;
  const unsigned LeftIndex = Spec.Vex ? 1 : 0;
  const auto Slice = [&](NdVar Input) {
    if (Input.Size == Bytes)
      return Input;
    const auto Value = S.makeTemp(Bytes);
    S.emit(NdOp::SUBBYTES, Value, {Input, NdVar::cst(0, 4)});
    return Value;
  };
  const auto Left = Unary ? NdVar::cst(0, Bytes)
                          : Slice(L.operandRead(S, X86.operands[LeftIndex]));
  const auto Right =
      Memory ? S.computeEA(RHSOperand) : Slice(L.operandRead(S, RHSOperand));
  const auto Incoming = S.makeTemp(4);
  S.emitIntrinsic(Intrinsic::X86ReadMXCSR, Incoming);
  const auto Result = S.makeTemp(Bytes + 4);
  const unsigned Control =
      unsigned(Spec.Kind) | (Spec.Double ? 8 : 0) | (Spec.Scalar ? 16 : 0) |
      (Memory && Spec.Vex && !Spec.Scalar ? 32 : 0) | Spec.Topology;
  if (Memory)
    S.emitIntrinsic(Intrinsic::X86FPArithMemoryState, Result,
                    {Right, NdVar::cst(Control, 1), Left, Incoming},
                    NdMemoryOrdering::None, S.memoryAddressSpace(RHSOperand));
  else if (!Spec.Vex && Spec.Scalar &&
           unsigned(Spec.Kind) < unsigned(X86FPArithKind::FusedMultiplyAdd)) {
    const Intrinsic Old =
        Spec.Kind == X86FPArithKind::Add        ? Intrinsic::X86FPAddState
        : Spec.Kind == X86FPArithKind::Subtract ? Intrinsic::X86FPSubState
        : Spec.Kind == X86FPArithKind::Multiply ? Intrinsic::X86FPMulState
                                                : Intrinsic::X86FPDivState;
    S.emitIntrinsic(Old, Result, {Left, Right, Incoming});
  } else
    S.emitIntrinsic(Intrinsic::X86FPArithState, Result,
                    {NdVar::cst(Control, 1), Left, Right, Incoming});
  const auto Number = S.makeTemp(Bytes), State = S.makeTemp(4);
  S.emit(NdOp::SUBBYTES, Number, {Result, NdVar::cst(0, 4)});
  S.emit(NdOp::SUBBYTES, State, {Result, NdVar::cst(Bytes, 4)});
  S.emitVoidIntrinsic(Intrinsic::X86WriteMXCSR, {State});
  const auto Destination = L.operandWrite(X86.operands[0]);
  if (Spec.Scalar) {
    const auto Upper = S.makeTemp(16 - Element);
    S.emit(NdOp::SUBBYTES, Upper,
           {L.operandRead(S, X86.operands[LeftIndex]), NdVar::cst(Element, 4)});
    S.emit(NdOp::CONCAT, Destination, {Upper, Number});
  } else
    S.emit(NdOp::COPY, Destination, {Number});
  return true;
}
} // namespace neverd
#endif
