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
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/libc/LibCNames.h"

#include <functional>
#include <map>
#include <set>

namespace neverd {

bool isNoreturnCallExpr(const HighExpr &E) {
  // A bare return in the caller says nothing about whether another callee
  // returns. In particular, HighIR can assign a result to register version 0.
  // Only a known terminating operation authorizes omitting its result.
  return E.Kind == ExprKind::Call &&
         (libc::isNoReturnFunction(E.CallTarget) || isX86FastFailCall(E));
}

bool analyzeVoidReturn(const HighCAnalysisState &State, const HighFunc &Func,
                       VarNameFn VarFn, ExprStrFn ExprFn) {
  if (Func.SourceTypeHint && Func.SourceTypeHint->ReturnType)
    return Func.SourceTypeHint->ReturnType->Kind == NdTypeKind::Void;
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
      if (Src->Kind == ExprKind::Call) {
        if (isMsvcCxxThrowCallName(Src->CallTarget) || isNoreturnCallExpr(*Src))
          return true;
        if (Src->IntrinsicId != Intrinsic::None)
          return isSideeffectIntrinsic(Src->IntrinsicId) ||
                 !intrinsicCName(Src->IntrinsicId);
      }
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
    if (E.Kind == ExprKind::Undef)
      return true;
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
      // A retained private-frame load is a value boundary. Its address may
      // depend only on SP after bounded forwarding stops expanding a chain;
      // that does not make the loaded value an incidental incoming register.
      if (State.CanElideFrameStores)
        return false;
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
    if (E.Kind == ExprKind::Call)
      return isMsvcCxxThrowCallName(E.CallTarget) || isNoreturnCallExpr(E);
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

void analyzeUnusedAssigns(HighCAnalysisState &State, const HighFunc &Func,
                          VarNameFn VarFn, CallArgLimitFn ArgLimit) {
  auto ExprHasEffect = [](const HighExpr &E) -> bool {
    std::function<bool(const HighExpr &)> Walk = [&](const HighExpr &N) {
      if (N.Kind == ExprKind::Call || N.Kind == ExprKind::Store)
        return true;
      if (N.MemoryOrdering != NdMemoryOrdering::None)
        return true;
      for (const ExprPtr &Op : N.Operands)
        if (Op && Walk(*Op))
          return true;
      return false;
    };
    return Walk(E);
  };

  bool Changed = true;
  unsigned Guard = 0;
  while (Changed && Guard++ < 8) {
    Changed = false;
    std::map<std::string, TypeRef> Used;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (State.DeadStmts.count(&S))
        return;
      forEachRhsExpr(S, [&](const ExprPtr &E) {
        if (E)
          collectUsedVarsExpr(*E, Used, VarFn, ArgLimit);
      });
    });
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (State.DeadStmts.count(&S))
        return;
      if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
        return;
      if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
        return;
      if (ExprHasEffect(*S.Val))
        return;
      if (Used.count(VarFn(S.Dst->Var)))
        return;
      State.DeadStmts.insert(&S);
      Changed = true;
    });
  }
}

void analyzeUnusedCallResults(HighCAnalysisState &State, const HighFunc &Func,
                              VarNameFn VarFn, CallArgLimitFn ArgLimit) {
  std::map<std::string, TypeRef> Used;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    forEachRhsExpr(S, [&](const ExprPtr &E) {
      if (E)
        collectUsedVarsExpr(*E, Used, VarFn, ArgLimit);
    });
  });
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S) || State.OmittedCallResults.count(&S))
      return;
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return;
    if (S.Val->Kind != ExprKind::Call)
      return;
    if (isNoreturnCallExpr(*S.Val))
      return;
    if (Used.count(VarFn(S.Dst->Var)))
      return;
    State.OmittedCallResults.insert(&S);
  });

  // HighC prints cleanup `call();` and drops the trailing `return dest`.
  // The dest is still "used" by that skipped return, so omit it here.
  std::function<void(const std::vector<HighStmt> &)> WalkCleanup;
  WalkCleanup = [&](const std::vector<HighStmt> &Stmts) {
    for (const HighStmt &S : Stmts) {
      WalkCleanup(S.Body);
      WalkCleanup(S.ElseBody);
      for (const auto &C : S.Cases)
        WalkCleanup(C.Body);
      WalkCleanup(S.DefaultBody);
      for (size_t C = 0; C < S.EHClauseBodies.size(); ++C) {
        const bool Cleanup =
            C < S.EHClauses.size() &&
            S.EHClauses[C].Kind == HighEHClauseKind::CxxCleanup;
        if (Cleanup) {
          const auto &Body = S.EHClauseBodies[C];
          for (size_t J = 0; J < Body.size(); ++J) {
            const HighStmt &CS = Body[J];
            if (State.DeadStmts.count(&CS))
              continue;
            if (CS.Kind != StmtKind::Assign || !CS.Dst || !CS.Val)
              continue;
            if (CS.Val->Kind != ExprKind::Call)
              continue;
            const HighStmt *Ret = nullptr;
            for (size_t K = J + 1; K < Body.size(); ++K) {
              if (Body[K].Kind == StmtKind::Nop)
                continue;
              if (Body[K].Kind == StmtKind::Return)
                Ret = &Body[K];
              break;
            }
            if (Ret)
              State.OmittedCallResults.insert(&CS);
          }
        }
        WalkCleanup(S.EHClauseBodies[C]);
      }
    }
  };
  WalkCleanup(Func.Body);
}

} // namespace neverd
