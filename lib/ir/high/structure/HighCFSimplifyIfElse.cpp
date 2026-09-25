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

#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedIR.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neverd {

namespace {

// These facts cover the entire function, including entries from a different
// statement list. Rewrites only remove addresses and references, so retaining
// the original sets during the traversal is conservative.
class ContinuationFolder {
  const MedFunc *Med;
  va_t FunctionEntry = 0;
  std::set<va_t> Targets;
  std::set<va_t> NativeEntries;
  std::unordered_map<va_t, size_t> Owners;
  std::map<va_t, ExprPtr> SimpleReturns;

  static bool plainValue(const ExprPtr &E) {
    return E && (E->Kind == ExprKind::Var || E->Kind == ExprKind::Const) &&
           E->Operands.empty() && E->MemoryOrdering == NdMemoryOrdering::None &&
           E->MemoryAddressSpace == NdMemoryAddressSpace::Default;
  }

  bool removableTransfer(const HighStmt &S) const {
    return S.Kind == StmtKind::Goto && S.GotoTarget &&
           S.GotoTarget != InvalidVA && !Targets.count(S.Addr) &&
           (!S.Addr || S.Addr != FunctionEntry) &&
           (!Med || !S.Addr || S.Addr != Med->Entry) &&
           !NativeEntries.count(S.Addr);
  }

  bool uniqueAddress(va_t Address) const {
    auto It = Owners.find(Address);
    return Address && Address != InvalidVA && It != Owners.end() &&
           It->second == 1;
  }

  bool exactContinuation(const HighStmt &S, va_t Target) const {
    if (!Target || Target == InvalidVA || AddrMap::entryAddress(S) != Target)
      return false;
    if (uniqueAddress(Target))
      return true;
    // A recovered while(true) and its first native instruction can share an
    // address. Entering either executes exactly the same assignment prefix.
    // No other occurrence, later body entry or conditional loop is equivalent.
    if (S.Kind != StmtKind::While || S.LoopHeaderAddr != Target ||
        !plainValue(S.Cond) || S.Cond->Kind != ExprKind::Const ||
        !S.Cond->ConstVal)
      return false;
    size_t PrefixOwners = S.Addr == Target ? 1 : 0;
    for (const auto &Inner : S.Body) {
      if (Inner.Addr != Target)
        break;
      if (Inner.Kind != StmtKind::Assign || !Inner.Body.empty() ||
          !Inner.ElseBody.empty() || !Inner.Cases.empty() ||
          !Inner.DefaultBody.empty() || !Inner.EHClauseBodies.empty())
        return false;
      ++PrefixOwners;
    }
    auto It = Owners.find(Target);
    return PrefixOwners != 0 && It != Owners.end() &&
           It->second == PrefixOwners;
  }

