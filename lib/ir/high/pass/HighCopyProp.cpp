//===- HighCopyProp.cpp - Copy propagation and alias resolution ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Copy propagation, alias resolution, and expression inlining passes for
/// HighIR dead-code elimination.  Phases extracted from the monolithic DCE
/// pipeline:
///
///   Phase 1  — resolveRegAliases (zext/sext of same register)
///   Phase 2  — foldCopyChains (single-def copy chain folding)
///   Phase 3  — propagatePhiCopies (phi-style copy propagation)
///   Phase 4  — foldMultiUseCopies (multi-use copy folding)
///   Phase 5  — inlineSingleDefSingleUse (single-def single-use inlining)
///   Phase 6  — scopedCopyPropagation (scope-aware, outside loops)
///   Phase 11 — eliminateRegAliasCopies
///   Phase 12 — eliminateLoopAliases
///
/// Shared helpers: resolveCopyChains, rewriteRhsVars, countExprVarUses,
/// inlineSingleDefs.
///
/// See also:
///   HighDCE.cpp         — iterative DCE, expression simplification, rename
///   HighDCEDetail.h     — shared declarations
///
//===----------------------------------------------------------------------===//

#include "HighDCEDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"

#include <algorithm>
#include <functional>
#include <set>
#include <unordered_set>

namespace neverd {

//===----------------------------------------------------------------------===//
// Shared helpers
//===----------------------------------------------------------------------===//

bool containsNonMovableEffect(const ExprPtr &E) {
  std::vector<const HighExpr *> Worklist;
  if (E)
    Worklist.push_back(E.get());
  HighExprSet Seen;
  while (!Worklist.empty()) {
    const HighExpr *Current = Worklist.back();
    Worklist.pop_back();
    if (!Seen.insert(Current).second)
      continue;
    if (Current->Kind == ExprKind::Load || Current->Kind == ExprKind::Store ||
        Current->Kind == ExprKind::Call ||
        Current->Kind == ExprKind::EntryRegister ||
        Current->IntrinsicId != Intrinsic::None ||
        Current->Op == NdOp::ATOMIC_ADD || Current->Op == NdOp::ATOMIC_XCHG ||
        Current->Op == NdOp::ATOMIC_CMPXCHG ||
        Current->MemoryOrdering != NdMemoryOrdering::None ||
        Current->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return true;
    Current->forEachChildExpr(
        [&](const ExprPtr &Operand) { Worklist.push_back(Operand.get()); });
  }
  return false;
}

void resolveCopyChains(VarKeyMap<ExprPtr> &Map) {
  for (auto &[Key, Val] : Map) {
    VarKeySet Visited{Key};
    auto Cur = Val;
    while (Cur && Cur->Kind == ExprKind::Var) {
      auto CurKey = VK(Cur->Var);
      auto It = Map.find(CurKey);
      if (It == Map.end() || Visited.count(CurKey))
        break;
      Visited.insert(CurKey);
      Cur = It->second;
    }
    Val = Cur;
  }
}

// Source SSA operations become ordinary mutable assignments after PHIs are
// lowered. A one-definition expression can still read a loop-carried value;
// duplicating that expression after an edge write changes its value. All copy
// and expression inlining owners use this same conservative stability proof.
void filterStableCopyCandidates(const std::vector<HighStmt> &Stmts,
                                VarKeyMap<ExprPtr> &Candidates) {
  VarKeyMap<int> Definitions;
  VarKeyMap<VarKeySet> Dependents;
  VarKeySet Unstable;
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst || S.Dst->Kind != ExprKind::Var)
      return;
    auto Key = VK(S.Dst->Var);
    if (++Definitions[Key] > 1 || S.IsPhiCopy)
      Unstable.insert(Key);
    std::vector<ExprPtr> Work{S.Val};
    HighExprSet Seen;
    while (!Work.empty()) {
      auto E = Work.back();
      Work.pop_back();
      if (!E || !Seen.insert(E.get()).second)
        continue;
      if (E->Kind == ExprKind::Var) {
        auto Input = VK(E->Var);
        Dependents[Input].insert(Key);
        if (Input == Key)
          Unstable.insert(Key);
      }
      Work.insert(Work.end(), E->Operands.begin(), E->Operands.end());
    }
  });
  auto ReadsUnstable = [&](const ExprPtr &Root) {
    std::vector<ExprPtr> Work{Root};
    HighExprSet Seen;
    while (!Work.empty()) {
      auto E = Work.back();
      Work.pop_back();
      if (!E || !Seen.insert(E.get()).second)
        continue;
      if (E->Kind == ExprKind::Var && Unstable.count(VK(E->Var)))
        return true;
      Work.insert(Work.end(), E->Operands.begin(), E->Operands.end());
    }
    return false;
  };
  std::vector<VarKey> Work(Unstable.begin(), Unstable.end());
  while (!Work.empty()) {
    auto Key = Work.back();
    Work.pop_back();
    auto Users = Dependents.find(Key);
    if (Users == Dependents.end())
      continue;
    for (auto User : Users->second)
      if (Unstable.insert(User).second)
        Work.push_back(User);
  }
  for (auto I = Candidates.begin(); I != Candidates.end();)
    if (Definitions[I->first] != 1 || Unstable.count(I->first) ||
        ReadsUnstable(I->second))
      I = Candidates.erase(I);
    else
      ++I;
}

