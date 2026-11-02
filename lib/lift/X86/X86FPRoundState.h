//===- X86FPRoundState.h - ROUND numerical/state completion -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86FPROUNDSTATE_H
#define NEVERD_LIB_LIFT_X86_X86FPROUNDSTATE_H

#include "X86FPEncoding.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86/X86FPState.h"
#include "neverd/lift/X86/X86Lifter.h"

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
  FPEncodingInfo E;
  if (!parseFPEncoding(Insn, X86, L.targetArch(), Vex, 3, 1, E) ||
      E.Opcode != (Scalar ? 0x0a : 0x08) + unsigned(Double) ||
      (Scalar ? E.L : !E.unusedVvvv()))
    return Refuse();
  const unsigned Width = E.L ? 32 : 16;
  const unsigned RegisterBase = Width == 32 ? X86_REG_YMM0 : X86_REG_XMM0;
  if (X86.operands[0].size != Width ||
      X86.operands[0].reg != RegisterBase + E.vectorRegIndex() ||
      (Vex && Scalar &&
       (X86.operands[1].type != X86_OP_REG || X86.operands[1].size != 16 ||
        X86.operands[1].reg != X86_REG_XMM0 + E.vvvvIndex())))
    return Refuse();
  const unsigned NumericalBytes = Scalar ? (Double ? 8 : 4) : Width;
  const auto &SourceOperand = X86.operands[SourceIndex];
  if ((E.ModRM >> 6) == 3) {
    if (SourceOperand.type != X86_OP_REG || SourceOperand.size != Width ||
        SourceOperand.reg != RegisterBase + E.vectorRMIndex())
      return Refuse();
  } else if (SourceOperand.type != X86_OP_MEM ||
             SourceOperand.size != NumericalBytes)
    return Refuse();
  if (!validateFPEncodingTail(Insn, X86, E, SourceOperand, 1))
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
