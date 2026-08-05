//===- HighCFSimplify.cpp - Control-flow simplification for HighIR ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Simplifies the flat HighIR statement list into structured control
/// flow: if/else structuring, goto elimination, and store-goto to
/// return conversion.
///
/// Loop detection and switch recovery are in separate files:
///   HighLoopRecovery.cpp     — backward-goto → while-loop conversion
///   HighIfChainToSwitch.cpp  — if-chain → switch recovery
///
//===----------------------------------------------------------------------===//

#include "HighCFSimplifyDetail.h"

#include "neverd/ir/high/MedToHigh.h"

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
// Store-goto to return conversion
//===----------------------------------------------------------------------===//

static void convertStoreGotoToReturn(std::vector<HighStmt> &Stmts,
                                     const std::set<va_t> &LoopHeaders,
                                     const std::set<va_t> &LiveTargets) {
  auto TryConvert = [&](std::vector<HighStmt> &Body) {
    if (Body.size() < 2)
      return;
    if (Body.back().Kind != StmtKind::Goto)
      return;
    va_t GotoTarget = Body.back().GotoTarget;
    if (GotoTarget == 0 || GotoTarget == InvalidVA)
      return;
    if (LoopHeaders.count(GotoTarget))
      return;
    if (LiveTargets.count(GotoTarget))
      return;
    if (Body[Body.size() - 2].Kind != StmtKind::Store)
      return;
    auto &StoreStmt = Body[Body.size() - 2];
    HighStmt RetStmt;
    RetStmt.Kind = StmtKind::Return;
    RetStmt.Addr = StoreStmt.Addr;
    RetStmt.RetVal = StoreStmt.StoreVal;
    Body.pop_back();
    Body.pop_back();
    Body.push_back(std::move(RetStmt));
  };

  for (auto &S : Stmts) {
    if (S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse) {
      TryConvert(S.Body);
      TryConvert(S.ElseBody);
    }
    if (!S.Body.empty())
      convertStoreGotoToReturn(S.Body, LoopHeaders, LiveTargets);
    if (!S.ElseBody.empty())
      convertStoreGotoToReturn(S.ElseBody, LoopHeaders, LiveTargets);
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

static void structureIfElse(HighFunc &Func, int MaxPasses) {
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

//===----------------------------------------------------------------------===//
// collectLoopHeaders — gather addresses of while-loop heads
//===----------------------------------------------------------------------===//

static void collectLoopHeaders(const std::vector<HighStmt> &Stmts,
                               std::set<va_t> &LoopHeaders) {
  for (auto &S : Stmts) {
    if (S.Kind == StmtKind::While) {
      if (S.Addr != 0)
        LoopHeaders.insert(S.Addr);
      if (S.LoopHeaderAddr != 0)
        LoopHeaders.insert(S.LoopHeaderAddr);
    }
    if (!S.Body.empty())
      collectLoopHeaders(S.Body, LoopHeaders);
    if (!S.ElseBody.empty())
      collectLoopHeaders(S.ElseBody, LoopHeaders);
  }
}

//===----------------------------------------------------------------------===//
// foldStoreGotoAfterIf — if { ... } store X; goto → if-else { return X }
//===----------------------------------------------------------------------===//

static void foldStoreGotoAfterIf(HighFunc &Func) {
  for (size_t I = 0; I + 2 < Func.Body.size(); ++I) {
    auto &Stmt = Func.Body[I];
    if (Stmt.Kind != StmtKind::If && Stmt.Kind != StmtKind::IfElse)
      continue;
    auto &Store = Func.Body[I + 1];
    auto &Go = Func.Body[I + 2];
    if (Store.Kind != StmtKind::Store || Go.Kind != StmtKind::Goto)
      continue;
    if (Stmt.Kind == StmtKind::If)
      Stmt.Kind = StmtKind::IfElse;
    HighStmt RetStmt;
    RetStmt.Kind = StmtKind::Return;
    RetStmt.Addr = Store.Addr;
    RetStmt.RetVal = Store.StoreVal;
    Stmt.ElseBody.push_back(std::move(RetStmt));
    Func.Body.erase(Func.Body.begin() + static_cast<long>(I + 1),
                    Func.Body.begin() + static_cast<long>(I + 3));
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

    if (Cur.Kind == StmtKind::If && Next.Kind == StmtKind::If &&
        exprEquivalent(Cur.Cond, Next.Cond)) {
      for (auto &S : Next.Body)
        Cur.Body.push_back(std::move(S));
      Stmts.erase(Stmts.begin() + static_cast<long>(I + 1));
      continue;
    }

    if (Cur.Kind == StmtKind::IfElse && Next.Kind == StmtKind::IfElse &&
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

//===----------------------------------------------------------------------===//
// simplifyControlFlow -- the main entry point
//===----------------------------------------------------------------------===//

void MedToHighConverter::simplifyControlFlow(HighFunc &Func,
                                             const MedFunc &Med) {
  const bool IsMega = Func.Body.size() > 4000;

  std::unordered_map<va_t, int> AddrToBlock;
  AddrToBlock.reserve(Med.Blocks.size());
  for (auto &Block : Med.Blocks)
    if (!Block.Ops.empty())
      AddrToBlock[Block.Ops.front().Addr] = Block.Id;

  detectAndConvertLoops(Func, AddrToBlock, IsMega);

  if (!IsMega)
    recoverSwitchStatements(Func);

  int IfElseMaxPasses = IsMega ? 0 : (Med.Blocks.size() > 500) ? 3 : 10;
  structureIfElse(Func, IfElseMaxPasses);

  removeTrivialGotos(Func.Body);
  simplifyNestedGotos(Func.Body);

  std::set<va_t> LoopHeaders;
  collectLoopHeaders(Func.Body, LoopHeaders);

  std::set<va_t> LiveTargets;
  for (auto &S : Func.Body) {
    if (S.Kind == StmtKind::While && !S.Body.empty())
      for (auto &WS : S.Body)
        if (WS.Addr != 0)
          LiveTargets.insert(WS.Addr);
  }
  LiveTargets.insert(LoopHeaders.begin(), LoopHeaders.end());

  mergeConsecutiveCondBlocks(Func.Body);
  convertStoreGotoToReturn(Func.Body, LoopHeaders, LiveTargets);
  foldStoreGotoAfterIf(Func);
  inlineGotoReturns(Func, Med);
  cleanupGuardBeforeSwitch(Func);

  Func.Body.erase(
      std::remove_if(Func.Body.begin(), Func.Body.end(),
                     [](const HighStmt &S) { return S.Kind == StmtKind::Nop; }),
      Func.Body.end());
}
} // namespace neverd
