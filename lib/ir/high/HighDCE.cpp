//===- HighDCE.cpp - Dead code elimination for HighIR ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Multi-phase dead code elimination for HighIR: cycle breaking,
/// unreachable code removal, iterative liveness-based DCE, and variable
/// renaming.
///
/// Related files:
///   HighCopyProp.cpp       — copy propagation and alias resolution
///   HighDeadStoreElim.cpp  — consecutive / redundant stack store elimination
///   HighDCEDetail.h        — shared declarations
///
//===----------------------------------------------------------------------===//

#include "HighDCEDetail.h"

#include "neverd/ir/high/MedToHigh.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

#define DEBUG_TYPE "neverd-high-dce"

namespace neverd {

//===----------------------------------------------------------------------===//
// Expression cycle detection and breaking
//===----------------------------------------------------------------------===//

static bool exprHasCycle(const ExprPtr &E,
                         std::unordered_set<const HighExpr *> &Path,
                         std::unordered_set<const HighExpr *> &Safe) {
  if (!E)
    return false;
  auto *P = E.get();
  if (Safe.count(P))
    return false;
  if (!Path.insert(P).second)
    return true;
  for (auto &Op : E->Operands)
    if (exprHasCycle(Op, Path, Safe))
      return true;
  Path.erase(P);
  Safe.insert(P);
  return false;
}

static void breakExprCycles(ExprPtr &E,
                            std::unordered_set<const HighExpr *> &Path) {
  if (!E)
    return;
  if (!Path.insert(E.get()).second) {
    E = HighExpr::makeConst(0, 8);
    return;
  }
  for (auto &Op : E->Operands)
    breakExprCycles(Op, Path);
  Path.erase(E.get());
}

static void breakStmtCycles(std::vector<HighStmt> &Stmts) {
  std::unordered_set<const HighExpr *> Safe;
  walkStmts(Stmts, [&Safe](HighStmt &S) {
    forEachExpr(S, [&Safe](ExprPtr &EP) {
      std::unordered_set<const HighExpr *> Path;
      if (exprHasCycle(EP, Path, Safe)) {
        Path.clear();
        breakExprCycles(EP, Path);
      }
    });
  });
}

//===----------------------------------------------------------------------===//
// Shared DCE utilities
//===----------------------------------------------------------------------===//

static void collectRefExpr(const ExprPtr &E, VarKeySet &Refs,
                           std::unordered_set<const HighExpr *> &Seen) {
  if (!E || !Seen.insert(E.get()).second)
    return;
  if (E->Kind == ExprKind::Var)
    Refs.insert(VK(E->Var));
  for (auto &Op : E->Operands)
    collectRefExpr(Op, Refs, Seen);
}

static void collectStmtRefs(const std::vector<HighStmt> &Stmts,
                            VarKeySet &Refs) {
  std::unordered_set<const HighExpr *> Seen;
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Val)
      collectRefExpr(S.Val, Refs, Seen);
    if (S.Kind == StmtKind::Store) {
      collectRefExpr(S.StoreAddr, Refs, Seen);
      collectRefExpr(S.StoreVal, Refs, Seen);
    }
    if (S.Cond)
      collectRefExpr(S.Cond, Refs, Seen);
    if (S.RetVal)
      collectRefExpr(S.RetVal, Refs, Seen);
    if (S.CallExpr)
      collectRefExpr(S.CallExpr, Refs, Seen);
    if (S.SwitchExpr)
      collectRefExpr(S.SwitchExpr, Refs, Seen);
  });
}

static bool isDeadAssign(const HighStmt &S, const VarKeySet &Refs) {
  if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
    return false;
  if (S.Dst->Kind != ExprKind::Var)
    return false;
  if (S.Val->Kind == ExprKind::Call)
    return false;
  return Refs.count(VK(S.Dst->Var)) == 0;
}

static bool isSelfAssign(const HighStmt &S) {
  if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
    return false;
  if (S.Dst->Kind != ExprKind::Var || S.Val->Kind != ExprKind::Var)
    return false;
  return VK(S.Dst->Var) == VK(S.Val->Var);
}