void rewriteRhsVars(std::vector<HighStmt> &Stmts,
                    const VarKeyMap<ExprPtr> &Candidates) {
  auto Map = Candidates;
  filterStableCopyCandidates(Stmts, Map);
  HighExprSet Seen;
  std::function<void(ExprPtr &)> Rewrite = [&](ExprPtr &E) {
    if (!E)
      return;
    if (E->Kind == ExprKind::Var) {
      auto It = Map.find(VK(E->Var));
      if (It != Map.end()) {
        E = It->second;
        return;
      }
    }
    if (!Seen.insert(E.get()).second)
      return;
    for (auto &Op : E->Operands)
      Rewrite(Op);
    Rewrite(E->IndirectTarget);
  };
  walkStmts(Stmts, [&](HighStmt &S) { forEachRhsExpr(S, Rewrite); });
}

void countExprVarUses(const ExprPtr &E, VarKeyMap<int> &Uses,
                      HighExprSet &Seen) {
  if (!E || !Seen.insert(E.get()).second)
    return;
  if (E->Kind == ExprKind::Var)
    Uses[VK(E->Var)]++;
  E->forEachChildExpr(
      [&](const ExprPtr &Op) { countExprVarUses(Op, Uses, Seen); });
}

void countExprVarUsesUpToTwo(
    const ExprPtr &E, VarKeyMap<int> &Uses,
    std::unordered_map<const HighExpr *, uint8_t> &Visits) {
  std::vector<const HighExpr *> Work;
  if (E)
    Work.push_back(E.get());
  while (!Work.empty()) {
    const HighExpr *Current = Work.back();
    Work.pop_back();
    auto &Count = Visits[Current];
    if (Count == 2)
      continue;
    ++Count;
    if (Current->Kind == ExprKind::Var) {
      auto &UseCount = Uses[VK(Current->Var)];
      UseCount = std::min(2, UseCount + 1);
    }
    Current->forEachChildExpr([&](const ExprPtr &Child) {
      if (Child)
        Work.push_back(Child.get());
    });
  }
}

void inlineSingleDefs(std::vector<HighStmt> &Stmts,
                      const VarKeyMap<ExprPtr> &Candidates) {
  auto Defs = Candidates;
  filterStableCopyCandidates(Stmts, Defs);
  HighExprSet Seen;
  std::function<void(ExprPtr &)> DoInline = [&](ExprPtr &E) {
    if (!E)
      return;
    if (E->Kind == ExprKind::Var) {
      auto It = Defs.find(VK(E->Var));
      if (It != Defs.end()) {
        E = It->second;
        return;
      }
    }
    if (E->Kind == ExprKind::UnaryOp && E->Op == NdOp::BOOL_NOT &&
        !E->Operands.empty() && E->Operands[0]->Kind == ExprKind::Var) {
      auto It = Defs.find(VK(E->Operands[0]->Var));
      if (It != Defs.end()) {
        E->Operands[0] = It->second;
        return;
      }
    }
    if (!Seen.insert(E.get()).second)
      return;
    for (auto &Op : E->Operands)
      DoInline(Op);
    DoInline(E->IndirectTarget);
  };
  walkStmts(Stmts, [&](HighStmt &S) { forEachRhsExpr(S, DoInline); });
}

//===----------------------------------------------------------------------===//
// Phase 1: Resolve register aliases (zext/sext of same register)
//===----------------------------------------------------------------------===//

