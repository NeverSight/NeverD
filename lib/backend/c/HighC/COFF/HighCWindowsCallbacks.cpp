//===- HighCWindowsCallbacks.cpp - Native Windows callback labels -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep a table-owned handler inside its original frame in the plain C view.
/// Only the image's runtime enters these labels; ordinary flow skips them.
//===----------------------------------------------------------------------===//

#include "../HighCWriter.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"

namespace neverd {

bool HighCWriter::isEmbeddedWindowsCallback(const HighStmt &Stmt,
                                            size_t I) const {
  if ((Opts.TheArch != Arch::X64 && Opts.TheArch != Arch::AArch64) ||
      Opts.Format != BinaryFormat::COFF || !CurrentFunc ||
      !CurrentFunc->ExceptionMetadata || !Stmt.EHIsReducible ||
      Stmt.Kind != StmtKind::CxxTry || I >= Stmt.EHClauses.size() ||
      I >= Stmt.EHClauseBodies.size() || Stmt.EHClauseBodies[I].empty())
    return false;
  const auto &EH = *CurrentFunc->ExceptionMetadata;
  const auto &Clause = Stmt.EHClauses[I];
  if (EH.Registration || !EH.Cxx ||
      EH.model() != ExceptionModel::WindowsTable ||
      EH.ParseStatus != ExceptionParseStatus::Complete ||
      Clause.ParseStatus != ExceptionParseStatus::Complete ||
      EH.CodeRange.Begin != CurrentFunc->Entry ||
      Clause.Kind != HighEHClauseKind::CxxCatch || !Clause.HandlerVA ||
      DefinedFunctionsByAddress.count(Clause.HandlerVA) ||
      Clause.HandlerVA == CurrentFunc->Entry || !EH.ownsCode(Clause.HandlerVA))
    return false;
  // The structurer already proved the movable native body. Retain its entry
  // anchor and exact table identity; metadata alone cannot invent a callback.
  const auto &Body = Stmt.EHClauseBodies[I];
  if (Body.front().Addr != Clause.HandlerVA)
    return false;
  unsigned Matches = 0;
  for (const auto &Try : EH.Cxx->TryBlocks)
    for (const auto &Catch : Try.Handlers)
      if (Catch.HandlerVA == Clause.HandlerVA &&
          Catch.TypeDescriptorVA == Clause.TypeDescriptorVA &&
          Catch.Adjectives == Clause.Adjectives &&
          Catch.CatchObjectOffset == Clause.CatchObjectOffset &&
          Catch.ParentFrameOffset == Clause.ParentFrameOffset &&
          Catch.ContinuationVAs == Clause.ContinuationVAs)
        ++Matches;
  return Matches == 1;
}

void HighCWriter::writeEmbeddedNativeCallbacks(const HighStmt &Stmt,
                                               int Indent) {
  auto Embedded = [&](size_t I) {
    return isEmbeddedRegistrationCallback(Stmt, I) ||
           isEmbeddedWindowsCallback(Stmt, I);
  };
  bool HasEmbedded = false;
  for (size_t I = 0; I < Stmt.EHClauses.size(); ++I)
    HasEmbedded |= Embedded(I);
  if (!HasEmbedded)
    return;
  const bool Registration = Opts.TheArch == Arch::X86;
  const std::string Platform = Registration ? "x86" : "windows";
  const std::string Suffix = std::to_string(NativeExceptionRegionNumber++);
  const std::string After = "L_" + Platform + "_eh_after_" + Suffix;
  emitIndent(Indent);
  OS << "goto " << After << ";\n";
  const bool SavedHandler = InEHClauseBody;
  InEHClauseBody = true;
  llvm::scope_exit Restore([&] { InEHClauseBody = SavedHandler; });
  for (size_t I = 0; I < Stmt.EHClauses.size(); ++I) {
    if (!Embedded(I))
      continue;
    const std::string Address = llvm::utohexstr(Stmt.EHClauses[I].HandlerVA);
    emitIndent(Indent);
    OS << "/* Native " << (Registration ? "x86" : "Windows") << " callback @ 0x"
       << Address << ": entered by the exception runtime"
       << (Registration ? " with its private stack. */\n" : ". */\n");
    emitIndent(Indent);
    OS << "L_" << Platform << "_callback_" << Address << "_" << Suffix
       << ": {\n";
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
} // namespace neverd
