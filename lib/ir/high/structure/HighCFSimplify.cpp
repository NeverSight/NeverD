//===- HighCFSimplify.cpp - Control-flow simplification for HighIR ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Simplifies the flat HighIR statement list into structured control
/// flow: the simplifyControlFlow driver, goto elimination, and proven
/// return-block inlining.
///
/// If/else structuring, loop detection and switch recovery are in separate
/// files:
///   HighCFSimplifyIfElse.cpp — if(cond){goto} → if/else tree folding
///   HighLoopRecovery.cpp     — backward-goto → while-loop conversion
///   HighIfChainToSwitch.cpp  — if-chain → switch recovery
///
//===----------------------------------------------------------------------===//

#include "HighCFSimplifyDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace neverd {

//===----------------------------------------------------------------------===//
// Trivial goto removal (goto to next statement)
//===----------------------------------------------------------------------===//

static void removeTrivialGotos(std::vector<HighStmt> &Stmts,
                               const std::set<va_t> &Targets) {
  for (int I = static_cast<int>(Stmts.size()) - 2; I >= 0; --I) {
    if (Stmts[I].Kind != StmtKind::Goto)
      continue;
    va_t Target = Stmts[I].GotoTarget;
    if (Target == 0 || Target == InvalidVA)
      continue;
    va_t NextAddr = Stmts[static_cast<size_t>(I) + 1].Addr;
    if (NextAddr == 0)
      continue;
    // A target short of the next statement is fall-through only when it lies
    // past the goto itself (padding between them); a target at or before the
    // goto, such as the `jmp $` self-loop, is a real backward jump.
    const va_t Own = Stmts[I].Addr;
    const bool OwnKnown = Own != 0 && Own != InvalidVA;
    if (!(Target == NextAddr || (OwnKnown && Target > Own &&
                                 Target < NextAddr && NextAddr - Target <= 16)))
      continue;
    // A goto that is itself a branch target (a lone `jmp` block) keeps its
    // address as an empty anchor, so gotos to it still have a label.
    if (OwnKnown && Targets.count(Own)) {
      HighStmt Anchor;
      Anchor.Kind = StmtKind::Block;
      Anchor.Addr = Own;
      Stmts[I] = std::move(Anchor);
    } else {
      Stmts.erase(Stmts.begin() + I);
    }
  }
  for (auto &S : Stmts) {
    if (!S.Body.empty())
      removeTrivialGotos(S.Body, Targets);
    if (!S.ElseBody.empty())
      removeTrivialGotos(S.ElseBody, Targets);
  }
}

//===----------------------------------------------------------------------===//
// Nested goto simplification inside structured blocks
//===----------------------------------------------------------------------===//

static void simplifyNestedGotos(std::vector<HighStmt> &Stmts) {
  for (auto &S : Stmts) {
    if (S.Kind == StmtKind::While || S.Kind == StmtKind::If ||
        S.Kind == StmtKind::IfElse) {
      if (!S.Body.empty()) {
        for (int J = static_cast<int>(S.Body.size()) - 1; J >= 0; --J) {
          auto &InnerStmt = S.Body[J];
          if (InnerStmt.Kind != StmtKind::If)
            continue;
          if (InnerStmt.Body.size() != 1 ||
              InnerStmt.Body[0].Kind != StmtKind::Goto)
            continue;

          va_t IfGotoTarget = InnerStmt.Body[0].GotoTarget;
          size_t NextJ = static_cast<size_t>(J) + 1;
          if (NextJ >= S.Body.size())
            continue;

          std::vector<HighStmt> ElseStmts;
          size_t EndJ = NextJ;
          for (size_t K = NextJ; K < S.Body.size(); ++K) {
            ElseStmts.push_back(S.Body[K]);
            EndJ = K + 1;
            if (S.Body[K].Kind == StmtKind::Goto ||
                S.Body[K].Kind == StmtKind::Return ||
                S.Body[K].Kind == StmtKind::Continue)
              break;
          }

          if (ElseStmts.empty())
            continue;

          va_t ElseMergeAddr = 0;
          if (!ElseStmts.empty() && ElseStmts.back().Kind == StmtKind::Goto)
            ElseMergeAddr = ElseStmts.back().GotoTarget;

          std::vector<HighStmt> IfStmts;
          if (IfGotoTarget != 0 && EndJ < S.Body.size()) {
            size_t IfEnd = EndJ;
            for (size_t K = EndJ; K < S.Body.size(); ++K) {
              if (ElseMergeAddr != 0 && S.Body[K].Addr == ElseMergeAddr)
                break;
              IfStmts.push_back(S.Body[K]);
              IfEnd = K + 1;
              if (S.Body[K].Kind == StmtKind::Goto ||
                  S.Body[K].Kind == StmtKind::Return ||
                  S.Body[K].Kind == StmtKind::Continue)
                break;
            }
            if (!IfStmts.empty())
              EndJ = IfEnd;
          }

          InnerStmt.Kind = StmtKind::IfElse;
          if (!IfStmts.empty())
            InnerStmt.Body = std::move(IfStmts);
          InnerStmt.ElseBody = std::move(ElseStmts);

          S.Body.erase(S.Body.begin() + static_cast<long>(NextJ),
                       S.Body.begin() + static_cast<long>(EndJ));
        }
        simplifyNestedGotos(S.Body);
      }
      if (!S.ElseBody.empty())
        simplifyNestedGotos(S.ElseBody);
    }
  }
}

//===----------------------------------------------------------------------===//
// Guard-before-switch cleanup
//===----------------------------------------------------------------------===//

