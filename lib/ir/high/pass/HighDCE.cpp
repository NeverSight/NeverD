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
#include <vector>

#define DEBUG_TYPE "neverd-high-dce"

namespace neverd {

static bool isLocalExpr(const HighExpr &E) {
  return E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi;
}

//===----------------------------------------------------------------------===//
// Expression cycle detection and breaking
//===----------------------------------------------------------------------===//

static void
breakExprCycles(ExprPtr &Root,
                std::unordered_set<const HighExpr *> &KnownAcyclic) {
  struct Frame {
    ExprPtr *Slot = nullptr;
    size_t NextOperand = 0;
    bool Entered = false;
  };

  std::vector<Frame> Work{{&Root, 0, false}};
  std::unordered_set<const HighExpr *> Path;
  while (!Work.empty()) {
    Frame &Current = Work.back();
    if (!*Current.Slot) {
      Work.pop_back();
      continue;
    }

    HighExpr *Expr = Current.Slot->get();
    if (!Current.Entered) {
      if (KnownAcyclic.count(Expr)) {
        Work.pop_back();
        continue;
      }
      if (!Path.insert(Expr).second) {
        *Current.Slot = HighExpr::makeConst(0, 8);
        Work.pop_back();
        continue;
      }
      Current.Entered = true;
    }

    if (Current.NextOperand < Expr->Operands.size()) {
      ExprPtr &Operand = Expr->Operands[Current.NextOperand++];
      if (!Operand || KnownAcyclic.count(Operand.get()))
        continue;
      if (Path.count(Operand.get())) {
        Operand = HighExpr::makeConst(0, 8);
        continue;
      }
      Work.push_back({&Operand, 0, false});
      continue;
    }

    Path.erase(Expr);
    KnownAcyclic.insert(Expr);
    Work.pop_back();
  }
}

static void breakStmtCycles(std::vector<HighStmt> &Stmts) {
  std::unordered_set<const HighExpr *> Safe;
  walkStmts(Stmts, [&Safe](HighStmt &S) {
    forEachExpr(S, [&Safe](ExprPtr &EP) { breakExprCycles(EP, Safe); });
  });
}

//===----------------------------------------------------------------------===//
// Shared DCE utilities
//===----------------------------------------------------------------------===//

static void collectRefExpr(const ExprPtr &Root, VarKeySet &Refs) {
  std::unordered_set<const HighExpr *> Seen;
  std::vector<const HighExpr *> Work{Root.get()};
  while (!Work.empty()) {
    const auto *E = Work.back();
    Work.pop_back();
    if (!E || !Seen.insert(E).second)
      continue;
    if (isLocalExpr(*E))
      Refs.insert(VK(E->Var));
    for (const auto &Operand : E->Operands)
      Work.push_back(Operand.get());
  }
}

static bool removableAssignment(const HighStmt &S) {
  if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val ||
      !isLocalExpr(*S.Dst) || !S.Body.empty() || !S.ElseBody.empty() ||
      !S.Cases.empty() || !S.DefaultBody.empty() || !S.EHClauseBodies.empty())
    return false;
  std::unordered_set<const HighExpr *> Seen;
  std::vector<const HighExpr *> Work{S.Val.get()};
  while (!Work.empty()) {
    const auto *E = Work.back();
    Work.pop_back();
    if (!E || !Seen.insert(E).second)
      continue;
    if (E->Kind == ExprKind::Call || E->Kind == ExprKind::Store ||
        E->MemoryOrdering != NdMemoryOrdering::None ||
        E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    for (const auto &Operand : E->Operands)
      Work.push_back(Operand.get());
  }
  return true;
}

static void collectLiveRefs(const std::vector<HighStmt> &Stmts,
                            VarKeySet &Live) {
  VarKeyMap<VarKeySet> Dependencies;
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (removableAssignment(S)) {
      // PHI edge copies may define the same local on several paths. Union all
      // dependencies instead of selecting a traversal-dependent definition.
      collectRefExpr(S.Val, Dependencies[VK(S.Dst->Var)]);
    } else {
      // Calls, memory effects and control-flow expressions are observable
      // roots. A variable referenced only by other dead assignments is not.
      forEachExpr(S, [&](const ExprPtr &E) { collectRefExpr(E, Live); });
    }
  });
  std::vector<VarKey> Work(Live.begin(), Live.end());
  for (size_t I = 0; I < Work.size(); ++I) {
    const auto Found = Dependencies.find(Work[I]);
    if (Found == Dependencies.end())
      continue;
    for (const auto &Input : Found->second)
      if (Live.insert(Input).second)
        Work.push_back(Input);
  }
}

