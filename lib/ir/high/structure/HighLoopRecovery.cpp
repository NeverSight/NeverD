//===- HighLoopRecovery.cpp - While-loop recovery from backward gotos -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Detects backward gotos in the flat HighIR statement list and converts
/// them into while-loop constructs.  Handles latch-conditional loops,
/// header-conditional loops, phi-copy ordering inside loop bodies, and
/// break/continue conversion for nested gotos.
///
/// See also:
///   HighCFSimplify.cpp         — main simplifyControlFlow entry point
///   HighIfChainToSwitch.cpp    — if-chain to switch recovery
///   HighCFSimplifyDetail.h     — shared AddrMap helper
///
//===----------------------------------------------------------------------===//

#include "HighCFSimplifyDetail.h"

#include "neverd/ir/high/MedToHigh.h"

#include <functional>
#include <optional>
#include <set>

namespace neverd {

void detectAndConvertLoops(HighFunc &Func,
                           const std::unordered_map<va_t, int> &AddrToBlock,
                           bool IsMega) {
  AddrMap AM;
  AM.rebuild(Func.Body);

  auto FindStmtAt = [&](va_t Target) -> std::optional<size_t> {
    auto It = AM.Idx.find(Target);
    if (It != AM.Idx.end() && It->second < Func.Body.size() &&
        Func.Body[It->second].Kind != StmtKind::Nop)
      return It->second;
    AM.ensureSorted();
    auto LowerBound = std::lower_bound(AM.Sorted.begin(), AM.Sorted.end(),
                                       std::make_pair(Target, size_t(0)));
    if (LowerBound != AM.Sorted.end() &&
        LowerBound->second < Func.Body.size() &&
        Func.Body[LowerBound->second].Kind != StmtKind::Nop)
      return LowerBound->second;
    return std::nullopt;
  };

  const int MaxLoopPasses = IsMega ? 2 : 4;
  for (int LoopPass = 0; LoopPass < MaxLoopPasses; ++LoopPass) {
    bool FoundLoop = false;
    AM.rebuild(Func.Body);

    for (int I = static_cast<int>(Func.Body.size()) - 1; I >= 0; --I) {
      auto &GotoStmt = Func.Body[I];
      if (GotoStmt.Kind == StmtKind::Nop)
        continue;

      va_t Target = 0;
      ExprPtr LatchCond;
      if (GotoStmt.Kind == StmtKind::Goto) {
        Target = GotoStmt.GotoTarget;
      } else if (GotoStmt.Kind == StmtKind::If && GotoStmt.Body.size() == 1 &&
                 GotoStmt.Body[0].Kind == StmtKind::Goto) {
        Target = GotoStmt.Body[0].GotoTarget;
        LatchCond = GotoStmt.Cond;
      }
      if (Target == 0 || Target == InvalidVA)
        continue;

      auto OptIdx = FindStmtAt(Target);
      if (!OptIdx)
        continue;
      size_t HeaderIdx = *OptIdx;
      if (HeaderIdx > static_cast<size_t>(I))
        continue;

      std::vector<HighStmt> LoopBody;
      LoopBody.reserve(static_cast<size_t>(I) - HeaderIdx);
      for (size_t K = HeaderIdx; K < static_cast<size_t>(I); ++K) {
        if (Func.Body[K].Kind == StmtKind::Nop)
          continue;
        LoopBody.push_back(std::move(Func.Body[K]));
      }

      ExprPtr WhileCond;
      va_t LoopExitTarget = 0;

      if (LatchCond) {
        WhileCond = HighExpr::makeConst(1, 1);

        va_t ExitAddr = (static_cast<size_t>(I) + 1 < Func.Body.size())
                            ? Func.Body[static_cast<size_t>(I) + 1].Addr
                            : 0;
        HighStmt ExitCheck;
        ExitCheck.Kind = StmtKind::If;
        ExitCheck.Addr = Func.Body[I].Addr;
        ExitCheck.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, LatchCond);
        HighStmt ExitGoto;
        ExitGoto.Kind = StmtKind::Goto;
        ExitGoto.GotoTarget = ExitAddr;
        ExitCheck.Body.push_back(ExitGoto);
        LoopBody.push_back(ExitCheck);
      } else if (!LoopBody.empty() && LoopBody[0].Kind == StmtKind::If &&
                 LoopBody[0].Body.size() == 1 &&
                 LoopBody[0].Body[0].Kind == StmtKind::Goto) {
        va_t ExitTarget = LoopBody[0].Body[0].GotoTarget;
        if (ExitTarget != 0) {
          auto ExitIt = AM.Idx.find(ExitTarget);
          bool ExitsAfterLoop = (ExitIt == AM.Idx.end()) ||
                                (ExitIt->second > static_cast<size_t>(I));
          if (ExitsAfterLoop && LoopBody[0].Cond) {
            WhileCond = HighExpr::makeUnary(NdOp::BOOL_NOT, LoopBody[0].Cond);
            LoopExitTarget = ExitTarget;
            LoopBody.erase(LoopBody.begin());
          }
        }
      }

      if (LoopExitTarget != 0) {
        LoopBody.erase(std::remove_if(LoopBody.begin(), LoopBody.end(),
                                      [LoopExitTarget](const HighStmt &S) {
                                        return S.Addr != 0 &&
                                               S.Addr >= LoopExitTarget &&
                                               S.Kind != StmtKind::Goto;
                                      }),
                       LoopBody.end());
        LoopBody.erase(std::remove_if(LoopBody.begin(), LoopBody.end(),
                                      [LoopExitTarget](const HighStmt &S) {
                                        return S.Kind == StmtKind::Goto &&
                                               S.GotoTarget >= LoopExitTarget;
                                      }),
                       LoopBody.end());
      }

      if (!WhileCond)
        WhileCond = HighExpr::makeConst(1, 1);

      HighStmt WhileStmt;
      WhileStmt.Kind = StmtKind::While;
      WhileStmt.Addr = Func.Body[HeaderIdx].Addr;
      WhileStmt.LoopHeaderAddr = Target;

      if (LatchCond) {
        for (size_t K = 0; K < HeaderIdx; ++K) {
          auto &S = Func.Body[K];
          if (S.Kind != StmtKind::Goto)
            continue;
          va_t GotoDest = S.GotoTarget;
          if (GotoDest == 0 || GotoDest == InvalidVA)
            continue;
          va_t LoopStart = Func.Body[HeaderIdx].Addr;
          va_t LoopEnd = Func.Body[I].Addr;
          if (GotoDest >= LoopStart && GotoDest <= LoopEnd) {
            WhileStmt.LoopHeaderAddr = GotoDest;
            break;
          }
        }
        if (WhileStmt.LoopHeaderAddr == Target) {
          for (size_t K = 0; K < HeaderIdx; ++K) {
            auto &S = Func.Body[K];
            if (S.Kind != StmtKind::If)
              continue;
            for (auto &InnerStmt : S.Body) {
              if (InnerStmt.Kind != StmtKind::Goto)
                continue;
              va_t GotoDest = InnerStmt.GotoTarget;
              va_t LoopStart = Func.Body[HeaderIdx].Addr;
              va_t LoopEnd = Func.Body[I].Addr;
              if (GotoDest >= LoopStart && GotoDest <= LoopEnd) {
                WhileStmt.LoopHeaderAddr = GotoDest;
                goto FoundEntry;
              }
            }
          }
        FoundEntry:;
        }
      }
      WhileStmt.Cond = WhileCond;

      {
        std::vector<HighStmt> NonPhi, PhiStmts;
        for (auto &S : LoopBody) {
          if (S.IsPhiCopy)
            PhiStmts.push_back(std::move(S));
          else
            NonPhi.push_back(std::move(S));
        }

        auto GetWritten = [](const HighStmt &S) -> std::string {
          if (S.Dst && S.Dst->Kind == ExprKind::Var)
            return S.Dst->Var.display();
          return {};
        };
        std::function<void(const ExprPtr &, std::set<std::string> &)>
            CollectReads;
        CollectReads = [&](const ExprPtr &E, std::set<std::string> &Out) {
          if (!E)
            return;
          if (E->Kind == ExprKind::Var)
            Out.insert(E->Var.display());
          for (auto &Op : E->Operands)
            CollectReads(Op, Out);
        };

        std::set<std::string> AllWritten;
        for (auto &PS : PhiStmts)
          if (auto W = GetWritten(PS); !W.empty())
            AllWritten.insert(W);

        std::vector<HighStmt> Ordered;
        std::vector<bool> Placed(PhiStmts.size(), false);
        for (size_t Round = 0; Round < PhiStmts.size() + 1; ++Round) {
          bool Progress = false;
          for (size_t PI = 0; PI < PhiStmts.size(); ++PI) {
            if (Placed[PI])
              continue;
            std::set<std::string> Reads;
            CollectReads(PhiStmts[PI].Val, Reads);
            bool Blocked = false;
            for (size_t PJ = 0; PJ < PhiStmts.size(); ++PJ) {
              if (PI == PJ || Placed[PJ])
                continue;
              auto W = GetWritten(PhiStmts[PJ]);
              if (!W.empty() && Reads.count(W)) {
                Blocked = true;
                break;
              }
            }
            if (!Blocked) {
              Ordered.push_back(std::move(PhiStmts[PI]));
              Placed[PI] = true;
              Progress = true;
            }
          }
          if (!Progress)
            break;
        }
        for (size_t PI = 0; PI < PhiStmts.size(); ++PI)
          if (!Placed[PI])
            Ordered.push_back(std::move(PhiStmts[PI]));

        auto FindLastExitIf = [](std::vector<HighStmt> &Stmts) -> int {
          for (int K = static_cast<int>(Stmts.size()) - 1; K >= 0; --K) {
            auto &S = Stmts[K];
            if ((S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse) &&
                !S.Body.empty() &&
                (S.Body[0].Kind == StmtKind::Break ||
                 S.Body[0].Kind == StmtKind::Continue ||
                 S.Body[0].Kind == StmtKind::Goto))
              return K;
            if (S.Kind != StmtKind::Assign)
              break;
          }
          return -1;
        };

        int BreakIdx = LatchCond ? FindLastExitIf(NonPhi) : -1;
        if (BreakIdx >= 0 && !Ordered.empty()) {
          auto &BreakIf = NonPhi[BreakIdx];

          std::function<bool(const ExprPtr &)> HasLoad;
          HasLoad = [&](const ExprPtr &E) -> bool {
            if (!E)
              return false;
            if (E->Kind == ExprKind::Load)
              return true;
            for (auto &Op : E->Operands)
              if (HasLoad(Op))
                return true;
            return false;
          };

          std::vector<HighStmt> PreBreakCopies;
          for (auto &PC : Ordered) {
            if (HasLoad(PC.Val))
              continue;
            HighStmt Dup;
            Dup.Kind = PC.Kind;
            Dup.Addr = PC.Addr;
            Dup.IsPhiCopy = true;
            if (PC.Dst)
              Dup.Dst = std::make_shared<HighExpr>(*PC.Dst);
            if (PC.Val)
              Dup.Val = std::make_shared<HighExpr>(*PC.Val);
            PreBreakCopies.push_back(std::move(Dup));
          }
          if (!PreBreakCopies.empty()) {
            PreBreakCopies.insert(PreBreakCopies.end(),
                                  std::make_move_iterator(BreakIf.Body.begin()),
                                  std::make_move_iterator(BreakIf.Body.end()));
            BreakIf.Body = std::move(PreBreakCopies);
          }
        }

        NonPhi.insert(NonPhi.end(), std::make_move_iterator(Ordered.begin()),
                      std::make_move_iterator(Ordered.end()));
        LoopBody = std::move(NonPhi);
      }

      WhileStmt.Body = std::move(LoopBody);

      va_t LoopExitAddr = 0;
      if (static_cast<size_t>(I) + 1 < Func.Body.size())
        LoopExitAddr = Func.Body[static_cast<size_t>(I) + 1].Addr;

      auto IsExit = [&](va_t GT) -> bool {
        if (GT == 0 || GT == InvalidVA)
          return false;
        if (LoopExitAddr != 0 && GT == LoopExitAddr)
          return true;
        if (LoopExitAddr != 0 && GT < LoopExitAddr && LoopExitAddr - GT <= 16) {
          auto AIt = AddrToBlock.find(GT);
          if (AIt != AddrToBlock.end()) {
            auto EIt = AddrToBlock.find(LoopExitAddr);
            if (EIt != AddrToBlock.end() && AIt->second == EIt->second)
              return true;
          }
          if (GT > Func.Body[HeaderIdx].Addr)
            return true;
        }
        return false;
      };

      std::function<void(std::vector<HighStmt> &, va_t)> ConvertLoopGotos;
      ConvertLoopGotos = [&](std::vector<HighStmt> &Stmts, va_t Hdr) {
        for (auto &S : Stmts) {
          if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
              S.GotoTarget != InvalidVA) {
            if (IsExit(S.GotoTarget))
              S.Kind = StmtKind::Break;
            else if (S.GotoTarget == Hdr)
              S.Kind = StmtKind::Continue;
          }
          if (S.Kind == StmtKind::If && S.Body.size() == 1 &&
              S.Body[0].Kind == StmtKind::Goto) {
            va_t GotoDest = S.Body[0].GotoTarget;
            if (IsExit(GotoDest))
              S.Body[0].Kind = StmtKind::Break;
            else if (GotoDest == Hdr)
              S.Body[0].Kind = StmtKind::Continue;
          }
          if (!S.Body.empty())
            ConvertLoopGotos(S.Body, Hdr);
          if (!S.ElseBody.empty())
            ConvertLoopGotos(S.ElseBody, Hdr);
        }
      };
      ConvertLoopGotos(WhileStmt.Body, Target);

      Func.Body[HeaderIdx] = std::move(WhileStmt);
      for (size_t K = HeaderIdx + 1; K <= static_cast<size_t>(I); ++K) {
        Func.Body[K].Kind = StmtKind::Nop;
        Func.Body[K].Addr = 0;
      }
      FoundLoop = true;
      AM.SortedValid = false;
      I = static_cast<int>(HeaderIdx);
    }
    if (!FoundLoop)
      break;
    Func.Body.erase(std::remove_if(Func.Body.begin(), Func.Body.end(),
                                   [](const HighStmt &S) {
                                     return S.Kind == StmtKind::Nop;
                                   }),
                    Func.Body.end());
  }
}

} // namespace neverd