static void cleanupGuardBeforeSwitch(HighFunc &Func) {
  for (size_t I = 0; I < Func.Body.size(); ++I) {
    if (Func.Body[I].Kind != StmtKind::If)
      continue;
    if (Func.Body[I].Body.size() != 1)
      continue;

    size_t SwIdx = I + 1;
    while (SwIdx < Func.Body.size() &&
           (Func.Body[SwIdx].Kind == StmtKind::Assign ||
            Func.Body[SwIdx].Kind == StmtKind::Nop))
      ++SwIdx;
    if (SwIdx >= Func.Body.size() || Func.Body[SwIdx].Kind != StmtKind::Switch)
      continue;
    if (Func.Body[SwIdx].DefaultBody.empty())
      continue;

    auto &IfBody = Func.Body[I].Body[0];
    auto &DefBody = Func.Body[SwIdx].DefaultBody;
    bool BodiesMatch = false;

    auto GetConstVal = [](const ExprPtr &E) -> std::optional<uint64_t> {
      if (!E)
        return std::nullopt;
      if (E->Kind == ExprKind::Const)
        return E->ConstVal;
      if (!E->Operands.empty() && E->Operands[0]->Kind == ExprKind::Const) {
        if (E->Kind == ExprKind::Cast)
          return E->Operands[0]->ConstVal;
        if (E->Kind == ExprKind::UnaryOp &&
            (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT))
          return E->Operands[0]->ConstVal;
      }
      return std::nullopt;
    };

    if (IfBody.Kind == StmtKind::Return && !DefBody.empty() &&
        DefBody.back().Kind == StmtKind::Return) {
      auto LV = GetConstVal(IfBody.RetVal);
      auto RV = GetConstVal(DefBody.back().RetVal);
      if (LV && RV) {
        uint64_t Mask = 0xFFFFFFFF;
        BodiesMatch = ((*LV & Mask) == (*RV & Mask));
      } else if (!IfBody.RetVal && !DefBody.back().RetVal)
        BodiesMatch = true;
    }
    if (IfBody.Kind == StmtKind::Goto && DefBody.size() == 1 &&
        DefBody[0].Kind == StmtKind::Goto)
      BodiesMatch = (IfBody.GotoTarget == DefBody[0].GotoTarget);

    if (BodiesMatch) {
      Func.Body.erase(Func.Body.begin() + static_cast<long>(I));
      --I;
    }
  }
}

//===----------------------------------------------------------------------===//
// inlineGotoReturns — replace goto→return-block with inline return
//===----------------------------------------------------------------------===//

void MedToHighConverter::inlineGotoReturns(HighFunc &Func, const MedFunc &Med) {
  std::map<va_t, ExprPtr> ReturnBlocks;
  for (auto &MedBlock : Med.Blocks) {
    if (MedBlock.Ops.size() < 2 || MedBlock.Ops.size() > 4)
      continue;
    auto &LastOp = MedBlock.Ops.back();
    if (LastOp.Opcode != NdOp::RETURN)
      continue;
    for (int J = static_cast<int>(MedBlock.Ops.size()) - 2; J >= 0; --J) {
      auto &CurrOp = MedBlock.Ops[J];
      if (CurrOp.Output.Kind == MedVar::Reg && CurrOp.Output.RegOff == 0) {
        if ((CurrOp.Opcode == NdOp::COPY || CurrOp.Opcode == NdOp::INT_ZEXT) &&
            CurrOp.NumInputs >= 1)
          ReturnBlocks[MedBlock.Ops.front().Addr] =
              medvarToExpr(CurrOp.Inputs[0]);
        break;
      }
    }
  }

  std::function<void(std::vector<HighStmt> &)> Rewrite;
  Rewrite = [&](std::vector<HighStmt> &Stmts) {
    for (auto &S : Stmts) {
      if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
          S.GotoTarget != InvalidVA) {
        auto It = ReturnBlocks.find(S.GotoTarget);
        if (It != ReturnBlocks.end()) {
          S.Kind = StmtKind::Return;
          S.RetVal = It->second;
        }
      }
      Rewrite(S.Body);
      Rewrite(S.ElseBody);
      for (auto &C : S.Cases)
        Rewrite(C.Body);
      Rewrite(S.DefaultBody);
    }
  };
  Rewrite(Func.Body);
}

//===----------------------------------------------------------------------===//
// Merge consecutive if-blocks with the same condition
//===----------------------------------------------------------------------===//

static bool exprEquivalent(const ExprPtr &A, const ExprPtr &B) {
  if (!A || !B)
    return false;
  return A->structuralEq(*B);
}

static void mergeConsecutiveCondBlocks(std::vector<HighStmt> &Stmts) {
  for (size_t I = 0; I + 1 < Stmts.size();) {
    auto &Cur = Stmts[I];
    auto &Next = Stmts[I + 1];

    if (Cur.Kind == StmtKind::If && Next.Kind == StmtKind::If && Cur.Cond &&
        Next.Cond && !Cur.Cond->hasOrderedMemoryAccess() &&
        !Next.Cond->hasOrderedMemoryAccess() &&
        exprEquivalent(Cur.Cond, Next.Cond)) {
      for (auto &S : Next.Body)
        Cur.Body.push_back(std::move(S));
      Stmts.erase(Stmts.begin() + static_cast<long>(I + 1));
      continue;
    }

    if (Cur.Kind == StmtKind::IfElse && Next.Kind == StmtKind::IfElse &&
        Cur.Cond && Next.Cond && !Cur.Cond->hasOrderedMemoryAccess() &&
        !Next.Cond->hasOrderedMemoryAccess() &&
        exprEquivalent(Cur.Cond, Next.Cond)) {
      for (auto &S : Next.Body)
        Cur.Body.push_back(std::move(S));
      for (auto &S : Next.ElseBody)
        Cur.ElseBody.push_back(std::move(S));
      Stmts.erase(Stmts.begin() + static_cast<long>(I + 1));
      continue;
    }

    ++I;
  }

  for (auto &S : Stmts) {
    if (!S.Body.empty())
      mergeConsecutiveCondBlocks(S.Body);
    if (!S.ElseBody.empty())
      mergeConsecutiveCondBlocks(S.ElseBody);
  }
}

// `if (c) { v = base + k; } use(v)` after goto-folding drops the false-edge
// PHI `v = base`.  Seed that incoming value before the if when the then-arm
// is an add/sub update of a named base.
static const HighExpr *unwrapNamed(const HighExpr *E) {
  unsigned Depth = 0;
  while (E && Depth++ < 8 && !E->Operands.empty() && E->Operands[0] &&
         (E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast ||
          (E->Kind == ExprKind::UnaryOp &&
           (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT))))
    E = E->Operands[0].get();
  if (E && (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi))
    return E;
  return nullptr;
}

static const HighExpr *ifThenUpdateBase(const HighExpr &Val) {
  const HighExpr *Cur = &Val;
  unsigned Depth = 0;
  while (Cur && Depth++ < 8 && !Cur->Operands.empty() && Cur->Operands[0] &&
         (Cur->Kind == ExprKind::Cast || Cur->Kind == ExprKind::BitCast ||
          (Cur->Kind == ExprKind::UnaryOp &&
           (Cur->Op == NdOp::INT_ZEXT || Cur->Op == NdOp::INT_SEXT))))
    Cur = Cur->Operands[0].get();
  if (!Cur || Cur->Kind != ExprKind::BinOp || Cur->Operands.size() != 2 ||
      (Cur->Op != NdOp::INT_ADD && Cur->Op != NdOp::INT_SUB))
    return nullptr;
  if (const HighExpr *Left = unwrapNamed(Cur->Operands[0].get()))
    return Left;
  if (Cur->Op == NdOp::INT_ADD)
    return unwrapNamed(Cur->Operands[1].get());
  return nullptr;
}

