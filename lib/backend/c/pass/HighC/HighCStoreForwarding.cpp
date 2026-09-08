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
#include "neverd/backend/c/render/CTypeFormat.h"

#include <algorithm>

namespace neverd {

namespace {

bool isForwardableInteger(const TypeRef &Type) {
  return Type && Type->Kind == NdTypeKind::Int &&
         (Type->Size == 1 || Type->Size == 2 || Type->Size == 4 ||
          Type->Size == 8);
}

std::string storedIntegerValue(const HighExpr &Value, ExprStrFn ExprFn) {
  // C promotes byte/short arithmetic to int. Preserve the truncation that
  // the actual memory write performed before a later load interprets it.
  return "(" + typeToC(Value.Type) + ")(" + ExprFn(Value) + ")";
}

// This renderer substitutes loads by address across the entire function. That
// is valid only for immutable private slots initialized before every use in a
// straight-line body. It has no CFG reaching-definition/alias analysis for
// mutable memory. Preserve those accesses instead of moving a later store
// backward across an earlier load, branch, call, or loop iteration.
bool hasImmutableReachingStores(const HighCAnalysisState &State,
                                const HighFunc &Func, VarNameFn VarFn,
                                ExprStrFn ExprFn) {
  if (!State.CanElideFrameStores)
    return false;
  std::set<std::string> InitializedSlots;
  std::set<std::string> DefinedVars;
  auto AddressKey = [&](const HighExpr &Address) {
    const auto It = State.AddressKeys.find(&Address);
    return It == State.AddressKeys.end() ? ExprFn(Address) : It->second;
  };
  std::function<bool(const HighExpr &, unsigned)> CheckValue =
      [&](const HighExpr &Expr, unsigned Depth) {
        if (Depth > 128 || Expr.Kind == ExprKind::Call ||
            Expr.Kind == ExprKind::Addr || Expr.Kind == ExprKind::Store)
          return false;
        if (Expr.Kind == ExprKind::Load)
          return Expr.Operands.size() == 1 && Expr.Operands[0] &&
                 InitializedSlots.count(AddressKey(*Expr.Operands[0])) != 0;
        if (Expr.Kind == ExprKind::Var)
          return Expr.Var.Kind == MedVar::Param ||
                 DefinedVars.count(VarFn(Expr.Var)) != 0;
        for (const auto &Operand : Expr.Operands)
          if (!Operand || !CheckValue(*Operand, Depth + 1))
            return false;
        return true;
      };
  for (const auto &Stmt : Func.Body) {
    if (State.DeadStmts.count(&Stmt))
      continue;
    if (!Stmt.Body.empty() || !Stmt.ElseBody.empty() || !Stmt.Cases.empty() ||
        !Stmt.DefaultBody.empty() || !Stmt.EHClauseBodies.empty())
      return false;
    if (Stmt.Kind == StmtKind::Store) {
      if (!Stmt.StoreAddr || !Stmt.StoreVal || !CheckValue(*Stmt.StoreVal, 0) ||
          !InitializedSlots.insert(AddressKey(*Stmt.StoreAddr)).second)
        return false;
    } else if (Stmt.Kind == StmtKind::Assign) {
      if (!Stmt.Dst || Stmt.Dst->Kind != ExprKind::Var || !Stmt.Val ||
          Stmt.Dst->Var.Kind == MedVar::Param || !CheckValue(*Stmt.Val, 0) ||
          !DefinedVars.insert(VarFn(Stmt.Dst->Var)).second)
        return false;
    } else if (Stmt.Kind == StmtKind::Return ||
               Stmt.Kind == StmtKind::ExprStmt) {
      bool Valid = true;
      forEachRhsExpr(Stmt, [&](const ExprPtr &Expr) {
        if (Expr)
          Valid &= CheckValue(*Expr, 0);
      });
      if (!Valid)
        return false;
    } else {
      return false;
    }
  }
  return true;
}

} // namespace

void analyzeStoreForwarding(HighCAnalysisState &State, const HighFunc &Func,
                            VarNameFn VarFn, ExprStrFn ExprFn) {
  State.StoreFwd.clear();
  State.StoreFwdDeps.clear();
  State.ForwardedAddressDeps.clear();
  if (!hasImmutableReachingStores(State, Func, VarFn, ExprFn))
    return;

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
  std::map<std::string, std::vector<std::pair<std::string, TypeRef>>> SlotLoads;
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
        E.MemoryOrdering == NdMemoryOrdering::None) {
      const std::string Addr = ExprFn(*E.Operands[0]);
      LoadedAddrs.insert(Addr);
      SlotLoads[AddressKey(*E.Operands[0])].emplace_back(Addr, E.Type);
    }
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
  std::map<std::string, size_t> ReadCastBytes;
  for (const std::string &Addr : LoadedAddrs) {
    auto It = AddrToVal.find(Addr);
    if (It == AddrToVal.end())
      continue;
    const HighExpr &Value = *AddrToValExpr.at(Addr);
    if (!isForwardableInteger(Value.Type))
      continue;

    auto Loads = SlotLoads.find(AddrToKey.at(Addr));
    if (Loads == SlotLoads.end())
      continue;
    bool Compatible = true;
    size_t MaxReadCastBytes = 0;
    for (const auto &[LoadAddr, LoadType] : Loads->second) {
      // Every alias must be substituted before removing the only store.
      // Float/integer or pointer reinterpretation requires a bitcast, not a
      // numeric conversion; retain those memory boundaries conservatively.
      if (LoadAddr != Addr || !isForwardableInteger(LoadType) ||
          LoadType->Size != Value.Type->Size) {
        Compatible = false;
        break;
      }
      MaxReadCastBytes =
          std::max(MaxReadCastBytes, typeToC(LoadType).size() + 4);
    }
    if (Compatible) {
      CandidateValues.emplace(Addr, storedIntegerValue(Value, ExprFn));
      ReadCastBytes.emplace(Addr, MaxReadCastBytes);
    }
  }