static bool isDeadAssign(const HighStmt &S, const VarKeySet &Refs) {
  return removableAssignment(S) && Refs.count(VK(S.Dst->Var)) == 0;
}

static bool isSelfAssign(const HighStmt &S) {
  if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
    return false;
  if (!isLocalExpr(*S.Dst) || !isLocalExpr(*S.Val))
    return false;
  return VK(S.Dst->Var) == VK(S.Val->Var);
}

static std::unordered_set<va_t>
referencedStatementEntries(const std::vector<HighStmt> &Stmts) {
  std::unordered_set<va_t> Entries;
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      Entries.insert(S.GotoTarget);
  });
  return Entries;
}

static bool eliminateDeadAssigns(std::vector<HighStmt> &Stmts,
                                 const VarKeySet &Refs,
                                 const std::unordered_set<va_t> &Entries) {
  bool Changed = false;
  if (!Entries.empty()) {
    for (size_t I = 0; I < Stmts.size();) {
      const size_t First = I++;
      const va_t Entry = Stmts[First].Addr;
      while (I < Stmts.size() && Stmts[I].Addr == Entry)
        ++I;
      if (!Entries.count(Entry))
        continue;
      bool AllDead = true;
      for (size_t J = First; J < I; ++J)
        AllDead &= isSelfAssign(Stmts[J]) || isDeadAssign(Stmts[J], Refs);
      if (!AllDead)
        continue;
      // A dead value does not make its branch entry dead. Keep that exact
      // position as an empty statement block while discarding the expression.
      // Several consecutive assignments can come from one instruction: keep
      // only one entry, and let any surviving statement own its original label.
      // Nops are removed by a later pass; an empty block also emits a valid
      // statement when the label is the last entry in its containing body.
      auto &S = Stmts[First];
      S = HighStmt{};
      S.Kind = StmtKind::Block;
      S.Addr = Entry;
      Changed = true;
    }
  }
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
    if (!S.Body.empty() && eliminateDeadAssigns(S.Body, Refs, Entries))
      Changed = true;
    if (!S.ElseBody.empty() && eliminateDeadAssigns(S.ElseBody, Refs, Entries))
      Changed = true;
    for (auto &C : S.Cases)
      if (eliminateDeadAssigns(C.Body, Refs, Entries))
        Changed = true;
    if (!S.DefaultBody.empty() &&
        eliminateDeadAssigns(S.DefaultBody, Refs, Entries))
      Changed = true;
    for (auto &Clause : S.EHClauseBodies)
      if (eliminateDeadAssigns(Clause, Refs, Entries))
        Changed = true;
  }
  return Changed;
}

//===----------------------------------------------------------------------===//
// Phase 0: Remove unreachable code after terminators
//===----------------------------------------------------------------------===//

static bool removeUnreachableCode(std::vector<HighStmt> &Stmts,
                                  const std::unordered_set<va_t> &Entries) {
  bool HasEntries = false;
  bool FallsThrough = true;
  size_t Kept = 0;
  for (size_t I = 0; I < Stmts.size(); ++I) {
    auto &S = Stmts[I];
    bool HasEntry = Entries.count(S.Addr) != 0;
    HasEntry |= removeUnreachableCode(S.Body, Entries);
    HasEntry |= removeUnreachableCode(S.ElseBody, Entries);
    for (auto &C : S.Cases)
      HasEntry |= removeUnreachableCode(C.Body, Entries);
    HasEntry |= removeUnreachableCode(S.DefaultBody, Entries);
    // Exception bodies retain their existing cleanup policy, but an entry
    // inside one still makes the containing statement reachable.
    for (const auto &Clause : S.EHClauseBodies)
      walkStmts(Clause, [&](const HighStmt &Nested) {
        HasEntry |= Entries.count(Nested.Addr) != 0;
      });
    HasEntries |= HasEntry;

    // A terminator ends fallthrough, not incoming branches. Resume at the
    // next referenced entry, including an entry nested in a statement tree.
    if (!FallsThrough && !HasEntry)
      continue;
    FallsThrough = S.Kind != StmtKind::Return && S.Kind != StmtKind::Break &&
                   S.Kind != StmtKind::Continue && !switchAlwaysReturns(S);
    if ((S.Kind == StmtKind::Call && isNonReturningSourceCall(S.CallExpr)) ||
        ((S.Kind == StmtKind::Assign || S.Kind == StmtKind::ExprStmt) &&
         isNonReturningSourceCall(S.Val)))
      FallsThrough = false;
    if (Kept != I)
      Stmts[Kept] = std::move(S);
    ++Kept;
  }
  Stmts.resize(Kept);
  return HasEntries;
}

