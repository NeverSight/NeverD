//===- HighCFuncWriter.cpp - HighIR function-level rendering --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Function-level orchestration for the HighIR C emitter: analysis pass
/// scheduling, local variable declaration emission, and function signature
/// rendering.  Statement-level rendering lives in HighCStmtWriter.cpp.
///
//===----------------------------------------------------------------------===//

#include "HighCWriter.h"

#include "neverd/ArchSupport.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"

#include <limits>

namespace neverd {

namespace {

uint64_t checkedStackAdd(uint64_t Left, uint64_t Right) {
  if (Left > std::numeric_limits<uint64_t>::max() - Right)
    llvm::report_fatal_error("HighC synthetic stack size overflow");
  return Left + Right;
}

uint64_t checkedStackAlign(uint64_t Size) {
  constexpr uint64_t Mask = kSyntheticStackAlignment - 1;
  return checkedStackAdd(Size, Mask) & ~Mask;
}

} // anonymous namespace

void HighCWriter::runAnalysisPasses(const HighFunc &Func) {
  GotoTargets.clear();
  collectGotoTargets(Func.Body);

  auto VarFn = [this](const MedVar &V) { return varName(V); };
  auto ExprFn = [this](const HighExpr &E) { return exprStr(E); };
  analyzeDeadStores(Analysis, Func, VarFn, ExprFn);
  analyzeStoreForwarding(Analysis, Func, VarFn, ExprFn);
  InferredVoid = analyzeVoidReturn(Analysis, Func, VarFn, ExprFn);

  HiLoPairs.clear();
  auto RegisterHiLo = [this](const HighStmt &S, const HighExpr &CE) {
    if (CE.IntrinsicOutputs.size() < 2)
      return;
    const char *Collapse = hiloCollapseExpr(CE.IntrinsicId);
    if (!Collapse)
      return;
    std::string Lo = varName(CE.IntrinsicOutputs[0]);
    std::string Hi = varName(CE.IntrinsicOutputs[1]);
    if (!Analysis.DeadVars.count(Lo) && !Analysis.DeadVars.count(Hi))
      HiLoPairs.push_back({Lo, Hi, Collapse, &S});
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (Analysis.DeadStmts.count(&S))
      return;
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        S.Dst->Kind == ExprKind::Var && S.Val->Kind == ExprKind::Call &&
        !S.Val->IntrinsicOutputs.empty()) {
      auto Rendered = MultiOutputRender{}(
          Opts.TheArch, S.Val->IntrinsicId, S.Val->IntrinsicOutputs,
          S.Val->Operands, [this](const HighExpr &E) { return exprStr(E); },
          [this](const MedVar &V) { return varName(V); },
          [this](const MedVar &V) {
            return !Analysis.DeadVars.count(varName(V));
          });
      if (!Rendered.empty())
        Analysis.DeadVars.insert(varName(S.Dst->Var));
      RegisterHiLo(S, *S.Val);
    }
    if (S.Kind == StmtKind::Call && S.CallExpr &&
        !S.CallExpr->IntrinsicOutputs.empty())
      RegisterHiLo(S, *S.CallExpr);
  });

  if (InferredVoid)
    analyzeVoidDeadChain(Analysis, Func, VarFn);

  if (!HiLoPairs.empty() && !InferredVoid) {
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Return || !S.RetVal)
        return;
      auto Collapsed = collapseHiLo(*S.RetVal);
      (void)Collapsed;
    });
  }

  FuncReturnType = InferredVoid ? nullptr : Func.ReturnType;
}

void HighCWriter::emitLocalDecls(const HighFunc &Func,
                                 const std::set<std::string> &ParamNames) {
  auto VarFn = [this](const MedVar &V) { return varName(V); };

  std::map<std::string, TypeRef> UsedVars;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (Analysis.DeadStmts.count(&S))
      return;
    forEachExpr(S, [&](const ExprPtr &E) {
      if (E)
        collectUsedVarsExpr(*E, UsedVars, VarFn);
    });
  });

  if (!Analysis.StoreFwd.empty()) {
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Store || !S.StoreAddr || !S.StoreVal)
        return;
      std::string Addr = exprStr(*S.StoreAddr);
      if (Analysis.StoreFwd.count(Addr))
        collectUsedVarsExpr(*S.StoreVal, UsedVars, VarFn);
    });
  }

  std::set<std::string> DeclaredNames(ParamNames);
  for (auto &Local : Func.Locals) {
    if (DeclaredNames.count(Local.Name))
      continue;
    if (UsedVars.find(Local.Name) == UsedVars.end())
      continue;
    if (Analysis.DeadVars.count(Local.Name))
      continue;
    DeclaredNames.insert(Local.Name);
    emitIndent(1);
    OS << typeToC(Local.Type) << " " << Local.Name << ";\n";
  }

  for (auto &[Name, Ty] : UsedVars) {
    if (DeclaredNames.count(Name))
      continue;
    if (Analysis.DeadVars.count(Name))
      continue;
    DeclaredNames.insert(Name);
    emitIndent(1);
    OS << typeToC(Ty) << " " << Name << ";\n";
  }
  if (!DeclaredNames.empty())
    OS << "\n";
}

