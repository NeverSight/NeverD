//===- X86FPComiState.h - Authenticated scalar comparison lifting --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain raw operand roles and one flags/MXCSR completion for SSE/VEX COMI.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_LIFT_X86_X86FPCOMISTATE_H
#define NEVERD_LIB_LIFT_X86_X86FPCOMISTATE_H
#include "X86FPEncoding.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86/X86FPState.h"
#include "neverd/lift/X86/X86Lifter.h"
namespace neverd {
inline bool liftFPComiState(X86Lifter &L, X86Lifter::LiftState &S,
                            const cs_insn *Insn, const cs_x86 &X86, bool Double,
                            bool Quiet, bool Vex) {
  const auto Refuse = [&]() -> bool {
    throw UnliftedInstruction(Insn ? Insn->address : InvalidVA,
                              Insn ? Insn->mnemonic : nullptr,
                              Insn ? Insn->op_str : nullptr);
  };
  if (!Insn || !Insn->size || Insn->size > 15 || X86.op_count != 2 ||
      (L.targetArch() != Arch::X64 && L.targetArch() != Arch::X86) ||
      X86.operands[0].type != X86_OP_REG || X86.operands[0].size != 16 ||
      X86.encoding.imm_offset || X86.encoding.imm_size || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID)
    return Refuse();
  FPEncodingInfo E;
  if (!parseFPEncoding(Insn, X86, L.targetArch(), Vex, 1, Double ? 1 : 0, E) ||
      E.Opcode != (Quiet ? 0x2e : 0x2f) || E.L || !E.unusedVvvv() ||
      X86.operands[0].reg != X86_REG_XMM0 + E.vectorRegIndex())
    return Refuse();
  const unsigned Bytes = Double ? 8 : 4;
  const auto &RightOp = X86.operands[1];
  const bool Memory = (E.ModRM >> 6) != 3;
  if (Memory ? RightOp.type != X86_OP_MEM || RightOp.size != Bytes
             : RightOp.type != X86_OP_REG || RightOp.size != 16 ||
                   RightOp.reg != X86_REG_XMM0 + E.vectorRMIndex())
    return Refuse();
  if (!validateFPEncodingTail(Insn, X86, E, RightOp))
    return Refuse();
  const auto Scalar = [&](const cs_x86_op &Op) {
    const auto Value = L.operandRead(S, Op), Narrow = S.makeTemp(Bytes);
    S.emit(NdOp::SUBBYTES, Narrow, {Value, NdVar::cst(0, 4)});
    return Narrow;
  };
  const auto Left = Scalar(X86.operands[0]);
  const auto Right = Memory ? S.computeEA(RightOp) : Scalar(RightOp);
  const auto Incoming = S.makeTemp(4), Completed = S.makeTemp(8);
  const unsigned Control = (Quiet ? 1 : 0) | (Double ? 2 : 0) | (Vex ? 4 : 0);
  S.emitIntrinsic(Intrinsic::X86ReadMXCSR, Incoming);
  if (Memory)
    S.emitIntrinsic(Intrinsic::X86FPComiMemoryState, Completed,
                    {Right, NdVar::cst(Control, 1), Left, Incoming},
                    NdMemoryOrdering::None, S.memoryAddressSpace(RightOp));
  else
    S.emitIntrinsic(Intrinsic::X86FPComiState, Completed,
                    {NdVar::cst(Control, 1), Left, Right, Incoming});
  const auto Flags = S.makeTemp(4), Outgoing = S.makeTemp(4);
  S.emit(NdOp::SUBBYTES, Flags, {Completed, NdVar::cst(0, 4)});
  S.emit(NdOp::SUBBYTES, Outgoing, {Completed, NdVar::cst(4, 4)});
  S.emitVoidIntrinsic(Intrinsic::X86WriteMXCSR, {Outgoing});
  for (const auto [Reg, Shift] :
       {std::pair{x86reg::CF, 0}, std::pair{x86reg::PF, 2},
        std::pair{x86reg::ZF, 6}}) {
    const auto Bit = S.makeTemp(4), Byte = S.makeTemp(1);
    S.emit(NdOp::INT_RIGHT, Bit, {Flags, NdVar::cst(Shift, 4)});
    S.emit(NdOp::SUBBYTES, Byte, {Bit, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_AND, NdVar::reg(Reg, 1), {Byte, NdVar::cst(1, 1)});
  }
  for (auto Reg : {x86reg::OF, x86reg::SF, x86reg::AF})
    S.emit(NdOp::COPY, NdVar::reg(Reg, 1), {NdVar::cst(0, 1)});
  return true;
}
} // namespace neverd
#endif
