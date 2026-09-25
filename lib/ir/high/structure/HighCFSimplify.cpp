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
    // Only register bookkeeping (epilogue restores, stack adjustment) may
    // sit beside the return value: a goto replaced by `return v` must not
    // skip a store or call, or a load that computes v itself.
    bool OnlyBookkeeping = true;
    for (size_t J = 0; J + 1 < MedBlock.Ops.size(); ++J) {
      const MedOp &Op = MedBlock.Ops[J];
      OnlyBookkeeping &=
          Op.Output.Kind == MedVar::Reg && Op.Opcode != NdOp::STORE &&
          Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL &&
          Op.Opcode != NdOp::INTRINSIC && Op.Opcode != NdOp::ATOMIC_XCHG &&
          Op.Opcode != NdOp::ATOMIC_ADD && Op.Opcode != NdOp::ATOMIC_CMPXCHG;
    }
    if (!OnlyBookkeeping)
      continue;
    for (int J = static_cast<int>(MedBlock.Ops.size()) - 2; J >= 0; --J) {
      auto &CurrOp = MedBlock.Ops[J];
      if (CurrOp.Output.Kind == MedVar::Reg && CurrOp.Output.RegOff == 0) {
        const bool DefinedHere = std::any_of(
            MedBlock.Ops.begin(), MedBlock.Ops.end(), [&](const MedOp &Op) {
              return CurrOp.NumInputs >= 1 && !Op.Output.isConst() &&
                     Op.Output == CurrOp.Inputs[0];
            });
        if ((CurrOp.Opcode == NdOp::COPY || CurrOp.Opcode == NdOp::INT_ZEXT) &&
            CurrOp.NumInputs >= 1 && !DefinedHere)
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
/// only such assignments and removed statements); nullopt otherwise.  A
/// block member may carry its own address only when no jump targets it.
static std::optional<size_t> pureAssignCount(const HighStmt &S,
                                             const std::set<va_t> &Targets) {
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
    if (Child.Addr != 0 && Child.Addr != S.Addr && Targets.count(Child.Addr))
      return std::nullopt;
    std::optional<size_t> ChildCount = pureAssignCount(Child, Targets);
    if (!ChildCount || (Child.Kind == StmtKind::Block && !Child.Body.empty()))
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
  std::set<va_t> Targets;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto)
      Targets.insert(S.GotoTarget);
  });
  std::map<va_t, std::vector<HighStmt>> Tails;
  // The tail that runs from Stmts[I]: a few pure assignments ending in a
  // return, in a jump to a known tail, or at the end of the list followed
  // by \p Cont (what runs after the construct owning this list).
  auto TailAt = [&](const std::vector<HighStmt> &Stmts, size_t I,
                    const std::vector<HighStmt> *Cont)
      -> std::optional<std::vector<HighStmt>> {
    // An empty block can anchor the label ahead of the tail.
    // Removed statements (Nop) print nothing and are skipped.
    size_t First = I;
    while (First < Stmts.size() && ((Stmts[First].Kind == StmtKind::Block &&
                                     Stmts[First].Body.empty()) ||
                                    Stmts[First].Kind == StmtKind::Nop))
      ++First;
    size_t J = First;
    size_t Assigns = 0;
    while (J < Stmts.size()) {
      std::optional<size_t> Count = pureAssignCount(Stmts[J], Targets);
      if (!Count || Assigns + *Count > kMaxTailAssigns)
        break;
      Assigns += *Count;
      ++J;
    }
    if (J < Stmts.size() && Stmts[J].Kind == StmtKind::Return &&
        (!Stmts[J].RetVal || isTailValue(*Stmts[J].RetVal)))
      return std::vector<HighStmt>(Stmts.begin() + First,
                                   Stmts.begin() + J + 1);
    const std::vector<HighStmt> *Rest = nullptr;
    if (J < Stmts.size() && Stmts[J].Kind == StmtKind::Goto &&
        Stmts[J].GotoTarget != Stmts[I].Addr) {
      // Edge copies ahead of a jump to a shared return epilogue: the
      // tail is those copies followed by the epilogue's own tail.
      auto Target = Tails.find(Stmts[J].GotoTarget);
      if (Target != Tails.end())
        Rest = &Target->second;
    } else if (J == Stmts.size()) {
      Rest = Cont;
    }
    if (!Rest || Assigns + Rest->size() > kMaxComposedTail)
      return std::nullopt;
    std::vector<HighStmt> Tail(Stmts.begin() + First, Stmts.begin() + J);
    Tail.insert(Tail.end(), Rest->begin(), Rest->end());
    return Tail;
  };
  std::function<void(std::vector<HighStmt> &, const std::vector<HighStmt> *)>
      Collect = [&](std::vector<HighStmt> &Stmts,
                    const std::vector<HighStmt> *Cont) {
        for (size_t I = 0; I < Stmts.size(); ++I) {
          const va_t Label = Stmts[I].Addr;
          if (Label != 0 && Label != InvalidVA && !Tails.count(Label) &&
              (I == 0 || Stmts[I - 1].Addr != Label))
            if (auto Tail = TailAt(Stmts, I, Cont))
              Tails.emplace(Label, std::move(*Tail));
          // Falling off an if/else arm or block continues after it.
          std::optional<std::vector<HighStmt>> After;
          const StmtKind K = Stmts[I].Kind;
          if (K == StmtKind::If || K == StmtKind::IfElse ||
              K == StmtKind::Block)
            After = I + 1 < Stmts.size()
                        ? TailAt(Stmts, I + 1, Cont)
                        : (Cont ? std::optional(*Cont) : std::nullopt);
          const std::vector<HighStmt> *ChildCont = After ? &*After : nullptr;
          Collect(Stmts[I].Body, ChildCont);
          Collect(Stmts[I].ElseBody, ChildCont);
          for (auto &C : Stmts[I].Cases)
            Collect(C.Body, nullptr);
          Collect(Stmts[I].DefaultBody, nullptr);
        }
      };
  // A composed tail needs its epilogue's tail first; the epilogue usually
  // follows the jumps to it, so repeat until no new tail appears.
  for (size_t Round = 0; Round < 4; ++Round) {
    const size_t Before = Tails.size();
    Collect(Body, nullptr);
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
              const va_t Site = Stmts[I].Addr;
              for (HighStmt &C : Copy)
                C.Addr = Site;
              walkStmts(Copy, [&](HighStmt &C) {
                if (C.Addr != 0)
                  C.Addr = Site;
              });
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

bool loopifyBackwardGotos(std::vector<HighStmt> &Body) {
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
  // Gotos to \p X inside \p S, or -1 when one sits in a nested loop (where
  // `continue` would bind to that loop).
  std::function<int(const HighStmt &, va_t, bool)> Count =
      [&](const HighStmt &S, va_t X, bool InLoop) -> int {
    if (S.Kind == StmtKind::Goto && S.GotoTarget == X)
      return InLoop ? -1 : 1;
    const bool Loop = S.Kind == StmtKind::While ||
                      S.Kind == StmtKind::DoWhile || S.Kind == StmtKind::For;
    int N = 0;
    auto Add = [&](const std::vector<HighStmt> &L) {
      for (const HighStmt &C : L) {
        if (N < 0)
          return;
        const int M = Count(C, X, InLoop || Loop);
        N = M < 0 ? -1 : N + M;
      }
    };
    Add(S.Body);
    Add(S.ElseBody);
    for (const auto &C : S.Cases)
      Add(C.Body);
    Add(S.DefaultBody);
    for (const auto &ClauseBody : S.EHClauseBodies)
      Add(ClauseBody);
    return N;
  };
  std::function<void(std::vector<HighStmt> &, va_t)> ToContinue =
      [&](std::vector<HighStmt> &L, va_t X) {
        for (HighStmt &S : L) {
          if (S.Kind == StmtKind::Goto && S.GotoTarget == X) {
            S.Kind = StmtKind::Continue;
            S.GotoTarget = 0;
            continue;
          }
          ToContinue(S.Body, X);
          ToContinue(S.ElseBody, X);
          for (auto &C : S.Cases)
            ToContinue(C.Body, X);
          ToContinue(S.DefaultBody, X);
          for (auto &ClauseBody : S.EHClauseBodies)
            ToContinue(ClauseBody, X);
        }
      };
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (size_t K = 0; K < L.size(); ++K) {
          const va_t X = L[K].Addr;
          if (X == 0 || X == InvalidVA || Pinned.count(X) ||
              (K > 0 && L[K - 1].Addr == X) || !Uses.count(X))
            continue;
          // Every jump to X must come from K onward in this list.
          size_t M = K;
          int Inside = 0;
          bool Bad = false;
          for (size_t J = K; J < L.size() && !Bad; ++J) {
            const int N = Count(L[J], X, false);
            if (N < 0)
              Bad = true;
            else if (N > 0) {
              Inside += N;
              M = J;
            }
          }
          if (Bad || Inside == 0 || static_cast<unsigned>(Inside) != Uses[X])
            continue;
          std::vector<HighStmt> Region(
              std::make_move_iterator(L.begin() + K),
              std::make_move_iterator(L.begin() + M + 1));
          bool HasTry = false;
          walkStmts(Region, [&](const HighStmt &S) {
            HasTry |= S.Kind == StmtKind::SEHTry ||
                      S.Kind == StmtKind::CxxTry ||
                      S.Kind == StmtKind::ItaniumTry;
          });
          if (HasTry || hasLooseBreakOrContinue(Region)) {
            std::move(Region.begin(), Region.end(), L.begin() + K);
            continue;
          }
          ToContinue(Region, X);
          const StmtKind LastKind = Region.back().Kind;
          if (LastKind != StmtKind::Return && LastKind != StmtKind::Goto &&
              LastKind != StmtKind::Continue) {
            HighStmt Break;
            Break.Kind = StmtKind::Break;
            Region.push_back(std::move(Break));
          }
          HighStmt Loop;
          Loop.Kind = StmtKind::While;
          Loop.Body = std::move(Region);
          L.erase(L.begin() + K + 1, L.begin() + M + 1);
          L[K] = std::move(Loop);
          Uses.erase(X);
          Changed = true;
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
  return Changed;
}

bool hoistLoopEntryLabels(std::vector<HighStmt> &Body) {
  std::map<va_t, unsigned> Uses;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      ++Uses[S.GotoTarget];
    for (const HighEHClause &Clause : S.EHClauses)
      if (Clause.HandlerVA != 0 && Clause.HandlerVA != InvalidVA)
        Uses[Clause.HandlerVA] = ~0u;
  });
  bool Changed = false;
  walkStmts(Body, [&](HighStmt &S) {
    // Entering `while (1)` at the top of its body is entering the loop.
    const bool Forever =
        !S.Cond || (S.Cond->Kind == ExprKind::Const && S.Cond->ConstVal != 0);
    if (S.Kind != StmtKind::While || !Forever || S.Body.empty() ||
        (S.Addr != 0 && S.Addr != InvalidVA))
      return;
    const va_t X = S.Body.front().Addr;
    auto It = Uses.find(X);
    if (X == 0 || X == InvalidVA || It == Uses.end() || It->second == ~0u)
      return;
    unsigned Inside = 0;
    walkStmts(S.Body, [&](const HighStmt &C) {
      Inside += C.Kind == StmtKind::Goto && C.GotoTarget == X;
    });
    if (Inside != 0)
      return;
    for (HighStmt &C : S.Body) {
      if (C.Addr != X)
        break;
      C.Addr = 0;
    }
    S.Addr = X;
    Changed = true;
  });
  return Changed;
}

