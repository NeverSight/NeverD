//===- HighCUsedVars.cpp - HighIR used-variable collection ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Used-variable collection shared by the HighIR C emitter and its analyses.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/pass/HighC/HighCPasses.h"

namespace neverd {

namespace {

template <typename Visitor>
void walkExprNodes(const HighExpr &Root, Visitor &&Visit) {
  std::set<const HighExpr *> Seen;
  std::vector<const HighExpr *> Work{&Root};
  while (!Work.empty()) {
    const HighExpr *Expr = Work.back();
    Work.pop_back();
    if (!Seen.insert(Expr).second)
      continue;
    Visit(*Expr);
    for (auto It = Expr->Operands.rbegin(); It != Expr->Operands.rend(); ++It)
      if (*It)
        Work.push_back(It->get());
  }
}

} // anonymous namespace

void collectUsedVarsExpr(const HighExpr &Expr,
                         std::map<std::string, TypeRef> &Vars,
                         VarNameFn VarFn) {
  walkExprNodes(Expr, [&](const HighExpr &E) {
    if (E.Kind == ExprKind::Var) {
      std::string Name = VarFn(E.Var);
      if (Vars.find(Name) == Vars.end())
        Vars[Name] = E.Type;
    }
    for (auto &CO : E.IntrinsicOutputs) {
      std::string Name = VarFn(CO);
      auto Ty = NdType::makeInt(CO.Size, false);
      if (Vars.find(Name) == Vars.end())
        Vars[Name] = Ty;
    }
  });
}

void collectUsedVars(const std::vector<HighStmt> &Stmts,
                     std::map<std::string, TypeRef> &Vars, VarNameFn VarFn) {
  for (auto &S : Stmts) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (E)
        collectUsedVarsExpr(*E, Vars, VarFn);
    });
    collectUsedVars(S.Body, Vars, VarFn);
    collectUsedVars(S.ElseBody, Vars, VarFn);
    for (auto &C : S.Cases)
      collectUsedVars(C.Body, Vars, VarFn);
    collectUsedVars(S.DefaultBody, Vars, VarFn);
  }
}

} // namespace neverd
