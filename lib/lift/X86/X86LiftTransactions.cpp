//===- X86LiftTransactions.cpp - Bounded transaction instructions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep transaction begin, queries, commit and explicit abort at their native
/// instruction boundary. XBEGIN keeps its original abort continuation as a
/// second exit of its own instruction.
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

#include <cstdint>

namespace neverd {
namespace {

bool validTransactionEncoding(const cs_insn *Insn, const cs_x86 &X,
                              bool Mode64) {
  // XBEGIN is C7 F8 with a rel32 fallback; the others are three opcode bytes.
  const unsigned Body = Insn->id == X86_INS_XBEGIN ? 6 : 3;
  if (!Insn->detail || Insn->size < Body ||
      Insn->size > x86::kMaxInstructionLen)
    return false;
  const unsigned Offset = Insn->size - Body;
  uint8_t Prefix[4]{}, Rex = 0;
  for (unsigned I = 0; I < Offset; ++I) {
    const auto Byte = Insn->bytes[I];
    if (Mode64 && Byte >= 0x40 && Byte <= 0x4f) {
      Rex = Byte;
      continue;
    }
    Rex = 0;
    if (Byte == 0xf2 || Byte == 0xf3)
      Prefix[0] = Byte;
    else if (Byte == 0x66)
      Prefix[2] = Byte;
    else if (Byte == 0x67)
      Prefix[3] = Byte;
    else if (Byte == 0x26 || Byte == 0x2e || Byte == 0x36 || Byte == 0x3e ||
             Byte == 0x64 || Byte == 0x65)
      Prefix[1] = Byte;
    else
      return false;
  }
  // Capstone drops ignored repeat prefixes from its normalized prefix slot.
  if ((X.prefix[0] && X.prefix[0] != Prefix[0]) || X.rex != Rex ||
      X.prefix[1] != Prefix[1] || X.prefix[2] != Prefix[2] ||
      X.prefix[3] != Prefix[3] || X.opcode[2] || X.opcode[3] ||
      X.encoding.disp_size || X.encoding.disp_offset ||
      X.addr_size != (Mode64 ? (Prefix[3] ? 4 : 8) : (Prefix[3] ? 2 : 4)))
    return false;
  if (Insn->id == X86_INS_XBEGIN) {
    // The 16-bit operand size truncates the fallback address; only the
    // rel32 form names its continuation exactly.
    if (Prefix[2] || X.op_count != 1 || X.operands[0].type != X86_OP_IMM ||
        X.opcode[0] != 0xc7 || X.opcode[1] || X.modrm != 0xf8 ||
        X.encoding.modrm_offset != Offset + 1 || X.encoding.imm_size != 4 ||
        X.encoding.imm_offset != Offset + 2 || Insn->bytes[Offset] != 0xc7 ||
        Insn->bytes[Offset + 1] != 0xf8)
      return false;
    const auto Rel =
        static_cast<int32_t>(readLE<uint32_t>(Insn->bytes + Offset + 2));
    const uint64_t Mask = Mode64 ? UINT64_MAX : UINT32_MAX;
    return (uint64_t(X.operands[0].imm) & Mask) ==
           ((Insn->address + Insn->size + uint64_t(int64_t(Rel))) & Mask);
  }
  if (Insn->id == X86_INS_XABORT) {
    if (X.op_count != 1 || X.operands[0].type != X86_OP_IMM ||
        (X.operands[0].size != 1 && X.operands[0].size != 2 &&
         X.operands[0].size != 4 && X.operands[0].size != 8) ||
        X.opcode[0] != 0xc6 || X.opcode[1] || X.modrm != 0xf8 ||
        X.encoding.modrm_offset != Offset + 1 || X.encoding.imm_size != 1 ||
        X.encoding.imm_offset != Offset + 2 || Insn->bytes[Offset] != 0xc6 ||
        Insn->bytes[Offset + 1] != 0xf8)
      return false;
    const auto Code = Insn->bytes[Offset + 2];
    // Capstone may sign-extend the encoded byte to its mode-sized operand.
    return X.operands[0].imm == Code ||
           X.operands[0].imm == static_cast<int8_t>(Code);
  }
  const uint8_t ModRM = Insn->id == X86_INS_XEND ? 0xd5 : 0xd6;
  return (Insn->id == X86_INS_XEND || Insn->id == X86_INS_XTEST) &&
         !X.op_count && X.opcode[0] == 0x0f && X.opcode[1] == 0x01 &&
         X.modrm == ModRM && X.encoding.modrm_offset == Offset + 2 &&
         !X.encoding.imm_size && !X.encoding.imm_offset &&
         Insn->bytes[Offset] == 0x0f && Insn->bytes[Offset + 1] == 0x01 &&
         Insn->bytes[Offset + 2] == ModRM;
}
} // namespace

bool liftTransactions(X86Lifter &L, X86Lifter::LiftState &S,
                      const cs_insn *Insn, const cs_x86 &X86) {
  const bool Mode64 = L.targetArch() == Arch::X64;
  if (!validTransactionEncoding(Insn, X86, Mode64))
    throw UnliftedInstruction(Insn->address, Insn->mnemonic, Insn->op_str);
  if (Insn->id == X86_INS_XBEGIN) {
    // XBEGIN has two exits. A started transaction falls through with EAX
    // unchanged. An abort restores the registers this instruction saw,
    // discards the transaction's memory updates, writes the abort status to
    // EAX and resumes at the encoded fallback. The _xbegin() status names
    // which exit ran; branching from here to the original fallback keeps
    // that continuation and its checkpoint. An abort status keeps its
    // reserved bits 23:6 clear, so it never equals the started value ~0.
    const uint64_t Mask = Mode64 ? UINT64_MAX : UINT32_MAX;
    const NdVar Status = S.makeTemp(4);
    S.emitIntrinsic(Intrinsic::Xbegin, Status, {});
    const NdVar Aborted = S.makeTemp(1);
    S.emit(NdOp::INT_NOTEQUAL, Aborted, {Status, NdVar::scalar(UINT32_MAX, 4)});
    if (Mode64) {
      // A 32-bit general register result zero-extends into RAX.
      const NdVar Wide = S.makeTemp(8);
      S.emit(NdOp::INT_ZEXT, Wide, {Status});
      const NdVar Rax = NdVar::reg(x86reg::RAX, 8);
      S.emit(NdOp::SELECT, Rax, {Aborted, Wide, Rax});
    } else {
      const NdVar Eax = NdVar::reg(x86reg::RAX, 4);
      S.emit(NdOp::SELECT, Eax, {Aborted, Status, Eax});
    }
    S.emit(NdOp::COND_BR, {},
           {NdVar::cst(uint64_t(X86.operands[0].imm) & Mask, 8), Aborted});
    return true;
  }
  if (Insn->id == X86_INS_XABORT) {
    // Only a normal completion falls through; an abort resumes at the
    // enclosing checkpoint. Never invent a missing or inconsistent code.
    S.emitIntrinsic(Intrinsic::Xabort, NdVar(),
                    {NdVar::scalar(Insn->bytes[Insn->size - 1], 1)});
  } else if (Insn->id == X86_INS_XTEST) {
    NdVar InTransaction = S.makeTemp(1);
    S.emitIntrinsic(Intrinsic::Xtest, InTransaction, {});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {InTransaction, NdVar::scalar(0, 1)});
    for (uint64_t Flag :
         {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::SF, x86reg::OF})
      S.emit(NdOp::COPY, NdVar::reg(Flag, 1), {NdVar::scalar(0, 1)});
  } else {
    S.emitIntrinsic(Intrinsic::Xend, NdVar());
  }
  return true;
}
} // namespace neverd
