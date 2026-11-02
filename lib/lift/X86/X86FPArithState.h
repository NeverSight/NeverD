//===- X86FPArithState.h - Legacy/VEX FP instruction completion -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86FPARITHSTATE_H
#define NEVERD_LIB_LIFT_X86_X86FPARITHSTATE_H

#include "X86FPEncoding.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86/X86FPState.h"
#include "neverd/lift/X86/X86Lifter.h"

namespace neverd {
struct FPArithStateSpec {
  X86FPArithKind Kind;
  uint8_t Opcode;
  bool Double;
  bool Scalar;
  bool Vex;
  unsigned Topology = 0;
  unsigned Order = 0;
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
#define FP_FMA(Name, Opcode, Flags, Order)                                     \
  case X86_INS_V##Name##PS:                                                    \
    Spec = {X86FPArithKind::FusedMultiplyAdd,                                  \
            Opcode,                                                            \
            false,                                                             \
            false,                                                             \
            true,                                                              \
            Flags,                                                             \
            Order};                                                            \
    return true;                                                               \
  case X86_INS_V##Name##PD:                                                    \
    Spec = {X86FPArithKind::FusedMultiplyAdd,                                  \
            Opcode,                                                            \
            true,                                                              \
            false,                                                             \
            true,                                                              \
            Flags,                                                             \
            Order};                                                            \
    return true;                                                               \
  case X86_INS_V##Name##SS:                                                    \
    Spec = {X86FPArithKind::FusedMultiplyAdd,                                  \
            Opcode + 1,                                                        \
            false,                                                             \
            true,                                                              \
            true,                                                              \
            Flags,                                                             \
            Order};                                                            \
    return true;                                                               \
  case X86_INS_V##Name##SD:                                                    \
    Spec = {X86FPArithKind::FusedMultiplyAdd,                                  \
            Opcode + 1,                                                        \
            true,                                                              \
            true,                                                              \
            true,                                                              \
            Flags,                                                             \
            Order};                                                            \
    return true;
    FP_FMA(FMADD132, 0x98, 0, 0)
    FP_FMA(FMADD213, 0xa8, 0, 1)
    FP_FMA(FMADD231, 0xb8, 0, 2)
    FP_FMA(FMSUB132, 0x9a, 128, 0)
    FP_FMA(FMSUB213, 0xaa, 128, 1)
    FP_FMA(FMSUB231, 0xba, 128, 2)
    FP_FMA(FNMADD132, 0x9c, 64, 0)
    FP_FMA(FNMADD213, 0xac, 64, 1)
    FP_FMA(FNMADD231, 0xbc, 64, 2)
    FP_FMA(FNMSUB132, 0x9e, 192, 0)
    FP_FMA(FNMSUB213, 0xae, 192, 1)
    FP_FMA(FNMSUB231, 0xbe, 192, 2)
#undef FP_FMA
#define FP_FMA_ALTERNATING(Name, Opcode, Flags, Order)                         \
  case X86_INS_V##Name##PS:                                                    \
    Spec = {X86FPArithKind::FusedMultiplyAdd,                                  \
            Opcode,                                                            \
            false,                                                             \
            false,                                                             \
            true,                                                              \
            Flags,                                                             \
            Order};                                                            \
    return true;                                                               \
  case X86_INS_V##Name##PD:                                                    \
    Spec = {X86FPArithKind::FusedMultiplyAdd,                                  \
            Opcode,                                                            \
            true,                                                              \
            false,                                                             \
            true,                                                              \
            Flags,                                                             \
            Order};                                                            \
    return true;
    FP_FMA_ALTERNATING(FMADDSUB132, 0x96, 3072, 0)
    FP_FMA_ALTERNATING(FMADDSUB213, 0xa6, 3072, 1)
    FP_FMA_ALTERNATING(FMADDSUB231, 0xb6, 3072, 2)
    FP_FMA_ALTERNATING(FMSUBADD132, 0x97, 1024, 0)
    FP_FMA_ALTERNATING(FMSUBADD213, 0xa7, 1024, 1)
    FP_FMA_ALTERNATING(FMSUBADD231, 0xb7, 1024, 2)
#undef FP_FMA_ALTERNATING
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
  const bool Fma = Spec.Kind == X86FPArithKind::FusedMultiplyAdd;
  const bool HasVvvv = Spec.Vex && (!Unary || Spec.Scalar);
  const unsigned SourceIndex = HasVvvv ? 2 : 1;
  if (X86.op_count != SourceIndex + 1 || X86.operands[0].type != X86_OP_REG)
    return Refuse();
  for (unsigned Index = 0; Index < X86.op_count; ++Index)
    if (X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID ||
        X86.operands[Index].avx_zero_opmask)
      return Refuse();
  const unsigned PP = Fma             ? 1
                      : Spec.Topology ? (Spec.Double ? 1 : 3)
                      : Spec.Scalar   ? (Spec.Double ? 3 : 2)
                                      : (Spec.Double ? 1 : 0);
  FPEncodingInfo E;
  if (!parseFPEncoding(Insn, X86, L.targetArch(), Spec.Vex, Fma ? 2 : 1, PP,
                       E) ||
      E.Opcode != Spec.Opcode || (Fma && E.W != Spec.Double) ||
      (Spec.Scalar && E.L) || (!HasVvvv && !E.unusedVvvv()))
    return Refuse();
  const unsigned Width = E.L ? 32 : 16;
  const unsigned Base = Width == 32 ? X86_REG_YMM0 : X86_REG_XMM0;
  if (X86.operands[0].size != Width ||
      X86.operands[0].reg != Base + E.vectorRegIndex() ||
      (HasVvvv &&
       (X86.operands[1].type != X86_OP_REG || X86.operands[1].size != Width ||
        X86.operands[1].reg != Base + E.vvvvIndex())))
    return Refuse();
  const unsigned Element = Spec.Double ? 8 : 4;
  const unsigned Bytes = Spec.Scalar ? Element : Width;
  const auto &RHSOperand = X86.operands[SourceIndex];
  if ((E.ModRM >> 6) == 3) {
    if (RHSOperand.type != X86_OP_REG || RHSOperand.size != Width ||
        RHSOperand.reg != Base + E.vectorRMIndex())
      return Refuse();
  } else if (RHSOperand.type != X86_OP_MEM || RHSOperand.size != Bytes)
    return Refuse();
  if (!validateFPEncodingTail(Insn, X86, E, RHSOperand))
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
  if (Fma) {
    const auto Old = Slice(L.operandRead(S, X86.operands[0]));
    const unsigned FmaControl = makeX86FPFmaStateControl(
        Spec.Order, Spec.Double, Spec.Scalar, Memory, (Spec.Topology & 64) != 0,
        (Spec.Topology & 128) != 0, (Spec.Topology & 1024) != 0,
        (Spec.Topology & 2048) != 0);
    if (Memory)
      S.emitIntrinsic(Intrinsic::X86FPFmaMemoryState, Result,
                      {Right, NdVar::cst(FmaControl, 2), Old, Left, Incoming},
                      NdMemoryOrdering::None, S.memoryAddressSpace(RHSOperand));
    else
      S.emitIntrinsic(Intrinsic::X86FPFmaState, Result,
                      {NdVar::cst(FmaControl, 2), Old, Left, Right, Incoming});
  } else if (Memory)
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
           {L.operandRead(S, X86.operands[Fma ? 0 : LeftIndex]),
            NdVar::cst(Element, 4)});
    S.emit(NdOp::CONCAT, Destination, {Upper, Number});
  } else
    S.emit(NdOp::COPY, Destination, {Number});
  return true;
}
} // namespace neverd
#endif