static bool stmtUsesVar(const HighStmt &S, const MedVar &Var) {
  bool Hit = false;
  forEachRhsExpr(S, [&](const ExprPtr &E) {
    if (!E || Hit)
      return;
    std::function<void(const HighExpr &)> Walk = [&](const HighExpr &N) {
      if (Hit)
        return;
      if ((N.Kind == ExprKind::Var || N.Kind == ExprKind::Phi) &&
          varKey(N.Var) == varKey(Var))
        Hit = true;
      for (const ExprPtr &Op : N.Operands)
        if (Op)
          Walk(*Op);
    };
    Walk(*E);
  });
  return Hit;
}

static void seedIfThenJoinInits(std::vector<HighStmt> &Stmts) {
  for (size_t I = 0; I < Stmts.size(); ++I) {
    if (!Stmts[I].Body.empty())
      seedIfThenJoinInits(Stmts[I].Body);
    if (!Stmts[I].ElseBody.empty())
      seedIfThenJoinInits(Stmts[I].ElseBody);
    for (auto &C : Stmts[I].Cases)
      if (!C.Body.empty())
        seedIfThenJoinInits(C.Body);
    if (!Stmts[I].DefaultBody.empty())
      seedIfThenJoinInits(Stmts[I].DefaultBody);
    if (!Stmts[I].Cond ||
        (Stmts[I].Kind != StmtKind::If && Stmts[I].Kind != StmtKind::IfElse))
      continue;

    auto LastVarAssign = [](const std::vector<HighStmt> &Body)
        -> const HighStmt * {
      const HighStmt *Hit = nullptr;
      for (const HighStmt &Inner : Body) {
        if (Inner.Kind == StmtKind::Assign && Inner.Dst && Inner.Val &&
            Inner.Dst->Kind == ExprKind::Var)
          Hit = &Inner;
      }
      return Hit;
    };
    const HighStmt *ThenAssign = LastVarAssign(Stmts[I].Body);
    const HighStmt *ElseAssign = LastVarAssign(Stmts[I].ElseBody);
    const HighStmt *LastAssign = ThenAssign;
    if (Stmts[I].Kind == StmtKind::IfElse) {
      if (ThenAssign && ElseAssign)
        continue;
      LastAssign = ThenAssign ? ThenAssign : ElseAssign;
    }
    if (!LastAssign)
      continue;
    const MedVar Dest = LastAssign->Dst->Var;
    bool UsedAfter = false;
    for (size_t J = I + 1; J < Stmts.size(); ++J) {
      if (stmtUsesVar(Stmts[J], Dest)) {
        UsedAfter = true;
        break;
      }
    }
    if (!UsedAfter)
      continue;
    bool AssignedBefore = false;
    for (size_t J = 0; J < I; ++J) {
      if (Stmts[J].Kind == StmtKind::Assign && Stmts[J].Dst &&
          Stmts[J].Dst->Kind == ExprKind::Var &&
          varKey(Stmts[J].Dst->Var) == varKey(Dest)) {
        AssignedBefore = true;
        break;
      }
    }
    if (AssignedBefore)
      continue;
    const HighExpr *Base = ifThenUpdateBase(*LastAssign->Val);
    if (!Base || varKey(Base->Var) == varKey(Dest))
      continue;
    HighStmt Init;
    Init.Kind = StmtKind::Assign;
    Init.Addr = Stmts[I].Addr;
    Init.Dst = HighExpr::makeVar(Dest, LastAssign->Dst->Type);
    Init.Val = std::make_shared<HighExpr>(*Base);
    Stmts.insert(Stmts.begin() + static_cast<long>(I), std::move(Init));
    ++I;
  }
}

//===----------------------------------------------------------------------===//
// simplifyControlFlow -- the main entry point
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Duplicate small return tails into the gotos that reach them
//===----------------------------------------------------------------------===//

static bool isPureValue(const HighExpr &E, unsigned Depth = 0) {
  if (Depth > 16)
    return false;
  switch (E.Kind) {
  case ExprKind::Var:
  case ExprKind::Const:
  case ExprKind::Phi:
    return true;
  case ExprKind::BinOp:
  case ExprKind::UnaryOp:
  case ExprKind::Cast:
  case ExprKind::BitCast:
    if (E.Op == NdOp::ATOMIC_ADD || E.Op == NdOp::ATOMIC_XCHG ||
        E.Op == NdOp::ATOMIC_CMPXCHG)
      return false;
    for (const ExprPtr &Op : E.Operands)
      if (!Op || !isPureValue(*Op, Depth + 1))
        return false;
    return true;
  default:
    return false;
  }
}

/// A value a return tail may copy: pure, or reading memory, with no call,
/// store or atomic.  Each path executes exactly one copy, so duplicating a
/// load is as exact as duplicating arithmetic.
static bool isTailValue(const HighExpr &E, unsigned Depth = 0) {
  if (Depth > 32)
    return false;
  switch (E.Kind) {
  case ExprKind::Var:
  case ExprKind::Const:
  case ExprKind::Phi:
    return true;
  case ExprKind::Load:
    if (E.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        E.MemoryOrdering != NdMemoryOrdering::None)
      return false;
    [[fallthrough]];
  case ExprKind::BinOp:
  case ExprKind::UnaryOp:
  case ExprKind::Cast:
  case ExprKind::BitCast:
    if (E.Op == NdOp::ATOMIC_ADD || E.Op == NdOp::ATOMIC_XCHG ||
        E.Op == NdOp::ATOMIC_CMPXCHG)
      return false;
    for (const ExprPtr &Op : E.Operands)
      if (!Op || !isTailValue(*Op, Depth + 1))
        return false;
    return true;
  default:
    return false;
  }
}

