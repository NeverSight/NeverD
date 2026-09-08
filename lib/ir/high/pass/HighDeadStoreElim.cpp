//===- HighDeadStoreElim.cpp - Dead store elimination for HighIR ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dead store elimination passes for HighIR:
///   - Consecutive dead store elimination (same-destination rewrites)
///   - Redundant stack store elimination (stores duplicated in call args)
///
/// See also:
///   HighDCE.cpp         — main DCE orchestration, iterative liveness DCE
///   HighCopyProp.cpp    — copy propagation and alias resolution
///   HighDCEDetail.h     — shared declarations
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/HighIR.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace neverd {

//===----------------------------------------------------------------------===//
// Consecutive dead store elimination
//===----------------------------------------------------------------------===//

void elimConsecutiveDeadStores(std::vector<HighStmt> &Stmts) {
  Stmts.erase(
      std::remove_if(Stmts.begin(), Stmts.end(),
                     [](const HighStmt &S) { return S.Kind == StmtKind::Nop; }),
      Stmts.end());

  for (size_t I = 0; I + 1 < Stmts.size(); ++I) {
    auto &CurrStmt = Stmts[I];
    auto &NextStmt = Stmts[I + 1];
    if (CurrStmt.Kind != StmtKind::Assign || NextStmt.Kind != StmtKind::Assign)
      continue;
    if (CurrStmt.Val && CurrStmt.Val->hasOrderedMemoryAccess())
      continue;
    if (!CurrStmt.Dst || !NextStmt.Dst)
      continue;
    if (CurrStmt.Dst->Kind != ExprKind::Var ||
        NextStmt.Dst->Kind != ExprKind::Var)
      continue;

    const auto &Current = CurrStmt.Dst->Var;
    const auto &Next = NextStmt.Dst->Var;
    // Physical registers are reused by distinct SSA values. Equal right-hand
    // sides do not make those destinations one mutable local, and a widening
    // conversion does not make the narrow and wide values interchangeable.
    const bool SameVariable =
        Current.Kind == Next.Kind && Current.TheArch == Next.TheArch &&
        Current.Id == Next.Id && Current.SSAVer == Next.SSAVer &&
        Current.RenameTag == Next.RenameTag && Current.Size == Next.Size &&
        Current.RegOff == Next.RegOff;
    if (!SameVariable || !CurrStmt.Val || !NextStmt.Val)
      continue;

    bool NextUsesCurr = false;
    std::unordered_set<const HighExpr *> Seen;
    std::function<void(const ExprPtr &)> CheckRef = [&](const ExprPtr &E) {
      if (!E || NextUsesCurr || !Seen.insert(E.get()).second)
        return;
      if (E->Kind == ExprKind::Var && E->Var == CurrStmt.Dst->Var)
        NextUsesCurr = true;
      for (auto &Op : E->Operands)
        CheckRef(Op);
    };
    if (NextStmt.Val)
      CheckRef(NextStmt.Val);

    if (NextUsesCurr)
      continue;

    bool HasEffect = false;
    std::vector<const HighExpr *> Pending{CurrStmt.Val.get()};
    std::unordered_set<const HighExpr *> EffectSeen;
    while (!Pending.empty()) {
      const auto *Expression = Pending.back();
      Pending.pop_back();
      if (!Expression || !EffectSeen.insert(Expression).second)
        continue;
      HasEffect |= Expression->Kind == ExprKind::Call ||
                   Expression->Kind == ExprKind::Store;
      for (const auto &Operand : Expression->Operands)
        Pending.push_back(Operand.get());
    }
    if (CurrStmt.Val->Kind == ExprKind::Call) {
      CurrStmt.Kind = StmtKind::Call;
      CurrStmt.CallExpr = CurrStmt.Val;
      CurrStmt.Dst = nullptr;
      CurrStmt.Val = nullptr;
    } else if (HasEffect) {
      CurrStmt.Kind = StmtKind::ExprStmt;
      CurrStmt.Dst = nullptr;
    } else {
      Stmts.erase(Stmts.begin() + static_cast<long>(I));
      --I;
    }
  }

  for (auto &S : Stmts) {
    elimConsecutiveDeadStores(S.Body);
    elimConsecutiveDeadStores(S.ElseBody);
    for (auto &C : S.Cases)
      elimConsecutiveDeadStores(C.Body);
    elimConsecutiveDeadStores(S.DefaultBody);
  }
}

} // namespace neverd