  bool ownsReturn(const HighStmt &Owner, const HighStmt &Return,
                  const std::vector<HighStmt> &Body, size_t Prefix,
                  size_t ReturnIndex) const {
    if (Return.Kind != StmtKind::Return || !uniqueAddress(Return.Addr) ||
        Return.Addr == FunctionEntry || Targets.count(Return.Addr))
      return false;
    for (size_t I = Prefix; I < ReturnIndex; ++I) {
      const auto &Label = Body[I];
      if (Label.Kind != StmtKind::Block || !Label.Body.empty() ||
          !Label.ElseBody.empty() || !Label.Cases.empty() ||
          !Label.DefaultBody.empty() || !Label.EHClauseBodies.empty() ||
          !uniqueAddress(Label.Addr) || Label.Addr == FunctionEntry ||
          Targets.count(Label.Addr))
        return false;
    }
    if (!Med)
      return true;

    // DCE can erase the native block's entry assignments. Check the original
    // block, not just the surviving return, for another incoming edge.
    const MedBlock *Block = nullptr;
    for (const auto &Candidate : Med->Blocks) {
      if (Candidate.Ops.empty() ||
          Candidate.Ops.back().Opcode != NdOp::RETURN ||
          Candidate.Ops.back().Addr != Return.Addr)
        continue;
      if (Block)
        return false;
      Block = &Candidate;
    }
    if (!Block || Block->Preds.size() != 1 || !Block->Succs.empty() ||
        Block->Id < 0 || static_cast<size_t>(Block->Id) >= Med->Blocks.size() ||
        &Med->Blocks[Block->Id] != Block || Targets.count(Block->StartAddr) ||
        (FunctionEntry && (Block->StartAddr == FunctionEntry ||
                           Block->Ops.front().Addr == FunctionEntry)) ||
        (Med->Entry &&
         (Block->StartAddr == Med->Entry ||
          Block->Ops.front().Addr == Med->Entry || Return.Addr == Med->Entry)))
      return false;
    for (const auto &Op : Block->Ops)
      if (Targets.count(Op.Addr) ||
          (FunctionEntry && Op.Addr == FunctionEntry) ||
          (Med->Entry && Op.Addr == Med->Entry))
        return false;
    // DCE deliberately retains empty entry labels. They can move with the
    // return only when they belong to this same single-predecessor block;
    // adjacency alone cannot authorize moving a different native entry.
    for (size_t I = Prefix; I < ReturnIndex; ++I) {
      const va_t Address = Body[I].Addr;
      if (Address == Med->Entry ||
          (Address != Block->StartAddr &&
           std::none_of(Block->Ops.begin(), Block->Ops.end(),
                        [&](const MedOp &Op) { return Op.Addr == Address; })))
        return false;
    }
    const int PredId = Block->Preds.front();
    if (PredId < 0 || static_cast<size_t>(PredId) >= Med->Blocks.size())
      return false;
    const auto &Pred = Med->Blocks[PredId];
    return Pred.Id == PredId && !Pred.Ops.empty() &&
           Pred.Ops.back().Opcode == NdOp::COND_BR &&
           Pred.Ops.back().Addr == Owner.Addr && uniqueAddress(Owner.Addr) &&
           std::find(Pred.Succs.begin(), Pred.Succs.end(), Block->Id) !=
               Pred.Succs.end();
  }

  void fold(std::vector<HighStmt> &Body, unsigned Depth) {
    if (Depth > 64)
      return;
    for (size_t I = 0; I < Body.size(); ++I) {
      auto &S = Body[I];
      fold(S.Body, Depth + 1);
      fold(S.ElseBody, Depth + 1);
      for (auto &Case : S.Cases)
        fold(Case.Body, Depth + 1);
      fold(S.DefaultBody, Depth + 1);

      if (S.Kind == StmtKind::Goto) {
        auto It = SimpleReturns.find(S.GotoTarget);
        if (It != SimpleReturns.end()) {
          HighStmt Return;
          Return.Kind = StmtKind::Return;
          Return.Addr = S.Addr;
          Return.RetVal = It->second;
          S = std::move(Return);
        }
        continue;
      }
      if (S.Kind != StmtKind::IfElse || !S.Cond)
        continue;

      if (!S.Body.empty() && !S.ElseBody.empty() &&
          removableTransfer(S.Body.back()) &&
          removableTransfer(S.ElseBody.back()) &&
          S.Body.back().GotoTarget == S.ElseBody.back().GotoTarget) {
        HighStmt Transfer;
        Transfer.Kind = StmtKind::Goto;
        Transfer.GotoTarget = S.Body.back().GotoTarget;
        S.Body.pop_back();
        S.ElseBody.pop_back();
        // Hoist one transfer without moving or duplicating its destination.
        Body.insert(Body.begin() + I + 1, std::move(Transfer));
        continue; // The insertion may invalidate S.
      }

      size_t ReturnIndex = I + 1;
      while (ReturnIndex < Body.size() &&
             Body[ReturnIndex].Kind == StmtKind::Block &&
             Body[ReturnIndex].Body.empty())
        ++ReturnIndex;
      if (ReturnIndex + 1 >= Body.size() ||
          !ownsReturn(S, Body[ReturnIndex], Body, I + 1, ReturnIndex))
        continue;
      auto *Taken = &S.Body;
      auto *Fallthrough = &S.ElseBody;
      if (S.Body.empty())
        std::swap(Taken, Fallthrough);
      if (Taken->empty() || !Fallthrough->empty() ||
          !removableTransfer(Taken->back()) ||
          !exactContinuation(Body[ReturnIndex + 1], Taken->back().GotoTarget))
        continue;
      // if (c) { ...; goto tail; } return r; tail:
      // becomes if (c) { ...; } else { return r; } tail:
      // The condition is evaluated once and the shared tail stays in place.
      Taken->pop_back();
      for (size_t J = I + 1; J <= ReturnIndex; ++J)
        Fallthrough->push_back(std::move(Body[J]));
      Body.erase(Body.begin() + I + 1, Body.begin() + ReturnIndex + 1);
    }
  }

public:
  explicit ContinuationFolder(const MedFunc *Med) : Med(Med) {}