/// Number of pure variable assignments in \p S when it is one (or a block of
/// only such assignments and removed statements); nullopt otherwise.
static std::optional<size_t> pureAssignCount(const HighStmt &S) {
  if (S.Kind == StmtKind::Nop)
    return 0;
  if (S.Kind == StmtKind::Assign) {
    if (!S.Dst || S.Dst->Kind != ExprKind::Var || !S.Val)
      return std::nullopt;
    if (isTailValue(*S.Val))
      return 1;
    // One direct call whose arguments are tail values: each path still runs
    // exactly one copy of it.
    if (S.Val->Kind == ExprKind::Call &&
        S.Val->IntrinsicId == Intrinsic::None &&
        S.Val->IntrinsicOutputs.empty() &&
        std::all_of(S.Val->Operands.begin(), S.Val->Operands.end(),
                    [](const ExprPtr &Op) { return Op && isTailValue(*Op); }))
      return 1;
    return std::nullopt;
  }
  if (S.Kind != StmtKind::Block)
    return std::nullopt;
  size_t Count = 0;
  for (const HighStmt &Child : S.Body) {
    if (Child.Addr != 0 && Child.Addr != S.Addr)
      return std::nullopt;
    std::optional<size_t> ChildCount = pureAssignCount(Child);
    if (!ChildCount || Child.Kind == StmtKind::Block)
      return std::nullopt;
    Count += *ChildCount;
  }
  return Count;
}

/// `goto L` where L starts a few pure assignments and a return (typically
/// `result = 1; return result;` shared through an epilogue) becomes a copy
/// of those statements.  The labelled original stays for any other path, so
/// no code is removed; only the jump is.
bool duplicateSmallReturnTails(std::vector<HighStmt> &Body) {
  constexpr size_t kMaxTailAssigns = 3;
  constexpr size_t kMaxComposedTail = 2 * kMaxTailAssigns + 1;
  std::map<va_t, std::vector<HighStmt>> Tails;
  std::function<void(std::vector<HighStmt> &)> Collect =
      [&](std::vector<HighStmt> &Stmts) {
        for (size_t I = 0; I < Stmts.size(); ++I) {
          const va_t Label = Stmts[I].Addr;
          if (Label != 0 && Label != InvalidVA && !Tails.count(Label) &&
              (I == 0 || Stmts[I - 1].Addr != Label)) {
            // An empty block can anchor the label ahead of the tail.
            // Removed statements (Nop) print nothing and are skipped.
            size_t First = I;
            while (First < Stmts.size() &&
                   ((Stmts[First].Kind == StmtKind::Block &&
                     Stmts[First].Body.empty()) ||
                    Stmts[First].Kind == StmtKind::Nop))
              ++First;
            size_t J = First;
            size_t Assigns = 0;
            while (J < Stmts.size()) {
              std::optional<size_t> Count = pureAssignCount(Stmts[J]);
              if (!Count || Assigns + *Count > kMaxTailAssigns)
                break;
              Assigns += *Count;
              ++J;
            }
            if (J < Stmts.size() && Stmts[J].Kind == StmtKind::Return &&
                (!Stmts[J].RetVal || isTailValue(*Stmts[J].RetVal))) {
              Tails.emplace(Label,
                            std::vector<HighStmt>(Stmts.begin() + First,
                                                  Stmts.begin() + J + 1));
            } else if (J < Stmts.size() && Stmts[J].Kind == StmtKind::Goto &&
                       Stmts[J].GotoTarget != Label) {
              // Edge copies ahead of a jump to a shared return epilogue: the
              // tail is those copies followed by the epilogue's own tail.
              auto Target = Tails.find(Stmts[J].GotoTarget);
              if (Target != Tails.end() &&
                  Assigns + Target->second.size() <= kMaxComposedTail) {
                std::vector<HighStmt> Tail(Stmts.begin() + First,
                                           Stmts.begin() + J);
                Tail.insert(Tail.end(), Target->second.begin(),
                            Target->second.end());
                Tails.emplace(Label, std::move(Tail));
              }
            }
          }
          Collect(Stmts[I].Body);
          Collect(Stmts[I].ElseBody);
          for (auto &C : Stmts[I].Cases)
            Collect(C.Body);
          Collect(Stmts[I].DefaultBody);
        }
      };
  // A composed tail needs its epilogue's tail first; the epilogue usually
  // follows the jumps to it, so repeat until no new tail appears.
  for (size_t Round = 0; Round < 4; ++Round) {
    const size_t Before = Tails.size();
    Collect(Body);
    if (Tails.size() == Before)
      break;
  }
  if (Tails.empty())
    return false;
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Rewrite =
      [&](std::vector<HighStmt> &Stmts) {
        for (size_t I = 0; I < Stmts.size(); ++I) {
          if (Stmts[I].Kind == StmtKind::Goto) {
            auto It = Tails.find(Stmts[I].GotoTarget);
            if (It != Tails.end()) {
              std::vector<HighStmt> Copy = It->second;
              // The copies are not jump targets; keep the label unique.
              for (HighStmt &C : Copy)
                C.Addr = Stmts[I].Addr;
              Stmts.erase(Stmts.begin() + I);
              Stmts.insert(Stmts.begin() + I, Copy.begin(), Copy.end());
              I += Copy.size() - 1;
              Changed = true;
              continue;
            }
          }
          Rewrite(Stmts[I].Body);
          Rewrite(Stmts[I].ElseBody);
          for (auto &C : Stmts[I].Cases)
            Rewrite(C.Body);
          Rewrite(Stmts[I].DefaultBody);
        }
      };
  Rewrite(Body);
  return Changed;
}

/// Late goto reduction on the final statement tree.  Every rewrite moves or
/// merges statements within one statement list and keeps their order of
/// execution; nothing is removed except a jump made redundant by the move.
///
///  * `if (a) goto L; if (b) goto L;`  ->  `if (a || b) goto L;`
///  * a block entered only by one forward `if (c) goto X;`, placed after a
///    terminator and ending in `return`/`goto`, moves into that `if`.
///  * `if (c) { ...; goto Y; } S...; Y:`  ->  `if (c) { ... } else { S... }`.
/// True when \p Stmts contains a break or continue that would bind to a loop
/// wrapped around it.  A nested loop owns both; a switch owns only break.
static bool hasLooseBreakOrContinue(const std::vector<HighStmt> &Stmts,
                                    bool InSwitch = false) {
  for (const HighStmt &S : Stmts) {
    if (S.Kind == StmtKind::Continue ||
        (S.Kind == StmtKind::Break && !InSwitch))
      return true;
    switch (S.Kind) {
    case StmtKind::While:
    case StmtKind::DoWhile:
    case StmtKind::For:
      break;
    case StmtKind::Switch:
      for (const auto &C : S.Cases)
        if (hasLooseBreakOrContinue(C.Body, /*InSwitch=*/true))
          return true;
      if (hasLooseBreakOrContinue(S.DefaultBody, /*InSwitch=*/true))
        return true;
      break;
    default:
      if (hasLooseBreakOrContinue(S.Body, InSwitch) ||
          hasLooseBreakOrContinue(S.ElseBody, InSwitch))
        return true;
      for (const auto &ClauseBody : S.EHClauseBodies)
        if (hasLooseBreakOrContinue(ClauseBody, InSwitch))
          return true;
      break;
    }
  }
  return false;
}