void removeUnreachableCode(std::vector<HighStmt> &Stmts) {
  const auto Entries = referencedStatementEntries(Stmts);
  removeUnreachableCode(Stmts, Entries);
}

//===----------------------------------------------------------------------===//
// Pre-DCE: Quick dead assignment elimination pass
//===----------------------------------------------------------------------===//

static void preDCE(std::vector<HighStmt> &Stmts,
                   const std::unordered_set<va_t> &Entries) {
  size_t Before = Stmts.size();
  for (int PreIter = 0; PreIter < 8; ++PreIter) {
    VarKeySet Refs;
    collectLiveRefs(Stmts, Refs);
    if (!eliminateDeadAssigns(Stmts, Refs, Entries))
      break;
  }
  if (Stmts.size() < Before)
    LLVM_DEBUG(llvm::dbgs() << "    pre-dce: " << Before << " -> "
                            << Stmts.size() << " stmts\n");
}

void eliminateUnusedValues(std::vector<HighStmt> &Stmts) {
  coalesceBranchEntryStatements(Stmts);
  preDCE(Stmts, referencedStatementEntries(Stmts));
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

static void iterativeDCE(HighFunc &Func,
                         const std::unordered_set<va_t> &Entries) {
  for (int Outer = 0; Outer < 6; ++Outer) {
    LLVM_DEBUG(llvm::dbgs() << "      dce iter " << Outer << "/6 (" << Func.Name
                            << ", " << Func.Body.size() << " stmts)\n");
    bool DCEChanged = false;
    for (int Iter = 0; Iter < 10; ++Iter) {
      VarKeySet Refs;
      collectLiveRefs(Func.Body, Refs);
      if (!eliminateDeadAssigns(Func.Body, Refs, Entries))
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
        if (S.Val->Kind != ExprKind::Call && !S.Val->hasOrderedMemoryAccess() &&
            !containsMemoryRead(S.Val))
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
  coalesceBranchEntryStatements(Func.Body);
  const auto Entries = referencedStatementEntries(Func.Body);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 0: unreachable (" << Func.Name
                          << ", " << Func.Body.size() << " stmts)\n");
  removeUnreachableCode(Func.Body, Entries);

  preDCE(Func.Body, Entries);

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

  eliminateDeadConditions(Func.Body);

  // A call argument equal to a stored value does not make the memory write
  // dead: the callee or a later load may still observe the frame slot.

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
  iterativeDCE(Func, Entries);

  elimUnreadPrivateFrameStores(Func, TargetArch);
  narrowSourceConcatLocals(Func);
  // Removing a vector carrier can expose a second, narrower scalar copy
  // view. Re-prove that view with the same bounds before assigning names.
  narrowSourceConcatLocals(Func);

  LLVM_DEBUG(llvm::dbgs() << "    dce phase 14: var rename (" << Func.Name
                          << ", " << Func.Body.size() << " stmts)\n");
  renameVars(Func.Body);

  postRenameCleanup(Func.Body);

  // Run semantic rewriting after every pass that substitutes expressions.
  // Its result is a shared DAG; feeding it back through alias propagation can
  // replace one of its own leaves with the DAG that contains it and create a
  // cycle.  At this point expressions are fully assembled.  Normalize any
  // cycle exposed by sharing before the C analyses traverse the result.
  LLVM_DEBUG(llvm::dbgs() << "    dce phase 15: semantic simplify ("
                          << Func.Name << ", " << Func.Body.size()
                          << " stmts)\n");
  simplifyExprSemantics(Func.Body);
  breakStmtCycles(Func.Body);
  eliminateDeadConditions(Func.Body);
}

} // namespace neverd