void HighCWriter::writeFunction(const HighFunc &Func) {
  CurrentFunc = &Func;
  runAnalysisPasses(Func);

  bool NeedsFrameStorage = false;
  auto VisitFrameUses = [&](auto &&Self, const HighExpr &Expr,
                            bool IsValue) -> void {
    if (NeedsFrameStorage)
      return;
    if (Expr.Kind == ExprKind::Addr && !Expr.Operands.empty() &&
        Expr.Operands[0] && Expr.Operands[0]->Kind == ExprKind::Load) {
      for (const ExprPtr &AddressOperand : Expr.Operands[0]->Operands)
        if (AddressOperand)
          Self(Self, *AddressOperand, true);
      return;
    }
    if (IsValue && Expr.Kind == ExprKind::Load && !Expr.Operands.empty()) {
      std::string Addr = exprStr(*Expr.Operands[0]);
      auto FwdIt = Analysis.StoreFwd.find(Addr);
      if (FwdIt != Analysis.StoreFwd.end()) {
        auto DepIt = Analysis.StoreFwdDeps.find(Addr);
        NeedsFrameStorage = DepIt != Analysis.StoreFwdDeps.end() &&
                            DepIt->second.count("frame_base") != 0;
        return;
      }
    }
    if (Expr.Kind == ExprKind::Var && varName(Expr.Var) == "frame_base") {
      NeedsFrameStorage = true;
      return;
    }
    for (const ExprPtr &Operand : Expr.Operands)
      if (Operand)
        Self(Self, *Operand, true);
  };
  walkStmts(Func.Body, [&](const HighStmt &Stmt) {
    if (NeedsFrameStorage || Analysis.DeadStmts.count(&Stmt) ||
        (InferredVoid && Stmt.Kind == StmtKind::Return))
      return;
    if (Stmt.Kind == StmtKind::Assign && Stmt.Dst &&
        Stmt.Dst->Kind == ExprKind::Load)
      VisitFrameUses(VisitFrameUses, *Stmt.Dst, false);
    forEachRhsExpr(Stmt, [&](const ExprPtr &Expr) {
      if (Expr)
        VisitFrameUses(VisitFrameUses, *Expr, true);
    });
  });

  std::string RetType;
  if (InferredVoid)
    RetType = "void";
  else
    RetType = FuncReturnType ? typeToC(FuncReturnType) : "uint32_t";

  std::string FName = Func.Name;
  if (!FName.empty() && FName[0] == '_')
    FName = FName.substr(1);

  if (Opts.EmitComments && !Func.DebugName.empty() &&
      Func.DebugName != Func.Name) {
    OS << "/* " << Func.DebugName;
    if (!Func.SourceFile.empty())
      OS << " @ " << Func.SourceFile << ":" << Func.SourceLine;
    OS << " */\n";
  }

  OS << RetType << " " << FName << "(";
  for (size_t I = 0; I < Func.Params.size(); ++I) {
    if (I > 0)
      OS << ", ";
    OS << typeToC(Func.Params[I].Type) << " " << Func.Params[I].Name;
  }
  if (Func.Params.empty())
    OS << "void";
  OS << ") {\n";

  std::set<std::string> ParamNames;
  for (auto &P : Func.Params)
    ParamNames.insert(P.Name);
  if (Func.FrameSize > 0 || Func.FrameHeadroom > 0)
    ParamNames.insert("frame_base");
  if (NeedsFrameStorage &&
      (Func.FrameSize > 0 || Func.FrameHeadroom > 0)) {
    uint64_t LowerSize = checkedStackAlign(
        Func.FrameSize > 0 ? static_cast<uint64_t>(Func.FrameSize) : 0);
    uint64_t UpperSize = checkedStackAlign(
        Func.FrameHeadroom > 0 ? static_cast<uint64_t>(Func.FrameHeadroom) : 0);
    uint64_t EntryResidue =
        syntheticEntryStackResidue(Opts.TheArch, Opts.Format);
    uint64_t FrameBaseOffset = checkedStackAdd(LowerSize, EntryResidue);
    uint64_t StorageSize = checkedStackAdd(FrameBaseOffset, UpperSize);
    emitIndent(1);
    OS << "_Alignas(" << kSyntheticStackAlignment << ") uint8_t stack_storage["
       << StorageSize << "];\n";
    emitIndent(1);
    OS << "const uintptr_t frame_base = (uintptr_t)(stack_storage + "
       << FrameBaseOffset << ");\n";
  }
  emitLocalDecls(Func, ParamNames);

  writeStmts(Func.Body, 1);
  OS << "}\n";
}

} // namespace neverd