/// Two statement lists of assignments, stores, returns and gotos that do the
/// same thing and contain no entered label.
static bool sameStraightLineBody(const std::vector<HighStmt> &A,
                                 const std::vector<HighStmt> &B) {
  if (A.size() != B.size())
    return false;
  for (size_t I = 0; I < A.size(); ++I) {
    const HighStmt &X = A[I], &Y = B[I];
    if (X.Kind != Y.Kind)
      return false;
    switch (X.Kind) {
    case StmtKind::Assign:
      if (!exprEquivalent(X.Dst, Y.Dst) || !exprEquivalent(X.Val, Y.Val))
        return false;
      break;
    case StmtKind::Return:
      if ((X.RetVal || Y.RetVal) && !exprEquivalent(X.RetVal, Y.RetVal))
        return false;
      break;
    case StmtKind::Goto:
      if (X.GotoTarget != Y.GotoTarget)
        return false;
      break;
    case StmtKind::Nop:
      break;
    default:
      return false;
    }
  }
  return true;
}

bool reduceSingleUseGotos(std::vector<HighStmt> &Body) {
  // Every way a statement address is entered: gotos and __except handlers.
  std::map<va_t, unsigned> Uses;
  std::set<va_t> Pinned;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      ++Uses[S.GotoTarget];
    for (const HighEHClause &Clause : S.EHClauses)
      if (Clause.HandlerVA != 0 && Clause.HandlerVA != InvalidVA)
        Pinned.insert(Clause.HandlerVA);
  });
  auto usesOf = [&](va_t Addr) -> unsigned {
    if (Addr == 0 || Addr == InvalidVA)
      return 0;
    if (Pinned.count(Addr))
      return ~0u;
    auto It = Uses.find(Addr);
    return It == Uses.end() ? 0 : It->second;
  };
  auto isTerminator = [](const HighStmt &S) {
    return S.Kind == StmtKind::Return || S.Kind == StmtKind::Goto;
  };
  auto isCondGoto = [](const HighStmt &S) {
    return S.Kind == StmtKind::If && S.Cond && S.ElseBody.empty() &&
           S.Body.size() == 1 && S.Body[0].Kind == StmtKind::Goto &&
           S.Body[0].GotoTarget != 0 && S.Body[0].GotoTarget != InvalidVA;
  };
  // A statement starts an entered label when its address is referenced and
  // the previous statement belongs to another address.
  auto labelStart = [&](const std::vector<HighStmt> &L, size_t I) {
    return L[I].Addr != 0 && L[I].Addr != InvalidVA &&
           (I == 0 || L[I - 1].Addr != L[I].Addr) && usesOf(L[I].Addr) != 0;
  };

  // Reads of each variable anywhere in the function (assignment targets are
  // writes, not reads).
  std::unordered_map<VarKey, unsigned, VarKeyHash> Reads;
  std::function<void(const ExprPtr &)> CountReads = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (E->Kind == ExprKind::Var)
      ++Reads[varKey(E->Var)];
    for (const ExprPtr &Op : E->Operands)
      CountReads(Op);
  };
  walkStmts(Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (&E == &S.Dst && S.Kind == StmtKind::Assign && E &&
          E->Kind == ExprKind::Var)
        return;
      CountReads(E);
    });
  });
  // Replace the single read of \p Key in \p E with \p Value.
  std::function<bool(ExprPtr &, const VarKey &, const ExprPtr &)> Substitute =
      [&](ExprPtr &E, const VarKey &Key, const ExprPtr &Value) {
        if (!E)
          return false;
        if (E->Kind == ExprKind::Var && varKey(E->Var) == Key) {
          E = Value;
          return true;
        }
        for (ExprPtr &Op : E->Operands)
          if (Substitute(Op, Key, Value))
            return true;
        return false;
      };
  // A load or pure value with no call inside it.
  std::function<bool(const HighExpr &, unsigned)> Foldable =
      [&](const HighExpr &E, unsigned Depth) -> bool {
    if (Depth > 32 || E.Kind == ExprKind::Call || E.Kind == ExprKind::Store ||
        E.Kind == ExprKind::Phi || E.Kind == ExprKind::Undef)
      return false;
    if (E.Kind == ExprKind::BinOp &&
        (E.Op == NdOp::ATOMIC_ADD || E.Op == NdOp::ATOMIC_XCHG ||
         E.Op == NdOp::ATOMIC_CMPXCHG))
      return false;
    for (const ExprPtr &Op : E.Operands)
      if (!Op || !Foldable(*Op, Depth + 1))
        return false;
    return true;
  };

  // Remove the trailing goto of \p L.  A goto that itself carries an entered
  // label leaves an empty block behind so that label still exists.
  auto popGoto = [&](std::vector<HighStmt> &L) {
    HighStmt &G = L.back();
    if (G.Addr != 0 && G.Addr != InvalidVA && usesOf(G.Addr) != 0 &&
        (L.size() == 1 || L[L.size() - 2].Addr != G.Addr)) {
      HighStmt Anchor;
      Anchor.Kind = StmtKind::Block;
      Anchor.Addr = G.Addr;
      G = std::move(Anchor);
      return;
    }
    L.pop_back();
  };

  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit = [&](std::vector<HighStmt>
                                                               &L) {
    for (size_t I = 0; I < L.size(); ++I) {
      // `if (c) {} else { A }`  ->  `if (!c) { A }`.
      if (L[I].Kind == StmtKind::IfElse && L[I].Cond && L[I].Body.empty() &&
          !L[I].ElseBody.empty()) {
        L[I].Kind = StmtKind::If;
        L[I].Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, L[I].Cond);
        L[I].Body = std::move(L[I].ElseBody);
        L[I].ElseBody.clear();
      }
      // T11: `X: S...; goto X;` -> `while (1) { S... }` and
      // `X: S...; if (c) goto X;` -> `do { S... } while (c);`.
      if (L[I].Kind == StmtKind::Goto || isCondGoto(L[I])) {
        const bool Conditional = L[I].Kind == StmtKind::If;
        const va_t X = Conditional ? L[I].Body[0].GotoTarget : L[I].GotoTarget;
        size_t K = I;
        while (K > 0 && !(L[K - 1].Addr == X && labelStart(L, K - 1)))
          --K;
        if (K > 0 && !labelStart(L, I) &&
            (Conditional || L[I].Addr == 0 || L[I].Addr == InvalidVA ||
             L[I].Addr != L[I - 1].Addr)) {
          --K;
          std::vector<HighStmt> LoopBody(L.begin() + K, L.begin() + I);
          if (!LoopBody.empty() && !hasLooseBreakOrContinue(LoopBody)) {
            HighStmt Loop;
            Loop.Kind = Conditional ? StmtKind::DoWhile : StmtKind::While;
            Loop.Cond = Conditional ? L[I].Cond : HighExpr::makeConst(1, 1);
            Loop.LoopHeaderAddr = X;
            Loop.Body = std::move(LoopBody);
            --Uses[X];
            L.erase(L.begin() + K, L.begin() + I + 1);
            L.insert(L.begin() + K, std::move(Loop));
            I = K;
            Changed = true;
          }
        }
      }
      // T10: `if (a) { if (b) { S } }`  ->  `if (a && b) { S }`.
      while (L[I].Kind == StmtKind::If && L[I].Cond && L[I].ElseBody.empty() &&
             L[I].Body.size() == 1 && L[I].Body[0].Kind == StmtKind::If &&
             L[I].Body[0].Cond && L[I].Body[0].ElseBody.empty() &&
             (L[I].Body[0].Addr == 0 || L[I].Body[0].Addr == InvalidVA ||
              usesOf(L[I].Body[0].Addr) == 0)) {
        HighStmt Inner = std::move(L[I].Body[0]);
        ExprPtr Merged =
            HighExpr::makeBinop(NdOp::BOOL_AND, L[I].Cond, Inner.Cond);
        Merged->Type = NdType::makeInt(1, false);
        L[I].Cond = Merged;
        L[I].Body = std::move(Inner.Body);
        Changed = true;
      }
      // T0: a temporary read only by the next condition is evaluated at
      // exactly the same point when folded into it.
      if (I + 1 < L.size() && L[I].Kind == StmtKind::Assign &&
          !L[I].IsPhiCopy && L[I].Dst && L[I].Dst->Kind == ExprKind::Var &&
          L[I].Val && Foldable(*L[I].Val, 0) && !labelStart(L, I) &&
          !labelStart(L, I + 1) &&
          (L[I + 1].Kind == StmtKind::If ||
           L[I + 1].Kind == StmtKind::IfElse) &&
          L[I + 1].Cond) {
        const VarKey Key = varKey(L[I].Dst->Var);
        auto ReadIt = Reads.find(Key);
        unsigned CondReads = 0;
        std::function<void(const ExprPtr &)> CountKey = [&](const ExprPtr &E) {
          if (!E)
            return;
          if (E->Kind == ExprKind::Var && varKey(E->Var) == Key)
            ++CondReads;
          for (const ExprPtr &Op : E->Operands)
            CountKey(Op);
        };
        CountKey(L[I + 1].Cond);
        if (ReadIt != Reads.end() && ReadIt->second == 1 && CondReads == 1 &&
            Substitute(L[I + 1].Cond, Key, L[I].Val)) {
          ReadIt->second = 0;
          L.erase(L.begin() + I);
          Changed = true;
        }
      }
      // T3: merge a run of conditional jumps to one target.
      while (I + 1 < L.size() && isCondGoto(L[I]) && isCondGoto(L[I + 1]) &&
             L[I].Body[0].GotoTarget == L[I + 1].Body[0].GotoTarget &&
             !labelStart(L, I + 1)) {
        ExprPtr Merged =
            HighExpr::makeBinop(NdOp::BOOL_OR, L[I].Cond, L[I + 1].Cond);
        Merged->Type = NdType::makeInt(1, false);
        L[I].Cond = Merged;
        --Uses[L[I + 1].Body[0].GotoTarget];
        L.erase(L.begin() + I + 1);
        Changed = true;
      }
      // T4: consecutive ifs with the same straight-line body.
      while (I + 1 < L.size() && L[I].Kind == StmtKind::If && L[I].Cond &&
             L[I].ElseBody.empty() && L[I + 1].Kind == StmtKind::If &&
             L[I + 1].Cond && L[I + 1].ElseBody.empty() &&
             !labelStart(L, I + 1) && !L[I].Body.empty() &&
             sameStraightLineBody(L[I].Body, L[I + 1].Body)) {
        ExprPtr Merged =
            HighExpr::makeBinop(NdOp::BOOL_OR, L[I].Cond, L[I + 1].Cond);
        Merged->Type = NdType::makeInt(1, false);
        L[I].Cond = Merged;
        walkStmts(L[I + 1].Body, [&](const HighStmt &S) {
          if (S.Kind == StmtKind::Goto)
            --Uses[S.GotoTarget];
        });
        L.erase(L.begin() + I + 1);
        Changed = true;
      }
      // T2': any `if (c) { ...; goto Y; } S...; Y:` becomes if/else.
      if (L[I].Kind == StmtKind::If && L[I].Cond && L[I].ElseBody.empty() &&
          !L[I].Body.empty() && L[I].Body.back().Kind == StmtKind::Goto) {
        const va_t Y = L[I].Body.back().GotoTarget;
        size_t J = I + 1;
        while (J < L.size() && !(L[J].Addr == Y && labelStart(L, J)))
          ++J;
        // A label after a terminator starts an out-of-line block; moving
        // that block (T1) reads better than wrapping everything before it.
        if (J < L.size() && J > I + 1 && !isTerminator(L[J - 1])) {
          popGoto(L[I].Body);
          --Uses[Y];
          L[I].Kind = StmtKind::IfElse;
          L[I].ElseBody.assign(std::make_move_iterator(L.begin() + I + 1),
                               std::make_move_iterator(L.begin() + J));
          L.erase(L.begin() + I + 1, L.begin() + J);
          if (L[I].Body.empty()) {
            // `if (c) goto Y; S...; Y:`  ->  `if (!c) { S... }`.
            L[I].Kind = StmtKind::If;
            L[I].Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, L[I].Cond);
            L[I].Body = std::move(L[I].ElseBody);
            L[I].ElseBody.clear();
          }
          Changed = true;
          continue;
        }
      }
      // T9: `if (a) goto L; if (b) { L: S }`  ->  `if (a || b) { S }`.
      if (isCondGoto(L[I]) && I + 1 < L.size() &&
          L[I + 1].Kind == StmtKind::If && L[I + 1].Cond &&
          L[I + 1].ElseBody.empty() && !L[I + 1].Body.empty() &&
          !labelStart(L, I + 1) &&
          L[I + 1].Body.front().Addr == L[I].Body[0].GotoTarget) {
        --Uses[L[I].Body[0].GotoTarget];
        ExprPtr Merged =
            HighExpr::makeBinop(NdOp::BOOL_OR, L[I].Cond, L[I + 1].Cond);
        Merged->Type = NdType::makeInt(1, false);
        L[I + 1].Cond = Merged;
        L.erase(L.begin() + I);
        Changed = true;
        if (I > 0)
          --I;
        continue;
      }
      // T8: `if (c) { A; goto Y; } else { Y: B }`  ->  `if (c) { A }` B.
      if (L[I].Kind == StmtKind::IfElse && L[I].Cond && !L[I].Body.empty() &&
          L[I].Body.back().Kind == StmtKind::Goto && !L[I].ElseBody.empty() &&
          L[I].ElseBody.front().Addr == L[I].Body.back().GotoTarget &&
          L[I].Body.back().GotoTarget != 0 &&
          L[I].Body.back().GotoTarget != InvalidVA) {
        --Uses[L[I].Body.back().GotoTarget];
        popGoto(L[I].Body);
        std::vector<HighStmt> Tail = std::move(L[I].ElseBody);
        L[I].ElseBody.clear();
        L[I].Kind = StmtKind::If;
        L.insert(L.begin() + I + 1, std::make_move_iterator(Tail.begin()),
                 std::make_move_iterator(Tail.end()));
        Changed = true;
      }
      if (!isCondGoto(L[I]))
        continue;
      const va_t X = L[I].Body[0].GotoTarget;
      if (usesOf(X) != 1)
        continue;
      // T1: find X's block after this jump, entered only here.
      size_t K = I + 1;
      while (K < L.size() && !(L[K].Addr == X && labelStart(L, K)))
        ++K;
      if (K >= L.size() || K == I + 1 || !isTerminator(L[K - 1]))
        continue;
      size_t M = K;
      bool OtherEntry = false;
      while (M < L.size() && !isTerminator(L[M])) {
        if (M > K && labelStart(L, M) && L[M].Addr != X) {
          OtherEntry = true;
          break;
        }
        ++M;
      }
      if (OtherEntry || M >= L.size() ||
          (M > K && labelStart(L, M) && L[M].Addr != X))
        continue;
      std::vector<HighStmt> Block(std::make_move_iterator(L.begin() + K),
                                  std::make_move_iterator(L.begin() + M + 1));
      L.erase(L.begin() + K, L.begin() + M + 1);
      --Uses[X];
      L[I].Body = std::move(Block);
      Changed = true;

      // T2: the moved block rejoins at a label after this `if`.
      const HighStmt &Last = L[I].Body.back();
      if (Last.Kind != StmtKind::Goto)
        continue;
      const va_t Y = Last.GotoTarget;
      size_t J = I + 1;
      while (J < L.size() && !(L[J].Addr == Y && labelStart(L, J)))
        ++J;
      if (J >= L.size())
        continue;
      popGoto(L[I].Body);
      --Uses[Y];
      L[I].Kind = StmtKind::IfElse;
      L[I].ElseBody.assign(std::make_move_iterator(L.begin() + I + 1),
                           std::make_move_iterator(L.begin() + J));
      L.erase(L.begin() + I + 1, L.begin() + J);
      if (L[I].Body.empty()) {
        // `if (c) {} else { S }`  ->  `if (!c) { S }`.
        L[I].Kind = StmtKind::If;
        L[I].Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, L[I].Cond);
        L[I].Body = std::move(L[I].ElseBody);
        L[I].ElseBody.clear();
      }
    }
    for (HighStmt &S : L) {
      Visit(S.Body);
      Visit(S.ElseBody);
      for (auto &C : S.Cases)
        Visit(C.Body);
      Visit(S.DefaultBody);
      for (auto &ClauseBody : S.EHClauseBodies)
        Visit(ClauseBody);
    }
  };
  Visit(Body);

  // T1g: a block entered only by one goto anywhere in the tree, placed after
  // a terminator and ending in one, is spliced in place of that goto.  The
  // original statements become Nop.  Blocks never cross a __try boundary and
  // carry no break/continue whose target the move could change.
  struct Site {
    std::vector<HighStmt> *List = nullptr;
    size_t Index = 0;
    const HighStmt *Try = nullptr;
    // Every (list, index) enclosing the site, outermost first.
    std::vector<std::pair<const std::vector<HighStmt> *, size_t>> Chain;
  };
  auto prevNonNop = [](const std::vector<HighStmt> &L,
                       size_t K) -> const HighStmt * {
    while (K > 0) {
      --K;
      if (L[K].Kind != StmtKind::Nop)
        return &L[K];
    }
    return nullptr;
  };
  for (unsigned Applied = 0; Applied < 512; ++Applied) {
    std::map<va_t, Site> Labels;
    std::vector<std::pair<Site, va_t>> Gotos;
    std::vector<std::pair<const std::vector<HighStmt> *, size_t>> Chain;
    std::function<void(std::vector<HighStmt> &, const HighStmt *)> Collect =
        [&](std::vector<HighStmt> &L, const HighStmt *Try) {
          for (size_t I = 0; I < L.size(); ++I) {
            HighStmt &S = L[I];
            if (labelStart(L, I) && S.Kind != StmtKind::Nop)
              Labels.emplace(S.Addr, Site{&L, I, Try, Chain});
            if (S.Kind == StmtKind::Goto && usesOf(S.GotoTarget) == 1)
              Gotos.push_back({Site{&L, I, Try, Chain}, S.GotoTarget});
            Chain.push_back({&L, I});
            const HighStmt *Inner = S.Kind == StmtKind::SEHTry ? &S : Try;
            Collect(S.Body, Inner);
            Collect(S.ElseBody, Try);
            for (auto &C : S.Cases)
              Collect(C.Body, Try);
            Collect(S.DefaultBody, Try);
            for (auto &ClauseBody : S.EHClauseBodies)
              Collect(ClauseBody, nullptr);
            Chain.pop_back();
          }
        };
    Collect(Body, nullptr);
    bool Done = false;
    for (auto &[GotoSite, X] : Gotos) {
      auto LabelIt = Labels.find(X);
      if (LabelIt == Labels.end())
        continue;
      Site &Block = LabelIt->second;
      std::vector<HighStmt> &LL = *Block.List;
      const size_t K = Block.Index;
      const HighStmt *Prev = prevNonNop(LL, K);
      if (!Prev || !isTerminator(*Prev) || Block.Try != GotoSite.Try)
        continue;
      size_t M = K;
      bool Bad = false;
      while (M < LL.size() && !isTerminator(LL[M])) {
        if (M > K && labelStart(LL, M))
          Bad = true;
        ++M;
      }
      if (Bad || M >= LL.size() || (M > K && labelStart(LL, M)))
        continue;
      // The goto must not sit inside the block it would receive.
      for (const auto &[List, Index] : GotoSite.Chain)
        if (List == &LL && Index >= K && Index <= M)
          Bad = true;
      if (GotoSite.List == &LL && GotoSite.Index >= K && GotoSite.Index <= M)
        Bad = true;
      std::vector<HighStmt> Moved(LL.begin() + K, LL.begin() + M + 1);
      walkStmts(Moved, [&](const HighStmt &S) {
        Bad |= S.Kind == StmtKind::Break || S.Kind == StmtKind::Continue ||
               S.Kind == StmtKind::SEHTry;
      });
      if (Bad)
        continue;
      for (size_t J = K; J <= M; ++J) {
        HighStmt Removed;
        Removed.Kind = StmtKind::Nop;
        LL[J] = std::move(Removed);
      }
      std::vector<HighStmt> &GL = *GotoSite.List;
      // A goto carrying an entered label keeps it as an empty block.
      HighStmt &G = GL[GotoSite.Index];
      size_t InsertAt = GotoSite.Index;
      if (labelStart(GL, GotoSite.Index)) {
        HighStmt Anchor;
        Anchor.Kind = StmtKind::Block;
        Anchor.Addr = G.Addr;
        G = std::move(Anchor);
        ++InsertAt;
      } else {
        GL.erase(GL.begin() + GotoSite.Index);
      }
      GL.insert(GL.begin() + InsertAt, std::make_move_iterator(Moved.begin()),
                std::make_move_iterator(Moved.end()));
      --Uses[X];
      Changed = true;
      Done = true;
      break;
    }
    if (!Done)
      break;
  }

  // T5: a goto ending an if/else/block body whose target is exactly the
  // statement that runs next after that construct is a fall-through.
  // T6: `if (c) { ...; goto F; } rest...` where F follows the whole list
  // becomes `if (c) { ... } else { rest... }`: the rest flows to F as well.
  std::function<void(std::vector<HighStmt> &, va_t)> DropFallthrough =
      [&](std::vector<HighStmt> &L, va_t Follow) {
        if (Follow != 0 && Follow != InvalidVA)
          for (size_t I = 0; I + 1 < L.size(); ++I)
            if (L[I].Kind == StmtKind::If && L[I].Cond &&
                L[I].ElseBody.empty() && L[I].Body.size() >= 2 &&
                L[I].Body.back().Kind == StmtKind::Goto &&
                L[I].Body.back().GotoTarget == Follow) {
              popGoto(L[I].Body);
              --Uses[Follow];
              L[I].Kind = StmtKind::IfElse;
              L[I].ElseBody.assign(std::make_move_iterator(L.begin() + I + 1),
                                   std::make_move_iterator(L.end()));
              L.erase(L.begin() + I + 1, L.end());
              Changed = true;
              break;
            }
        if (!L.empty() && L.back().Kind == StmtKind::Goto && Follow != 0 &&
            Follow != InvalidVA && L.back().GotoTarget == Follow) {
          --Uses[Follow];
          popGoto(L);
          Changed = true;
        }
        for (size_t I = 0; I < L.size(); ++I) {
          // The next statement starts a label only when it begins its
          // address group; otherwise a goto to that address lands earlier.
          va_t Next = Follow;
          if (I + 1 < L.size())
            Next = L[I + 1].Addr != L[I].Addr ? L[I + 1].Addr : InvalidVA;
          HighStmt &S = L[I];
          switch (S.Kind) {
          case StmtKind::If:
          case StmtKind::IfElse:
          case StmtKind::Block:
            DropFallthrough(S.Body, Next);
            DropFallthrough(S.ElseBody, Next);
            break;
          default:
            DropFallthrough(S.Body, InvalidVA);
            DropFallthrough(S.ElseBody, InvalidVA);
            for (auto &C : S.Cases)
              DropFallthrough(C.Body, InvalidVA);
            DropFallthrough(S.DefaultBody, InvalidVA);
            for (auto &ClauseBody : S.EHClauseBodies)
              DropFallthrough(ClauseBody, InvalidVA);
            break;
          }
        }
      };
  DropFallthrough(Body, InvalidVA);
  return Changed;
}

