//===- X86FPMaskCompareState.h - Authenticated CMP lifting ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve SSE/VEX comparison predicates, masks and MXCSR completion.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_LIFT_X86_X86FPMASKCOMPARESTATE_H
#define NEVERD_LIB_LIFT_X86_X86FPMASKCOMPARESTATE_H

#include "X86FPEncoding.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86/X86FPState.h"
#include "neverd/lift/X86/X86Lifter.h"

namespace neverd {
/// Distinguish scalar CMP from string CMPSD before trusting decoded operands.
inline bool hasLegacyFPMaskCompareOpcode(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->size || Insn->size > 15)
    return false;
  unsigned I = 0;
  for (; I < Insn->size; ++I) {
    const auto B = Insn->bytes[I];
    if (!(B == 0x26 || B == 0x2e || B == 0x36 || B == 0x3e || B == 0x64 ||
          B == 0x65 || B == 0x66 || B == 0x67 || B == 0xf2 || B == 0xf3 ||
          (B & 0xf0) == 0x40))
      break;
  }
  if (Target == Arch::X64 && I + 2 < Insn->size && Insn->bytes[I] == 0xd5 &&
      (Insn->bytes[I + 1] & 0x80))
    return Insn->bytes[I + 2] == 0xc2;
  return I + 1 < Insn->size && Insn->bytes[I] == 0x0f &&
         Insn->bytes[I + 1] == 0xc2;
}
inline bool liftFPMaskCompareState(X86Lifter &L, X86Lifter::LiftState &S,
                                   const cs_insn *Insn, const cs_x86 &X86,
                                   bool Double, bool Scalar, bool Vex) {
  const auto Refuse = [&]() -> bool {
    throw UnliftedInstruction(Insn ? Insn->address : InvalidVA,
                              Insn ? Insn->mnemonic : nullptr,
                              Insn ? Insn->op_str : nullptr);
  };
  const unsigned SourceIndex = Vex ? 2 : 1;
  if (!Insn || !Insn->size || Insn->size > 15 ||
      (L.targetArch() != Arch::X64 && L.targetArch() != Arch::X86) ||
      (X86.op_count != SourceIndex + 1 && X86.op_count != SourceIndex + 2) ||
      X86.operands[0].type != X86_OP_REG || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID || X86.encoding.imm_size != 1 ||
      X86.encoding.imm_offset != Insn->size - 1)
    return Refuse();
  for (unsigned I = 0; I < X86.op_count; ++I)
    if (X86.operands[I].avx_bcast != X86_AVX_BCAST_INVALID ||
        X86.operands[I].avx_zero_opmask)
      return Refuse();
  const unsigned Immediate = Insn->bytes[Insn->size - 1];
  if (X86.op_count == SourceIndex + 2 &&
      (X86.operands[SourceIndex + 1].type != X86_OP_IMM ||
       X86.operands[SourceIndex + 1].imm != Immediate))
    return Refuse();
  const unsigned Control = (Double ? 1 : 0) | (Scalar ? 2 : 0) | (Vex ? 4 : 0);
  const unsigned Predicate = x86FPMaskComparePredicate(Control, Immediate);
  if (Vex ? X86.sse_cc != X86_SSE_CC_INVALID ||
                (X86.avx_cc != X86_AVX_CC_INVALID &&
                 unsigned(X86.avx_cc) != Predicate + 1)
          : X86.avx_cc != X86_AVX_CC_INVALID ||
                (X86.sse_cc != X86_SSE_CC_INVALID &&
                 unsigned(X86.sse_cc) != Predicate + 1))
    return Refuse();
  const unsigned PP = Scalar ? (Double ? 3 : 2) : (Double ? 1 : 0);
  FPEncodingInfo E;
  if (!parseFPEncoding(Insn, X86, L.targetArch(), Vex, 1, PP, E) ||
      E.Opcode != 0xc2 || (Scalar && E.L))
    return Refuse();
  const unsigned Width = E.L ? 32 : 16;
  const unsigned Base = Width == 32 ? X86_REG_YMM0 : X86_REG_XMM0;
  if (X86.operands[0].size != Width ||
      X86.operands[0].reg != Base + E.vectorRegIndex() ||
      (Vex &&
       (X86.operands[1].type != X86_OP_REG || X86.operands[1].size != Width ||
        X86.operands[1].reg != Base + E.vvvvIndex())))
    return Refuse();
  const unsigned Element = Double ? 8 : 4, Bytes = Scalar ? Element : Width;
  const auto &RHS = X86.operands[SourceIndex];
  const bool Memory = (E.ModRM >> 6) != 3;
  if (Memory ? RHS.type != X86_OP_MEM || RHS.size != Bytes
             : RHS.type != X86_OP_REG || RHS.size != Width ||
                   RHS.reg != Base + E.vectorRMIndex())
    return Refuse();
  if (!validateFPEncodingTail(Insn, X86, E, RHS, 1))
    return Refuse();
  const auto Slice = [&](NdVar Input) {
    if (Input.Size == Bytes)
      return Input;
    const auto Narrow = S.makeTemp(Bytes);
    S.emit(NdOp::SUBBYTES, Narrow, {Input, NdVar::cst(0, 4)});
    return Narrow;
  };
  const auto LeftFull = L.operandRead(S, X86.operands[Vex ? 1 : 0]);
  const auto Left = Slice(LeftFull);
  const auto Right = Memory ? S.computeEA(RHS) : Slice(L.operandRead(S, RHS));
  const auto Incoming = S.makeTemp(4), Completed = S.makeTemp(Bytes + 4);
  S.emitIntrinsic(Intrinsic::X86ReadMXCSR, Incoming);
  if (Memory)
    S.emitIntrinsic(Intrinsic::X86FPMaskCompareMemoryState, Completed,
                    {Right, NdVar::cst(Control, 1), NdVar::cst(Immediate, 1),
                     Left, Incoming},
                    NdMemoryOrdering::None, S.memoryAddressSpace(RHS));
  else
    S.emitIntrinsic(Intrinsic::X86FPMaskCompareState, Completed,
                    {NdVar::cst(Control, 1), NdVar::cst(Immediate, 1), Left,
                     Right, Incoming});
  const auto Mask = S.makeTemp(Bytes), State = S.makeTemp(4);
  S.emit(NdOp::SUBBYTES, Mask, {Completed, NdVar::cst(0, 4)});
  S.emit(NdOp::SUBBYTES, State, {Completed, NdVar::cst(Bytes, 4)});
  S.emitVoidIntrinsic(Intrinsic::X86WriteMXCSR, {State});
  const auto Destination = L.operandWrite(X86.operands[0]);
  if (Scalar) {
    const auto Upper = S.makeTemp(16 - Element);
    S.emit(NdOp::SUBBYTES, Upper, {LeftFull, NdVar::cst(Element, 4)});
    S.emit(NdOp::CONCAT, Destination, {Upper, Mask});
  } else
    S.emit(NdOp::COPY, Destination, {Mask});
  return true;
}
} // namespace neverd
#endif // NEVERD_LIB_LIFT_X86_X86FPMASKCOMPARESTATE_H