void resolveRegAliases(std::vector<HighStmt> &Stmts) {
  VarKeyMap<ExprPtr> AliasMap;
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return;
    if (S.Dst->Kind != ExprKind::Var || S.Dst->Var.Kind != MedVar::Reg)
      return;
    if (S.Val->Kind == ExprKind::UnaryOp &&
        (S.Val->Op == NdOp::INT_ZEXT || S.Val->Op == NdOp::INT_SEXT) &&
        !S.Val->Operands.empty() && S.Val->Operands[0]->Kind == ExprKind::Var &&
        S.Val->Operands[0]->Var.Kind == MedVar::Reg &&
        S.Dst->Var.RegOff == S.Val->Operands[0]->Var.RegOff)
      AliasMap[VK(S.Dst->Var)] = S.Val;
    if (S.Val->Kind == ExprKind::Var && S.Val->Var.Kind == MedVar::Reg &&
        S.Dst->Var.RegOff == S.Val->Var.RegOff &&
        S.Dst->Var.Size == S.Val->Var.Size)
      AliasMap[VK(S.Dst->Var)] = S.Val;
  });
  if (!AliasMap.empty())
    rewriteRhsVars(Stmts, AliasMap);
}

//===----------------------------------------------------------------------===//
// Phase 2: Fold single-def copy chains
//===----------------------------------------------------------------------===//

void foldCopyChains(HighFunc &Func) {
  VarKeyMap<int> VarDefCount;
  VarKeyMap<int> VarUseCount;
  VarKeyMap<ExprPtr> VarDefValue;
  std::unordered_map<const HighExpr *, uint8_t> Visits;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var) {
      auto Key = VK(S.Dst->Var);
      VarDefCount[Key]++;
      VarDefValue[Key] = S.Val;
    }
    forEachRhsExpr(S, [&](const ExprPtr &E) {
      countExprVarUsesUpToTwo(E, VarUseCount, Visits);
    });
  });

  VarKeyMap<ExprPtr> FoldMap;
  for (auto &S : Func.Body) {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      continue;
    if (S.Dst->Kind != ExprKind::Var || S.Val->Kind != ExprKind::Var)
      continue;
    auto SrcKey = VK(S.Val->Var);
    auto DstKey = VK(S.Dst->Var);
    if (SrcKey == DstKey)
      continue;
    if (VarDefCount[SrcKey] != 1 || VarUseCount[SrcKey] != 1)
      continue;
    auto It = VarDefValue.find(SrcKey);
    if (It == VarDefValue.end() || !It->second)
      continue;
    if (It->second->Kind == ExprKind::Call ||
        It->second->hasOrderedMemoryAccess() ||
        containsNonMovableEffect(It->second))
      continue;
    FoldMap[DstKey] = It->second;
  }
  filterStableCopyCandidates(Func.Body, FoldMap);
  if (!FoldMap.empty()) {
    for (auto &S : Func.Body) {
      if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
        continue;
      if (S.Dst->Kind != ExprKind::Var || S.Val->Kind != ExprKind::Var)
        continue;
      auto It = FoldMap.find(VK(S.Dst->Var));
      if (It != FoldMap.end())
        S.Val = It->second;
    }
  }
  // A replacement can still read a source from another folded copy. Recompute
  // liveness after rewriting, rather than deleting the old set of sources;
  // the shared DCE also preserves any branch entries at dead assignments.
}

//===----------------------------------------------------------------------===//
// Phase 3: Phi-style copy propagation
//===----------------------------------------------------------------------===//

void propagatePhiCopies(std::vector<HighStmt> &Stmts) {
  VarKeyMap<int> DefCount;
  VarKeyMap<ExprPtr> CopyMap;
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var) {
      auto Key = VK(S.Dst->Var);
      DefCount[Key]++;
      if (S.Val && S.Val->Kind == ExprKind::Var)
        CopyMap[Key] = S.Val;
    }
  });
  for (auto It = CopyMap.begin(); It != CopyMap.end();) {
    if (DefCount[It->first] > 1)
      It = CopyMap.erase(It);
    else
      ++It;
  }
  if (!CopyMap.empty()) {
    resolveCopyChains(CopyMap);
    rewriteRhsVars(Stmts, CopyMap);
  }
}

//===----------------------------------------------------------------------===//
// Phase 4: Multi-use copy folding
//===----------------------------------------------------------------------===//