static bool eliminateDeadAssigns(std::vector<HighStmt> &Stmts,
                                 const VarKeySet &Refs) {
  bool Changed = false;
  Stmts.erase(std::remove_if(Stmts.begin(), Stmts.end(),
                             [&](const HighStmt &S) {
                               if (isSelfAssign(S)) {
                                 Changed = true;
                                 return true;
                               }
                               if (isDeadAssign(S, Refs)) {
                                 Changed = true;
                                 return true;
                               }
                               return false;
                             }),
              Stmts.end());
  for (auto &S : Stmts) {
    if (!S.Body.empty() && eliminateDeadAssigns(S.Body, Refs))
      Changed = true;
    if (!S.ElseBody.empty() && eliminateDeadAssigns(S.ElseBody, Refs))
      Changed = true;
    for (auto &C : S.Cases)
      if (eliminateDeadAssigns(C.Body, Refs))
        Changed = true;
    if (!S.DefaultBody.empty() && eliminateDeadAssigns(S.DefaultBody, Refs))
      Changed = true;
  }
  return Changed;
}

//===----------------------------------------------------------------------===//
// Phase 0: Remove unreachable code after terminators
//===----------------------------------------------------------------------===//

static void removeUnreachableCode(std::vector<HighStmt> &Stmts) {
  for (size_t I = 0; I < Stmts.size(); ++I) {
    bool IsTerminator = (Stmts[I].Kind == StmtKind::Return ||
                         Stmts[I].Kind == StmtKind::Break ||
                         Stmts[I].Kind == StmtKind::Continue);

    if (!IsTerminator && Stmts[I].Kind == StmtKind::Switch) {
      auto &SwitchStmt = Stmts[I];
      bool AllTerminate = !SwitchStmt.Cases.empty();
      for (auto &C : SwitchStmt.Cases) {
        if (C.Body.empty() || (C.Body.back().Kind != StmtKind::Return &&
                               C.Body.back().Kind != StmtKind::Break &&
                               C.Body.back().Kind != StmtKind::Goto)) {
          AllTerminate = false;
          break;
        }
      }
      if (AllTerminate && !SwitchStmt.DefaultBody.empty()) {
        if (SwitchStmt.DefaultBody.back().Kind != StmtKind::Return &&
            SwitchStmt.DefaultBody.back().Kind != StmtKind::Break &&
            SwitchStmt.DefaultBody.back().Kind != StmtKind::Goto)
          AllTerminate = false;
      }
      IsTerminator = AllTerminate;
    }

    if (IsTerminator && I + 1 < Stmts.size()) {
      Stmts.erase(Stmts.begin() + static_cast<long>(I + 1), Stmts.end());
      break;
    }

    if (!Stmts[I].Body.empty())
      removeUnreachableCode(Stmts[I].Body);
    if (!Stmts[I].ElseBody.empty())
      removeUnreachableCode(Stmts[I].ElseBody);
    for (auto &C : Stmts[I].Cases)
      if (!C.Body.empty())
        removeUnreachableCode(C.Body);
    if (!Stmts[I].DefaultBody.empty())
      removeUnreachableCode(Stmts[I].DefaultBody);
  }
}

//===----------------------------------------------------------------------===//
// Pre-DCE: Quick dead assignment elimination pass
//===----------------------------------------------------------------------===//

static void preDCE(std::vector<HighStmt> &Stmts) {
  std::function<bool(std::vector<HighStmt> &, const VarKeySet &)> PreElim;
  PreElim = [&](std::vector<HighStmt> &Body, const VarKeySet &Refs) -> bool {
    bool Changed = false;
    Body.erase(std::remove_if(Body.begin(), Body.end(),
                              [&](const HighStmt &S) {
                                if (S.Kind == StmtKind::Assign && S.Dst &&
                                    S.Val && S.Dst->Kind == ExprKind::Var &&
                                    S.Val->Kind != ExprKind::Call &&
                                    Refs.count(VK(S.Dst->Var)) == 0) {
                                  Changed = true;
                                  return true;
                                }
                                if (S.Kind == StmtKind::Assign && S.Dst &&
                                    S.Val && S.Dst->Kind == ExprKind::Var &&
                                    S.Val->Kind == ExprKind::Var &&
                                    VK(S.Dst->Var) == VK(S.Val->Var)) {
                                  Changed = true;
                                  return true;
                                }
                                return false;
                              }),
               Body.end());
    for (auto &S : Body) {
      if (!S.Body.empty() && PreElim(S.Body, Refs))
        Changed = true;
      if (!S.ElseBody.empty() && PreElim(S.ElseBody, Refs))
        Changed = true;
      for (auto &C : S.Cases)
        if (PreElim(C.Body, Refs))
          Changed = true;
      if (!S.DefaultBody.empty() && PreElim(S.DefaultBody, Refs))
        Changed = true;
    }
    return Changed;
  };

  size_t Before = Stmts.size();
  for (int PreIter = 0; PreIter < 8; ++PreIter) {
    VarKeySet Refs;
    collectStmtRefs(Stmts, Refs);
    if (!PreElim(Stmts, Refs))
      break;
  }
  if (Stmts.size() < Before)
    LLVM_DEBUG(llvm::dbgs() << "    pre-dce: " << Before << " -> "
                            << Stmts.size() << " stmts\n");
}

