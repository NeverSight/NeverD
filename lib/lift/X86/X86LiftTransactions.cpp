//===- X86LiftTransactions.cpp - Bounded transaction instructions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep transaction queries, commit and explicit abort at their native
/// instruction boundary. XBEGIN's original checkpoint remains separate.
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/support/ISAEncoding.h"

#include <cstdint>

namespace neverd {
namespace {

bool validTransactionEncoding(const cs_insn *Insn, const cs_x86 &X,
                              bool Mode64) {
  if (!Insn->detail || Insn->size < 3 || Insn->size > x86::kMaxInstructionLen)
    return false;
  const unsigned Offset = Insn->size - 3;
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
  if (!validTransactionEncoding(Insn, X86, L.targetArch() == Arch::X64))
    throw UnliftedInstruction(Insn->address, Insn->mnemonic, Insn->op_str);
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
