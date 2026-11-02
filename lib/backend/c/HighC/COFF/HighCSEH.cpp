//===- HighCSEH.cpp - Native Windows SEH annotation -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep native dispatch visible when its protected ranges are not reducible.
//===----------------------------------------------------------------------===//

#include "../HighCWriter.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <stdexcept>

namespace neverd {

namespace {

/// Whether \p Body is what attachSEHHandlerEntryCopies gives an except clause
/// whose handler ordinary flow also enters: the copies of the values the
/// runtime enters the handler with, then the jump to the handler.  Such a
/// clause moves no handler code out of its native position.
bool isSEHHandlerEntryBody(const HighEHClause &Clause,
                           const std::vector<HighStmt> &Body) {
  if (Clause.Kind != HighEHClauseKind::SEHExcept || !Clause.HandlerVA ||
      Body.empty())
    return false;
  const HighStmt &Jump = Body.back();
  if (Jump.Kind != StmtKind::Goto || Jump.GotoTarget != Clause.HandlerVA)
    return false;
  return std::all_of(Body.begin(), Body.end() - 1, [](const HighStmt &S) {
    return S.Kind == StmtKind::Assign && S.Dst && S.Val;
  });
}

} // namespace

std::string HighCWriter::sehFilterName(const HighEHClause &Clause) {
  if (Clause.Kind != HighEHClauseKind::SEHExcept || !Clause.FilterOrActionVA ||
      Clause.FilterValue)
    return {};
  // Declarations and filter expressions must use the same definition/image/
  // debug identity. A named filter is a callee even though no Call expression
  // appears in the protected body.
  if (auto It = DefinedFunctionsByAddress.find(Clause.FilterOrActionVA);
      It != DefinedFunctionsByAddress.end() && It->second)
    return functionIdentifier(*It->second);
  if (Opts.Image) {
    const std::string &Listed = imageFunctionNameAt(Clause.FilterOrActionVA);
    if (!llvm::StringRef(Listed).starts_with(kAutoFuncPrefix))
      return functionIdentifier(Listed);
  }
  if (Dbg)
    if (auto Symbol = Dbg->resolveFunction(Clause.FilterOrActionVA);
        Symbol && !Symbol->Name.empty())
      return functionIdentifier(Symbol->Name);
  return functionIdentifier(
      (kAutoFuncPrefix + llvm::utohexstr(Clause.FilterOrActionVA)).str());
}

void HighCWriter::writeSEHScopeAnnotation(const HighFunc &Func, int Indent) {
  if (!Func.ExceptionMetadata || !Func.ExceptionMetadata->SEH)
    return;
  const auto &Scopes = Func.ExceptionMetadata->SEH->Scopes;
  for (size_t I = 0; I < Scopes.size(); ++I) {
    const auto &Scope = Scopes[I];
    const char *Kind = Scope.Kind == SEHScopeKind::Finally    ? "finally"
                       : Scope.Kind == SEHScopeKind::CatchAll ? "except-all"
                                                              : "filter";
    emitIndent(Indent);
    OS << " * seh.scope[" << I << "]: " << Kind << " [0x"
       << llvm::utohexstr(Scope.GuardedRange.Begin) << ", 0x"
       << llvm::utohexstr(Scope.GuardedRange.End) << ")";
    if (Scope.FilterOrFinallyVA)
      OS << " filter_or_finally=0x" << llvm::utohexstr(Scope.FilterOrFinallyVA);
    if (Scope.NormalizedFilterVA)
      OS << " normalized_filter=0x"
         << llvm::utohexstr(Scope.NormalizedFilterVA);
    if (Scope.HandlerVA)
      OS << " handler=0x" << llvm::utohexstr(Scope.HandlerVA);
    if (Scope.ContinuationVA)
      OS << " continuation=0x" << llvm::utohexstr(Scope.ContinuationVA);
    OS << ", status=" << getExceptionParseStatusName(Scope.ParseStatus) << "\n";
  }
}

void HighCWriter::writeUnstructuredSEHRegion(const HighStmt &Stmt, int Indent) {
  // A handler keeps its native position in the body. A clause may carry only
  // the runtime's entry copies into a handler ordinary flow also reaches.
  std::vector<size_t> Entries;
  for (size_t I = 0; I < Stmt.EHClauseBodies.size(); ++I) {
    if (Stmt.EHClauseBodies[I].empty())
      continue;
    if (I >= Stmt.EHClauses.size() ||
        !isSEHHandlerEntryBody(Stmt.EHClauses[I], Stmt.EHClauseBodies[I]))
      throw std::invalid_argument(
          "unstructured SEH view requires the original native handler body");
    Entries.push_back(I);
  }
  emitIndent(Indent);
  OS << "/* unstructured SEH region [0x" << llvm::utohexstr(Stmt.EHRange.Begin)
     << ", 0x" << llvm::utohexstr(Stmt.EHRange.End)
     << "): dispatch is supplied by the image's exception runtime.\n";
  for (const auto &Clause : Stmt.EHClauses) {
    const bool Finally = Clause.Kind == HighEHClauseKind::SEHFinally;
    emitIndent(Indent);
    OS << " * " << (Finally ? "finally: action" : "except: filter") << " @ 0x"
       << llvm::utohexstr(Clause.FilterOrActionVA) << ", handler @ 0x"
       << llvm::utohexstr(Clause.HandlerVA) << ", state=" << Clause.State
       << ", status=" << getExceptionParseStatusName(Clause.ParseStatus);
    for (va_t Continuation : Clause.ContinuationVAs)
      OS << ", continuation @ 0x" << llvm::utohexstr(Continuation);
    if (!Finally && Clause.FilterValue)
      OS << ", filter_value=" << sehFilterValueText(*Clause.FilterValue);
    OS << "\n";
  }
  // These records remain essential when optional descriptive comments are
  // disabled. With comments enabled the function annotation already owns them.
  if (CurrentFunc && !Opts.EmitComments) {
    writeSEHScopeAnnotation(*CurrentFunc, Indent);
    writeRegistrationAnnotation(*CurrentFunc, Indent);
  }
  emitIndent(Indent);
  OS << " */\n";
  emitIndent(Indent);
  OS << "{\n";
  writeTryBody(Stmt.Body, Indent + 1);
  emitIndent(Indent);
  OS << "}\n";
  writeSEHHandlerEntries(Stmt, Entries, Indent);
}

void HighCWriter::writeSEHHandlerEntries(const HighStmt &Stmt,
                                         llvm::ArrayRef<size_t> Entries,
                                         int Indent) {
  if (Entries.empty())
    return;
  const std::string Platform = Opts.TheArch == Arch::X86 ? "x86" : "windows";
  const std::string Suffix = std::to_string(NativeExceptionRegionNumber++);
  const std::string After = "L_" + Platform + "_eh_after_" + Suffix;
  emitIndent(Indent);
  OS << "goto " << After << ";\n";
  const bool SavedHandler = InEHClauseBody;
  InEHClauseBody = true;
  llvm::scope_exit Restore([&] { InEHClauseBody = SavedHandler; });
  for (size_t I : Entries) {
    const std::string Address = llvm::utohexstr(Stmt.EHClauses[I].HandlerVA);
    emitIndent(Indent);
    OS << "/* Native exception entry to the handler @ 0x" << Address
       << ": entered by the exception runtime. */\n";
    emitIndent(Indent);
    OS << "L_" << Platform << "_entry_" << Address << "_" << Suffix << ": {\n";
    writeStmts(Stmt.EHClauseBodies[I], Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
  }
  emitIndent(Indent);
  OS << After << ":;\n";
}

} // namespace neverd
