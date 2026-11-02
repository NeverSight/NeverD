//===- X86FPApprox12.h - Legacy/VEX reciprocal approximation -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86FPAPPROX12_H
#define NEVERD_LIB_LIFT_X86_X86FPAPPROX12_H

#include "X86FPEncoding.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86/X86FPState.h"
#include "neverd/lift/X86/X86Lifter.h"

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
  FPEncodingInfo E;
  if (!parseFPEncoding(Insn, X86, L.targetArch(), Vex, 1, Scalar ? 2 : 0, E) ||
      E.Opcode != (Rsqrt ? 0x52 : 0x53) || (Scalar ? E.L : !E.unusedVvvv()))
    return Refuse();
  const unsigned Width = E.L ? 32 : 16;
  const unsigned RegisterBase = Width == 32 ? X86_REG_YMM0 : X86_REG_XMM0;
  if (X86.operands[0].size != Width ||
      X86.operands[0].reg != RegisterBase + E.vectorRegIndex() ||
      (Vex && Scalar &&
       (X86.operands[1].type != X86_OP_REG || X86.operands[1].size != 16 ||
        X86.operands[1].reg != X86_REG_XMM0 + E.vvvvIndex())))
    return Refuse();
  const unsigned Bytes = Scalar ? 4 : Width;
  const auto &Operand = X86.operands[SourceIndex];
  if ((E.ModRM >> 6) == 3) {
    if (Operand.type != X86_OP_REG || Operand.size != Width ||
        Operand.reg != RegisterBase + E.vectorRMIndex())
      return Refuse();
  } else if (Operand.type != X86_OP_MEM || Operand.size != Bytes)
    return Refuse();
  if (!validateFPEncodingTail(Insn, X86, E, Operand))
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