  if (CandidateValues.empty())
    return;

  // The C writer serializes expression DAGs as trees.  Repeatedly rendering a
  // chain such as slot[n] = load(slot[n-1]) + load(slot[n-1]) therefore doubles
  // the stored string at every level.  Select forwarding candidates in
  // dependency order and stop inlining at a finite per-expression budget.  A
  // rejected candidate is deliberately absent from StoreFwd, so its original
  // store and every load from it remain an executable memory boundary.
  constexpr size_t MaxForwardedExpressionBytes = 16 * 1024;
  constexpr size_t MaxTotalForwardedBytes = 16 * 1024;
  constexpr size_t MaxDependencyWalkNodes = MaxForwardedExpressionBytes;

  std::map<std::string, std::vector<std::string>> DirectDeps;
  std::set<std::string> Eligible;
  for (const auto &[Addr, RawValue] : CandidateValues) {
    if (RawValue.size() + ReadCastBytes.at(Addr) > MaxForwardedExpressionBytes)
      continue;
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

  size_t TotalForwardedBytes = 0;
  while (!Ready.empty()) {
    std::string Addr = *Ready.begin();
    Ready.erase(Ready.begin());

    // Account for the actual store cast, each substituted load cast, and the
    // largest final load cast before rendering an expanded expression tree.
    size_t UpperBound = CandidateValues[Addr].size() + ReadCastBytes.at(Addr);
    bool WithinBudget = UpperBound <= MaxForwardedExpressionBytes;
    for (const std::string &Dep : DirectDeps[Addr]) {
      auto FwdIt = State.StoreFwd.find(Dep);
      if (FwdIt == State.StoreFwd.end())
        continue;
      const size_t AddedBytes = FwdIt->second.size() + ReadCastBytes.at(Dep);
      if (AddedBytes > MaxForwardedExpressionBytes - UpperBound) {
        WithinBudget = false;
        break;
      }
      UpperBound += AddedBytes;
    }

    if (WithinBudget) {
      std::string Expanded;
      auto ExprIt = AddrToValExpr.find(Addr);
      if (ExprIt != AddrToValExpr.end() && ExprIt->second)
        Expanded = storedIntegerValue(*ExprIt->second, ExprFn);
      const size_t ReadBytes = Expanded.size() + ReadCastBytes.at(Addr);
      const size_t ReadCount = SlotLoads.at(AddrToKey.at(Addr)).size();
      // A rejected parent stays as a real store, but each of its loads can
      // still expand an accepted child. Charge every raw reader, including
      // readers in stores that may later disappear, so neither this boundary
      // nor many independent readers can multiply the function's output
      // beyond the total budget. Counting deleted readers is conservative.
      if (!Expanded.empty() && ReadBytes <= MaxForwardedExpressionBytes &&
          ReadCount != 0 &&
          ReadBytes <=
              (MaxTotalForwardedBytes - TotalForwardedBytes) / ReadCount) {
        TotalForwardedBytes += ReadBytes * ReadCount;
        State.StoreFwd.emplace(Addr, std::move(Expanded));
      }
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
      if (!State.StoreFwd.count(Addr) || !ValueExpr)
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