void foldMultiUseCopies(std::vector<HighStmt> &Stmts) {
  VarKeyMap<int> MultiDefCount;
  VarKeyMap<int> TotalUseCount;
  VarKeyMap<int> CopyUseCount;
  VarKeyMap<ExprPtr> MultiDefValue;
  VarKeyMap<bool> DefIsCall;
  HighExprSet Seen;
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var &&
        S.Val) {
      auto Key = VK(S.Dst->Var);
      MultiDefCount[Key]++;
      MultiDefValue[Key] = S.Val;
      DefIsCall[Key] = (S.Val->Kind == ExprKind::Call);
      if (S.Val->Kind == ExprKind::Var)
        CopyUseCount[VK(S.Val->Var)]++;
    }
    forEachRhsExpr(
        S, [&](const ExprPtr &E) { countExprVarUses(E, TotalUseCount, Seen); });
  });
  VarKeyMap<ExprPtr> PropMap;
  for (auto &[Key, NumDefs] : MultiDefCount) {
    if (NumDefs != 1 || DefIsCall[Key])
      continue;
    auto Val = MultiDefValue[Key];
    if (!Val || Val->hasOrderedMemoryAccess() || containsNonMovableEffect(Val))
      continue;
    auto TotalIt = TotalUseCount.find(Key);
    auto CopyIt = CopyUseCount.find(Key);
    if (TotalIt == TotalUseCount.end() || TotalIt->second == 0)
      continue;
    if (CopyIt == CopyUseCount.end())
      continue;
    if (TotalIt->second == CopyIt->second)
      PropMap[Key] = Val;
  }
  filterStableCopyCandidates(Stmts, PropMap);
  if (!PropMap.empty()) {
    walkStmts(Stmts, [&](HighStmt &S) {
      if (S.Kind == StmtKind::Assign && S.Val && S.Val->Kind == ExprKind::Var) {
        auto It = PropMap.find(VK(S.Val->Var));
        if (It != PropMap.end())
          S.Val = It->second;
      }
    });
  }
}

//===----------------------------------------------------------------------===//
// Phase 5: Inline single-def single-use expressions
//===----------------------------------------------------------------------===//

void inlineSingleDefSingleUse(std::vector<HighStmt> &Stmts) {
  VarKeyMap<ExprPtr> SingleUseDefs;
  VarKeyMap<int> SingleDefCount;
  VarKeyMap<int> SingleUseCount;
  std::unordered_map<const HighExpr *, uint8_t> Visits;
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        S.Dst->Kind == ExprKind::Var) {
      auto Key = VK(S.Dst->Var);
      SingleDefCount[Key]++;
      bool IsInlineable =
          S.Val->Kind == ExprKind::BinOp || S.Val->Kind == ExprKind::UnaryOp ||
          S.Val->Kind == ExprKind::Var || S.Val->Kind == ExprKind::Const;
      if (IsInlineable && !S.Val->hasOrderedMemoryAccess() &&
          !containsNonMovableEffect(S.Val))
        SingleUseDefs[Key] = S.Val;
    }
    forEachRhsExpr(S, [&](const ExprPtr &E) {
      countExprVarUsesUpToTwo(E, SingleUseCount, Visits);
    });
  });
  for (auto It = SingleUseDefs.begin(); It != SingleUseDefs.end();) {
    if (SingleDefCount[It->first] != 1 || SingleUseCount[It->first] > 1)
      It = SingleUseDefs.erase(It);
    else
      ++It;
  }
  if (!SingleUseDefs.empty())
    inlineSingleDefs(Stmts, SingleUseDefs);
}

namespace {

/// How many times \p Func assigns and reads each variable, each count capped
/// at two, and the addresses a jump or a handler enters.
struct AssignsAndReads {
  VarKeyMap<int> Defs, Reads;
  std::set<va_t> Targets;
};

AssignsAndReads countAssignsAndReads(const HighFunc &Func) {
  AssignsAndReads Counts;
  auto CountReads = [&](const ExprPtr &Root) {
    std::vector<const HighExpr *> Work{Root.get()};
    while (!Work.empty()) {
      const HighExpr *E = Work.back();
      Work.pop_back();
      if (!E)
        continue;
      if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) {
        int &Count = Counts.Reads[VK(E->Var)];
        Count = std::min(2, Count + 1);
      }
      for (const MedVar &Output : E->IntrinsicOutputs)
        Counts.Defs[VK(Output)] = 2;
      E->forEachChildExpr([&](const ExprPtr &C) { Work.push_back(C.get()); });
    }
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      Counts.Targets.insert(S.GotoTarget);
    for (const HighEHClause &Clause : S.EHClauses)
      Counts.Targets.insert(Clause.HandlerVA);
    if (S.Kind == StmtKind::Assign && S.Dst &&
        (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi)) {
      int &Count = Counts.Defs[VK(S.Dst->Var)];
      Count = std::min(2, Count + 1);
    }
    forEachRhsExpr(S, CountReads);
    // A destination in memory reads its address.
    if (S.Dst && S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      CountReads(S.Dst);
  });
  return Counts;
}

} // namespace

