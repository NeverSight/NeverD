//===- HighIRPrintRows.cpp - Statement-aware HighIR rows ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Stream structured HighIR without reconstructing source rows from text.
//===----------------------------------------------------------------------===//

#include "neverd/ir/Limits.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighIRPrint.h"

#include "llvm/ADT/StringExtras.h"

#include <stdexcept>
#include <string>

namespace neverd {
namespace {

void print(const HighStmt &S, int Indent, unsigned Depth, HighIRRowSink Sink) {
  if (Indent < 0 || unsigned(Indent) > limits::ir::kMaxHighIRPrintNesting ||
      Depth > limits::ir::kMaxHighIRPrintNesting)
    throw std::length_error("HighIR statement print nesting limit exceeded");
  const std::string Pad(2 * Indent, ' ');
  auto Row = [&](std::string Text, const HighStmt *Origin) {
    Text += '\n';
    Sink(Text, Origin);
  };
  auto Body = [&](const auto &Statements, int At) {
    for (const auto &Child : Statements)
      print(Child, At, Depth + 1, Sink);
  };
  auto Expr = [](const ExprPtr &E) { return E ? E->str() : "?"; };
  switch (S.Kind) {
  case StmtKind::Assign:
    Row(Pad + Expr(S.Dst) + " = " + Expr(S.Val) + ";", &S);
    break;
  case StmtKind::Store:
    Row(Pad + memoryAddressSpaceQualifier(S.MemoryAddressSpace) + "*" +
            Expr(S.StoreAddr) + " = " + Expr(S.StoreVal) + ";",
        &S);
    break;
  case StmtKind::Return:
    Row(Pad + "return " + (S.RetVal ? S.RetVal->str() : "") + ";", &S);
    break;
  case StmtKind::Call:
    Row(Pad + (S.CallExpr ? S.CallExpr->str() : "?()") + ";", &S);
    break;
  case StmtKind::ExprStmt:
    Row(Pad + Expr(S.Val) + ";", &S);
    break;
  case StmtKind::Goto:
    Row(Pad + "goto 0x" + llvm::utohexstr(S.GotoTarget) + ";", &S);
    break;
  case StmtKind::Break:
    Row(Pad + "break;", &S);
    break;
  case StmtKind::Continue:
    Row(Pad + "continue;", &S);
    break;
  case StmtKind::Nop:
    Row("", nullptr);
    break;
  case StmtKind::If:
  case StmtKind::IfElse:
  case StmtKind::While:
  case StmtKind::For: {
    const char *Prefix = S.Kind == StmtKind::While ? "while ("
                         : S.Kind == StmtKind::For ? "for (; "
                                                   : "if (";
    Row(Pad + Prefix + Expr(S.Cond) +
            (S.Kind == StmtKind::For ? ";) {" : ") {"),
        &S);
    Body(S.Body, Indent + 1);
    if (S.Kind == StmtKind::IfElse) {
      Row(Pad + "} else {", nullptr);
      Body(S.ElseBody, Indent + 1);
    }
    Row(Pad + "}", nullptr);
    break;
  }
  case StmtKind::DoWhile:
    Row(Pad + "do {", nullptr);
    Body(S.Body, Indent + 1);
    Row(Pad + "} while (" + Expr(S.Cond) + ");", &S);
    break;
  case StmtKind::Block:
    if (S.Body.empty())
      Row(Pad + "{}", nullptr);
    else {
      Row(Pad + "{", nullptr);
      Body(S.Body, Indent + 1);
      Row(Pad + "}", nullptr);
    }
    break;
  case StmtKind::Switch:
    Row(Pad + "switch (" + Expr(S.SwitchExpr) + ") {", &S);
    for (const auto &C : S.Cases) {
      Row(Pad + "  case " + std::to_string(C.Value) + ":", nullptr);
      Body(C.Body, Indent + 2);
    }
    if (!S.DefaultBody.empty()) {
      Row(Pad + "  default:", nullptr);
      Body(S.DefaultBody, Indent + 2);
    }
    Row(Pad + "}", nullptr);
    break;
  case StmtKind::SEHTry:
  case StmtKind::CxxTry:
  case StmtKind::ItaniumTry: {
    const char *Name = S.Kind == StmtKind::SEHTry   ? "__try"
                       : S.Kind == StmtKind::CxxTry ? "cxx.try"
                                                    : "itanium.try";
    Row(Pad + Name + " [0x" + llvm::utohexstr(S.EHRange.Begin) + ", 0x" +
            llvm::utohexstr(S.EHRange.End) + ") {",
        &S);
    Body(S.Body, Indent + 1);
    Row(Pad + "}", nullptr);
    for (size_t I = 0; I < S.EHClauses.size(); ++I) {
      const auto &C = S.EHClauses[I];
      std::string Text =
          Pad + "handler 0x" +
          llvm::utohexstr(C.HandlerVA ? C.HandlerVA : C.FilterOrActionVA) + ";";
      if (C.Kind == HighEHClauseKind::CxxCleanup)
        Text += " state=" + std::to_string(C.State) +
                " kind=" + getCxxUnwindActionKindName(C.UnwindActionKind) +
                " object_offset=" + std::to_string(C.UnwindObjectOffset) + ";";
      if (C.Kind == HighEHClauseKind::ItaniumCatch)
        Text +=
            " catch " +
            (C.TypeName.empty()
                 ? (C.TypeDescriptorVA
                        ? "typeinfo@0x" + llvm::utohexstr(C.TypeDescriptorVA)
                        : "...")
                 : C.TypeName) +
            " filter=" + std::to_string(C.TypeFilter) + ";";
      if (C.Kind == HighEHClauseKind::ItaniumSpec) {
        Text += " spec(";
        for (size_t J = 0; J < C.SpecTypeNames.size(); ++J)
          Text += (J ? ", " : "") + C.SpecTypeNames[J];
        Text += ") filter=" + std::to_string(C.TypeFilter) + ";";
      }
      for (size_t J = 1; J < C.LandingPadVAs.size(); ++J)
        Text += " pad 0x" + llvm::utohexstr(C.LandingPadVAs[J]) + ";";
      Row(Text, nullptr);
      if (I < S.EHClauseBodies.size() && !S.EHClauseBodies[I].empty()) {
        Row(Pad + "{", nullptr);
        Body(S.EHClauseBodies[I], Indent + 1);
        Row(Pad + "}", nullptr);
      }
    }
    break;
  }
  default:
    throw std::invalid_argument("unknown HighIR statement kind");
  }
}
} // namespace

void printHighStatementRows(const HighStmt &Statement, int Indent,
                            HighIRRowSink Sink) {
  print(Statement, Indent, 0, Sink);
}
} // namespace neverd
