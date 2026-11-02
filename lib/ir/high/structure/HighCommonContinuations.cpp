//===- HighCommonContinuations.cpp - Share terminal continuations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Fold a terminal conditional arm and an equivalent fallthrough suffix into
/// one continuation. Only temporary names confined to those copies may differ.
//===----------------------------------------------------------------------===//

#include "HighCFSimplifyDetail.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/high/MedToHigh.h"

#include <functional>
#include <iterator>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace neverd {
namespace {

using Counts = VarKeyMap<size_t>;

void countNames(const HighStmt &S, Counts &Into) {
  HighExprSet Seen;
  VarKeySet Names;
  std::vector<const HighExpr *> Work;
  forEachExpr(S, [&](const ExprPtr &E) { Work.push_back(E.get()); });
  while (!Work.empty()) {
    const auto *E = Work.back();
    Work.pop_back();
    if (!E || !Seen.insert(E).second)
      continue;
    if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi)
      Names.insert(varKey(E->Var));
    for (const MedVar &V : E->IntrinsicOutputs)
      Names.insert(varKey(V));
    E->forEachChildExpr(
        [&](const ExprPtr &Child) { Work.push_back(Child.get()); });
  }
  for (auto Key : Names)
    ++Into[Key];
}

class TerminalMatch {
public:
  TerminalMatch(const Counts &AllNames, const std::set<va_t> &Targets,
                size_t &Budget)
      : AllNames(AllNames), Targets(Targets), Budget(Budget) {}

  bool match(const std::vector<HighStmt> &Left,
             const std::vector<HighStmt> &Right, size_t Begin) {
    if (!body(Left, 0, Right, Begin))
      return false;
    for (const HighStmt *S : Compared)
      countNames(*S, Inside);
    for (const auto &[From, To] : Renames)
      for (const auto Key : {From, varKey(To)}) {
        const auto Found = AllNames.find(Key);
        if (Found == AllNames.end() || Found->second != Inside[Key])
          return false;
      }
    return true;
  }

private:
  const Counts &AllNames;
  const std::set<va_t> &Targets;
  VarKeyMap<MedVar> Renames;
  VarKeySet Destinations;
  Counts Inside;
  std::vector<const HighStmt *> Compared;
  size_t &Budget;

  bool expression(const ExprPtr &L, const ExprPtr &R) {
    if (!L || !R)
      return L == R;
    // Compare the complete expression contract, including casts, memory
    // ordering and call arguments. No algebra or memory equivalence is guessed.
    std::function<ExprPtr(const ExprPtr &, bool)> Rename =
        [&](const ExprPtr &E, bool ApplyRenames) {
          if (!E || !Budget)
            return ExprPtr{};
          --Budget;
          if (!E->IntrinsicOutputs.empty() || E->SourceCallHint ||
              E->Kind == ExprKind::Undef)
            return ExprPtr{};
          auto Copy = std::make_shared<HighExpr>(*E);
          if (ApplyRenames &&
              (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi)) {
            if (auto It = Renames.find(varKey(E->Var)); It != Renames.end())
              Copy->Var = It->second;
          }
          for (ExprPtr &Operand : Copy->Operands) {
            Operand = Rename(Operand, ApplyRenames);
            if (!Operand)
              return ExprPtr{};
          }
          if (Copy->IndirectTarget) {
            Copy->IndirectTarget = Rename(Copy->IndirectTarget, ApplyRenames);
            if (!Copy->IndirectTarget)
              return ExprPtr{};
          }
          // An unresolved call carries its instruction address as a lookup key,
          // not its callee. The actual runtime target above must compare equal.
          if (Copy->Kind == ExprKind::Call && Copy->IsIndirectCall &&
              (Copy->CallTarget.empty() || Copy->CallTarget == "indirect") &&
              Copy->CallAddr == Copy->CallSiteAddr && Copy->IndirectTarget)
            Copy->CallAddr = 0;
          return Copy;
        };
    auto A = Rename(L, true);
    auto B = Rename(R, false);
    std::function<bool(const ExprPtr &, const ExprPtr &)> SameTypes =
        [&](const ExprPtr &X, const ExprPtr &Y) {
          if (!X || !Y)
            return X == Y;
          if (X->Kind != Y->Kind || X->IntrinsicId != Y->IntrinsicId ||
              !equalSourceTypes(X->Type, Y->Type) ||
              X->Operands.size() != Y->Operands.size())
            return false;
          if ((X->Kind == ExprKind::Var || X->Kind == ExprKind::Phi) &&
              X->Var != Y->Var)
            return false;
          for (size_t I = 0; I < X->Operands.size(); ++I)
            if (!SameTypes(X->Operands[I], Y->Operands[I]))
              return false;
          return SameTypes(X->IndirectTarget, Y->IndirectTarget);
        };
    return A && B && SameTypes(A, B) && A->structuralEq(*B);
  }

