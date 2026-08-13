//===- HighCFSimplifyIfElse.cpp - if/else structuring for HighIR
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Folds `if (cond) goto L;` patterns in the flat HighIR statement list into
/// if/else trees, inferring the merge point from the destinations the two
/// branches jump to.
///
/// See also:
///   HighCFSimplify.cpp         — main simplifyControlFlow entry point
///   HighCFSimplifyDetail.h     — shared AddrMap helper
///
//===----------------------------------------------------------------------===//

#include "HighCFSimplifyDetail.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace neverd {

//===----------------------------------------------------------------------===//
// structureIfElse helpers
//===----------------------------------------------------------------------===//

struct ElseTargetInfo {
  va_t Target = 0;
  size_t GotoIdx = 0;
  bool HasEarlyReturn = false;
  size_t ReturnIdx = SIZE_MAX;
  std::vector<size_t> FallthroughIndices;
};

/// Scan forward from \p NextI to find the else branch target.  When the
/// statements between the if and the else-goto contain a return, the
/// HasEarlyReturn flag and FallthroughIndices are populated.
static ElseTargetInfo findElseTarget(const std::vector<HighStmt> &Body,
                                     size_t NextI, va_t IfTarget) {
  ElseTargetInfo Info;
  if (Body[NextI].Kind == StmtKind::Goto) {
    Info.Target = Body[NextI].GotoTarget;
    Info.GotoIdx = NextI;
    return Info;
  }

  for (size_t K = NextI; K < Body.size(); ++K) {
    auto &S = Body[K];
    if (S.Kind == StmtKind::While || S.Kind == StmtKind::If)
      break;
    if (S.Kind == StmtKind::Goto) {
      Info.Target = S.GotoTarget;
      Info.GotoIdx = K;
      break;
    }
    if (S.IsPhiCopy)
      continue;
    bool IsFallthrough =
        (S.Addr != 0 && S.Addr < IfTarget) || S.Kind == StmtKind::Return;
    if (IsFallthrough) {
      Info.FallthroughIndices.push_back(K);
      if (S.Kind == StmtKind::Return) {
        Info.HasEarlyReturn = true;
        Info.ReturnIdx = K;
        break;
      }
    }
  }
  return Info;
}

struct StmtCollectResult {
  std::vector<HighStmt> Stmts;
  size_t Start = SIZE_MAX, End = SIZE_MAX;
};

/// Collect a run of statements beginning at \p Target, stopping at
/// \p Merge or a terminating goto/return.
static StmtCollectResult
collectStmtsForTarget(const std::vector<HighStmt> &Body, AddrMap &AM,
                      va_t Target, va_t Merge) {
  StmtCollectResult Result;
  size_t StartIdx = SIZE_MAX;
  auto It = AM.Idx.find(Target);
  if (It != AM.Idx.end()) {
    StartIdx = It->second;
  } else {
    AM.ensureSorted();
    auto LB = std::lower_bound(AM.Sorted.begin(), AM.Sorted.end(),
                               std::make_pair(Target, size_t(0)));
    if (LB != AM.Sorted.end() && LB->first - Target <= 16)
      StartIdx = LB->second;
  }
  if (StartIdx == SIZE_MAX)
    return Result;
  Result.Start = StartIdx;
  for (size_t K = StartIdx; K < Body.size(); ++K) {
    auto &S = Body[K];
    if (Merge != 0 && S.Addr != 0 && S.Addr >= Merge)
      break;
    Result.Stmts.push_back(S);
    Result.End = K + 1;
    if (S.Kind == StmtKind::Return || S.Kind == StmtKind::Goto)
      break;
  }
  return Result;
}

/// Find the goto destination of the block starting at \p Target.
static va_t findGotoDestination(const std::vector<HighStmt> &Body,
                                const AddrMap &AM, va_t Target) {
  auto It = AM.Idx.find(Target);
  if (It == AM.Idx.end())
    return 0;
  for (size_t K = It->second; K < Body.size(); ++K) {
    if (Body[K].Kind == StmtKind::Goto)
      return Body[K].GotoTarget;
    if (Body[K].Kind == StmtKind::Return)
      return 0;
  }
  return 0;
}

/// Infer the merge target from if/else goto destinations.
static va_t inferMergeTarget(va_t IfTarget, va_t IfDest, va_t ElseTarget,
                             va_t ElseDest) {
  if (IfDest != 0 && IfDest == ElseDest)
    return IfDest;
  if (IfDest == ElseTarget)
    return ElseTarget;
  if (ElseDest == IfTarget)
    return IfTarget;
  if (IfDest == 0 && ElseTarget != 0 && ElseTarget > IfTarget)
    return ElseTarget;
  return 0;
}

/// Strip a trailing goto that jumps to \p MergeTarget.
static void trimMergeGoto(std::vector<HighStmt> &Body, va_t MergeTarget) {
  if (!Body.empty() && Body.back().Kind == StmtKind::Goto &&
      Body.back().GotoTarget == MergeTarget)
    Body.pop_back();
}

