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

#include "neverd/ir/TargetRegInfo.h"
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
    if (!CurrStmt.Dst || !NextStmt.Dst)
      continue;
    if (CurrStmt.Dst->Kind != ExprKind::Var ||
        NextStmt.Dst->Kind != ExprKind::Var)
      continue;

    bool SameVar = CurrStmt.Dst->structuralEq(*NextStmt.Dst);
    if (!SameVar) {
      SameVar = (CurrStmt.Dst->Var.Kind == NextStmt.Dst->Var.Kind &&
                 CurrStmt.Dst->Var.Id == NextStmt.Dst->Var.Id &&
                 CurrStmt.Dst->Var.RegOff == NextStmt.Dst->Var.RegOff &&
                 CurrStmt.Dst->Var.Size == NextStmt.Dst->Var.Size &&
                 CurrStmt.Dst->Var.SSAVer == NextStmt.Dst->Var.SSAVer);
    }
    if (!SameVar && CurrStmt.Dst->Var.Kind == MedVar::Reg &&
        NextStmt.Dst->Var.Kind == MedVar::Reg &&
        CurrStmt.Dst->Var.RegOff == NextStmt.Dst->Var.RegOff && CurrStmt.Val &&
        NextStmt.Val) {
      bool Equivalent = CurrStmt.Val->structuralEq(*NextStmt.Val);
      if (!Equivalent && NextStmt.Val->Kind == ExprKind::UnaryOp &&
          (NextStmt.Val->Op == NdOp::INT_ZEXT ||
           NextStmt.Val->Op == NdOp::INT_SEXT) &&
          !NextStmt.Val->Operands.empty())
        Equivalent = CurrStmt.Val->structuralEq(*NextStmt.Val->Operands[0]);
      if (Equivalent)
        SameVar = true;
    }

    if (!SameVar)
      continue;

    bool NextUsesCurr = false;
    std::unordered_set<const HighExpr *> Seen;
    std::function<void(const ExprPtr &)> CheckRef = [&](const ExprPtr &E) {
      if (!E || NextUsesCurr || !Seen.insert(E.get()).second)
        return;
      if (E->Kind == ExprKind::Var && E->structuralEq(*CurrStmt.Dst))
        NextUsesCurr = true;
      for (auto &Op : E->Operands)
        CheckRef(Op);
    };
    if (NextStmt.Val)
      CheckRef(NextStmt.Val);

    if (NextUsesCurr)
      continue;

    bool IsTrueAlias = (!CurrStmt.Dst->structuralEq(*NextStmt.Dst) &&
                        CurrStmt.Dst->Var.Kind == MedVar::Reg &&
                        NextStmt.Dst->Var.Kind == MedVar::Reg &&
                        CurrStmt.Dst->Var.RegOff == NextStmt.Dst->Var.RegOff &&
                        CurrStmt.Dst->Var.Size != NextStmt.Dst->Var.Size);

    if (IsTrueAlias) {
      ExprPtr OldDst = NextStmt.Dst;
      ExprPtr Replacement = CurrStmt.Dst;
      std::unordered_set<const HighExpr *> RewriteSeen;
      std::function<void(ExprPtr &)> RewriteRef = [&](ExprPtr &E) {
        if (!E)
          return;
        if (E->Kind == ExprKind::Var && E->structuralEq(*OldDst)) {
          E = Replacement;
          return;
        }
        if (!RewriteSeen.insert(E.get()).second)
          return;
        for (auto &Op : E->Operands)
          RewriteRef(Op);
      };
      walkStmts(Stmts, [&](HighStmt &St) { forEachRhsExpr(St, RewriteRef); });
      Stmts.erase(Stmts.begin() + static_cast<long>(I) + 1);
      --I;
    } else if (CurrStmt.Val && CurrStmt.Val->Kind == ExprKind::Call) {
      CurrStmt.Kind = StmtKind::Call;
      CurrStmt.CallExpr = CurrStmt.Val;
      CurrStmt.Dst = nullptr;
      CurrStmt.Val = nullptr;
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

//===----------------------------------------------------------------------===//
// Redundant stack store elimination
//===----------------------------------------------------------------------===//

void eliminateRedundantStackStores(HighFunc &Func, Arch TargetArch) {
  const auto &TRI = getTargetRegInfo(TargetArch);
  for (size_t I = 0; I + 1 < Func.Body.size(); ++I) {
    auto &StoreStmt = Func.Body[I];
    auto &NextStmt = Func.Body[I + 1];
    if (StoreStmt.Kind != StmtKind::Store)
      continue;
    if (NextStmt.Kind != StmtKind::Assign && NextStmt.Kind != StmtKind::Call)
      continue;
    ExprPtr CallExpr;
    if (NextStmt.Kind == StmtKind::Assign && NextStmt.Val &&
        NextStmt.Val->Kind == ExprKind::Call)
      CallExpr = NextStmt.Val;
    else if (NextStmt.Kind == StmtKind::Call)
      CallExpr = NextStmt.CallExpr;
    if (!CallExpr || !StoreStmt.StoreAddr || !StoreStmt.StoreVal)
      continue;
    bool IsStackPtrStore = false;
    if (StoreStmt.StoreAddr->Kind == ExprKind::BinOp &&
        StoreStmt.StoreAddr->Op == NdOp::INT_SUB &&
        !StoreStmt.StoreAddr->Operands.empty() &&
        StoreStmt.StoreAddr->Operands[0]->Kind == ExprKind::Var &&
        StoreStmt.StoreAddr->Operands[0]->Var.Kind == MedVar::Reg)
      IsStackPtrStore =
          TRI.isFrameReg(StoreStmt.StoreAddr->Operands[0]->Var.RegOff);
    if (StoreStmt.StoreAddr->Kind == ExprKind::Var &&
        StoreStmt.StoreAddr->Var.Kind == MedVar::Reg)
      IsStackPtrStore = TRI.isFrameReg(StoreStmt.StoreAddr->Var.RegOff);
    if (IsStackPtrStore) {
      bool ValInArgs = false;
      for (auto &Arg : CallExpr->Operands) {
        if (Arg && StoreStmt.StoreVal &&
            Arg->structuralEq(*StoreStmt.StoreVal)) {
          ValInArgs = true;
          break;
        }
      }
      if (ValInArgs) {
        Func.Body.erase(Func.Body.begin() + static_cast<long>(I));
        --I;
      }
    }
  }
}

} // namespace neverd
