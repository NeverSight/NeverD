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
#include <unordered_set>

namespace neverd {

//===----------------------------------------------------------------------===//
// Shared helpers
//===----------------------------------------------------------------------===//

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

void rewriteRhsVars(std::vector<HighStmt> &Stmts,
                    const VarKeyMap<ExprPtr> &Map) {
  std::unordered_set<const HighExpr *> Seen;
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
  };
  walkStmts(Stmts, [&](HighStmt &S) { forEachRhsExpr(S, Rewrite); });
}

void countExprVarUses(const ExprPtr &E, VarKeyMap<int> &Uses,
                      std::unordered_set<const HighExpr *> &Seen) {
  if (!E || !Seen.insert(E.get()).second)
    return;
  if (E->Kind == ExprKind::Var)
    Uses[VK(E->Var)]++;
  for (auto &Op : E->Operands)
    countExprVarUses(Op, Uses, Seen);
}

void inlineSingleDefs(std::vector<HighStmt> &Stmts,
                      const VarKeyMap<ExprPtr> &Defs) {
  std::unordered_set<const HighExpr *> Seen;
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
      AliasMap[VK(S.Dst->Var)] = S.Val->Operands[0];
    if (S.Val->Kind == ExprKind::Var && S.Val->Var.Kind == MedVar::Reg &&
        S.Dst->Var.RegOff == S.Val->Var.RegOff &&
        S.Dst->Var.Size > S.Val->Var.Size)
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
  std::unordered_set<const HighExpr *> Seen;
  std::function<void(const std::vector<HighStmt> &)> ScanStmts;
  ScanStmts = [&](const std::vector<HighStmt> &Stmts) {
    for (auto &S : Stmts) {
      if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var) {
        auto Key = VK(S.Dst->Var);
        VarDefCount[Key]++;
        VarDefValue[Key] = S.Val;
      }
      forEachRhsExpr(
          S, [&](const ExprPtr &E) { countExprVarUses(E, VarUseCount, Seen); });
      ScanStmts(S.Body);
      ScanStmts(S.ElseBody);
    }
  };
  ScanStmts(Func.Body);

  VarKeyMap<ExprPtr> FoldMap;
  VarKeySet FoldSources;
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
        It->second->hasOrderedMemoryAccess())
      continue;
    FoldMap[DstKey] = It->second;
    FoldSources.insert(SrcKey);
  }
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
    Func.Body.erase(std::remove_if(Func.Body.begin(), Func.Body.end(),
                                   [&](const HighStmt &S) {
                                     return S.Kind == StmtKind::Assign &&
                                            S.Dst &&
                                            S.Dst->Kind == ExprKind::Var &&
                                            FoldSources.count(VK(S.Dst->Var)) >
                                                0;
                                   }),
                    Func.Body.end());
  }
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
  std::unordered_set<const HighExpr *> Seen;
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
    if (!Val || Val->hasOrderedMemoryAccess())
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
  std::unordered_set<const HighExpr *> Seen;
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        S.Dst->Kind == ExprKind::Var) {
      auto Key = VK(S.Dst->Var);
      SingleDefCount[Key]++;
      bool IsInlineable =
          S.Val->Kind == ExprKind::BinOp || S.Val->Kind == ExprKind::UnaryOp ||
          S.Val->Kind == ExprKind::Var || S.Val->Kind == ExprKind::Const;
      if (IsInlineable && !S.Val->hasOrderedMemoryAccess())
        SingleUseDefs[Key] = S.Val;
    }
    forEachRhsExpr(S, [&](const ExprPtr &E) {
      countExprVarUses(E, SingleUseCount, Seen);
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
      Src = S.Val->Operands[0];
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
      std::unordered_set<const HighExpr *> UseSeen;
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
          for (auto &Op : E->Operands)
            CollectUsed(Op);
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
          Src = BodyStmt.Val->Operands[0];
        if (Src)
          SafeAliases[DstKey] = Src;
      }
      if (!SafeAliases.empty()) {
        std::unordered_set<const HighExpr *> Seen;
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
