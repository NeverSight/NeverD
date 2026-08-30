//===- HighIfChainToSwitch.cpp - If-chain to switch recovery -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recovers switch statements from cascading if-chains that compare the
/// same variable against a series of constants.  Handles default case
/// detection, dead-code cleanup after fully-terminating switches, and
/// guard-before-switch merging.
///
/// See also:
///   HighCFSimplify.cpp         — main simplifyControlFlow entry point
///   HighLoopRecovery.cpp       — while-loop recovery
///   NdOpSwitchRecovery.cpp     — jump-table-based switch recovery (LowIR)
///
//===----------------------------------------------------------------------===//

#include "HighCFSimplifyDetail.h"

#include "neverd/ir/high/MedToHigh.h"

#include <optional>
#include <set>

namespace neverd {

void recoverSwitchStatements(HighFunc &Func) {
  for (size_t I = 0; I < Func.Body.size(); ++I) {
    if (Func.Body[I].Kind != StmtKind::If)
      continue;
    if (Func.Body[I].Body.size() != 1 ||
        Func.Body[I].Body[0].Kind != StmtKind::Goto)
      continue;
    if (!Func.Body[I].Cond)
      continue;
    if (Func.Body[I].Cond->hasOrderedMemoryAccess())
      continue;

    auto ExtractCaseVal = [](const ExprPtr &Cond)
        -> std::optional<std::pair<std::string, uint64_t>> {
      if (!Cond || Cond->Kind != ExprKind::BinOp || Cond->Op != NdOp::INT_EQUAL)
        return std::nullopt;
      auto &Ops = Cond->Operands;
      if (Ops.size() != 2)
        return std::nullopt;
      if (Ops[0]->Kind == ExprKind::BinOp && Ops[0]->Op == NdOp::INT_SUB &&
          Ops[1]->Kind == ExprKind::Const && Ops[1]->ConstVal == 0) {
        auto &SubOps = Ops[0]->Operands;
        if (SubOps.size() == 2 && SubOps[1]->Kind == ExprKind::Const) {
          std::string Key = SubOps[0]->str();
          return std::make_pair(Key, SubOps[1]->ConstVal);
        }
      }
      if (Ops[1]->Kind == ExprKind::Const) {
        std::string Key = Ops[0]->str();
        return std::make_pair(Key, Ops[1]->ConstVal);
      }
      return std::nullopt;
    };

    auto FirstCase = ExtractCaseVal(Func.Body[I].Cond);
    if (!FirstCase)
      continue;

    struct CaseInfo {
      uint64_t Value;
      va_t BodyAddr;
      size_t StmtIdx;
    };
    std::vector<CaseInfo> CaseInfos;
    CaseInfos.push_back(
        {FirstCase->second, Func.Body[I].Body[0].GotoTarget, I});

    size_t ChainEnd = I + 1;
    std::set<uint64_t> SeenValues;
    SeenValues.insert(FirstCase->second);
    for (size_t J = I + 1; J < Func.Body.size(); ++J) {
      auto &S = Func.Body[J];
      if (S.Kind != StmtKind::If || S.Body.size() != 1 ||
          S.Body[0].Kind != StmtKind::Goto)
        break;
      auto CaseVal = ExtractCaseVal(S.Cond);
      if (!CaseVal || S.Cond->hasOrderedMemoryAccess() ||
          CaseVal->first != FirstCase->first)
        break;
      if (SeenValues.count(CaseVal->second))
        break;
      SeenValues.insert(CaseVal->second);
      CaseInfos.push_back({CaseVal->second, S.Body[0].GotoTarget, J});
      ChainEnd = J + 1;
    }

    if (CaseInfos.size() < 3)
      continue;

    va_t DefaultTarget = 0;
    size_t DefaultGotoIdx = SIZE_MAX;
    if (ChainEnd < Func.Body.size() &&
        Func.Body[ChainEnd].Kind == StmtKind::Goto) {
      DefaultTarget = Func.Body[ChainEnd].GotoTarget;
      DefaultGotoIdx = ChainEnd;
      ChainEnd++;
    }

    HighStmt SwitchStmt;
    SwitchStmt.Kind = StmtKind::Switch;
    SwitchStmt.Addr = Func.Body[I].Addr;
    for (auto &Case : CaseInfos) {
      size_t Idx = Case.StmtIdx;
      auto &Cond = Func.Body[Idx].Cond;
      if (Cond && Cond->Kind == ExprKind::BinOp && Cond->Operands.size() == 2) {
        auto &LHS = Cond->Operands[0];
        if (LHS->Kind == ExprKind::BinOp && LHS->Op == NdOp::INT_SUB &&
            LHS->Operands.size() == 2) {
          SwitchStmt.SwitchExpr = LHS->Operands[0];
          break;
        }
        SwitchStmt.SwitchExpr = LHS;
      }
    }
    if (!SwitchStmt.SwitchExpr)
      SwitchStmt.SwitchExpr = HighExpr::makeConst(0, 4);

    auto FindAndCollect = [&](va_t Target) -> std::vector<HighStmt> {
      std::vector<HighStmt> Result;
      for (size_t K = 0; K < Func.Body.size(); ++K) {
        if (Func.Body[K].Addr >= Target && Func.Body[K].Addr != 0 &&
            (Func.Body[K].Addr == Target || Func.Body[K].Addr - Target <= 16)) {
          for (size_t M = K; M < Func.Body.size(); ++M) {
            Result.push_back(Func.Body[M]);
            if (Func.Body[M].Kind == StmtKind::Goto ||
                Func.Body[M].Kind == StmtKind::Return)
              break;
          }
          break;
        }
      }
      if (!Result.empty() && Result.back().Kind == StmtKind::Goto)
        Result.pop_back();
      return Result;
    };

    std::set<size_t> Consumed;
    for (auto &Case : CaseInfos) {
      SwitchCase NewCase;
      NewCase.Value = Case.Value;
      NewCase.Body = FindAndCollect(Case.BodyAddr);
      SwitchStmt.Cases.push_back(std::move(NewCase));

      Consumed.insert(Case.StmtIdx);
      for (size_t K = 0; K < Func.Body.size(); ++K) {
        if (Func.Body[K].Addr >= Case.BodyAddr && Func.Body[K].Addr != 0 &&
            (Func.Body[K].Addr == Case.BodyAddr ||
             Func.Body[K].Addr - Case.BodyAddr <= 16)) {
          for (size_t M = K; M < Func.Body.size(); ++M) {
            Consumed.insert(M);
            if (Func.Body[M].Kind == StmtKind::Goto ||
                Func.Body[M].Kind == StmtKind::Return)
              break;
          }
          break;
        }
      }
    }

    if (DefaultTarget != 0) {
      SwitchStmt.DefaultBody = FindAndCollect(DefaultTarget);
      Consumed.insert(DefaultGotoIdx);
      for (size_t K = 0; K < Func.Body.size(); ++K) {
        if (Func.Body[K].Addr >= DefaultTarget && Func.Body[K].Addr != 0 &&
            (Func.Body[K].Addr == DefaultTarget ||
             Func.Body[K].Addr - DefaultTarget <= 16)) {
          for (size_t M = K; M < Func.Body.size(); ++M) {
            Consumed.insert(M);
            if (Func.Body[M].Kind == StmtKind::Goto ||
                Func.Body[M].Kind == StmtKind::Return)
              break;
          }
          break;
        }
      }
    }

    Func.Body[I] = std::move(SwitchStmt);
    std::vector<size_t> ToRemove(Consumed.begin(), Consumed.end());
    std::sort(ToRemove.rbegin(), ToRemove.rend());
    for (size_t Idx : ToRemove) {
      if (Idx != I && Idx < Func.Body.size())
        Func.Body.erase(Func.Body.begin() + static_cast<long>(Idx));
    }

    if (I + 1 < Func.Body.size() && Func.Body[I].Kind == StmtKind::Switch) {
      auto &SwitchRef = Func.Body[I];
      if (SwitchRef.DefaultBody.empty() &&
          (Func.Body[I + 1].Kind == StmtKind::Store ||
           Func.Body[I + 1].Kind == StmtKind::Assign)) {
        SwitchRef.DefaultBody.push_back(Func.Body[I + 1]);
        Func.Body.erase(Func.Body.begin() + static_cast<long>(I + 1));
      }
    }

    if (I + 1 < Func.Body.size() && Func.Body[I].Kind == StmtKind::Switch) {
      auto &SwitchRef = Func.Body[I];
      bool AllCasesTerminate = true;
      for (auto &C : SwitchRef.Cases) {
        if (C.Body.empty() || (C.Body.back().Kind != StmtKind::Return &&
                               C.Body.back().Kind != StmtKind::Break &&
                               C.Body.back().Kind != StmtKind::Goto)) {
          AllCasesTerminate = false;
          break;
        }
      }
      if (!SwitchRef.DefaultBody.empty()) {
        if (SwitchRef.DefaultBody.back().Kind != StmtKind::Return &&
            SwitchRef.DefaultBody.back().Kind != StmtKind::Break &&
            SwitchRef.DefaultBody.back().Kind != StmtKind::Goto)
          AllCasesTerminate = false;
      }
      if (AllCasesTerminate) {
        size_t J = I + 1;
        while (J < Func.Body.size()) {
          auto &Dead = Func.Body[J];
          if (Dead.Kind == StmtKind::While || Dead.Kind == StmtKind::Switch)
            break;
          J++;
        }
        if (J > I + 1) {
          Func.Body.erase(Func.Body.begin() + static_cast<long>(I + 1),
                          Func.Body.begin() + static_cast<long>(J));
        }
      }
    }

    break;
  }
}

} // namespace neverd