  bool statement(const HighStmt &L, const HighStmt &R) {
    if (!Budget || L.Kind != R.Kind || Targets.count(L.Addr) ||
        Targets.count(R.Addr) || L.MemoryOrdering != R.MemoryOrdering ||
        L.MemoryAddressSpace != R.MemoryAddressSpace || L.IsPhiCopy ||
        R.IsPhiCopy || !L.Cases.empty() || !R.Cases.empty() ||
        !L.DefaultBody.empty() || !R.DefaultBody.empty() ||
        !L.EHClauses.empty() || !R.EHClauses.empty() ||
        !L.EHClauseBodies.empty() || !R.EHClauseBodies.empty() ||
        !L.CxxContinuationReturnTargets.empty() ||
        !R.CxxContinuationReturnTargets.empty())
      return false;
    --Budget;
    Compared.push_back(&L);
    Compared.push_back(&R);
    switch (L.Kind) {
    case StmtKind::Assign:
      if (!L.Dst || !R.Dst || L.Dst->Kind != ExprKind::Var ||
          R.Dst->Kind != ExprKind::Var || L.Dst->Var.Kind != MedVar::Temp ||
          R.Dst->Var.Kind != MedVar::Temp ||
          !equalSourceTypes(L.Dst->Type, R.Dst->Type) ||
          Renames.count(varKey(L.Dst->Var)) ||
          !Destinations.insert(varKey(R.Dst->Var)).second)
        return false;
      // Compare the RHS before this assignment defines the new local.
      if (!expression(L.Val, R.Val))
        return false;
      Renames.emplace(varKey(L.Dst->Var), R.Dst->Var);
      break;
    case StmtKind::If:
    case StmtKind::IfElse:
    case StmtKind::Return:
    case StmtKind::Call:
    case StmtKind::Store:
    case StmtKind::ExprStmt:
      break;
    default:
      return false;
    }
    return expression(L.Dst, R.Dst) &&
           (L.Kind == StmtKind::Assign || expression(L.Val, R.Val)) &&
           expression(L.Cond, R.Cond) && expression(L.RetVal, R.RetVal) &&
           expression(L.CallExpr, R.CallExpr) &&
           expression(L.StoreAddr, R.StoreAddr) &&
           expression(L.StoreVal, R.StoreVal) && body(L.Body, 0, R.Body, 0) &&
           body(L.ElseBody, 0, R.ElseBody, 0);
  }

  bool anchor(const HighStmt &S) const {
    return !Targets.count(S.Addr) &&
           (S.Kind == StmtKind::Nop ||
            (S.Kind == StmtKind::Block && S.Body.empty()));
  }

  bool body(const std::vector<HighStmt> &L, size_t I,
            const std::vector<HighStmt> &R, size_t J) {
    while (true) {
      while (I < L.size() && anchor(L[I]))
        ++I;
      while (J < R.size() && anchor(R[J]))
        ++J;
      if (I == L.size() || J == R.size())
        return I == L.size() && J == R.size();
      if (!statement(L[I++], R[J++]))
        return false;
    }
  }
};

} // namespace

