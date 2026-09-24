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

#include <algorithm>

namespace neverd {

void collectUsedVarsExpr(const HighExpr &Expr,
                         std::map<std::string, TypeRef> &Vars,
                         VarNameFn VarFn, CallArgLimitFn ArgLimit) {
  std::set<const HighExpr *> Seen;
  std::vector<const HighExpr *> Work{&Expr};
  while (!Work.empty()) {
    const HighExpr *Cur = Work.back();
    Work.pop_back();
    if (!Cur || !Seen.insert(Cur).second)
      continue;
    if (isNamedValueExpr(*Cur)) {
      std::string Name = VarFn(Cur->Var);
      if (!Name.empty() && Vars.find(Name) == Vars.end())
        Vars[Name] = Cur->Type;
    }
    for (auto &CO : Cur->IntrinsicOutputs) {
      std::string Name = VarFn(CO);
      if (Name.empty())
        continue;
      auto Ty = NdType::makeInt(CO.Size, false);
      if (Vars.find(Name) == Vars.end())
        Vars[Name] = Ty;
    }
    size_t Limit = Cur->Operands.size();
    if (Cur->Kind == ExprKind::Call && ArgLimit)
      Limit = std::min(Limit, ArgLimit(*Cur));
    for (size_t I = Limit; I > 0; --I)
      if (Cur->Operands[I - 1])
        Work.push_back(Cur->Operands[I - 1].get());
    if (Cur->IndirectTarget)
      Work.push_back(Cur->IndirectTarget.get());
  }
}

void collectUsedVars(const std::vector<HighStmt> &Stmts,
                     std::map<std::string, TypeRef> &Vars, VarNameFn VarFn,
                     CallArgLimitFn ArgLimit) {
  for (auto &S : Stmts) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (E)
        collectUsedVarsExpr(*E, Vars, VarFn, ArgLimit);
    });
    collectUsedVars(S.Body, Vars, VarFn, ArgLimit);
    collectUsedVars(S.ElseBody, Vars, VarFn, ArgLimit);
    for (auto &C : S.Cases)
      collectUsedVars(C.Body, Vars, VarFn, ArgLimit);
    collectUsedVars(S.DefaultBody, Vars, VarFn, ArgLimit);
    for (auto &ClauseBody : S.EHClauseBodies)
      collectUsedVars(ClauseBody, Vars, VarFn, ArgLimit);
  }
}

} // namespace neverd
