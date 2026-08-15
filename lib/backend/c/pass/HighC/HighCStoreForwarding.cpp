//===- HighCStoreForwarding.cpp - Store forwarding analysis -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Store-to-load forwarding analysis for the HighIR C emitter.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/pass/HighC/HighCPasses.h"

namespace neverd {

void analyzeStoreForwarding(HighCAnalysisState &State, const HighFunc &Func,
                            VarNameFn VarFn, ExprStrFn ExprFn) {
  State.StoreFwd.clear();
  State.StoreFwdDeps.clear();
  State.ForwardedAddressDeps.clear();

  bool HasOrderedMemory = false;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.MemoryOrdering != NdMemoryOrdering::None)
      HasOrderedMemory = true;
    forEachExpr(S, [&](const ExprPtr &E) {
      if (E && E->hasOrderedMemoryAccess())
        HasOrderedMemory = true;
    });
  });
  if (HasOrderedMemory)
    return;

  std::map<std::string, std::string> AddrToVal;
  std::map<std::string, const HighExpr *> AddrToValExpr;
  std::map<std::string, std::string> AddrToKey;
  std::set<std::string> LoadedAddrs;
  auto AddressKey = [&](const HighExpr &Addr) {
    auto It = State.AddressKeys.find(&Addr);
    return It == State.AddressKeys.end() ? ExprFn(Addr) : It->second;
  };
  std::function<void(const HighExpr &)> ScanValueLoads;
  ScanValueLoads = [&](const HighExpr &E) {
    if (E.Kind == ExprKind::Addr && !E.Operands.empty() && E.Operands[0] &&
        E.Operands[0]->Kind == ExprKind::Load) {
      for (const ExprPtr &AddressOperand : E.Operands[0]->Operands)
        if (AddressOperand)
          ScanValueLoads(*AddressOperand);
      return;
    }
    if (E.Kind == ExprKind::Load && !E.Operands.empty() &&
        E.MemoryOrdering == NdMemoryOrdering::None)
      LoadedAddrs.insert(ExprFn(*E.Operands[0]));
    for (const ExprPtr &Operand : E.Operands)
      if (Operand)
        ScanValueLoads(*Operand);
  };

  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    if (S.Kind == StmtKind::Store && S.StoreAddr && S.StoreVal &&
        S.MemoryOrdering == NdMemoryOrdering::None) {
      std::string Addr = ExprFn(*S.StoreAddr);
      std::string Val = ExprFn(*S.StoreVal);
      AddrToVal[Addr] = Val;
      AddrToValExpr[Addr] = S.StoreVal.get();
      AddrToKey[Addr] = AddressKey(*S.StoreAddr);
    }
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Load)
      for (const ExprPtr &AddressOperand : S.Dst->Operands)
        if (AddressOperand)
          ScanValueLoads(*AddressOperand);
    forEachRhsExpr(S, [&](const ExprPtr &E) {
      if (E)
        ScanValueLoads(*E);
    });
  });

  for (const std::string &Addr : LoadedAddrs) {
    auto It = AddrToVal.find(Addr);
    if (It != AddrToVal.end()) {
      State.StoreFwd[Addr] = It->second;
      auto ExprIt = AddrToValExpr.find(Addr);
      if (ExprIt != AddrToValExpr.end()) {
        std::map<std::string, TypeRef> Vars;
        collectUsedVarsExpr(*ExprIt->second, Vars, VarFn);
        for (const auto &[Name, _] : Vars)
          State.StoreFwdDeps[Addr].insert(Name);
      }
    }
  }

  if (State.StoreFwd.empty())
    return;

  std::set<std::string> UsedParams;
  for (auto &[Addr, Val] : State.StoreFwd) {
    auto ExprIt = AddrToValExpr.find(Addr);
    if (ExprIt != AddrToValExpr.end() &&
        ExprIt->second->Kind == ExprKind::Var &&
        ExprIt->second->Var.Kind != MedVar::Param) {
      std::string VName = VarFn(ExprIt->second->Var);
      for (auto &P : Func.Params) {
        if (UsedParams.count(P.Name))
          continue;
        bool TypeOk = (!P.Type || !ExprIt->second->Type ||
                       P.Type->Size == ExprIt->second->Type->Size);
        if (TypeOk) {
          UsedParams.insert(P.Name);
          State.StoreFwd[Addr] = P.Name;
          State.StoreFwdDeps[Addr] = {P.Name};
          State.DeadVars.insert(VName);
          break;
        }
      }
    }
  }

  for (const auto &[Addr, _] : State.StoreFwd) {
    auto KeyIt = AddrToKey.find(Addr);
    auto DepIt = State.StoreFwdDeps.find(Addr);
    if (KeyIt != AddrToKey.end() && DepIt != State.StoreFwdDeps.end())
      State.ForwardedAddressDeps[KeyIt->second] = DepIt->second;
  }

  for (auto &[Addr, _] : State.StoreFwd) {
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (State.DeadStmts.count(&S))
        return;
      if (S.Kind == StmtKind::Store && S.StoreAddr &&
          S.MemoryOrdering == NdMemoryOrdering::None) {
        if (ExprFn(*S.StoreAddr) == Addr)
          State.DeadStmts.insert(&S);
      }
    });
  }

  auto CollectAliveSkipFwd = [&](const HighExpr &E,
                                 std::set<std::string> &Out) {
    std::set<const HighExpr *> Seen;
    std::function<void(const HighExpr &)> Visit = [&](const HighExpr &Ex) {
      if (!Seen.insert(&Ex).second)
        return;
      if (Ex.Kind == ExprKind::Addr && !Ex.Operands.empty() && Ex.Operands[0] &&
          Ex.Operands[0]->Kind == ExprKind::Load) {
        for (const ExprPtr &AddressOperand : Ex.Operands[0]->Operands)
          if (AddressOperand)
            Visit(*AddressOperand);
        return;
      }
      if (Ex.Kind == ExprKind::Load && !Ex.Operands.empty() &&
          Ex.MemoryOrdering == NdMemoryOrdering::None) {
        std::string Addr = ExprFn(*Ex.Operands[0]);
        auto FwdIt = State.StoreFwd.find(Addr);
        if (FwdIt != State.StoreFwd.end()) {
          auto DepIt = State.StoreFwdDeps.find(Addr);
          if (DepIt != State.StoreFwdDeps.end())
            Out.insert(DepIt->second.begin(), DepIt->second.end());
          return;
        }
      }
      if (Ex.Kind == ExprKind::Var)
        Out.insert(VarFn(Ex.Var));
      for (auto &Op : Ex.Operands)
        if (Op)
          Visit(*Op);
    };
    Visit(E);
  };

  bool Changed = true;
  while (Changed) {
    Changed = false;
    std::set<std::string> Alive;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (State.DeadStmts.count(&S))
        return;
      if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Load)
        for (const ExprPtr &AddressOperand : S.Dst->Operands)
          if (AddressOperand)
            CollectAliveSkipFwd(*AddressOperand, Alive);
      forEachRhsExpr(S, [&](const ExprPtr &E) {
        if (E)
          CollectAliveSkipFwd(*E, Alive);
      });
    });
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (State.DeadStmts.count(&S))
        return;
      if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var) {
        if (S.Val &&
            (S.Val->Kind == ExprKind::Call || S.Val->hasOrderedMemoryAccess()))
          return;
        std::string Name = VarFn(S.Dst->Var);
        if (!Alive.count(Name)) {
          State.DeadStmts.insert(&S);
          if (State.DeadVars.insert(Name).second)
            Changed = true;
        }
      }
    });
  }
}

} // namespace neverd