//===----------------------------------------------------------------------===//
// structureIfElse — fold if(cond){goto} patterns into if/else trees
//===----------------------------------------------------------------------===//

void structureIfElse(HighFunc &Func, int MaxPasses) {
  AddrMap AM;
  bool Changed = true;
  int Pass = 0;
  while (Changed && Pass++ < MaxPasses) {
    Changed = false;
    AM.rebuild(Func.Body);

    for (int I = static_cast<int>(Func.Body.size()) - 1; I >= 0; --I) {
      auto &Stmt = Func.Body[I];
      if (Stmt.Kind != StmtKind::If)
        continue;
      if (Stmt.Body.size() != 1 || Stmt.Body[0].Kind != StmtKind::Goto)
        continue;

      va_t IfTarget = Stmt.Body[0].GotoTarget;
      if (IfTarget == 0 || IfTarget == InvalidVA)
        continue;

      size_t NextI = static_cast<size_t>(I) + 1;
      if (NextI >= Func.Body.size())
        continue;

      auto Else = findElseTarget(Func.Body, NextI, IfTarget);

      // Early-return fold.
      if (Else.HasEarlyReturn && Else.Target == 0 &&
          Else.ReturnIdx != SIZE_MAX) {
        Stmt.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Stmt.Cond);
        Stmt.Body.clear();
        for (size_t Idx : Else.FallthroughIndices)
          Stmt.Body.push_back(std::move(Func.Body[Idx]));
        for (auto It = Else.FallthroughIndices.rbegin();
             It != Else.FallthroughIndices.rend(); ++It)
          Func.Body.erase(Func.Body.begin() + static_cast<long>(*It));
        Changed = true;
        continue;
      }

      // Same-target fold: both branches goto the same address.
      if (IfTarget == Else.Target) {
        Stmt.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Stmt.Cond);
        Stmt.Body.clear();
        for (size_t K = NextI; K < Else.GotoIdx; ++K)
          Stmt.Body.push_back(std::move(Func.Body[K]));
        Func.Body.erase(Func.Body.begin() + static_cast<long>(NextI),
                        Func.Body.begin() +
                            static_cast<long>(Else.GotoIdx + 1));
        Changed = true;
        continue;
      }

      // General if/else structuring with merge-point inference.
      va_t IfDest = findGotoDestination(Func.Body, AM, IfTarget);
      va_t ElseDest = Else.Target != 0
                          ? findGotoDestination(Func.Body, AM, Else.Target)
                          : 0;
      va_t MergeTarget =
          inferMergeTarget(IfTarget, IfDest, Else.Target, ElseDest);

      auto IfResult =
          collectStmtsForTarget(Func.Body, AM, IfTarget, MergeTarget);

      std::vector<HighStmt> ElseBody;
      size_t InlineStart = NextI;
      size_t InlineEnd = NextI;

      if (Else.GotoIdx > NextI) {
        for (size_t K = NextI; K <= Else.GotoIdx; ++K)
          ElseBody.push_back(Func.Body[K]);
        InlineEnd = Else.GotoIdx + 1;
      } else if (Else.Target != 0) {
        auto ElseResult =
            collectStmtsForTarget(Func.Body, AM, Else.Target, MergeTarget);
        ElseBody = std::move(ElseResult.Stmts);
        InlineEnd = Else.GotoIdx + 1;
      }

      if (IfResult.Stmts.empty() && ElseBody.empty())
        continue;

      trimMergeGoto(IfResult.Stmts, MergeTarget);
      trimMergeGoto(ElseBody, MergeTarget);

      if (!ElseBody.empty()) {
        Stmt.Kind = StmtKind::IfElse;
        Stmt.Body = std::move(IfResult.Stmts);
        Stmt.ElseBody = std::move(ElseBody);
      } else {
        Stmt.Body = std::move(IfResult.Stmts);
      }

      // Erase inlined ranges (largest-first to preserve indices).
      std::vector<std::pair<size_t, size_t>> Ranges;
      if (IfResult.Start != SIZE_MAX && IfResult.End != SIZE_MAX &&
          IfResult.Start > static_cast<size_t>(I))
        Ranges.push_back({IfResult.Start, IfResult.End});
      if (InlineEnd > InlineStart)
        Ranges.push_back({InlineStart, InlineEnd});

      std::sort(Ranges.begin(), Ranges.end(),
                [](auto &A, auto &B) { return A.first > B.first; });
      for (auto &[Start, End] : Ranges) {
        if (End <= Func.Body.size())
          Func.Body.erase(Func.Body.begin() + static_cast<long>(Start),
                          Func.Body.begin() + static_cast<long>(End));
      }

      Changed = true;
    }
  }
}

} // namespace neverd