bool inlineAdjacentLoads(HighFunc &Func) {
  // A handler may observe which statement a faulting read belonged to.
  if (Func.StructuredExceptionRegions || Func.UnstructuredExceptionRegions)
    return false;
  AssignsAndReads Counts = countAssignsAndReads(Func);
  VarKeyMap<int> &Defs = Counts.Defs, &Reads = Counts.Reads;
  const std::set<va_t> &Targets = Counts.Targets;
  // Evaluating \p E reads memory at most: no call, store, intrinsic or
  // ordered access.
  std::function<bool(const ExprPtr &)> Pure = [&](const ExprPtr &E) {
    if (!E)
      return true;
    if (E->Kind == ExprKind::Call || E->Kind == ExprKind::Store ||
        E->IntrinsicId != Intrinsic::None || !E->IntrinsicOutputs.empty() ||
        E->MemoryOrdering != NdMemoryOrdering::None ||
        E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    bool Ok = true;
    E->forEachChildExpr([&](const ExprPtr &C) { Ok = Ok && Pure(C); });
    return Ok;
  };
  // The slot holding variable \p Key in \p E, when \p E reads it once.
  std::function<ExprPtr *(ExprPtr &, const VarKey &)> SlotOf =
      [&](ExprPtr &E, const VarKey &Key) -> ExprPtr * {
    if (!E)
      return nullptr;
    if (E->Kind == ExprKind::Var && VK(E->Var) == Key)
      return &E;
    for (ExprPtr &C : E->Operands)
      if (ExprPtr *Found = SlotOf(C, Key))
        return Found;
    return nullptr;
  };
  // The statement \p N reads its operands before any effect of its own, so a
  // read moved into it keeps its place among the program's effects: every
  // operand is pure, or \p N is one call whose arguments are.
  auto Takes = [&](HighStmt &N, const VarKey &Key) -> ExprPtr * {
    auto CallArgument = [&](ExprPtr &Call) -> ExprPtr * {
      if (!Call || Call->Kind != ExprKind::Call)
        return nullptr;
      for (const ExprPtr &Operand : Call->Operands)
        if (!Pure(Operand))
          return nullptr;
      return SlotOf(Call, Key);
    };
    switch (N.Kind) {
    case StmtKind::Assign:
      if (!N.Dst || N.Dst->Kind != ExprKind::Var)
        return nullptr;
      if (Pure(N.Val))
        return SlotOf(N.Val, Key);
      return CallArgument(N.Val);
    case StmtKind::Call:
      return CallArgument(N.CallExpr);
    case StmtKind::Store:
      if (!Pure(N.StoreAddr) || !Pure(N.StoreVal))
        return nullptr;
      if (ExprPtr *Slot = SlotOf(N.StoreAddr, Key))
        return Slot;
      return SlotOf(N.StoreVal, Key);
    case StmtKind::Return:
      if (Pure(N.RetVal))
        return SlotOf(N.RetVal, Key);
      return CallArgument(N.RetVal);
    case StmtKind::If:
    case StmtKind::IfElse:
      return Pure(N.Cond) ? SlotOf(N.Cond, Key) : nullptr;
    case StmtKind::Switch:
      return Pure(N.SwitchExpr) ? SlotOf(N.SwitchExpr, Key) : nullptr;
    default:
      // A loop condition runs again on each pass; the read ran once.
      return nullptr;
    }
  };
  auto Entered = [&](const HighStmt &S) {
    return S.Addr != 0 && S.Addr != InvalidVA && Targets.count(S.Addr);
  };
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (size_t I = 0; I + 1 < L.size();) {
          HighStmt &Def = L[I];
          HighStmt &Use = L[I + 1];
          // The C writer may print a PHI copy away from its place, which
          // would move the read with it.
          const bool Candidate =
              Def.Kind == StmtKind::Assign && Def.Dst &&
              Def.Dst->Kind == ExprKind::Var && Def.Dst->Operands.empty() &&
              (Def.Dst->Var.Kind == MedVar::Reg ||
               Def.Dst->Var.Kind == MedVar::Temp) &&
              Def.Val && Def.Val->Kind == ExprKind::Load && Pure(Def.Val) &&
              Def.Dst->Type && Def.Val->Type &&
              Def.Dst->Type->Size == Def.Val->Type->Size &&
              Defs[VK(Def.Dst->Var)] == 1 && Reads[VK(Def.Dst->Var)] == 1 &&
              !Entered(Def) && !Entered(Use) && !Def.IsPhiCopy &&
              !Use.IsPhiCopy;
          if (Candidate)
            if (ExprPtr *Slot = Takes(Use, VK(Def.Dst->Var));
                Slot && (*Slot)->Type &&
                (*Slot)->Type->Size == Def.Val->Type->Size) {
              *Slot = Def.Val;
              L.erase(L.begin() + I);
              Changed = true;
              continue;
            }
          ++I;
        }
        for (HighStmt &S : L) {
          Visit(S.Body);
          Visit(S.ElseBody);
          for (auto &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
        }
      };
  Visit(Func.Body);
  return Changed;
}

bool foldCopiesIntoDefinitions(HighFunc &Func) {
  // A handler may read a local whatever statement raised.
  if (Func.StructuredExceptionRegions || Func.UnstructuredExceptionRegions)
    return false;
  AssignsAndReads Counts = countAssignsAndReads(Func);
  VarKeyMap<int> &Defs = Counts.Defs, &Reads = Counts.Reads;
  const std::set<va_t> &Targets = Counts.Targets;
  auto Local = [](const ExprPtr &E) {
    return E && E->Kind == ExprKind::Var && E->Operands.empty() && E->Type &&
           (E->Var.Kind == MedVar::Reg || E->Var.Kind == MedVar::Temp);
  };
  // \p S or a statement nested in it reads or writes \p Key.
  std::function<bool(const HighStmt &, const VarKey &)> Mentions =
      [&](const HighStmt &S, const VarKey &Key) {
        bool Found = false;
        auto Scan = [&](const ExprPtr &Root) {
          std::vector<const HighExpr *> Work{Root.get()};
          while (!Found && !Work.empty()) {
            const HighExpr *E = Work.back();
            Work.pop_back();
            if (!E)
              continue;
            Found |= (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) &&
                     VK(E->Var) == Key;
            for (const MedVar &Output : E->IntrinsicOutputs)
              Found |= VK(Output) == Key;
            E->forEachChildExpr(
                [&](const ExprPtr &C) { Work.push_back(C.get()); });
          }
        };
        forEachExpr(S, Scan);
        for (const auto *List : {&S.Body, &S.ElseBody, &S.DefaultBody})
          for (const HighStmt &T : *List)
            Found = Found || Mentions(T, Key);
        for (const auto &C : S.Cases)
          for (const HighStmt &T : C.Body)
            Found = Found || Mentions(T, Key);
        for (const auto &ClauseBody : S.EHClauseBodies)
          for (const HighStmt &T : ClauseBody)
            Found = Found || Mentions(T, Key);
        return Found;
      };
  // Control may leave \p S other than at its end, or enter it at a label.
  std::function<bool(const HighStmt &)> Leaves = [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto || S.Kind == StmtKind::Return ||
        S.Kind == StmtKind::Break || S.Kind == StmtKind::Continue ||
        S.Kind == StmtKind::SEHTry || S.Kind == StmtKind::CxxTry ||
        S.Kind == StmtKind::ItaniumTry ||
        (S.Addr != 0 && S.Addr != InvalidVA && Targets.count(S.Addr)))
      return true;
    for (const auto *List : {&S.Body, &S.ElseBody, &S.DefaultBody})
      for (const HighStmt &T : *List)
        if (Leaves(T))
          return true;
    for (const auto &C : S.Cases)
      for (const HighStmt &T : C.Body)
        if (Leaves(T))
          return true;
    return false;
  };
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (size_t I = 0; I < L.size(); ++I) {
          HighStmt &Def = L[I];
          if (Def.Kind != StmtKind::Assign || !Local(Def.Dst) || !Def.Val)
            continue;
          const VarKey T = VK(Def.Dst->Var);
          if (Defs[T] != 1 || Reads[T] != 1)
            continue;
          // `t = e; S...; x = t;` where S neither touches x nor t, nor
          // leaves nor is entered: x takes e at once, `x = e; S...;`.  e is
          // evaluated where it was; only x's new value comes earlier, and
          // nothing before the copy reads x.
          for (size_t J = I + 1; J < L.size(); ++J) {
            HighStmt &Copy = L[J];
            if (Copy.Kind == StmtKind::Assign && Local(Copy.Dst) && Copy.Val &&
                Copy.Val->Kind == ExprKind::Var && Copy.Val->Operands.empty() &&
                VK(Copy.Val->Var) == T) {
              const VarKey X = VK(Copy.Dst->Var);
              const bool SameType =
                  Copy.Dst->Type->Kind == Def.Dst->Type->Kind &&
                  Copy.Dst->Type->Size == Def.Dst->Type->Size &&
                  Copy.Val->Type && Copy.Val->Type->Size == Def.Dst->Type->Size;
              const bool Entered = Copy.Addr != 0 && Copy.Addr != InvalidVA &&
                                   Targets.count(Copy.Addr);
              if (SameType && !Entered && !(X == T) &&
                  std::none_of(
                      L.begin() + I + 1, L.begin() + J,
                      [&](const HighStmt &S) { return Mentions(S, X); })) {
                Def.Dst = Copy.Dst;
                L.erase(L.begin() + J);
                Changed = true;
              }
              break;
            }
            if (Mentions(Copy, T) || Leaves(Copy))
              break;
          }
        }
        for (HighStmt &S : L) {
          Visit(S.Body);
          Visit(S.ElseBody);
          for (auto &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
        }
      };
  Visit(Func.Body);
  return Changed;
}

