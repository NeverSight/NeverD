//===- X86F16CState.h - F16C instruction completion lifter -*- C++ -*-===//
// NeverD Decompiler
#ifndef NEVERD_LIB_LIFT_X86_X86F16CSTATE_H
#define NEVERD_LIB_LIFT_X86_X86F16CSTATE_H
#include "X86LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86/X86FPState.h"
#include "neverd/lift/X86/X86Lifter.h"
namespace neverd {
inline bool liftF16CState(X86Lifter &L, X86Lifter::LiftState &S,
                          const cs_insn *Insn, const cs_x86 &X86, bool Narrow) {
  const auto Refuse = [&]() -> bool {
    throw UnliftedInstruction(Insn ? Insn->address : InvalidVA,
                              Insn ? Insn->mnemonic : nullptr,
                              Insn ? Insn->op_str : nullptr);
  };
  CanonicalVex3EncodingInfo E;
  if (!parseCanonicalVex3EncodingInfo(Insn, X86, L.targetArch(), E) ||
      (E.P0 & 31) != (Narrow ? 3 : 2) || (E.P1 & 0xfb) != 0x79 ||
      E.Opcode != (Narrow ? 0x1d : 0x13) || X86.op_count != (Narrow ? 3 : 2) ||
      X86.avx_sae || X86.avx_rm != X86_AVX_RM_INVALID)
    return Refuse();
  const unsigned Control = (Narrow ? 1 : 0) | ((E.P1 & 4) ? 2 : 0);
  const unsigned SourceBytes = x86F16CSourceBytes(Control);
  const unsigned Bytes = x86F16CDestinationBytes(Control);
  const unsigned Reg = E.regIndex();
  const auto &Register = X86.operands[Narrow ? 1 : 0];
  const auto &RM = X86.operands[Narrow ? 0 : 1];
  const unsigned RegBytes = Narrow ? SourceBytes : Bytes;
  if (Register.type != X86_OP_REG || Register.size != RegBytes ||
      Register.reg != (RegBytes == 32 ? X86_REG_YMM0 : X86_REG_XMM0) + Reg)
    return Refuse();
  for (unsigned Index = 0; Index < 2; ++Index)
    if (X86.operands[Index].avx_zero_opmask ||
        X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID)
      return Refuse();
  const bool Memory = (E.ModRM >> 6) != 3;
  if (Memory) {
    if (RM.type != X86_OP_MEM ||
        RM.size != (Narrow ? x86F16CStoreBytes(Control) : SourceBytes) ||
        !validateCanonicalVex3MemoryTail(Insn, X86, E, RM, Narrow ? 1 : 0))
      return Refuse();
  } else if (RM.type != X86_OP_REG || RM.size != 16 ||
             RM.reg != X86_REG_XMM0 + E.rmIndex() ||
             !validateCanonicalVex3RegisterTail(Insn, X86, E, Narrow ? 1 : 0))
    return Refuse();
  unsigned Immediate = 0;
  if (Narrow) {
    Immediate = Insn->bytes[Insn->size - 1];
    if (X86.encoding.imm_offset != Insn->size - 1 ||
        X86.encoding.imm_size != 1 || X86.operands[2].type != X86_OP_IMM ||
        X86.operands[2].size != 1 || uint64_t(X86.operands[2].imm) != Immediate)
      return Refuse();
  } else if (X86.encoding.imm_offset || X86.encoding.imm_size)
    return Refuse();
  auto Source = Narrow   ? L.operandRead(S, Register)
                : Memory ? S.computeEA(RM)
                         : L.operandRead(S, RM);
  if (!Narrow && !Memory && Source.Size != SourceBytes) {
    auto Low = S.makeTemp(SourceBytes);
    S.emit(NdOp::SUBBYTES, Low, {Source, NdVar::cst(0, 4)});
    Source = Low;
  }
  auto Incoming = S.makeTemp(4);
  S.emitIntrinsic(Intrinsic::X86ReadMXCSR, Incoming);
  const auto Space =
      Memory ? S.memoryAddressSpace(RM) : NdMemoryAddressSpace::Default;
  if (Narrow && Memory) {
    auto Outgoing = S.makeTemp(4);
    S.emitIntrinsic(Intrinsic::X86F16CStoreState, Outgoing,
                    {S.computeEA(RM), Source, NdVar::cst(Control, 1),
                     NdVar::cst(Immediate, 1), Incoming},
                    NdMemoryOrdering::None, Space);
    S.emitVoidIntrinsic(Intrinsic::X86WriteMXCSR, {Outgoing});
  } else {
    auto Completed = S.makeTemp(Bytes + 4), Number = S.makeTemp(Bytes),
         Outgoing = S.makeTemp(4);
    S.emitIntrinsic(
        Memory ? Intrinsic::X86F16CReadState : Intrinsic::X86F16CState,
        Completed,
        {Source, NdVar::cst(Control, 1), NdVar::cst(Immediate, 1), Incoming},
        NdMemoryOrdering::None, Space);
    S.emit(NdOp::SUBBYTES, Number, {Completed, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, Outgoing, {Completed, NdVar::cst(Bytes, 4)});
    S.emitVoidIntrinsic(Intrinsic::X86WriteMXCSR, {Outgoing});
    S.emit(NdOp::COPY, L.operandWrite(X86.operands[0]), {Number});
  }
  return true;
}
} // namespace neverd
#endif
