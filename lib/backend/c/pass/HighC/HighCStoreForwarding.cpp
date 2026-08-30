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
    if (S.MemoryOrdering != NdMemoryOrdering::None ||
        S.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      HasOrderedMemory = true;
    forEachExpr(S, [&](const ExprPtr &E) {
      if (!E)
        return;
      std::set<const HighExpr *> Seen;
      std::vector<const HighExpr *> Work{E.get()};
      while (!Work.empty()) {
        const HighExpr *Node = Work.back();
        Work.pop_back();
        if (!Node || !Seen.insert(Node).second)
          continue;
        if (Node->MemoryOrdering != NdMemoryOrdering::None ||
            Node->MemoryAddressSpace != NdMemoryAddressSpace::Default)
          HasOrderedMemory = true;
        for (const ExprPtr &Operand : Node->Operands)
          Work.push_back(Operand.get());
      }
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

  std::map<std::string, std::string> CandidateValues;
  for (const std::string &Addr : LoadedAddrs) {
    auto It = AddrToVal.find(Addr);
    if (It != AddrToVal.end())
      CandidateValues.emplace(Addr, It->second);
  }

  if (CandidateValues.empty())
    return;

  std::set<std::string> CallResults;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        S.Dst->Kind == ExprKind::Var && S.Val->Kind == ExprKind::Call)
      CallResults.insert(VarFn(S.Dst->Var));
  });

  std::set<std::string> UsedParams;
  std::set<std::string> ParamForwardedAddrs;
  std::map<std::string, std::string> ParamSourceVars;
  for (auto &[Addr, Val] : CandidateValues) {
    auto ExprIt = AddrToValExpr.find(Addr);
    if (ExprIt != AddrToValExpr.end() &&
        ExprIt->second->Kind == ExprKind::Var &&
        ExprIt->second->Var.Kind != MedVar::Param) {
      std::string VName = VarFn(ExprIt->second->Var);
      // A call/intrinsic result spilled to the stack is a newly produced
      // value, not a recoverable incoming parameter.  Substituting a same-size
      // parameter here silently discards the call result on reload.
      if (CallResults.count(VName))
        continue;
      for (auto &P : Func.Params) {
        if (UsedParams.count(P.Name))
          continue;
        bool TypeOk = (!P.Type || !ExprIt->second->Type ||
                       P.Type->Size == ExprIt->second->Type->Size);
        if (TypeOk) {
          UsedParams.insert(P.Name);
          Val = P.Name;
          ParamForwardedAddrs.insert(Addr);
          ParamSourceVars.emplace(Addr, std::move(VName));
          break;
        }
      }
    }
  }

  // The C writer serializes expression DAGs as trees.  Repeatedly rendering a
  // chain such as slot[n] = load(slot[n-1]) + load(slot[n-1]) therefore doubles
  // the stored string at every level.  Select forwarding candidates in
  // dependency order and stop inlining at a finite per-expression budget.  A
  // rejected candidate is deliberately absent from StoreFwd, so its original
  // store and every load from it remain an executable memory boundary.
  constexpr size_t MaxForwardedExpressionBytes = 16 * 1024;
  constexpr size_t MaxDependencyWalkNodes = MaxForwardedExpressionBytes;

  std::map<std::string, std::vector<std::string>> DirectDeps;
  std::set<std::string> Eligible;
  for (const auto &[Addr, RawValue] : CandidateValues) {
    if (RawValue.size() > MaxForwardedExpressionBytes)
      continue;
    if (ParamForwardedAddrs.count(Addr)) {
      Eligible.insert(Addr);
      continue;
    }

    auto ExprIt = AddrToValExpr.find(Addr);
    if (ExprIt == AddrToValExpr.end() || !ExprIt->second)
      continue;

    std::vector<const HighExpr *> Work{ExprIt->second};
    size_t Visited = 0;
    bool Complete = true;
    while (!Work.empty()) {
      const HighExpr *Expr = Work.back();
      Work.pop_back();
      if (!Expr)
        continue;
      if (++Visited > MaxDependencyWalkNodes) {
        Complete = false;
        break;
      }

      // Taking the address of a load renders only the load's address.  The
      // load itself is an lvalue and must not become a forwarded value.
      if (Expr->Kind == ExprKind::Addr && !Expr->Operands.empty() &&
          Expr->Operands[0] && Expr->Operands[0]->Kind == ExprKind::Load) {
        for (const ExprPtr &AddressOperand : Expr->Operands[0]->Operands)
          if (AddressOperand)
            Work.push_back(AddressOperand.get());
        continue;
      }

      if (Expr->Kind == ExprKind::Load && !Expr->Operands.empty() &&
          Expr->MemoryOrdering == NdMemoryOrdering::None) {
        std::string LoadedAddr = ExprFn(*Expr->Operands[0]);
        if (CandidateValues.count(LoadedAddr))
          DirectDeps[Addr].push_back(std::move(LoadedAddr));
      }
      for (const ExprPtr &Operand : Expr->Operands)
        if (Operand)
          Work.push_back(Operand.get());
    }
    if (Complete)
      Eligible.insert(Addr);
  }

  std::map<std::string, std::set<std::string>> UniqueDeps;
  std::map<std::string, std::vector<std::string>> Users;
  std::map<std::string, size_t> PendingDeps;
  for (const std::string &Addr : Eligible) {
    for (const std::string &Dep : DirectDeps[Addr])
      if (Eligible.count(Dep))
        UniqueDeps[Addr].insert(Dep);
    PendingDeps[Addr] = UniqueDeps[Addr].size();
    for (const std::string &Dep : UniqueDeps[Addr])
      Users[Dep].push_back(Addr);
  }

  std::set<std::string> Ready;
  for (const auto &[Addr, Pending] : PendingDeps)
    if (Pending == 0)
      Ready.insert(Addr);

  while (!Ready.empty()) {
    std::string Addr = *Ready.begin();
    Ready.erase(Ready.begin());

    size_t UpperBound = CandidateValues[Addr].size();
    bool WithinBudget = UpperBound <= MaxForwardedExpressionBytes;
    for (const std::string &Dep : DirectDeps[Addr]) {
      auto FwdIt = State.StoreFwd.find(Dep);
      if (FwdIt == State.StoreFwd.end())
        continue;
      if (FwdIt->second.size() > MaxForwardedExpressionBytes - UpperBound) {
        WithinBudget = false;
        break;
      }
      UpperBound += FwdIt->second.size();
    }

    if (WithinBudget) {
      std::string Expanded;
      if (ParamForwardedAddrs.count(Addr)) {
        Expanded = CandidateValues[Addr];
      } else {
        auto ExprIt = AddrToValExpr.find(Addr);
        if (ExprIt != AddrToValExpr.end() && ExprIt->second)
          Expanded = ExprFn(*ExprIt->second);
      }
      if (!Expanded.empty() && Expanded.size() <= MaxForwardedExpressionBytes)
        State.StoreFwd.emplace(Addr, std::move(Expanded));
    }

    for (const std::string &User : Users[Addr]) {
      auto PendingIt = PendingDeps.find(User);
      if (PendingIt != PendingDeps.end() && PendingIt->second > 0 &&
          --PendingIt->second == 0)
        Ready.insert(User);
    }
  }

  // Nodes left outside the dependency order contain or depend on a cycle.
  // They stay materialized in memory, just like candidates rejected by the
  // size bound.  This is conservative and preserves the original loads and
  // stores instead of inventing a value for a cyclic forwarding graph.
  if (State.StoreFwd.empty())
    return;

  for (const auto &[Addr, _] : State.StoreFwd) {
    if (ParamForwardedAddrs.count(Addr)) {
      State.StoreFwdDeps[Addr] = {CandidateValues[Addr]};
      auto SourceIt = ParamSourceVars.find(Addr);
      if (SourceIt != ParamSourceVars.end())
        State.DeadVars.insert(SourceIt->second);
      continue;
    }
    auto ExprIt = AddrToValExpr.find(Addr);
    if (ExprIt == AddrToValExpr.end() || !ExprIt->second)
      continue;
    std::map<std::string, TypeRef> Vars;
    collectUsedVarsExpr(*ExprIt->second, Vars, VarFn);
    for (const auto &[Name, _] : Vars)
      State.StoreFwdDeps[Addr].insert(Name);
  }

  auto CollectForwardedDeps = [&](const HighExpr &Root) {
    std::set<std::string> Deps;
    std::set<const HighExpr *> Seen;
    std::function<void(const HighExpr &)> Visit = [&](const HighExpr &E) {
      if (!Seen.insert(&E).second)
        return;
      if (E.Kind == ExprKind::Load && !E.Operands.empty() &&
          E.MemoryOrdering == NdMemoryOrdering::None) {
        std::string LoadedAddr = ExprFn(*E.Operands[0]);
        auto FwdIt = State.StoreFwd.find(LoadedAddr);
        auto DepIt = State.StoreFwdDeps.find(LoadedAddr);
        if (FwdIt != State.StoreFwd.end() &&
            DepIt != State.StoreFwdDeps.end()) {
          Deps.insert(DepIt->second.begin(), DepIt->second.end());
          return;
        }
      }
      if (E.Kind == ExprKind::Var)
        Deps.insert(VarFn(E.Var));
      for (const ExprPtr &Operand : E.Operands)
        if (Operand)
          Visit(*Operand);
    };
    Visit(Root);
    return Deps;
  };

  // Keep liveness dependencies in sync with the transitive value expansion.
  for (size_t Iter = 0; Iter <= State.StoreFwd.size(); ++Iter) {
    bool Changed = false;
    for (const auto &[Addr, ValueExpr] : AddrToValExpr) {
      if (!State.StoreFwd.count(Addr) || !ValueExpr ||
          ParamForwardedAddrs.count(Addr))
        continue;
      auto ExpandedDeps = CollectForwardedDeps(*ValueExpr);
      if (ExpandedDeps != State.StoreFwdDeps[Addr]) {
        State.StoreFwdDeps[Addr] = std::move(ExpandedDeps);
        Changed = true;
      }
    }
    if (!Changed)
      break;
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