//===----------------------------------------------------------------------===//
// Phase 6: Scope-aware copy propagation (outside loops)
//===----------------------------------------------------------------------===//

void scopedCopyPropagation(std::vector<HighStmt> &Stmts) {
  std::function<void(std::vector<HighStmt> &, bool)> ScopeCopyProp;
  ScopeCopyProp = [&](std::vector<HighStmt> &Body, bool IsLoopBody) {
    for (auto &S : Body) {
      bool ChildIsLoop =
          (S.Kind == StmtKind::While || S.Kind == StmtKind::DoWhile);
      ScopeCopyProp(S.Body, ChildIsLoop);
      ScopeCopyProp(S.ElseBody, false);
      for (auto &C : S.Cases)
        ScopeCopyProp(C.Body, false);
      ScopeCopyProp(S.DefaultBody, false);
    }
    if (IsLoopBody)
      return;
    VarKeyMap<int> LocalDefs;
    walkStmts(Body, [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var)
        LocalDefs[VK(S.Dst->Var)]++;
    });
    VarKeyMap<ExprPtr> LocalCopyMap;
    for (size_t I = 1; I < Body.size(); ++I) {
      auto &S = Body[I];
      if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
        continue;
      if (S.Dst->Kind != ExprKind::Var || S.Val->Kind != ExprKind::Var)
        continue;
      auto DstKey = VK(S.Dst->Var);
      auto SrcKey = VK(S.Val->Var);
      if (DstKey == SrcKey)
        continue;
      if (LocalDefs[DstKey] != 1)
        continue;
      if (LocalDefs.count(SrcKey) && LocalDefs[SrcKey] > 1)
        continue;
      LocalCopyMap[DstKey] = S.Val;
    }
    if (LocalCopyMap.empty())
      return;
    resolveCopyChains(LocalCopyMap);
    rewriteRhsVars(Body, LocalCopyMap);
  };
  ScopeCopyProp(Stmts, false);
}