bool groupSwitchCases(std::vector<HighStmt> &Body) {
  std::map<va_t, unsigned> Uses;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      ++Uses[S.GotoTarget];
    for (const HighEHClause &Clause : S.EHClauses)
      if (Clause.HandlerVA != 0 && Clause.HandlerVA != InvalidVA)
        Uses[Clause.HandlerVA] = ~0u;
  });
  auto labeled = [&](const HighStmt &S) {
    return S.Addr != 0 && S.Addr != InvalidVA && Uses.count(S.Addr) != 0;
  };
  // `goto Next` that leaves a case for the statement right after the switch
  // is a `break`.  Loops and nested switches own their own `break`.
  std::function<bool(std::vector<HighStmt> &, va_t)> GotoNextToBreak =
      [&](std::vector<HighStmt> &L, va_t Next) {
        bool Changed = false;
        for (HighStmt &S : L) {
          if (S.Kind == StmtKind::Goto && S.GotoTarget == Next) {
            if (--Uses[Next] == 0)
              Uses.erase(Next);
            S.Kind = StmtKind::Break;
            S.GotoTarget = 0;
            Changed = true;
          } else if (S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse ||
                     S.Kind == StmtKind::Block) {
            Changed |= GotoNextToBreak(S.Body, Next);
            Changed |= GotoNextToBreak(S.ElseBody, Next);
          }
        }
        return Changed;
      };
  // Where a case body goes: a lone goto's target, or 0 for leaving the
  // switch.  Bodies with any other content, or whose statement is itself a
  // label, have no exit key.
  constexpr va_t kLeave = 0;
  auto exitOf = [&](const std::vector<HighStmt> &L) -> std::optional<va_t> {
    if (L.empty())
      return kLeave;
    if (L.size() != 1 || labeled(L[0]))
      return std::nullopt;
    if (L[0].Kind == StmtKind::Break)
      return kLeave;
    if (L[0].Kind == StmtKind::Goto && L[0].GotoTarget != 0 &&
        L[0].GotoTarget != InvalidVA)
      return L[0].GotoTarget;
    return std::nullopt;
  };

  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (size_t I = 0; I < L.size(); ++I) {
          HighStmt &S = L[I];
          Visit(S.Body);
          Visit(S.ElseBody);
          for (SwitchCase &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody);
          if (S.Kind != StmtKind::Switch || S.Cases.empty())
            continue;
          if (I + 1 < L.size() && L[I + 1].Addr != 0 &&
              L[I + 1].Addr != InvalidVA && L[I + 1].Addr != S.Addr) {
            const va_t Next = L[I + 1].Addr;
            for (SwitchCase &C : S.Cases)
              Changed |= GotoNextToBreak(C.Body, Next);
            Changed |= GotoNextToBreak(S.DefaultBody, Next);
          }
          // Units: a run of fall-through cases and the case owning the body.
          std::vector<std::vector<SwitchCase>> Units;
          std::vector<SwitchCase> Cur;
          for (SwitchCase &C : S.Cases) {
            Cur.push_back(std::move(C));
            if (!Cur.back().FallsThrough) {
              Units.push_back(std::move(Cur));
              Cur.clear();
            }
          }
          if (!Cur.empty())
            Units.push_back(std::move(Cur));
          // Two case bodies are interchangeable when they leave for the
          // same place or are the same short label-free statement list.
          auto sameBody = [&](const std::vector<HighStmt> &A,
                              const std::vector<HighStmt> &B) {
            const std::optional<va_t> EA = exitOf(A), EB = exitOf(B);
            if (EA || EB)
              return EA == EB;
            if (A.size() > 8 || !sameStraightLineBody(A, B))
              return false;
            for (const HighStmt &X : A)
              if (labeled(X))
                return false;
            for (const HighStmt &X : B)
              if (labeled(X))
                return false;
            return true;
          };
          auto hasBody = [&](size_t U) {
            return !Units[U].back().FallsThrough;
          };
          auto dropBody = [&](std::vector<HighStmt> &L) {
            for (const HighStmt &X : L)
              if (X.Kind == StmtKind::Goto && X.GotoTarget != 0 &&
                  X.GotoTarget != InvalidVA && --Uses[X.GotoTarget] == 0)
                Uses.erase(X.GotoTarget);
            L.clear();
          };
          // A case that does what an explicit `default` does adds nothing.
          // Without one, a case that just leaves keeps its recovered label.
          std::vector<bool> Dropped(Units.size(), false);
          for (size_t U = 0; U < Units.size(); ++U)
            if (!S.DefaultBody.empty() && hasBody(U) &&
                sameBody(Units[U].back().Body, S.DefaultBody)) {
              Dropped[U] = true;
              dropBody(Units[U].back().Body);
              Changed = true;
            }
          // Cases with the same destination share one body.
          std::vector<SwitchCase> NewCases;
          std::vector<bool> Emitted(Units.size(), false);
          for (size_t U = 0; U < Units.size(); ++U) {
            if (Dropped[U] || Emitted[U])
              continue;
            std::vector<size_t> Group{U};
            if (hasBody(U))
              for (size_t V = U + 1; V < Units.size(); ++V)
                if (!Dropped[V] && !Emitted[V] && hasBody(V) &&
                    sameBody(Units[V].back().Body, Units[U].back().Body))
                  Group.push_back(V);
            for (size_t G = 0; G < Group.size(); ++G) {
              Emitted[Group[G]] = true;
              auto &Unit = Units[Group[G]];
              if (G + 1 < Group.size()) {
                dropBody(Unit.back().Body);
                Unit.back().FallsThrough = true;
                Changed = true;
              }
              for (SwitchCase &C : Unit)
                NewCases.push_back(std::move(C));
            }
          }
          S.Cases = std::move(NewCases);
        }
      };
  Visit(Body);
  return Changed;
}