  void run(HighFunc &Func) {
    FunctionEntry = Func.Entry;
    auto HasLanguageEH = [](const auto &Metadata) {
      return Metadata && (Metadata->hasLanguageTable() ||
                          Metadata->Personality != ExceptionPersonality::None ||
                          Metadata->PersonalityVA || Metadata->HandlerDataVA);
    };
    if (HasLanguageEH(Func.ExceptionMetadata) ||
        (Med && HasLanguageEH(Med->ExceptionMetadata)))
      return;
    bool HasEH = false;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Addr && S.Addr != InvalidVA)
        ++Owners[S.Addr];
      if (S.Kind == StmtKind::Goto && S.GotoTarget && S.GotoTarget != InvalidVA)
        Targets.insert(S.GotoTarget);
      HasEH |= S.Kind == StmtKind::SEHTry || S.Kind == StmtKind::CxxTry ||
               S.Kind == StmtKind::ItaniumTry || !S.EHClauseBodies.empty() ||
               !S.EHClauses.empty();
    });
    if (Med)
      for (const auto &Block : Med->Blocks) {
        HasEH |=
            !Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty();
        const va_t Start =
            Block.StartAddr ? Block.StartAddr
                            : (Block.Ops.empty() ? 0 : Block.Ops.front().Addr);
        if (Start && Start != InvalidVA)
          NativeEntries.insert(Start);
      }
    if (HasEH)
      return;

    // A dead entry assignment may leave an empty label immediately before a
    // return. Only a root-level, exact, unique label and a scalar value can be
    // forwarded: no statement, load, call or cleanup is skipped or duplicated.
    for (size_t I = 0; I + 1 < Func.Body.size(); ++I) {
      const auto &Label = Func.Body[I];
      const auto &Return = Func.Body[I + 1];
      if (Label.Kind == StmtKind::Block && Label.Body.empty() &&
          Label.ElseBody.empty() && Label.Cases.empty() &&
          Label.DefaultBody.empty() && uniqueAddress(Label.Addr) &&
          Return.Kind == StmtKind::Return && plainValue(Return.RetVal))
        SimpleReturns.emplace(Label.Addr, Return.RetVal);
    }
    fold(Func.Body, 0);
  }
};

} // namespace

