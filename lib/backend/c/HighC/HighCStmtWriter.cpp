//===- HighCStmtWriter.cpp - HighIR statement rendering -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Statement rendering for the HighIR C emitter: converts HighStmt
/// structures into indented C source blocks.  Function-level orchestration
/// lives in HighCFuncWriter.cpp.
///
//===----------------------------------------------------------------------===//

#include "HighCWriter.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd {

void HighCWriter::emitIndent(int Indent) { emitCIndent(OS, Indent); }

void HighCWriter::writeStmt(const HighStmt &Stmt, int Indent) {
  if (Analysis.DeadStmts.count(&Stmt))
    return;
  switch (Stmt.Kind) {
  case StmtKind::Assign:
    if (!Stmt.Dst || !Stmt.Val)
      return;
    if (Stmt.Val->Kind == ExprKind::Call &&
        !Stmt.Val->IntrinsicOutputs.empty()) {
      auto Rendered = MultiOutputRender{}(
          Opts.TheArch, Stmt.Val->IntrinsicId, Stmt.Val->IntrinsicOutputs,
          Stmt.Val->Operands, [this](const HighExpr &E) { return exprStr(E); },
          [this](const MedVar &V) { return varName(V); },
          [this](const MedVar &V) {
            return !Analysis.DeadVars.count(varName(V));
          });
      if (!Rendered.empty()) {
        emitIndent(Indent);
        OS << Rendered;
        HasCIntrinsics = true;
        break;
      }
    }
    if (Stmt.Val->Kind == ExprKind::Call &&
        Stmt.Val->IntrinsicId != Intrinsic::None &&
        (isSideeffectIntrinsic(Stmt.Val->IntrinsicId) ||
         !intrinsicCName(Stmt.Val->IntrinsicId))) {
      emitIndent(Indent);
      OS << exprStr(*Stmt.Val) << ";\n";
      break;
    }
    if (Stmt.Dst->Kind == ExprKind::Load && !Stmt.Dst->Operands.empty()) {
      emitIndent(Indent);
      OS << memoryStoreExpr(Stmt.Dst->Type, exprStr(*Stmt.Dst->Operands[0]),
                            exprStr(*Stmt.Val))
         << ";\n";
      break;
    }
    emitIndent(Indent);
    OS << exprStr(*Stmt.Dst) << " = " << exprStr(*Stmt.Val) << ";\n";
    break;

  case StmtKind::Store:
    if (!Stmt.StoreAddr || !Stmt.StoreVal)
      return;
    emitIndent(Indent);
    OS << memoryStoreExpr(Stmt.StoreVal->Type, exprStr(*Stmt.StoreAddr),
                          exprStr(*Stmt.StoreVal))
       << ";\n";
    break;

  case StmtKind::Call:
    if (!Stmt.CallExpr)
      return;
    if (!Stmt.CallExpr->IntrinsicOutputs.empty()) {
      auto Rendered = MultiOutputRender{}(
          Opts.TheArch, Stmt.CallExpr->IntrinsicId,
          Stmt.CallExpr->IntrinsicOutputs, Stmt.CallExpr->Operands,
          [this](const HighExpr &E) { return exprStr(E); },
          [this](const MedVar &V) { return varName(V); },
          [this](const MedVar &V) {
            return !Analysis.DeadVars.count(varName(V));
          });
      if (!Rendered.empty()) {
        emitIndent(Indent);
        OS << Rendered;
        HasCIntrinsics = true;
        break;
      }
    }
    emitIndent(Indent);
    OS << exprStr(*Stmt.CallExpr) << ";\n";
    break;

  case StmtKind::Return:
    emitIndent(Indent);
    if (InferredVoid)
      OS << "return;\n";
    else if (Stmt.RetVal)
      OS << "return " << formatReturnExpr(*Stmt.RetVal) << ";\n";
    else
      OS << "return;\n";
    break;

  case StmtKind::If:
    if (!Stmt.Cond)
      return;
    emitIndent(Indent);
    OS << "if (" << exprStr(*Stmt.Cond) << ") {\n";
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::IfElse:
    if (!Stmt.Cond)
      return;
    emitIndent(Indent);
    OS << "if (" << exprStr(*Stmt.Cond) << ") {\n";
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "} else {\n";
    writeStmts(Stmt.ElseBody, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::While:
    emitIndent(Indent);
    OS << "while (" << (Stmt.Cond ? exprStr(*Stmt.Cond) : "1") << ") {\n";
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::DoWhile:
    emitIndent(Indent);
    OS << "do {\n";
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "} while (" << (Stmt.Cond ? exprStr(*Stmt.Cond) : "1") << ");\n";
    break;

  case StmtKind::For:
    emitIndent(Indent);
    OS << "for (;;) {\n";
    if (Stmt.Cond) {
      emitIndent(Indent + 1);
      OS << "if (!(" << exprStr(*Stmt.Cond) << ")) break;\n";
    }
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::Switch:
    if (!Stmt.SwitchExpr)
      return;
    emitIndent(Indent);
    OS << "switch (" << exprStr(*Stmt.SwitchExpr) << ") {\n";
    for (auto &C : Stmt.Cases) {
      emitIndent(Indent);
      OS << "case " << constStr(C.Value) << ":\n";
      writeStmts(C.Body, Indent + 1);
      emitIndent(Indent + 1);
      OS << "break;\n";
    }
    if (!Stmt.DefaultBody.empty()) {
      emitIndent(Indent);
      OS << "default:\n";
      writeStmts(Stmt.DefaultBody, Indent + 1);
      emitIndent(Indent + 1);
      OS << "break;\n";
    }
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::Goto:
    emitIndent(Indent);
    OS << "goto L_" + llvm::utohexstr(Stmt.GotoTarget) + ";\n";
    break;

  case StmtKind::Break:
    emitIndent(Indent);
    OS << "break;\n";
    break;

  case StmtKind::Continue:
    emitIndent(Indent);
    OS << "continue;\n";
    break;

  case StmtKind::Block:
    emitIndent(Indent);
    OS << "{\n";
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::ExprStmt:
    if (Stmt.Val) {
      emitIndent(Indent);
      OS << exprStr(*Stmt.Val) << ";\n";
    }
    break;

  case StmtKind::Nop:
    break;
  }
}

void HighCWriter::writeStmts(const std::vector<HighStmt> &Stmts, int Indent) {
  for (auto &S : Stmts) {
    if (S.Addr != 0 && S.Addr != InvalidVA && GotoTargets.count(S.Addr))
      OS << "L_" + llvm::utohexstr(S.Addr) + ":\n";
    writeStmt(S, Indent);
  }
}

void HighCWriter::collectGotoTargets(const std::vector<HighStmt> &Stmts) {
  for (auto &S : Stmts) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      GotoTargets.insert(S.GotoTarget);
    collectGotoTargets(S.Body);
    collectGotoTargets(S.ElseBody);
    for (auto &C : S.Cases)
      collectGotoTargets(C.Body);
    collectGotoTargets(S.DefaultBody);
  }
}

} // namespace neverd