bool reduceSingleUseGotos(std::vector<HighStmt> &Body, bool SpliceRegions) {
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
    // An atomic is still evaluated exactly once, at the same point.
    if (Depth > 32 || E.Kind == ExprKind::Call || E.Kind == ExprKind::Store ||
        E.Kind == ExprKind::Phi || E.Kind == ExprKind::Undef)
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

  // Placeholders left by an earlier block splice would hide the statement
  // a rewrite inspects (the label opening an else, the next statement).
  std::function<void(std::vector<HighStmt> &)> DropPlaceholders =
      [&](std::vector<HighStmt> &L) {
        L.erase(std::remove_if(L.begin(), L.end(),
                               [&](const HighStmt &S) {
                                 return S.Kind == StmtKind::Nop &&
                                        usesOf(S.Addr) == 0;
                               }),
                L.end());
        for (HighStmt &S : L) {
          DropPlaceholders(S.Body);
          DropPlaceholders(S.ElseBody);
          for (auto &C : S.Cases)
            DropPlaceholders(C.Body);
          DropPlaceholders(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            DropPlaceholders(ClauseBody);
        }
      };
  DropPlaceholders(Body);

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
        // Once the local rewrites have settled (SpliceRegions), a jump that
        // T1 could not absorb still becomes an if/else.
        if (J < L.size() && J > I + 1 &&
            (!isTerminator(L[J - 1]) || SpliceRegions)) {
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
      // T12: `goto X; do { S...; X: } while (c);`  ->  `while (c) { S... }`.
      if (L[I].Kind == StmtKind::Goto && I + 1 < L.size() &&
          L[I + 1].Kind == StmtKind::DoWhile && L[I + 1].Cond &&
          usesOf(L[I].GotoTarget) == 1 && !L[I + 1].Body.empty()) {
        auto &Loop = L[I + 1].Body;
        const HighStmt &Last = Loop.back();
        if (Last.Kind == StmtKind::Block && Last.Body.empty() &&
            Last.Addr == L[I].GotoTarget &&
            (Loop.size() == 1 || Loop[Loop.size() - 2].Addr != Last.Addr) &&
            !(L[I].Addr != 0 && L[I].Addr != InvalidVA &&
              usesOf(L[I].Addr) != 0)) {
          --Uses[L[I].GotoTarget];
          Loop.pop_back();
          L[I + 1].Kind = StmtKind::While;
          L.erase(L.begin() + I);
          Changed = true;
          continue;
        }
      }
      // T12b: `goto X; do { A...; X: B... } while (c);`  ->
      // `while (1) { B...; if (!c) break; A... }`.
      if (L[I].Kind == StmtKind::Goto && I + 1 < L.size() &&
          L[I + 1].Kind == StmtKind::DoWhile && L[I + 1].Cond &&
          usesOf(L[I].GotoTarget) == 1 &&
          !(L[I].Addr != 0 && L[I].Addr != InvalidVA &&
            usesOf(L[I].Addr) != 0) &&
          !hasLooseBreakOrContinue(L[I + 1].Body)) {
        auto &Loop = L[I + 1].Body;
        size_t K = 1;
        while (K < Loop.size() && !(Loop[K].Addr == L[I].GotoTarget &&
                                    Loop[K - 1].Addr != Loop[K].Addr))
          ++K;
        if (K < Loop.size()) {
          std::vector<HighStmt> Rotated(
              std::make_move_iterator(Loop.begin() + K),
              std::make_move_iterator(Loop.end()));
          HighStmt Exit;
          Exit.Kind = StmtKind::If;
          Exit.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, L[I + 1].Cond);
          HighStmt Break;
          Break.Kind = StmtKind::Break;
          Exit.Body.push_back(std::move(Break));
          Rotated.push_back(std::move(Exit));
          Rotated.insert(Rotated.end(), std::make_move_iterator(Loop.begin()),
                         std::make_move_iterator(Loop.begin() + K));
          Loop = std::move(Rotated);
          L[I + 1].Kind = StmtKind::While;
          L[I + 1].Cond = nullptr;
          --Uses[L[I].GotoTarget];
          L.erase(L.begin() + I);
          Changed = true;
          continue;
        }
      }
      // T13: `if (c) { ...; return; } S...; Y:` where the arm jumps to Y:
      // the arm never falls through, so S can be its else, after which the
      // jumps to Y are fall-through exits of the if.
      if (SpliceRegions && L[I].Kind == StmtKind::If && L[I].Cond &&
          L[I].ElseBody.empty() && !L[I].Body.empty() &&
          isTerminator(L[I].Body.back())) {
        std::set<va_t> ArmTargets;
        walkStmts(L[I].Body, [&](const HighStmt &S) {
          if (S.Kind == StmtKind::Goto)
            ArmTargets.insert(S.GotoTarget);
        });
        if (L[I].Body.back().Kind == StmtKind::Goto)
          ArmTargets.erase(L[I].Body.back().GotoTarget);
        size_t J = I + 1;
        while (J < L.size() &&
               !(labelStart(L, J) && ArmTargets.count(L[J].Addr)))
          ++J;
        if (J < L.size() && J > I + 1) {
          L[I].Kind = StmtKind::IfElse;
          L[I].ElseBody.assign(std::make_move_iterator(L.begin() + I + 1),
                               std::make_move_iterator(L.begin() + J));
          L.erase(L.begin() + I + 1, L.begin() + J);
          Changed = true;
        }
      }
      // T8: `if (c) { A; goto Y; } else { B; Y: C }`
      //   ->  `if (c) { A } else { B }` C.
      if (L[I].Kind == StmtKind::IfElse && L[I].Cond && !L[I].Body.empty() &&
          L[I].Body.back().Kind == StmtKind::Goto && !L[I].ElseBody.empty() &&
          L[I].Body.back().GotoTarget != 0 &&
          L[I].Body.back().GotoTarget != InvalidVA) {
        const va_t Y = L[I].Body.back().GotoTarget;
        auto &Else = L[I].ElseBody;
        size_t J = 0;
        while (J < Else.size() &&
               !(Else[J].Addr == Y && (J == 0 || Else[J - 1].Addr != Y)))
          ++J;
        if (J < Else.size()) {
          --Uses[Y];
          popGoto(L[I].Body);
          std::vector<HighStmt> Tail(std::make_move_iterator(Else.begin() + J),
                                     std::make_move_iterator(Else.end()));
          Else.erase(Else.begin() + J, Else.end());
          if (Else.empty())
            L[I].Kind = StmtKind::If;
          else if (L[I].Body.empty()) {
            L[I].Kind = StmtKind::If;
            L[I].Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, L[I].Cond);
            L[I].Body = std::move(Else);
            Else.clear();
          }
          L.insert(L.begin() + I + 1, std::make_move_iterator(Tail.begin()),
                   std::make_move_iterator(Tail.end()));
          Changed = true;
        }
      }
      // T14 (late): `if (c) { ...; jump; } else { S... }`  ->
      // `if (c) { ...; jump; }` S...: the arm never reaches S.
      if (SpliceRegions && L[I].Kind == StmtKind::IfElse && L[I].Cond &&
          !L[I].Body.empty() && !L[I].ElseBody.empty() &&
          (isTerminator(L[I].Body.back()) ||
           L[I].Body.back().Kind == StmtKind::Break ||
           L[I].Body.back().Kind == StmtKind::Continue)) {
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
  // End of the region starting at the label at \p K: the first terminator
  // followed by a label (or the list end) such that every label starting
  // after K up to it is entered only by gotos inside the region.
  auto regionEnd = [&](const std::vector<HighStmt> &LL,
                       size_t K) -> std::optional<size_t> {
    constexpr size_t kMaxRegionStmts = 256;
    std::map<va_t, unsigned> Internal;
    std::set<va_t> Inner;
    for (size_t M = K; M < LL.size() && M - K < kMaxRegionStmts; ++M) {
      if (M > K && labelStart(LL, M)) {
        if (usesOf(LL[M].Addr) == ~0u)
          return std::nullopt;
        Inner.insert(LL[M].Addr);
      }
      std::function<void(const HighStmt &)> CountGotos =
          [&](const HighStmt &S) {
            if (S.Kind == StmtKind::Goto)
              ++Internal[S.GotoTarget];
          };
      CountGotos(LL[M]);
      walkStmts(LL[M].Body, CountGotos);
      walkStmts(LL[M].ElseBody, CountGotos);
      for (const auto &C : LL[M].Cases)
        walkStmts(C.Body, CountGotos);
      walkStmts(LL[M].DefaultBody, CountGotos);
      for (const auto &ClauseBody : LL[M].EHClauseBodies)
        walkStmts(ClauseBody, CountGotos);
      if (!isTerminator(LL[M]) ||
          !(M + 1 == LL.size() || labelStart(LL, M + 1)))
        continue;
      bool Closed = true;
      for (va_t Label : Inner)
        if (Internal[Label] != usesOf(Label)) {
          Closed = false;
          break;
        }
      if (Closed)
        return M;
    }
    return std::nullopt;
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
      if (Bad || M >= LL.size() || (M > K && labelStart(LL, M))) {
        // A longer region also qualifies when every label starting inside
        // it is entered only from inside it: the whole region then moves
        // as one unit and its internal jumps stay internal.
        std::optional<size_t> End =
            SpliceRegions ? regionEnd(LL, K) : std::nullopt;
        if (!End)
          continue;
        M = *End;
        Bad = false;
      }
      // The goto must not sit inside the block it would receive.
      for (const auto &[List, Index] : GotoSite.Chain)
        if (List == &LL && Index >= K && Index <= M)
          Bad = true;
      if (GotoSite.List == &LL && GotoSite.Index >= K && GotoSite.Index <= M)
        Bad = true;
      std::vector<HighStmt> Moved(LL.begin() + K, LL.begin() + M + 1);
      Bad |= hasLooseBreakOrContinue(Moved);
      walkStmts(Moved,
                [&](const HighStmt &S) { Bad |= S.Kind == StmtKind::SEHTry; });
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
                L[I].ElseBody.empty() && !L[I].Body.empty() &&
                L[I].Body.back().Kind == StmtKind::Goto &&
                L[I].Body.back().GotoTarget == Follow) {
              popGoto(L[I].Body);
              --Uses[Follow];
              L[I].Kind = StmtKind::IfElse;
              L[I].ElseBody.assign(std::make_move_iterator(L.begin() + I + 1),
                                   std::make_move_iterator(L.end()));
              L.erase(L.begin() + I + 1, L.end());
              if (L[I].Body.empty()) {
                // `if (c) goto F; rest...`  ->  `if (!c) { rest... }`.
                L[I].Kind = StmtKind::If;
                L[I].Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, L[I].Cond);
                L[I].Body = std::move(L[I].ElseBody);
                L[I].ElseBody.clear();
              }
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