//===----------------------------------------------------------------------===//
// Phase 7: Eliminate gotos that jump to the next while-loop header
//===----------------------------------------------------------------------===//

static void eliminateGotoToLoop(std::vector<HighStmt> &Stmts) {
  auto TryRemoveTrailingGoto = [](std::vector<HighStmt> &Body) {
    for (size_t I = 0; I + 1 < Body.size(); ++I) {
      if (Body[I].Kind != StmtKind::Goto)
        continue;
      if (Body[I + 1].Kind != StmtKind::While)
        continue;
      va_t Target = Body[I].GotoTarget;
      if (Target == 0 || Target == InvalidVA)
        continue;
      auto &Loop = Body[I + 1];
      if (Loop.LoopHeaderAddr == Target ||
          (!Loop.Body.empty() && Loop.Body[0].Addr == Target)) {
        Body.erase(Body.begin() + static_cast<long>(I));
        --I;
      }
    }
  };
  TryRemoveTrailingGoto(Stmts);
  for (size_t I = 0; I + 1 < Stmts.size(); ++I) {
    if (Stmts[I + 1].Kind != StmtKind::While)
      continue;
    auto &Loop = Stmts[I + 1];
    auto StripBodyGoto = [&](std::vector<HighStmt> &Body) {
      if (Body.empty() || Body.back().Kind != StmtKind::Goto)
        return;
      va_t GotoDest = Body.back().GotoTarget;
      if (GotoDest == 0 || GotoDest == InvalidVA)
        return;
      if (Loop.LoopHeaderAddr == GotoDest ||
          (!Loop.Body.empty() && Loop.Body[0].Addr == GotoDest))
        Body.pop_back();
    };
    if (Stmts[I].Kind == StmtKind::If || Stmts[I].Kind == StmtKind::IfElse) {
      StripBodyGoto(Stmts[I].Body);
      StripBodyGoto(Stmts[I].ElseBody);
    }
  }
}

//===----------------------------------------------------------------------===//
// Phase 8b: Eliminate dead constant conditions
//===----------------------------------------------------------------------===//

static void eliminateDeadConditions(std::vector<HighStmt> &Stmts) {
  for (size_t I = 0; I < Stmts.size();) {
    auto &S = Stmts[I];
    if ((S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse) && S.Cond &&
        S.Cond->Kind == ExprKind::Const && S.Cond->ConstVal == 0) {
      if (S.Kind == StmtKind::IfElse && !S.ElseBody.empty()) {
        auto ElseBody = std::move(S.ElseBody);
        Stmts.erase(Stmts.begin() + static_cast<long>(I));
        Stmts.insert(Stmts.begin() + static_cast<long>(I), ElseBody.begin(),
                     ElseBody.end());
      } else {
        Stmts.erase(Stmts.begin() + static_cast<long>(I));
      }
      continue;
    }
    eliminateDeadConditions(S.Body);
    eliminateDeadConditions(S.ElseBody);
    for (auto &C : S.Cases)
      eliminateDeadConditions(C.Body);
    eliminateDeadConditions(S.DefaultBody);
    ++I;
  }
}

//===----------------------------------------------------------------------===//
// Phase 13: Iterative liveness-based DCE with expression inlining
//===----------------------------------------------------------------------===//