void foldStructuredContinuations(HighFunc &Func, const MedFunc *Med) {
  ContinuationFolder(Med).run(Func);
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
                                     size_t NextI, va_t IfTarget,
                                     size_t TargetIndex) {
  ElseTargetInfo Info;
  if (Body[NextI].Kind == StmtKind::Goto) {
    Info.Target = Body[NextI].GotoTarget;
    Info.GotoIdx = NextI;
    return Info;
  }

  for (size_t K = NextI; K < Body.size() && K != TargetIndex; ++K) {
    auto &S = Body[K];
    if (S.Kind == StmtKind::While || S.Kind == StmtKind::If)
      break;
    if (S.Kind == StmtKind::Goto) {
      Info.Target = S.GotoTarget;
      Info.GotoIdx = K;
      break;
    }
    bool IsFallthrough = S.IsPhiCopy || (S.Addr != 0 && S.Addr < IfTarget) ||
                         S.Kind == StmtKind::Return;
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
  size_t Start = SIZE_MAX, End = SIZE_MAX;
};

static size_t findTargetIndex(AddrMap &AM, va_t Target) {
  auto It = AM.Idx.find(Target);
  if (It != AM.Idx.end())
    return It->second;
  AM.ensureSorted();
  auto LB = std::lower_bound(AM.Sorted.begin(), AM.Sorted.end(),
                             std::make_pair(Target, size_t(0)));
  return LB != AM.Sorted.end() && LB->first - Target <= 16 ? LB->second
                                                           : SIZE_MAX;
}

/// Locate a run without copying its nested statement trees. Ownership must be
/// established before moving any of its statements into a branch.
static StmtCollectResult
collectStmtsForTarget(const std::vector<HighStmt> &Body, AddrMap &AM,
                      va_t Target, va_t Merge) {
  StmtCollectResult Result;
  const size_t StartIdx = findTargetIndex(AM, Target);
  if (StartIdx == SIZE_MAX)
    return Result;
  Result.Start = StartIdx;
  for (size_t K = StartIdx; K < Body.size(); ++K) {
    auto &S = Body[K];
    const va_t Entry = AddrMap::entryAddress(S);
    if (Merge != 0 && Entry != 0 && Entry >= Merge)
      break;
    Result.End = K + 1;
    if (S.Kind == StmtKind::Return || S.Kind == StmtKind::Goto)
      break;
  }
  return Result;
}

static bool endsWithTransfer(const HighStmt &S) {
  return S.Kind == StmtKind::Goto || S.Kind == StmtKind::Return ||
         S.Kind == StmtKind::Break || S.Kind == StmtKind::Continue;
}

template <typename F> static void walkStatementTree(const HighStmt &S, F &&Fn) {
  Fn(S);
  walkStmts(S.Body, Fn);
  walkStmts(S.ElseBody, Fn);
  for (const auto &Case : S.Cases)
    walkStmts(Case.Body, Fn);
  walkStmts(S.DefaultBody, Fn);
  for (const auto &Clause : S.EHClauseBodies)
    walkStmts(Clause, Fn);
}

/// Moving a shared label into one branch would hide another incoming edge.
/// Preserve its original goto instead of cloning and retaining the target tree.
static bool ownsRun(const std::vector<HighStmt> &Body, AddrMap &AM,
                    StmtCollectResult Run, size_t Owner, const MedFunc *Med) {
  if (Run.Start == SIZE_MAX || Run.End == SIZE_MAX || Run.Start <= Owner ||
      Run.Start >= Run.End)
    return false;
  if (Run.Start != Owner + 1 && !endsWithTransfer(Body[Run.Start - 1]))
    return false;

  auto Contains = [&](va_t Address) {
    const size_t Index = findTargetIndex(AM, Address);
    return Index >= Run.Start && Index < Run.End;
  };
  for (size_t K = 0; K < Body.size(); ++K) {
    if (K == Owner || (K >= Run.Start && K < Run.End))
      continue;
    bool HasEntry = false;
    auto Check = [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Goto && Contains(S.GotoTarget))
        HasEntry = true;
    };
    walkStatementTree(Body[K], Check);
    if (HasEntry)
      return false;
  }
  if (Med) {
    // A loop header can have several incoming edges wholly inside this run.
    // Require exact current ownership of each predecessor's last operation;
    // an absent or externally repeated address cannot prove an internal edge.
    std::unordered_map<va_t, unsigned> AddressOwners;
    bool HaveAddressOwners = false;
    auto OwnsPredecessor = [&](int PredId, const MedBlock &Block,
                               bool IsEntry) {
      if (PredId < 0 || static_cast<size_t>(PredId) >= Med->Blocks.size())
        return false;
      const auto &Pred = Med->Blocks[PredId];
      if (Pred.Id != PredId || Pred.Ops.empty() ||
          std::find(Pred.Succs.begin(), Pred.Succs.end(), Block.Id) ==
              Pred.Succs.end())
        return false;
      const va_t Address = Pred.Ops.back().Addr;
      if (!Address || Address == InvalidVA)
        return false;
      if (!HaveAddressOwners) {
        for (size_t K = 0; K < Body.size(); ++K) {
          const unsigned Scope = K >= Run.Start && K < Run.End ? 1
                                 : K == Owner                  ? 2
                                                               : 4;
          walkStatementTree(Body[K], [&](const HighStmt &S) {
            if (S.Addr && S.Addr != InvalidVA)
              AddressOwners[S.Addr] |= Scope;
          });
        }
        HaveAddressOwners = true;
      }
      auto It = AddressOwners.find(Address);
      return It != AddressOwners.end() &&
             (It->second == 1 ||
              (IsEntry && Address == Body[Owner].Addr && It->second == 2));
    };
    for (const auto &Block : Med->Blocks) {
      const va_t Start = Block.StartAddr
                             ? Block.StartAddr
                             : (Block.Ops.empty() ? 0 : Block.Ops.front().Addr);
      if (!Contains(Start))
        continue;
      if (!Block.ExceptionalPreds.empty())
        return false;
      if (Block.Preds.size() > 1) {
        if (Block.Id < 0 ||
            static_cast<size_t>(Block.Id) >= Med->Blocks.size() ||
            &Med->Blocks[Block.Id] != &Block)
          return false;
        const bool IsEntry = findTargetIndex(AM, Start) == Run.Start;
        for (int Pred : Block.Preds)
          if (!OwnsPredecessor(Pred, Block, IsEntry))
            return false;
      }
    }
  }
  return true;
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

void structureIfElse(HighFunc &Func, int MaxPasses, const MedFunc *Med) {
  AddrMap AM;
  bool Changed = true;
  int Pass = 0;
  while (Changed && Pass++ < MaxPasses) {
    Changed = false;
    AM.rebuild(Func.Body);
    std::set<va_t> LiveTargets;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Goto)
        LiveTargets.insert(S.GotoTarget);
    });

    for (int I = static_cast<int>(Func.Body.size()) - 1; I >= 0; --I) {
      auto &Stmt = Func.Body[I];
      if (Stmt.Kind == StmtKind::IfElse && !Stmt.Body.empty() &&
          !Stmt.ElseBody.empty() && Stmt.Body.back().Kind == StmtKind::Goto &&
          Stmt.ElseBody.back().Kind == StmtKind::Goto) {
        const va_t Target = Stmt.Body.back().GotoTarget;
        if (!Target || Target == InvalidVA ||
            Stmt.ElseBody.back().GotoTarget != Target)
          continue;
        AM.rebuild(Func.Body);
        const size_t TargetIndex = findTargetIndex(AM, Target);
        if (TargetIndex == SIZE_MAX || TargetIndex <= static_cast<size_t>(I))
          continue;
        auto HasEntry = [&](va_t Address) {
          if (!Address || Address == InvalidVA)
            return false;
          if (LiveTargets.count(Address))
            return true;
          if (Med)
            for (const auto &Block : Med->Blocks) {
              const va_t Start =
                  Block.StartAddr
                      ? Block.StartAddr
                      : (Block.Ops.empty() ? 0 : Block.Ops.front().Addr);
              if (Start == Address)
                return true;
              for (const auto &Edge : Block.ExceptionalPreds)
                if (Edge.TargetVA == Address)
                  return true;
            }
          return false;
        };
        if (HasEntry(Stmt.Body.back().Addr) ||
            HasEntry(Stmt.ElseBody.back().Addr))
          continue;
        // Make the common transfer visible to an enclosing flat conditional.
        // Neither removed transfer owns a live label or native block entry.
        Stmt.Body.pop_back();
        Stmt.ElseBody.pop_back();
        if (TargetIndex != static_cast<size_t>(I) + 1) {
          HighStmt Transfer;
          Transfer.Kind = StmtKind::Goto;
          Transfer.GotoTarget = Target;
          Func.Body.insert(Func.Body.begin() + I + 1, std::move(Transfer));
        }
        Changed = true;
        continue;
      }
      if (Stmt.Kind != StmtKind::If)
        continue;
      if (Stmt.Body.empty() || Stmt.Body.back().Kind != StmtKind::Goto ||
          !std::all_of(Stmt.Body.begin(), Stmt.Body.end() - 1,
                       [](const HighStmt &S) { return S.IsPhiCopy; }))
        continue;
      std::vector<HighStmt> TakenCopies(Stmt.Body.begin(), Stmt.Body.end() - 1);

      // Earlier folds in this same pass may erase statements after I.
      // Address ownership must describe the current list, not stale indices.
      AM.rebuild(Func.Body);
      va_t IfTarget = Stmt.Body.back().GotoTarget;
      if (IfTarget == 0 || IfTarget == InvalidVA)
        continue;

      size_t NextI = static_cast<size_t>(I) + 1;
      if (NextI >= Func.Body.size())
        continue;

      // A block can start before its first surviving HighIR statement (for
      // example COPY return-register, RET). Its own return is never part of
      // the conditional's fallthrough arm.
      const size_t TargetIndex = findTargetIndex(AM, IfTarget);
      if (TargetIndex == SIZE_MAX || TargetIndex <= static_cast<size_t>(I))
        continue;
      auto Else = findElseTarget(Func.Body, NextI, IfTarget, TargetIndex);

      // Early-return fold.
      if (TakenCopies.empty() && Else.HasEarlyReturn && Else.Target == 0 &&
          Else.ReturnIdx != SIZE_MAX &&
          Else.FallthroughIndices.size() == Else.ReturnIdx - NextI + 1 &&
          ownsRun(Func.Body, AM, {NextI, Else.ReturnIdx + 1}, I, Med)) {
        // The complete false-edge prefix, including its PHI copies, belongs
        // before this return. Shared entries must retain their original scope.
        Stmt.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Stmt.Cond);
        Stmt.Body.clear();
        for (size_t Idx : Else.FallthroughIndices)
          Stmt.Body.push_back(std::move(Func.Body[Idx]));
        for (auto It = Else.FallthroughIndices.rbegin();
             It != Else.FallthroughIndices.rend(); ++It)
          Func.Body.erase(Func.Body.begin() + static_cast<long>(*It));
        // The taken edge still reaches its target: keep the transfer unless
        // the target is now the very next statement.
        const size_t After = static_cast<size_t>(I) + 1;
        if (After >= Func.Body.size() ||
            AddrMap::entryAddress(Func.Body[After]) != IfTarget) {
          HighStmt Transfer;
          Transfer.Kind = StmtKind::Goto;
          Transfer.GotoTarget = IfTarget;
          Func.Body.insert(Func.Body.begin() + static_cast<long>(After),
                           std::move(Transfer));
        }
        Changed = true;
        continue;
      }

      // Same-target fold: both branches goto the same address.
      if (IfTarget == Else.Target) {
        if (!ownsRun(Func.Body, AM, {NextI, Else.GotoIdx + 1}, I, Med))
          continue;
        // Both arms reach this target, but it need not be their physical
        // continuation: another branch can own statements between the common
        // transfer and its target. Keep that transfer unless the next emitted
        // statement is exactly the target; never fall through the intervening
        // code merely because both arms agree where to jump.
        const bool TargetFollows =
            Else.GotoIdx + 1 < Func.Body.size() &&
            AddrMap::entryAddress(Func.Body[Else.GotoIdx + 1]) == IfTarget;
        if (TakenCopies.empty()) {
          Stmt.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Stmt.Cond);
          Stmt.Body.clear();
          for (size_t K = NextI; K < Else.GotoIdx; ++K)
            Stmt.Body.push_back(std::move(Func.Body[K]));
        } else {
          Stmt.Kind = StmtKind::IfElse;
          Stmt.Body = std::move(TakenCopies);
          for (size_t K = NextI; K < Else.GotoIdx; ++K)
            Stmt.ElseBody.push_back(std::move(Func.Body[K]));
        }
        Func.Body.erase(Func.Body.begin() + static_cast<long>(NextI),
                        Func.Body.begin() +
                            static_cast<long>(Else.GotoIdx + TargetFollows));
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
      const size_t InlineEnd = Else.Target != 0 ? Else.GotoIdx + 1 : NextI;
      auto FoldSharedTail = [&]() {
        if (TargetIndex > NextI &&
            !ownsRun(Func.Body, AM, {NextI, TargetIndex}, I, Med))
          return false;
        // Both arms now continue at the original target. Only the exclusive
        // false prefix moves; shared tail labels and statements stay in place.
        std::vector<HighStmt> FalseBody;
        for (size_t K = NextI; K < TargetIndex; ++K)
          FalseBody.push_back(std::move(Func.Body[K]));
        Stmt.Kind = FalseBody.empty() ? StmtKind::If : StmtKind::IfElse;
        Stmt.Body = std::move(TakenCopies);
        Stmt.ElseBody = std::move(FalseBody);
        Func.Body.erase(Func.Body.begin() + static_cast<long>(NextI),
                        Func.Body.begin() + static_cast<long>(TargetIndex));
        return true;
      };
      // The immediately following target also belongs to the false edge.
      if (IfResult.Start == NextI ||
          !ownsRun(Func.Body, AM, IfResult, I, Med) ||
          (InlineEnd > NextI &&
           (!ownsRun(Func.Body, AM, {NextI, InlineEnd}, I, Med) ||
            (IfResult.Start < InlineEnd && IfResult.End > NextI)))) {
        Changed |= FoldSharedTail();
        continue;
      }

      // A run stopped at the merge still has a fallthrough successor. Moving
      // it must preserve that edge even when the merge is not adjacent to I.
      // PHI copies belong to the fallthrough edge, not to an instruction
      // entry. Their address can be the loop latch still inside the moved run;
      // naming it would reenter the latch or create a second label for it.
      const bool NeedsSuccessor =
          !endsWithTransfer(Func.Body[IfResult.End - 1]);
      const va_t Successor =
          IfResult.End < Func.Body.size()
              ? AddrMap::entryAddress(Func.Body[IfResult.End])
              : 0;
      if (NeedsSuccessor && (!Successor || Func.Body[IfResult.End].IsPhiCopy)) {
        Changed |= FoldSharedTail();
        continue;
      }

      std::vector<std::pair<size_t, size_t>> Ranges{
          {IfResult.Start, IfResult.End}};
      if (InlineEnd > NextI)
        Ranges.push_back({NextI, InlineEnd});
      size_t Continuation = NextI;
      for (size_t N = 0; N < Ranges.size(); ++N)
        for (auto [Start, End] : Ranges)
          if (Continuation >= Start && Continuation < End)
            Continuation = End;
      const bool MergeIsContinuation =
          MergeTarget != 0 && Continuation < Func.Body.size() &&
          findTargetIndex(AM, MergeTarget) == Continuation;

      std::vector<HighStmt> IfBody = std::move(TakenCopies);
      for (size_t K = IfResult.Start; K < IfResult.End; ++K)
        IfBody.push_back(std::move(Func.Body[K]));
      if (NeedsSuccessor) {
        HighStmt Transfer;
        Transfer.Kind = StmtKind::Goto;
        Transfer.GotoTarget = Successor;
        LiveTargets.insert(Transfer.GotoTarget);
        IfBody.push_back(std::move(Transfer));
      }
      std::vector<HighStmt> ElseBody;
      for (size_t K = NextI; K < InlineEnd; ++K)
        ElseBody.push_back(std::move(Func.Body[K]));
      if (MergeIsContinuation) {
        trimMergeGoto(IfBody, MergeTarget);
        trimMergeGoto(ElseBody, MergeTarget);
      }
      if (!ElseBody.empty())
        Stmt.Kind = StmtKind::IfElse;
      Stmt.Body = std::move(IfBody);
      Stmt.ElseBody = std::move(ElseBody);

      // Erase moved ranges largest-first; the preflight proved disjointness.
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
