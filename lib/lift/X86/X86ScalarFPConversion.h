//===- X86ScalarFPConversion.h - Scalar integer conversion state -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86SCALARFPCONVERSION_H
#define NEVERD_LIB_LIFT_X86_X86SCALARFPCONVERSION_H

#include "X86FPEncoding.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86/X86FPState.h"
#include "neverd/lift/X86/X86Lifter.h"

namespace neverd {
inline bool liftScalarFPIntegerState(X86Lifter &L, X86Lifter::LiftState &S,
                                     const cs_insn *Insn, const cs_x86 &X86,
                                     unsigned SourceBytes, bool Truncate,
                                     bool Vex) {
  const auto Refuse = [&]() -> bool {
    throw UnliftedInstruction(Insn ? Insn->address : InvalidVA,
                              Insn ? Insn->mnemonic : nullptr,
                              Insn ? Insn->op_str : nullptr);
  };
  if (!Insn || Insn->size == 0 || Insn->size > 15 ||
      (L.targetArch() != Arch::X64 && L.targetArch() != Arch::X86) ||
      (SourceBytes != 4 && SourceBytes != 8) || X86.op_count != 2 ||
      X86.operands[0].type != X86_OP_REG ||
      (X86.operands[0].size != 4 && X86.operands[0].size != 8) ||
      (X86.operands[1].type != X86_OP_REG &&
       X86.operands[1].type != X86_OP_MEM) ||
      X86.encoding.imm_offset || X86.encoding.imm_size)
    return Refuse();
  FPEncodingInfo E;
  if (!parseFPEncoding(Insn, X86, L.targetArch(), Vex, 1,
                       SourceBytes == 4 ? 2 : 3, E) ||
      E.Opcode != (Truncate ? 0x2c : 0x2d) || E.L || !E.unusedVvvv())
    return Refuse();
  const unsigned DestinationBytes = E.Mode64 && E.W ? 8 : 4;
  if (!matchesFPGPROperand(X86.operands[0], E.gprRegIndex(), DestinationBytes))
    return Refuse();
  if ((E.ModRM >> 6) == 3) {
    if (X86.operands[1].type != X86_OP_REG ||
        X86.operands[1].reg != X86_REG_XMM0 + E.vectorRMIndex() ||
        (X86.operands[1].size != 16 && X86.operands[1].size != SourceBytes))
      return Refuse();
  } else if (X86.operands[1].type != X86_OP_MEM ||
             X86.operands[1].size != SourceBytes)
    return Refuse();
  if (!validateFPEncodingTail(Insn, X86, E, X86.operands[1]))
    return Refuse();
  const auto Destination = L.operandWrite(X86.operands[0]);
  auto Source = L.operandRead(S, X86.operands[1]);
  if (Source.Size > SourceBytes) {
    const auto Narrow = S.makeTemp(SourceBytes);
    S.emit(NdOp::SUBBYTES, Narrow, {Source, NdVar::cst(0, 4)});
    Source = Narrow;
  }
  const auto State = S.makeTemp(4);
  S.emitIntrinsic(Intrinsic::X86ReadMXCSR, State, {});
  const auto Completed = S.makeTemp(DestinationBytes + 4);
  S.emitIntrinsic(Truncate ? Intrinsic::X86FPTruncToIntState
                           : Intrinsic::X86FPCvtToIntState,
                  Completed, {Source, State, NdVar::cst(DestinationBytes, 4)});
  const auto Number = S.makeTemp(DestinationBytes);
  const auto Outgoing = S.makeTemp(4);
  S.emit(NdOp::SUBBYTES, Number, {Completed, NdVar::cst(0, 4)});
  S.emit(NdOp::SUBBYTES, Outgoing,
         {Completed, NdVar::cst(DestinationBytes, 4)});
  S.emit(NdOp::COPY, Destination, {Number});
  S.emitIntrinsic(Intrinsic::X86WriteMXCSR, {}, {Outgoing});
  return true;
}
} // namespace neverd
#endif
