//===- HighCVoidAnalysis.cpp - Void return analysis -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Void-return inference and dead-chain propagation for the HighIR C emitter.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/pass/HighC/HighCPasses.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

namespace neverd {

bool analyzeVoidReturn(const HighCAnalysisState &State, const HighFunc &Func,
                       VarNameFn VarFn, ExprStrFn ExprFn) {
  if (Func.ReturnType && Func.ReturnType->Kind == NdTypeKind::Void)
    return true;

  std::set<std::string> ParamNames;
  for (const HighParam &Param : Func.Params)
    ParamNames.insert(Param.Name);

  std::map<std::string, MedVar> SeenVars;
  std::set<const HighExpr *> SeenExprs;
  std::function<void(const HighExpr &)> CollectVars = [&](const HighExpr &E) {
    if (!SeenExprs.insert(&E).second)
      return;
    if (E.Kind == ExprKind::Var)
      SeenVars.emplace(VarFn(E.Var), E.Var);
    for (const ExprPtr &Operand : E.Operands)
      if (Operand)
        CollectVars(*Operand);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (E)
        CollectVars(*E);
    });
  });

  std::map<std::string, const HighExpr *> VarSources;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var &&
        S.Val)
      VarSources[VarFn(S.Dst->Var)] = S.Val.get();
  });

  auto AddressKey = [&](const HighExpr &Addr) {
    auto It = State.AddressKeys.find(&Addr);
    return It == State.AddressKeys.end() ? ExprFn(Addr) : It->second;
  };
  std::set<std::string> SideeffectLoadKeys;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    forEachRhsExpr(S, [&](const ExprPtr &E) {
      if (!E || E->Kind != ExprKind::Call ||
          !isSideeffectIntrinsic(E->IntrinsicId))
        return;
      std::set<const HighExpr *> Seen;
      std::function<void(const HighExpr &)> CollectLoads =
          [&](const HighExpr &Operand) {
            if (!Seen.insert(&Operand).second)
              return;
            if (Operand.Kind == ExprKind::Load && !Operand.Operands.empty())
              SideeffectLoadKeys.insert(AddressKey(*Operand.Operands[0]));
            for (const ExprPtr &Child : Operand.Operands)
              if (Child)
                CollectLoads(*Child);
          };
      for (const ExprPtr &Operand : E->Operands)
        if (Operand)
          CollectLoads(*Operand);
    });
  });

  auto IsVoidSource = [&](const std::string &VName) -> bool {
    if (ParamNames.count(VName))
      return false;
    auto It = VarSources.find(VName);
    if (It != VarSources.end()) {
      auto *Src = It->second;
      if (Src->Kind == ExprKind::Call && Src->IntrinsicId != Intrinsic::None)
        return isSideeffectIntrinsic(Src->IntrinsicId) ||
               !intrinsicCName(Src->IntrinsicId);
      return false;
    }

    auto SeenIt = SeenVars.find(VName);
    if (SeenIt == SeenVars.end())
      return false;
    const MedVar &V = SeenIt->second;
    return V.Kind == MedVar::Reg && V.SSAVer == 0;
  };

  std::function<bool(const HighExpr &)> IsVoidExpr =
      [&](const HighExpr &E) -> bool {
    if (E.Kind == ExprKind::Var)
      return IsVoidSource(VarFn(E.Var));
    if (E.Kind == ExprKind::BinOp && E.Op == NdOp::SELECT &&
        E.Operands.size() == 3)
      return IsVoidExpr(*E.Operands[1]) && IsVoidExpr(*E.Operands[2]);
    if (E.Kind == ExprKind::UnaryOp &&
        (E.Op == NdOp::INT_ZEXT || E.Op == NdOp::INT_SEXT ||
         E.Op == NdOp::SUBBYTES) &&
        !E.Operands.empty())
      return IsVoidExpr(*E.Operands[0]);
    if (E.Kind == ExprKind::Cast && !E.Operands.empty())
      return IsVoidExpr(*E.Operands[0]);
    if (E.Kind == ExprKind::Load && !E.Operands.empty()) {
      const HighExpr &AddrExpr = *E.Operands[0];
      std::string Rendered = ExprFn(AddrExpr);
      std::string Key = AddressKey(AddrExpr);
      auto DepIt = State.StoreFwdDeps.find(Rendered);
      if (DepIt == State.StoreFwdDeps.end()) {
        DepIt = State.ForwardedAddressDeps.find(Key);
        if (DepIt == State.ForwardedAddressDeps.end())
          return false;
      }
      for (const std::string &Name : DepIt->second)
        // A void side effect may leave its address operand in the native
        // return register even though the source function returns no value.
        if (!IsVoidSource(Name) &&
            !(ParamNames.count(Name) && SideeffectLoadKeys.count(Key)))
          return false;
      return true;
    }
    if (E.Kind == ExprKind::Const)
      return true;
    return false;
  };

  bool HasReturn = false;
  bool AllVoid = true;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Return || !S.RetVal)
      return;
    HasReturn = true;
    if (IsVoidExpr(*S.RetVal))
      return;
    AllVoid = false;
  });
  return HasReturn && AllVoid;
}

void analyzeVoidDeadChain(HighCAnalysisState &State, const HighFunc &Func,
                          VarNameFn VarFn) {
  std::set<std::string> NonReturnUses;
  std::set<const HighExpr *> SeenUses;
  std::function<void(const HighExpr &)> CollectUse = [&](const HighExpr &E) {
    if (!SeenUses.insert(&E).second)
      return;
    if (E.Kind == ExprKind::Var)
      NonReturnUses.insert(VarFn(E.Var));
    for (const ExprPtr &Op : E.Operands)
      if (Op)
        CollectUse(*Op);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Return || State.DeadStmts.count(&S))
      return;
    forEachRhsExpr(S, [&](const ExprPtr &E) {
      if (E)
        CollectUse(*E);
    });
  });

  std::function<void(const HighExpr &)> Collect = [&](const HighExpr &E) {
    if (E.Kind == ExprKind::Var) {
      const std::string Name = VarFn(E.Var);
      if (!NonReturnUses.count(Name))
        State.DeadVars.insert(Name);
    }
    for (const ExprPtr &Op : E.Operands)
      if (Op)
        Collect(*Op);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Return && S.RetVal)
      Collect(*S.RetVal);
  });
}

} // namespace neverd