void MedToHighConverter::simplifyControlFlow(HighFunc &Func,
                                             const MedFunc &Med) {
  const bool IsMega = Func.Body.size() > limits::kMaxStructuredHighStmts;

  std::unordered_map<va_t, int> AddrToBlock;
  AddrToBlock.reserve(Med.Blocks.size());
  for (auto &Block : Med.Blocks)
    if (!Block.Ops.empty())
      AddrToBlock[Block.Ops.front().Addr] = Block.Id;

  detectAndConvertLoops(Func, AddrToBlock, Med, IsMega);

  if (!IsMega)
    recoverSwitchStatements(Func);

  int IfElseMaxPasses =
      IsMega ? 0
             : (Med.Blocks.size() > limits::kMaxIfElseStructuringBlocks)
                   ? limits::kIfElseLargeCfgPasses
                   : limits::kIfElseStructuringPasses;
  structureIfElse(Func, IfElseMaxPasses, &Med);

  removeTrivialGotos(Func.Body, gotoTargets(Func.Body));
  simplifyNestedGotos(Func.Body);
  seedIfThenJoinInits(Func.Body);

  mergeConsecutiveCondBlocks(Func.Body);
  inlineGotoReturns(Func, Med);
  cleanupGuardBeforeSwitch(Func);

  Func.Body.erase(
      std::remove_if(Func.Body.begin(), Func.Body.end(),
                     [](const HighStmt &S) { return S.Kind == StmtKind::Nop; }),
      Func.Body.end());
}
} // namespace neverd