bool foldDuplicateTerminalContinuations(HighFunc &Func, const MedFunc *Med) {
  // A matched arm is deleted. The unwinder can enter a catch continuation or
  // landing pad inside it without any goto, and its calls may belong to a
  // different unwind state than the tail's; neither is visible here.
  if (hasExceptionFlow(Func, Med))
    return false;
  Counts AllNames;
  std::set<va_t> Targets;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto)
      Targets.insert(S.GotoTarget);
    for (const HighEHClause &Clause : S.EHClauses)
      Targets.insert(Clause.HandlerVA);
    countNames(S, AllNames);
  });
  bool Changed = false;
  size_t Budget = limits::ir::kHighCommonContinuationBudget;
  auto Anchor = [&](const HighStmt &S) {
    return !Targets.count(S.Addr) &&
           (S.Kind == StmtKind::Nop ||
            (S.Kind == StmtKind::Block && S.Body.empty()));
  };
  const auto Live = [&](const std::vector<HighStmt> &Body) {
    std::vector<size_t> Result;
    for (size_t I = 0; I < Body.size(); ++I)
      if (!Anchor(Body[I]))
        Result.push_back(I);
    return Result;
  };
  std::function<void(std::vector<HighStmt> &)> Visit = [&](auto &Body) {
    for (HighStmt &S : Body) {
      Visit(S.Body);
      Visit(S.ElseBody);
      // Stay inside each protection region; no EH wrapper is moved.
      for (auto &Clause : S.EHClauseBodies)
        Visit(Clause);
    }
    const auto Positions = Live(Body);
    for (size_t N = 0; N < Positions.size(); ++N) {
      if (!Budget)
        return;
      const size_t I = Positions[N];
      HighStmt &Guard = Body[I];
      if (Guard.Kind != StmtKind::If || !Guard.Cond || !Guard.ElseBody.empty())
        continue;
      const auto Arm = Live(Guard.Body);
      if (Arm.empty() || Guard.Body[Arm.back()].Kind != StmtKind::Return ||
          Body[Positions.back()].Kind != StmtKind::Return ||
          Positions.size() - N - 1 < Arm.size())
        continue;
      const size_t Tail = Positions[Positions.size() - Arm.size()];
      // No existing label may acquire the inverted guard when its prefix moves.
      bool Entered = false;
      for (size_t J = I + 1; J < Tail; ++J) {
        const auto Check = [&](const HighStmt &S) {
          if (!Budget) {
            Entered = true;
            return;
          }
          --Budget;
          Entered |= Targets.count(S.Addr) != 0;
        };
        Check(Body[J]);
        walkStmts(Body[J].Body, Check);
        walkStmts(Body[J].ElseBody, Check);
        for (const auto &Case : Body[J].Cases)
          walkStmts(Case.Body, Check);
        walkStmts(Body[J].DefaultBody, Check);
        for (const auto &Clause : Body[J].EHClauseBodies)
          walkStmts(Clause, Check);
      }
      if (Entered || !TerminalMatch(AllNames, Targets, Budget)
                          .match(Guard.Body, Body, Tail))
        continue;
      Guard.Cond = invertHighCond(std::move(Guard.Cond));
      Guard.Body.assign(std::make_move_iterator(Body.begin() + I + 1),
                        std::make_move_iterator(Body.begin() + Tail));
      // Calls from the shared copies no longer have one instruction occurrence.
      for (size_t J = Tail; J < Body.size(); ++J) {
        auto ClearSites = [&](HighStmt &S) {
          forEachExpr(S,
                      [&](ExprPtr &E) { E = withMergedCallSites(E, nullptr); });
        };
        ClearSites(Body[J]);
        walkStmts(Body[J].Body, ClearSites);
        walkStmts(Body[J].ElseBody, ClearSites);
      }
      Body.erase(Body.begin() + I + 1, Body.begin() + Tail);
      Changed = true;
      break; // Rebuild positions on a later invocation, not from stale indexes.
    }
  };
  Visit(Func.Body);
  return Changed;
}

} // namespace neverd
