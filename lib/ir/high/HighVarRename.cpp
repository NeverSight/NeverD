//===- HighVarRename.cpp - Variable renaming for HighIR -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Sequential variable renaming (SSA-versioned register vars → short rename
/// tags) and post-rename cleanup (self-assign removal, consecutive dead
/// store elimination, and final dead-assignment pass).
///
//===----------------------------------------------------------------------===//

#include "HighDCEDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/high/MedToHigh.h"

#include <functional>
#include <unordered_set>

namespace neverd {

//===----------------------------------------------------------------------===//
// renameVars — assign sequential rename tags to SSA-versioned register vars
//===----------------------------------------------------------------------===//

void renameVars(std::vector<HighStmt> &Stmts) {
  VarKeyMap<int16_t> RenameMap;
  VarKeyMap<int> RenameIdMap;
  int16_t VarCounter = 0;
  int IdBase = limits::kVarRenameIdBase;
  std::unordered_set<const HighExpr *> Seen;
  std::function<void(const ExprPtr &)> CollectVars;
  CollectVars = [&](const ExprPtr &E) {
    if (!E || !Seen.insert(E.get()).second)
      return;
    if (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Reg &&
        E->Var.SSAVer > 0) {
      auto Key = VK(E->Var);
      if (RenameMap.find(Key) == RenameMap.end()) {
        RenameMap[Key] = VarCounter++;
        RenameIdMap[Key] = IdBase++;
      }
    }
    for (auto &Op : E->Operands)
      CollectVars(Op);
  };
  walkStmts(Stmts, [&](const HighStmt &S) { forEachExpr(S, CollectVars); });
  if (!RenameMap.empty()) {
    std::unordered_set<const HighExpr *> RenameSeen;
    std::function<void(ExprPtr &)> DoRename;
    DoRename = [&](ExprPtr &E) {
      if (!E || !RenameSeen.insert(E.get()).second)
        return;
      if (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Reg &&
          E->Var.SSAVer > 0) {
        auto Key = VK(E->Var);
        auto It = RenameMap.find(Key);
        if (It != RenameMap.end()) {
          E->Var.RenameTag = It->second;
          E->Var.Id = RenameIdMap[Key];
          E->Var.SSAVer = 0;
        }
      }
      for (auto &Op : E->Operands)
        DoRename(Op);
    };
    walkStmts(Stmts, [&](HighStmt &S) { forEachExpr(S, DoRename); });
  }
}

//===----------------------------------------------------------------------===//
// postRenameCleanup — self-assign removal, consecutive duplicate elimination
//===----------------------------------------------------------------------===//

static void collectRefExprLocal(const ExprPtr &E, VarKeySet &Refs,
                                std::unordered_set<const HighExpr *> &Seen) {
  if (!E || !Seen.insert(E.get()).second)
    return;
  if (E->Kind == ExprKind::Var)
    Refs.insert(VK(E->Var));
  for (auto &Op : E->Operands)
    collectRefExprLocal(Op, Refs, Seen);
}

static void collectStmtRefsLocal(const std::vector<HighStmt> &Stmts,
                                 VarKeySet &Refs) {
  std::unordered_set<const HighExpr *> Seen;
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Val)
      collectRefExprLocal(S.Val, Refs, Seen);
    if (S.Kind == StmtKind::Store) {
      collectRefExprLocal(S.StoreAddr, Refs, Seen);
      collectRefExprLocal(S.StoreVal, Refs, Seen);
    }
    if (S.Cond)
      collectRefExprLocal(S.Cond, Refs, Seen);
    if (S.RetVal)
      collectRefExprLocal(S.RetVal, Refs, Seen);
    if (S.CallExpr)
      collectRefExprLocal(S.CallExpr, Refs, Seen);
    if (S.SwitchExpr)
      collectRefExprLocal(S.SwitchExpr, Refs, Seen);
  });
}