//===----------------------------------------------------------------------===//
// Phase 11: Eliminate register alias copies
//===----------------------------------------------------------------------===//

void eliminateRegAliasCopies(HighFunc &Func) {
  auto IsRegAliasCopy = [](const HighStmt &S) -> bool {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return false;
    if (S.Dst->Kind != ExprKind::Var || S.Dst->Var.Kind != MedVar::Reg)
      return false;
    if (S.Val->Kind == ExprKind::Var && S.Val->Var.Kind == MedVar::Reg &&
        S.Val->Var.RegOff == S.Dst->Var.RegOff)
      return true;
    if (S.Val->Kind == ExprKind::UnaryOp &&
        (S.Val->Op == NdOp::INT_ZEXT || S.Val->Op == NdOp::INT_SEXT) &&
        !S.Val->Operands.empty() && S.Val->Operands[0]->Kind == ExprKind::Var &&
        S.Val->Operands[0]->Var.Kind == MedVar::Reg &&
        S.Val->Operands[0]->Var.RegOff == S.Dst->Var.RegOff)
      return true;
    if (S.Val->Kind == ExprKind::UnaryOp &&
        (S.Val->Op == NdOp::INT_ZEXT || S.Val->Op == NdOp::INT_SEXT) &&
        !S.Val->Operands.empty() && S.Val->Operands[0]->Kind == ExprKind::Const)
      return false;
    return false;
  };
  VarKeyMap<int> AllDefs;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var)
      AllDefs[VK(S.Dst->Var)]++;
  });
  VarKeyMap<ExprPtr> RegAliasMap;
  for (auto &S : Func.Body) {
    if (!IsRegAliasCopy(S))
      continue;
    auto DstKey = VK(S.Dst->Var);
    if (AllDefs[DstKey] > 1)
      continue;
    ExprPtr Src;
    if (S.Val->Kind == ExprKind::Var)
      Src = S.Val;
    else if (S.Val->Kind == ExprKind::UnaryOp && !S.Val->Operands.empty())
      Src = S.Val;
    if (Src && Src->Kind == ExprKind::Var && AllDefs[VK(Src->Var)] > 1)
      continue;
    if (Src)
      RegAliasMap[DstKey] = Src;
  }
  if (!RegAliasMap.empty()) {
    resolveCopyChains(RegAliasMap);
    rewriteRhsVars(Func.Body, RegAliasMap);
  }
}

