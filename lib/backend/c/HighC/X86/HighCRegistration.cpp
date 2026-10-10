//===- HighCRegistration.cpp - PE32 runtime entry inputs -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../HighCWriter.h"

#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"

#include <set>

namespace neverd {

bool HighCWriter::preservesRegistrationMemory(const HighFunc &Func) {
  // Named affine stack slots can hide a complete FS:[0] link/unlink chain.
  // An aligned frame still uses explicit byte addresses: keep its FS reads
  // and writes too, or a visible node store can name a hidden definition.
  return Func.ExceptionMetadata && Func.ExceptionMetadata->Registration &&
         Func.ExceptionMetadata->Registration->RealignedFrame.has_value();
}

bool HighCWriter::isEmbeddedRegistrationCallback(const HighStmt &Stmt,
                                                 size_t I) const {
  if (Opts.TheArch != Arch::X86 || !CurrentFunc ||
      !CurrentFunc->ExceptionMetadata ||
      !CurrentFunc->ExceptionMetadata->Registration ||
      CurrentFunc->ExceptionMetadata->Encoding !=
          ExceptionEncoding::X86CxxFuncInfo ||
      I >= Stmt.EHClauses.size() || I >= Stmt.EHClauseBodies.size() ||
      Stmt.EHClauses[I].Kind != HighEHClauseKind::CxxCatch ||
      Stmt.EHClauseBodies[I].empty())
    return false;
  const auto *Entry = &Stmt.EHClauseBodies[I].front();
  // The callback's first instruction may itself enter a protected region.
  // Only descend through unconditional try bodies, never a branch or a
  // nested handler whose runtime entry belongs to another invocation.
  while (Entry->Kind == StmtKind::CxxTry && !Entry->Body.empty())
    Entry = &Entry->Body.front();
  return Entry->Kind == StmtKind::Assign && Entry->Val &&
         Entry->Val->Kind == ExprKind::EntryRegister &&
         Entry->Val->EntryFunctionVA == CurrentFunc->Entry &&
         Entry->Val->EntryVA == Stmt.EHClauses[I].HandlerVA &&
         Entry->Addr == Entry->Val->EntryVA;
}

void HighCWriter::writeEmbeddedRegistrationCallbacks(const HighStmt &Stmt,
                                                     int Indent) {
  bool HasEmbedded = false;
  for (size_t I = 0; I < Stmt.EHClauses.size(); ++I)
    HasEmbedded |= isEmbeddedRegistrationCallback(Stmt, I);
  if (!HasEmbedded)
    return;
  const std::string Suffix = std::to_string(RegistrationRegionNumber++);
  const std::string After = "L_x86_eh_after_" + Suffix;
  emitIndent(Indent);
  OS << "goto " << After << ";\n";
  const bool SavedHandler = InEHClauseBody;
  InEHClauseBody = true;
  llvm::scope_exit Restore([&] { InEHClauseBody = SavedHandler; });
  for (size_t I = 0; I < Stmt.EHClauses.size(); ++I) {
    if (!isEmbeddedRegistrationCallback(Stmt, I))
      continue;
    const std::string Address = llvm::utohexstr(Stmt.EHClauses[I].HandlerVA);
    emitIndent(Indent);
    OS << "/* Native x86 callback @ 0x" << Address
       << ": entered by the exception runtime with its private stack. */\n";
    emitIndent(Indent);
    OS << "L_x86_callback_" << Address << "_" << Suffix << ": {\n";
    writeStmts(Stmt.EHClauseBodies[I], Indent + 1);
    emitIndent(Indent + 1);
    OS << "__builtin_unreachable(); /* callback has no ordinary fallthrough "
          "*/\n";
    emitIndent(Indent);
    OS << "}\n";
  }
  emitIndent(Indent);
  OS << After << ":;\n";
}

std::string HighCWriter::registrationEntryExpression(const HighExpr &E) const {
  if (Opts.TheArch != Arch::X86 || E.Kind != ExprKind::EntryRegister ||
      E.Var.Kind != MedVar::Reg || E.Var.TheArch != Arch::X86 ||
      E.Var.RegOff != getTargetRegInfo(Arch::X86).StackPointer ||
      E.Var.Size != 4 || !E.Type || E.Type->Size != 4 || !E.Operands.empty() ||
      E.EntryFunctionVA > UINT32_MAX || E.EntryVA > UINT32_MAX)
    llvm::report_fatal_error("HighC cannot render an unbound runtime register");
  return "__neverd_x86_callback_esp(0x" +
         llvm::utohexstr(E.EntryFunctionVA, true) + "u, 0x" +
         llvm::utohexstr(E.EntryVA, true) + "u)";
}

void HighCWriter::writeRegistrationEntryDeclarations(
    const std::vector<HighFunc> &Funcs) {
  bool Used = false;
  for (const auto &Func : Funcs) {
    // Resuming an outer catch reuses its original invocation's ESP. Bind
    // every such expression to the retained callback entry definition;
    // a continuation must not manufacture a new runtime invocation.
    std::set<va_t> Entries;
    walkStmts(Func.Body, [&](const HighStmt &Stmt) {
      if (Stmt.Kind != StmtKind::Assign || !Stmt.Val ||
          Stmt.Val->Kind != ExprKind::EntryRegister ||
          Stmt.Addr != Stmt.Val->EntryVA)
        return;
      const auto &Entry = *Stmt.Val;
      if (Entry.EntryFunctionVA != Func.Entry || !Func.ExceptionMetadata ||
          !Func.ExceptionMetadata->Registration ||
          !Func.ExceptionMetadata->Cxx || !Entries.insert(Entry.EntryVA).second)
        llvm::report_fatal_error(
            "runtime register has no unique callback entry");
      unsigned Owners = 0;
      for (const auto &Try : Func.ExceptionMetadata->Cxx->TryBlocks)
        for (const auto &Catch : Try.Handlers)
          Owners += Catch.HandlerVA == Entry.EntryVA;
      if (Owners != 1)
        llvm::report_fatal_error("runtime register has no unique catch owner");
    });
    walkStmts(Func.Body, [&](const HighStmt &Stmt) {
      HighExprSet Seen;
      std::vector<ExprPtr> Work;
      forEachExpr(Stmt, [&](const ExprPtr &E) { Work.push_back(E); });
      while (!Work.empty()) {
        const auto E = Work.back();
        Work.pop_back();
        if (!E || !Seen.insert(E.get()).second)
          continue;
        if (E->Kind == ExprKind::EntryRegister) {
          if (E->EntryFunctionVA != Func.Entry || !Entries.count(E->EntryVA))
            llvm::report_fatal_error(
                "runtime register lost its callback entry");
          registrationEntryExpression(*E);
          Used = true;
        }
        E->forEachChildExpr(
            [&](const ExprPtr &Child) { Work.push_back(Child); });
      }
    });
  }
  if (Used)
    OS << "/* EH view intrinsic: ESP supplied by the runtime at this callback "
          "entry.\n"
          " * Each invocation has its own stack; this is not the parent's ESP. "
          "*/\n"
          "extern uint32_t __neverd_x86_callback_esp(uint32_t parent, "
          "uint32_t callback);\n\n";
}

} // namespace neverd