static void iterativeDCE(HighFunc &Func) {
  for (int Outer = 0; Outer < 6; ++Outer) {
    LLVM_DEBUG(llvm::dbgs() << "      dce iter " << Outer << "/6 (" << Func.Name
                            << ", " << Func.Body.size() << " stmts)\n");
    bool DCEChanged = false;
    for (int Iter = 0; Iter < 10; ++Iter) {
      VarKeySet Refs;
      collectStmtRefs(Func.Body, Refs);
      if (!eliminateDeadAssigns(Func.Body, Refs))
        break;
      DCEChanged = true;
    }
    VarKeyMap<ExprPtr> InlineDefs;
    VarKeyMap<int> InlineDefCount;
    VarKeyMap<int> InlineUseCount;
    std::unordered_set<const HighExpr *> Seen;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
          S.Dst->Kind == ExprKind::Var) {
        auto Key = VK(S.Dst->Var);
        InlineDefCount[Key]++;
        if (S.Val->Kind != ExprKind::Call)
          InlineDefs[Key] = S.Val;
      }
      forEachRhsExpr(S, [&](const ExprPtr &E) {
        countExprVarUses(E, InlineUseCount, Seen);
      });
    });
    for (auto It = InlineDefs.begin(); It != InlineDefs.end();) {
      if (InlineDefCount[It->first] != 1 || InlineUseCount[It->first] != 1)
        It = InlineDefs.erase(It);
      else
        ++It;
    }
    bool InlineChanged = !InlineDefs.empty();
    if (InlineChanged)
      inlineSingleDefs(Func.Body, InlineDefs);
    simplifyAllExprs(Func.Body);
    if (!DCEChanged && !InlineChanged)
      break;
  }
}

//===----------------------------------------------------------------------===//
// eliminateDeadStmts -- the main multi-phase DCE entry point
//===----------------------------------------------------------------------===//

void MedToHighConverter::eliminateDeadStmts(HighFunc &Func) {
  ExprRecurseDepth = 0;
  breakStmtCycles(Func.Body);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 0: unreachable (" << Func.Name
                          << ", " << Func.Body.size() << " stmts)\n");
  removeUnreachableCode(Func.Body);

  preDCE(Func.Body);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 1: alias (" << Func.Name << ", "
                          << Func.Body.size() << " stmts)\n");
  resolveRegAliases(Func.Body);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 2: copy chain (" << Func.Name
                          << ", " << Func.Body.size() << " stmts)\n");
  foldCopyChains(Func);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 3: phi copy prop (" << Func.Name
                          << ", " << Func.Body.size() << " stmts)\n");
  propagatePhiCopies(Func.Body);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 4: multi-use fold (" << Func.Name
                          << ", " << Func.Body.size() << " stmts)\n");
  foldMultiUseCopies(Func.Body);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 5: inline sdsu (" << Func.Name
                          << ", " << Func.Body.size() << " stmts)\n");
  inlineSingleDefSingleUse(Func.Body);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 6: loop copy prop (" << Func.Name
                          << ", " << Func.Body.size() << " stmts)\n");
  scopedCopyPropagation(Func.Body);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 7: goto elim (" << Func.Name << ", "
                          << Func.Body.size() << " stmts)\n");
  eliminateGotoToLoop(Func.Body);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 8: expr simplify (" << Func.Name
                          << ", " << Func.Body.size() << " stmts)\n");
  simplifyAllExprs(Func.Body);

  // Only now is there anything to measure: the expressions have been assembled
  // out of their separate assignments, and their easy shapes already rewritten.
  LLVM_DEBUG(llvm::dbgs() << "    dce phase 8b: semantic simplify ("
                          << Func.Name << ", " << Func.Body.size()
                          << " stmts)\n");
  simplifyExprSemantics(Func.Body);

  eliminateDeadConditions(Func.Body);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 9: redundant stack store ("
                          << Func.Name << ", " << Func.Body.size()
                          << " stmts)\n");
  eliminateRedundantStackStores(Func, TargetArch);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 10: consec dead store ("
                          << Func.Name << ", " << Func.Body.size()
                          << " stmts)\n");
  elimConsecutiveDeadStores(Func.Body);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 11: reg alias copy elim ("
                          << Func.Name << ", " << Func.Body.size()
                          << " stmts)\n");
  eliminateRegAliasCopies(Func);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 12: loop alias (" << Func.Name
                          << ", " << Func.Body.size() << " stmts)\n");
  eliminateLoopAliases(Func.Body);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 13: iterative DCE (" << Func.Name
                          << ", " << Func.Body.size() << " stmts)\n");
  iterativeDCE(Func);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 14: var rename (" << Func.Name
                          << ", " << Func.Body.size() << " stmts)\n");
  renameVars(Func.Body);

  postRenameCleanup(Func.Body);
}

} // namespace neverd
