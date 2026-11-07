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
#include "neverd/decode/X86/X86TransactionEncoding.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Lifter.h"

#include <cstdint>

namespace neverd {
namespace {

bool validTransactionEncoding(const cs_insn *Insn, const cs_x86 &X,
                              bool Mode64) {
  if (!Insn->detail)
    return false;
  const auto E =
      x86TransactionEncoding({Insn->bytes, Insn->size}, Insn->address, Mode64);
  if (!E)
    return false;
  switch (E->Kind) {
  case X86TransactionKind::Begin:
    return Insn->id == X86_INS_XBEGIN &&
           x86TransactionBeginDetailMatches(X, *E, Mode64) &&
           uint64_t(X.operands[0].imm) == E->Target;
  case X86TransactionKind::Abort:
    return Insn->id == X86_INS_XABORT &&
           x86TransactionControlDetailMatches(X, *E, Mode64);
  case X86TransactionKind::End:
    return Insn->id == X86_INS_XEND &&
           x86TransactionControlDetailMatches(X, *E, Mode64);
  case X86TransactionKind::Test:
    return Insn->id == X86_INS_XTEST &&
           x86TransactionControlDetailMatches(X, *E, Mode64);
  }
  llvm_unreachable("invalid transaction kind");
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