//===----------------------------------------------------------------------===//
// Phase 12: Loop-internal alias elimination
//===----------------------------------------------------------------------===//

void eliminateLoopAliases(std::vector<HighStmt> &Stmts) {
  for (auto &S : Stmts) {
    if (S.Kind == StmtKind::While || S.Kind == StmtKind::DoWhile) {
      VarKeyMap<size_t> DefPos;
      for (size_t I = 0; I < S.Body.size(); ++I) {
        auto &BodyStmt = S.Body[I];
        if (BodyStmt.Kind == StmtKind::Assign && BodyStmt.Dst &&
            BodyStmt.Dst->Kind == ExprKind::Var) {
          auto DstKey = VK(BodyStmt.Dst->Var);
          if (DefPos.find(DstKey) == DefPos.end())
            DefPos[DstKey] = I;
        }
      }
      VarKeyMap<size_t> FirstUse;
      HighExprSet UseSeen;
      for (size_t I = 0; I < S.Body.size(); ++I) {
        auto &BodyStmt = S.Body[I];
        VarKeySet UsedVars;
        UseSeen.clear();
        std::function<void(const ExprPtr &)> CollectUsed;
        CollectUsed = [&](const ExprPtr &E) {
          if (!E || !UseSeen.insert(E.get()).second)
            return;
          if (E->Kind == ExprKind::Var)
            UsedVars.insert(VK(E->Var));
          E->forEachChildExpr(CollectUsed);
        };
        forEachRhsExpr(BodyStmt, CollectUsed);
        for (auto &Used : UsedVars)
          if (FirstUse.find(Used) == FirstUse.end())
            FirstUse[Used] = I;
      }
      VarKeyMap<ExprPtr> SafeAliases;
      for (size_t I = 0; I < S.Body.size(); ++I) {
        auto &BodyStmt = S.Body[I];
        if (BodyStmt.Kind != StmtKind::Assign || !BodyStmt.Dst || !BodyStmt.Val)
          continue;
        if (BodyStmt.Dst->Kind != ExprKind::Var ||
            BodyStmt.Dst->Var.Kind != MedVar::Reg)
          continue;
        auto DstKey = VK(BodyStmt.Dst->Var);
        auto FirstUseIt = FirstUse.find(DstKey);
        if (FirstUseIt != FirstUse.end() && FirstUseIt->second < I)
          continue;
        ExprPtr Src;
        if (BodyStmt.Val->Kind == ExprKind::Var &&
            BodyStmt.Val->Var.Kind == MedVar::Reg &&
            BodyStmt.Val->Var.RegOff == BodyStmt.Dst->Var.RegOff)
          Src = BodyStmt.Val;
        else if (BodyStmt.Val->Kind == ExprKind::UnaryOp &&
                 (BodyStmt.Val->Op == NdOp::INT_ZEXT ||
                  BodyStmt.Val->Op == NdOp::INT_SEXT) &&
                 !BodyStmt.Val->Operands.empty() &&
                 BodyStmt.Val->Operands[0]->Kind == ExprKind::Var &&
                 BodyStmt.Val->Operands[0]->Var.Kind == MedVar::Reg &&
                 BodyStmt.Val->Operands[0]->Var.RegOff ==
                     BodyStmt.Dst->Var.RegOff)
          Src = BodyStmt.Val;
        if (Src)
          SafeAliases[DstKey] = Src;
      }
      filterStableCopyCandidates(S.Body, SafeAliases);
      if (!SafeAliases.empty()) {
        HighExprSet Seen;
        std::function<void(ExprPtr &)> RewriteAlias;
        RewriteAlias = [&](ExprPtr &E) {
          if (!E)
            return;
          if (E->Kind == ExprKind::Var) {
            auto It = SafeAliases.find(VK(E->Var));
            if (It != SafeAliases.end()) {
              E = It->second;
              return;
            }
          }
          if (!Seen.insert(E.get()).second)
            return;
          for (auto &Op : E->Operands)
            RewriteAlias(Op);
          RewriteAlias(E->IndirectTarget);
        };
        for (auto &BodyStmt : S.Body)
          forEachRhsExpr(BodyStmt, RewriteAlias);
      }
    }
    eliminateLoopAliases(S.Body);
    eliminateLoopAliases(S.ElseBody);
    for (auto &C : S.Cases)
      eliminateLoopAliases(C.Body);
    eliminateLoopAliases(S.DefaultBody);
  }
}

} // namespace neverd
