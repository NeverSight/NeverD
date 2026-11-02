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

static void renameVars(std::vector<HighStmt> &Stmts,
                       std::vector<std::pair<MedVar, ExprPtr>> *FrameFacts) {
  VarKeyMap<int16_t> RenameMap;
  VarKeyMap<int> RenameIdMap;
  int16_t VarCounter = 0;
  int IdBase = limits::kVarRenameIdBase;
  HighExprSet Seen;
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
    E->forEachChildExpr(CollectVars);
  };
  walkStmts(Stmts, [&](const HighStmt &S) { forEachExpr(S, CollectVars); });
  if (!RenameMap.empty()) {
    if (FrameFacts)
      for (auto &[Variable, Address] : *FrameFacts) {
        const auto Key = VK(Variable);
        if (auto It = RenameMap.find(Key); It != RenameMap.end()) {
          Variable.RenameTag = It->second;
          Variable.Id = RenameIdMap.at(Key);
          Variable.SSAVer = 0;
        }
      }
    HighExprSet RenameSeen;
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
      DoRename(E->IndirectTarget);
    };
    walkStmts(Stmts, [&](HighStmt &S) { forEachExpr(S, DoRename); });
  }
}

void renameVars(std::vector<HighStmt> &Stmts) { renameVars(Stmts, nullptr); }

void renameVars(HighFunc &Func) {
  renameVars(Func.Body, &Func.ItaniumCallFrameOffsets);
}

//===----------------------------------------------------------------------===//
// postRenameCleanup — self-assign removal, consecutive duplicate elimination
//===----------------------------------------------------------------------===//

static void collectRefExprLocal(const ExprPtr &E, VarKeySet &Refs,
                                HighExprSet &Seen) {
  if (!E || !Seen.insert(E.get()).second)
    return;
  if (E->Kind == ExprKind::Var)
    Refs.insert(VK(E->Var));
  E->forEachChildExpr(
      [&](const ExprPtr &Op) { collectRefExprLocal(Op, Refs, Seen); });
}

static void collectStmtRefsLocal(const std::vector<HighStmt> &Stmts,
                                 VarKeySet &Refs) {
  HighExprSet Seen;
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

/// Addresses a jump or an exception handler enters; a statement removed at
/// one of them leaves an empty block so its label keeps its place.
static std::set<va_t> enteredAddresses(const std::vector<HighStmt> &Stmts) {
  std::set<va_t> Entered = gotoTargets(Stmts);
  walkStmts(Stmts, [&](const HighStmt &S) {
    for (const HighEHClause &Clause : S.EHClauses)
      Entered.insert(Clause.HandlerVA);
  });
  return Entered;
}

static bool eliminateDeadAssignsLocal(std::vector<HighStmt> &Stmts,
                                      const VarKeySet &Refs,
                                      const std::set<va_t> &Entered) {
  bool Changed = false;
  eraseKeepingBranchEntries(Stmts, Entered, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        S.Dst->Kind == ExprKind::Var && S.Val->Kind == ExprKind::Var &&
        VK(S.Dst->Var) == VK(S.Val->Var)) {
      Changed = true;
      return true;
    }
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        S.Dst->Kind == ExprKind::Var && S.Val->Kind != ExprKind::Call &&
        !S.Val->hasOrderedMemoryAccess() && Refs.count(VK(S.Dst->Var)) == 0) {
      Changed = true;
      return true;
    }
    return false;
  });
  for (auto &S : Stmts) {
    if (!S.Body.empty() && eliminateDeadAssignsLocal(S.Body, Refs, Entered))
      Changed = true;
    if (!S.ElseBody.empty() &&
        eliminateDeadAssignsLocal(S.ElseBody, Refs, Entered))
      Changed = true;
    for (auto &C : S.Cases)
      if (eliminateDeadAssignsLocal(C.Body, Refs, Entered))
        Changed = true;
    if (!S.DefaultBody.empty() &&
        eliminateDeadAssignsLocal(S.DefaultBody, Refs, Entered))
      Changed = true;
  }
  return Changed;
}

static void postRenameCleanupImpl(std::vector<HighStmt> &Stmts,
                                  const MedVar *SourceCapture) {
  const std::set<va_t> Entered = enteredAddresses(Stmts);
  std::function<void(std::vector<HighStmt> &)> RemoveSelfAssigns;
  RemoveSelfAssigns = [&](std::vector<HighStmt> &Body) {
    eraseKeepingBranchEntries(Body, Entered, [](const HighStmt &S) {
      return S.Kind == StmtKind::Assign && S.Dst && S.Val &&
             S.Dst->Kind == ExprKind::Var && S.Val->Kind == ExprKind::Var &&
             S.Dst->structuralEq(*S.Val);
    });
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
      HighExprSet CheckSeen;
      std::function<void(const ExprPtr &)> CheckRef = [&](const ExprPtr &E) {
        if (!E || CurrUsesPrev || !CheckSeen.insert(E.get()).second)
          return;
        if (E->Kind == ExprKind::Var && E->Var == Prev.Dst->Var)
          CurrUsesPrev = true;
        E->forEachChildExpr(CheckRef);
      };
      if (Curr.Val)
        CheckRef(Curr.Val);
      if (!CurrUsesPrev) {
        if (Prev.Val && Prev.Val->Kind == ExprKind::Call) {
          Prev.Kind = StmtKind::Call;
          Prev.CallExpr = Prev.Val;
          Prev.Dst = nullptr;
          Prev.Val = nullptr;
        } else if (!Prev.Val || !Prev.Val->hasOrderedMemoryAccess()) {
          if (Prev.Addr != 0 && Prev.Addr != InvalidVA &&
              Entered.count(Prev.Addr)) {
            // The overwritten value is dead; its label stays.
            HighStmt Anchor;
            Anchor.Kind = StmtKind::Block;
            Anchor.Addr = Prev.Addr;
            Prev = std::move(Anchor);
          } else {
            Body.erase(Body.begin() + static_cast<long>(I));
            --I;
          }
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
  if (SourceCapture)
    FinalRefs.insert(VK(*SourceCapture));
  collectStmtRefsLocal(Stmts, FinalRefs);
  eliminateDeadAssignsLocal(Stmts, FinalRefs, Entered);
}

void postRenameCleanup(std::vector<HighStmt> &Stmts) {
  postRenameCleanupImpl(Stmts, nullptr);
}

void postRenameCleanup(HighFunc &Func) {
  postRenameCleanupImpl(
      Func.Body, Func.SwiftErrorEntry ? &Func.SwiftErrorEntry->Input : nullptr);
}

} // namespace neverd