static bool eliminateDeadAssignsLocal(std::vector<HighStmt> &Stmts,
                                      const VarKeySet &Refs) {
  bool Changed = false;
  Stmts.erase(std::remove_if(Stmts.begin(), Stmts.end(),
                             [&](const HighStmt &S) {
                               if (S.Kind == StmtKind::Assign && S.Dst &&
                                   S.Val && S.Dst->Kind == ExprKind::Var &&
                                   S.Val->Kind == ExprKind::Var &&
                                   VK(S.Dst->Var) == VK(S.Val->Var)) {
                                 Changed = true;
                                 return true;
                               }
                               if (S.Kind == StmtKind::Assign && S.Dst &&
                                   S.Val && S.Dst->Kind == ExprKind::Var &&
                                   S.Val->Kind != ExprKind::Call &&
                                   Refs.count(VK(S.Dst->Var)) == 0) {
                                 Changed = true;
                                 return true;
                               }
                               return false;
                             }),
              Stmts.end());
  for (auto &S : Stmts) {
    if (!S.Body.empty() && eliminateDeadAssignsLocal(S.Body, Refs))
      Changed = true;
    if (!S.ElseBody.empty() && eliminateDeadAssignsLocal(S.ElseBody, Refs))
      Changed = true;
    for (auto &C : S.Cases)
      if (eliminateDeadAssignsLocal(C.Body, Refs))
        Changed = true;
    if (!S.DefaultBody.empty() &&
        eliminateDeadAssignsLocal(S.DefaultBody, Refs))
      Changed = true;
  }
  return Changed;
}

void postRenameCleanup(std::vector<HighStmt> &Stmts) {
  std::function<void(std::vector<HighStmt> &)> RemoveSelfAssigns;
  RemoveSelfAssigns = [&](std::vector<HighStmt> &Body) {
    Body.erase(std::remove_if(Body.begin(), Body.end(),
                              [](const HighStmt &S) {
                                return S.Kind == StmtKind::Assign && S.Dst &&
                                       S.Val && S.Dst->Kind == ExprKind::Var &&
                                       S.Val->Kind == ExprKind::Var &&
                                       S.Dst->structuralEq(*S.Val);
                              }),
               Body.end());
    for (auto &S : Body) {
      RemoveSelfAssigns(S.Body);
      RemoveSelfAssigns(S.ElseBody);
      for (auto &C : S.Cases)
        RemoveSelfAssigns(C.Body);
      RemoveSelfAssigns(S.DefaultBody);
    }
  };
  RemoveSelfAssigns(Stmts);

  std::function<void(std::vector<HighStmt> &)> PostConsecElim;
  PostConsecElim = [&](std::vector<HighStmt> &Body) {
    for (size_t I = 0; I + 1 < Body.size(); ++I) {
      auto &Prev = Body[I];
      auto &Curr = Body[I + 1];
      if (Prev.Kind != StmtKind::Assign || Curr.Kind != StmtKind::Assign)
        continue;
      if (!Prev.Dst || !Curr.Dst)
        continue;
      if (Prev.Dst->Kind != ExprKind::Var || Curr.Dst->Kind != ExprKind::Var)
        continue;
      if (!Prev.Dst->structuralEq(*Curr.Dst))
        continue;
      bool CurrUsesPrev = false;
      std::unordered_set<const HighExpr *> CheckSeen;
      std::function<void(const ExprPtr &)> CheckRef = [&](const ExprPtr &E) {
        if (!E || CurrUsesPrev || !CheckSeen.insert(E.get()).second)
          return;
        if (E->Kind == ExprKind::Var && E->structuralEq(*Prev.Dst))
          CurrUsesPrev = true;
        for (auto &Op : E->Operands)
          CheckRef(Op);
      };
      if (Curr.Val)
        CheckRef(Curr.Val);
      if (!CurrUsesPrev) {
        if (Prev.Val && Prev.Val->Kind == ExprKind::Call) {
          Prev.Kind = StmtKind::Call;
          Prev.CallExpr = Prev.Val;
          Prev.Dst = nullptr;
          Prev.Val = nullptr;
        } else {
          Body.erase(Body.begin() + static_cast<long>(I));
          --I;
        }
      }
    }
    for (auto &S : Body) {
      PostConsecElim(S.Body);
      PostConsecElim(S.ElseBody);
      for (auto &C : S.Cases)
        PostConsecElim(C.Body);
      PostConsecElim(S.DefaultBody);
    }
  };
  PostConsecElim(Stmts);

  VarKeySet FinalRefs;
  collectStmtRefsLocal(Stmts, FinalRefs);
  eliminateDeadAssignsLocal(Stmts, FinalRefs);
}

} // namespace neverd
