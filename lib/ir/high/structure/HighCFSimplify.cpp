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
#include "neverd/support/Diagnostic.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>

namespace neverd {

//===----------------------------------------------------------------------===//
// Trivial goto removal (goto to next statement)
//===----------------------------------------------------------------------===//

static void removeTrivialGotos(std::vector<HighStmt> &Stmts) {
  for (int I = static_cast<int>(Stmts.size()) - 2; I >= 0; --I) {
    if (Stmts[I].Kind != StmtKind::Goto)
      continue;
    va_t Target = Stmts[I].GotoTarget;
    if (Target == 0 || Target == InvalidVA)
      continue;
    va_t NextAddr = Stmts[static_cast<size_t>(I) + 1].Addr;
    if (NextAddr == 0)
      continue;
    if (Target == NextAddr || (Target < NextAddr && NextAddr - Target <= 16))
      Stmts.erase(Stmts.begin() + I);
  }
  for (auto &S : Stmts) {
    if (!S.Body.empty())
      removeTrivialGotos(S.Body);
    if (!S.ElseBody.empty())
      removeTrivialGotos(S.ElseBody);
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

void MedToHighConverter::simplifyControlFlow(HighFunc &Func,
                                             const MedFunc &Med) {
  const bool IsMega = Func.Body.size() > limits::kMaxStructuredHighStmts;
  const char *Detail = std::getenv("NEVERD_HIGHIR_DETAIL");
  const bool WantDetail = Detail && Detail[0] == '1' && Detail[1] == '\0';
  auto Now = [] { return std::chrono::steady_clock::now(); };
  auto ElapsedMs = [](auto Start, auto End) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(End - Start)
        .count();
  };

  std::unordered_map<va_t, int> AddrToBlock;
  AddrToBlock.reserve(Med.Blocks.size());
  for (auto &Block : Med.Blocks)
    if (!Block.Ops.empty())
      AddrToBlock[Block.Ops.front().Addr] = Block.Id;

  auto TLoops = Now();
  detectAndConvertLoops(Func, AddrToBlock, Med, IsMega);
  auto TSwitch = Now();
  if (!IsMega)
    recoverSwitchStatements(Func);

  int IfElseMaxPasses =
      IsMega ? 0
             : (Med.Blocks.size() > limits::kMaxIfElseStructuringBlocks)
                   ? limits::kIfElseLargeCfgPasses
                   : limits::kIfElseStructuringPasses;
  auto TIfElse = Now();
  structureIfElse(Func, IfElseMaxPasses, &Med);
  auto TPost = Now();

  removeTrivialGotos(Func.Body);
  simplifyNestedGotos(Func.Body);
  seedIfThenJoinInits(Func.Body);

  mergeConsecutiveCondBlocks(Func.Body);
  inlineGotoReturns(Func, Med);
  cleanupGuardBeforeSwitch(Func);

  Func.Body.erase(
      std::remove_if(Func.Body.begin(), Func.Body.end(),
                     [](const HighStmt &S) {
                       return S.Kind == StmtKind::Nop &&
                              (!S.Addr || S.Addr == InvalidVA);
                     }),
      Func.Body.end());
  auto TEnd = Now();
  if (WantDetail)
    syncWarning() << "m2h-simp: " << Med.Name
                  << " [loops=" << ElapsedMs(TLoops, TSwitch)
                  << "ms switch=" << ElapsedMs(TSwitch, TIfElse)
                  << "ms ifelse=" << ElapsedMs(TIfElse, TPost)
                  << "ms post=" << ElapsedMs(TPost, TEnd) << "ms]\n";
}
} // namespace neverd
