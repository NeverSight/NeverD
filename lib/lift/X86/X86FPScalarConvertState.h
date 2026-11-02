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

#include "X86FPEncoding.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86/X86FPState.h"
#include "neverd/lift/X86/X86Lifter.h"

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
  const bool PrefixDouble =
      Integer ? DestinationBytes == 8 : DestinationBytes == 4;
  FPEncodingInfo E;
  if (!parseFPEncoding(Insn, X86, L.targetArch(), Vex, 1, PrefixDouble ? 3 : 2,
                       E) ||
      E.Opcode != (Integer ? 0x2a : 0x5a) || E.L ||
      X86.operands[0].reg != X86_REG_XMM0 + E.vectorRegIndex() ||
      (Vex &&
       (X86.operands[1].type != X86_OP_REG || X86.operands[1].size != 16 ||
        X86.operands[1].reg != X86_REG_XMM0 + E.vvvvIndex())))
    return Refuse();
  const unsigned SourceBytes =
      Integer ? ((E.Mode64 && E.W) ? 8 : 4) : (DestinationBytes == 8 ? 4 : 8);
  const auto &SourceOperand = X86.operands[Vex ? 2 : 1];
  if ((E.ModRM >> 6) == 3) {
    if (Integer
            ? !matchesFPGPROperand(SourceOperand, E.gprRMIndex(), SourceBytes)
            : SourceOperand.type != X86_OP_REG || SourceOperand.size != 16 ||
                  SourceOperand.reg != X86_REG_XMM0 + E.vectorRMIndex())
      return Refuse();
  } else if (SourceOperand.type != X86_OP_MEM ||
             SourceOperand.size != SourceBytes)
    return Refuse();
  if (!validateFPEncodingTail(Insn, X86, E, SourceOperand))
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
