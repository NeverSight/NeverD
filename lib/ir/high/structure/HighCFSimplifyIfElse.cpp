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

#include "neverd/Limits.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/support/Diagnostic.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
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

static bool bodyIsSkipGoto(const std::vector<HighStmt> &Body);
static bool isValueAssign(const HighStmt &S, MedVar &Dest, ExprPtr &Val);
static void dropDuplicateSkipGotosNested(std::vector<HighStmt> &Body);
static void foldJoinValueGotoChainNested(std::vector<HighStmt> &Body);
static void invertEmptyThenAssignCallNested(std::vector<HighStmt> &Body);
static void invertExternalSkipGotoNested(std::vector<HighStmt> &Body);
static void foldSameTargetSkipGotoNested(std::vector<HighStmt> &Body);
static void flattenElseGotoNextLabelNested(std::vector<HighStmt> &Body,
                                           va_t FallthroughTarget = 0);
static void foldThenSkipOverNested(std::vector<HighStmt> &Body);
static void inlineSmallExclusiveJoinNested(std::vector<HighStmt> &Body,
                                           std::vector<HighStmt> &Root);
static void dropImpliedInnerCondsNested(std::vector<HighStmt> &Body);
static void dropImpliedFallthroughCondsNested(std::vector<HighStmt> &Body);
static void preferPositiveIfElseNested(std::vector<HighStmt> &Body);
static bool rewriteCallResultUndefStores(std::vector<HighStmt> &Body);
static bool rewriteGuardedCallThis(std::vector<HighStmt> &Body);
static void hoistIdenticalIfElseSuffixesNested(std::vector<HighStmt> &Body);

static bool exprHasCall(const HighExpr *E) {
  if (!E)
    return false;
  if (E->Kind == ExprKind::Call)
    return true;
  for (const auto &Op : E->Operands)
    if (exprHasCall(Op.get()))
      return true;
  return false;
}

static bool stmtIsSkipResidue(const HighStmt &S) {
  if (S.Kind == StmtKind::Nop || S.Kind == StmtKind::Block)
    return true;
  if (S.IsPhiCopy)
    return true;
  MedVar Dest;
  ExprPtr Val;
  return isValueAssign(S, Dest, Val) && Val && Val->Kind == ExprKind::Undef;
}

static bool bodyIsSkipArm(const std::vector<HighStmt> &Body) {
  if (bodyIsSkipGoto(Body))
    return true;
  return std::all_of(Body.begin(), Body.end(), stmtIsSkipResidue);
}

static size_t findSkipWorkEnd(const std::vector<HighStmt> &Body, size_t Begin) {
  size_t End = Begin;
  for (; End < Body.size() && End - Begin <= 32; ++End) {
    const StmtKind Kind = Body[End].Kind;
    if (Kind == StmtKind::Store || Kind == StmtKind::If ||
        Kind == StmtKind::IfElse || Kind == StmtKind::CxxTry ||
        Kind == StmtKind::SEHTry || Kind == StmtKind::ItaniumTry ||
        Kind == StmtKind::While || Kind == StmtKind::Goto ||
        Kind == StmtKind::Return)
      break;
  }
  return End;
}

static bool rangeHasObservableWork(const std::vector<HighStmt> &Body,
                                   size_t Begin, size_t End) {
  for (size_t K = Begin; K < End; ++K) {
    const HighStmt &S = Body[K];
    if (S.Kind == StmtKind::Call || S.Kind == StmtKind::If ||
        S.Kind == StmtKind::IfElse || S.Kind == StmtKind::Store ||
        S.Kind == StmtKind::CxxTry || S.Kind == StmtKind::SEHTry ||
        S.Kind == StmtKind::While)
      return true;
    if (S.Kind == StmtKind::Assign && S.Val && S.Val->Kind == ExprKind::Call)
      return true;
  }
  return false;
}

static size_t findSkipTargetInList(const std::vector<HighStmt> &Body, size_t After,
                                   va_t Target) {
  if (!Target || Target == InvalidVA)
    return SIZE_MAX;
  for (size_t K = After; K < Body.size(); ++K) {
    const va_t Address = AddrMap::entryAddress(Body[K]);
    if (Address == Target || (Address && Address >= Target && Address < Target + 32))
      return K;
  }
  return SIZE_MAX;
}

static bool invertSkipGotosIn(std::vector<HighStmt> &Body) {
  bool Changed = false;
  for (HighStmt &S : Body) {
    Changed |= invertSkipGotosIn(S.Body);
    Changed |= invertSkipGotosIn(S.ElseBody);
    for (auto &Clause : S.EHClauseBodies)
      Changed |= invertSkipGotosIn(Clause);
    Changed |= invertSkipGotosIn(S.DefaultBody);
    for (auto &Case : S.Cases)
      Changed |= invertSkipGotosIn(Case.Body);
  }
  for (int I = 0; I < static_cast<int>(Body.size()); ++I) {
    HighStmt &Stmt = Body[I];
    if ((Stmt.Kind != StmtKind::If && Stmt.Kind != StmtKind::IfElse) ||
        !Stmt.Cond || exprHasCall(Stmt.Cond.get()) || !bodyIsSkipArm(Stmt.Body))
      continue;
    const size_t NextI = static_cast<size_t>(I) + 1;
    size_t TargetIndex = SIZE_MAX;
    if (bodyIsSkipGoto(Stmt.Body)) {
      const va_t Target = Stmt.Body.back().GotoTarget;
      TargetIndex = findSkipTargetInList(Body, NextI, Target);
      if (TargetIndex == SIZE_MAX || TargetIndex <= NextI ||
          TargetIndex - NextI > 32)
        continue;
      bool SkipIsElseGoto = false;
      for (size_t K = NextI; K < TargetIndex; ++K) {
        if (Body[K].Kind == StmtKind::Goto && Body[K].GotoTarget &&
            Body[K].GotoTarget != InvalidVA && Body[K].GotoTarget != Target)
          SkipIsElseGoto = true;
      }
      if (SkipIsElseGoto)
        continue;
    } else if (Stmt.Body.empty()) {
      // `if (c) {} assign_call(); later_call();` — take only the first
      // assign-call. findSkipWorkEnd would swallow later_call (GetData)
      // or, if tightened, drop a throw ctor that sits after the call.
      size_t K = NextI;
      while (K < Body.size() &&
             (Body[K].Kind == StmtKind::Nop ||
              (Body[K].Kind == StmtKind::Block && Body[K].Body.empty())))
        ++K;
      if (K < Body.size() && K - NextI <= 8 &&
          Body[K].Kind == StmtKind::Assign && Body[K].Val &&
          Body[K].Val->Kind == ExprKind::Call)
        TargetIndex = K + 1;
      else
        TargetIndex = findSkipWorkEnd(Body, NextI);
      if (TargetIndex <= NextI || TargetIndex - NextI > 32)
        continue;
    } else {
      TargetIndex = findSkipWorkEnd(Body, NextI);
      if (TargetIndex <= NextI || TargetIndex - NextI > 32)
        continue;
    }
    if (!rangeHasObservableWork(Body, NextI, TargetIndex))
      continue;
    Stmt.Kind = StmtKind::If;
    Stmt.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Stmt.Cond);
    Stmt.ElseBody.clear();
    Stmt.Body.clear();
    for (size_t K = NextI; K < TargetIndex; ++K)
      Stmt.Body.push_back(std::move(Body[K]));
    Body.erase(Body.begin() + static_cast<long>(NextI),
               Body.begin() + static_cast<long>(TargetIndex));
    Changed = true;
  }
  return Changed;
}

static bool isCleanupOnlyCxxTry(const HighStmt &S) {
  if (S.Kind != StmtKind::CxxTry || S.EHClauses.empty())
    return false;
  for (const HighEHClause &C : S.EHClauses)
    if (C.Kind != HighEHClauseKind::CxxCleanup)
      return false;
  return true;
}

static void invertSkipGotosGuarded(std::vector<HighStmt> &Body) {
  for (HighStmt &S : Body) {
    // Cleanup-only `__wind` wraps object lifetime, not a catch diamond.
    // Running skip-goto invert on that body pulls later calls (GetData)
    // into a preceding `if` once a parent live-range wraps the function.
    if (S.Kind == StmtKind::SEHTry || S.Kind == StmtKind::ItaniumTry ||
        (S.Kind == StmtKind::CxxTry && !isCleanupOnlyCxxTry(S))) {
      invertSkipGotosIn(S.Body);
      for (auto &Clause : S.EHClauseBodies)
        invertSkipGotosIn(Clause);
    }
    invertSkipGotosGuarded(S.Body);
    invertSkipGotosGuarded(S.ElseBody);
    invertSkipGotosGuarded(S.DefaultBody);
    for (auto &Case : S.Cases)
      invertSkipGotosGuarded(Case.Body);
    for (auto &Clause : S.EHClauseBodies)
      invertSkipGotosGuarded(Clause);
  }
}

void invertSkipGotos(HighFunc &Func) {
  // Exception wrap builds try bodies after structureIfElse. Drop sibling
  // skip-gotos first; invert would nest the second skip into the first.
  dropDuplicateSkipGotosNested(Func.Body);
  foldJoinValueGotoChainNested(Func.Body);
  // Empty `if (c) {} assign_call();` is often outside the EH wrap. Skip-goto
  // invert stays try-only.
  invertEmptyThenAssignCallNested(Func.Body);
  invertExternalSkipGotoNested(Func.Body);
  foldSameTargetSkipGotoNested(Func.Body);
  flattenElseGotoNextLabelNested(Func.Body);
  foldThenSkipOverNested(Func.Body);
  inlineSmallExclusiveJoinNested(Func.Body, Func.Body);
  invertSkipGotosGuarded(Func.Body);
  dropImpliedInnerCondsNested(Func.Body);
  dropImpliedFallthroughCondsNested(Func.Body);
  // Peeling `!p || !IsKind || !q` down to `!q` can expose a skip-goto that
  // invertExternal already missed. Fold that now so later work stays reachable.
  invertExternalSkipGotoNested(Func.Body);
  flattenElseGotoNextLabelNested(Func.Body);
  foldThenSkipOverNested(Func.Body);
  inlineSmallExclusiveJoinNested(Func.Body, Func.Body);
  preferPositiveIfElseNested(Func.Body);
  rewriteCallResultUndefStores(Func.Body);
  rewriteGuardedCallThis(Func.Body);
  hoistIdenticalIfElseSuffixesNested(Func.Body);
}

// Keep a no-op hook so session-destroy crashes can be A/B tested without
// unlinking callers. The guarded invert above is the live implementation.

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

/// When PHIs were peeled off a Med block, the first surviving statement may
/// sit a few instructions after the COND_BR target. Map that target to the
/// earliest statement still inside the block.
static size_t findTargetIndexOrBlock(AddrMap &AM, va_t Target,
                                     const std::vector<HighStmt> &Body,
                                     const MedFunc *Med) {
  const size_t Exact = findTargetIndex(AM, Target);
  if (Exact != SIZE_MAX)
    return Exact;
  if (!Med || !Target || Target == InvalidVA)
    return SIZE_MAX;
  const MedBlock *Block = nullptr;
  va_t BlockEnd = 0;
  for (const auto &Candidate : Med->Blocks) {
    const va_t Start =
        Candidate.StartAddr
            ? Candidate.StartAddr
            : (Candidate.Ops.empty() ? 0 : Candidate.Ops.front().Addr);
    if (Start != Target)
      continue;
    Block = &Candidate;
    BlockEnd = Candidate.Ops.empty() ? Start + 1 : Candidate.Ops.back().Addr + 1;
    break;
  }
  if (!Block)
    return SIZE_MAX;
  size_t Best = SIZE_MAX;
  va_t BestAddr = 0;
  for (size_t I = 0; I < Body.size(); ++I) {
    const va_t Address = AddrMap::entryAddress(Body[I]);
    if (!Address || Address < Target || Address >= BlockEnd)
      continue;
    if (Best == SIZE_MAX || Address < BestAddr) {
      Best = I;
      BestAddr = Address;
    }
  }
  return Best;
}

static bool stmtIsAssignLike(const HighStmt &S) {
  return S.Kind == StmtKind::Assign || S.Kind == StmtKind::Nop ||
         S.Kind == StmtKind::Block || S.Kind == StmtKind::Goto;
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
static bool ownsRunHighIR(const std::vector<HighStmt> &Body, AddrMap &AM,
                          StmtCollectResult Run, size_t Owner) {
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
  return true;
}

static bool ownsRun(const std::vector<HighStmt> &Body, AddrMap &AM,
                    StmtCollectResult Run, size_t Owner, const MedFunc *Med) {
  if (!ownsRunHighIR(Body, AM, Run, Owner))
    return false;
  if (Med) {
    auto Contains = [&](va_t Address) {
      const size_t Index = findTargetIndex(AM, Address);
      return Index >= Run.Start && Index < Run.End;
    };
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

/// Invert-skip may stop before L when an outsider still jumps into the
/// range (for example, a goto from a sibling path into the join).
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

static bool stmtListFallsThrough(const std::vector<HighStmt> &Body);

static bool stmtFallsThrough(const HighStmt &S) {
  switch (S.Kind) {
  case StmtKind::Goto:
  case StmtKind::Return:
  case StmtKind::Break:
  case StmtKind::Continue:
    return false;
  case StmtKind::IfElse:
    return stmtListFallsThrough(S.Body) || stmtListFallsThrough(S.ElseBody);
  default:
    return true;
  }
}

static bool stmtListFallsThrough(const std::vector<HighStmt> &Body) {
  return Body.empty() || stmtFallsThrough(Body.back());
}

static bool ifBodyIsOnlyGoto(const std::vector<HighStmt> &Body) {
  return !Body.empty() && Body.back().Kind == StmtKind::Goto &&
         std::all_of(Body.begin(), Body.end() - 1,
                     [](const HighStmt &S) { return S.IsPhiCopy; });
}

/// Every path in \p Body ends at the same `goto Join` (no fallthrough, return,
/// or competing transfer). Used for
/// `if (c) { work; goto Join; } elseWork; Join:` — not invert-skip.
static va_t uniqueExitGoto(const std::vector<HighStmt> &Body) {
  if (Body.empty() || stmtListFallsThrough(Body))
    return 0;
  va_t Join = 0;
  bool Ok = true;
  auto Check = [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Return || S.Kind == StmtKind::Break ||
        S.Kind == StmtKind::Continue)
      Ok = false;
    if (S.Kind != StmtKind::Goto)
      return;
    if (!S.GotoTarget || S.GotoTarget == InvalidVA)
      Ok = false;
    else if (!Join)
      Join = S.GotoTarget;
    else if (S.GotoTarget != Join)
      Ok = false;
  };
  for (const HighStmt &S : Body)
    walkStatementTree(S, Check);
  return Ok ? Join : 0;
}

/// One statement whose every path is `goto Join`. An If that can fall through
/// is not a closed else arm when control still skips to a sibling.
static va_t uniqueExitGotoOne(const HighStmt &S) {
  if (S.Kind == StmtKind::Goto)
    return S.GotoTarget && S.GotoTarget != InvalidVA ? S.GotoTarget : 0;
  if (S.Kind == StmtKind::IfElse) {
    const va_t ThenJ = uniqueExitGoto(S.Body);
    const va_t ElseJ = uniqueExitGoto(S.ElseBody);
    return ThenJ && ThenJ == ElseJ ? ThenJ : 0;
  }
  return 0;
}

/// Else arm of `if (c) goto Else; then; goto Join; Else: ...; Join:`.
/// Stop at one closed IfElse or a straight-line run ending at `goto Join`.
/// Do not walk past a fallthrough If (that sibling owns the rest).
static size_t findDiamondElseEnd(const std::vector<HighStmt> &Body,
                                 size_t TargetIndex, size_t JoinIdx,
                                 va_t Join) {
  if (!Join || Join == InvalidVA || TargetIndex >= Body.size() ||
      TargetIndex > JoinIdx)
    return SIZE_MAX;
  for (size_t K = TargetIndex; K < JoinIdx && K < Body.size(); ++K) {
    const HighStmt &S = Body[K];
    if (S.Kind == StmtKind::Goto)
      return S.GotoTarget == Join ? K + 1 : SIZE_MAX;
    if (S.Kind == StmtKind::IfElse)
      return uniqueExitGotoOne(S) == Join ? K + 1 : SIZE_MAX;
    if (S.Kind == StmtKind::If || S.Kind == StmtKind::While ||
        S.Kind == StmtKind::Switch || S.Kind == StmtKind::CxxTry ||
        S.Kind == StmtKind::SEHTry || S.Kind == StmtKind::ItaniumTry)
      return SIZE_MAX;
  }
  return SIZE_MAX;
}

/// Else of `if (c) { work; goto Join; } elsework; Join:`. Straight-line only.
/// A sibling If/IfElse owns the rest and is not this else.
static size_t findSkipOverElseEnd(const std::vector<HighStmt> &Body,
                                  size_t NextI, size_t JoinIdx, va_t Join) {
  if (!Join || Join == InvalidVA || NextI >= Body.size() || NextI >= JoinIdx)
    return SIZE_MAX;
  for (size_t K = NextI; K < JoinIdx && K < Body.size(); ++K) {
    const HighStmt &S = Body[K];
    if (S.Kind == StmtKind::Goto)
      return S.GotoTarget == Join ? K + 1 : SIZE_MAX;
    if (S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse ||
        S.Kind == StmtKind::While || S.Kind == StmtKind::Switch ||
        S.Kind == StmtKind::CxxTry || S.Kind == StmtKind::SEHTry ||
        S.Kind == StmtKind::ItaniumTry || S.Kind == StmtKind::Return ||
        S.Kind == StmtKind::Break || S.Kind == StmtKind::Continue)
      return SIZE_MAX;
  }
  return JoinIdx;
}

/// Pop `goto Join` only along the trailing spine. A non-final `if` still
/// needs its goto so it does not fall into the next sibling.
static void trimJoinGotos(std::vector<HighStmt> &Body, va_t Join) {
  if (!Join || Join == InvalidVA)
    return;
  trimMergeGoto(Body, Join);
  if (Body.empty())
    return;
  HighStmt &Last = Body.back();
  if (Last.Kind == StmtKind::IfElse) {
    trimJoinGotos(Last.Body, Join);
    trimJoinGotos(Last.ElseBody, Join);
  } else if (Last.Kind == StmtKind::If) {
    trimJoinGotos(Last.Body, Join);
  }
}

static bool rangeHasLoop(const std::vector<HighStmt> &Body, size_t Begin,
                         size_t End) {
  for (size_t K = Begin; K < End; ++K)
    if (Body[K].Kind == StmtKind::While)
      return true;
  return false;
}

/// Skip edge of a MSVC null-check / predicate chain: PHI copies, empty
/// labels, undef clobbers, and scalar copies/casts of already-live values.
/// Those arms only exist so the fallthrough can meet `elseWork`.
static bool exprIsScalarMove(const HighExpr *E, bool AllowConst) {
  if (!E)
    return false;
  switch (E->Kind) {
  case ExprKind::Undef:
  case ExprKind::Var:
    return true;
  case ExprKind::Const:
    return AllowConst;
  case ExprKind::Cast:
  case ExprKind::BitCast:
  case ExprKind::UnaryOp:
    return !E->Operands.empty() &&
           exprIsScalarMove(E->Operands[0].get(), true);
  default:
    return false;
  }
}

static bool armIsSkippable(const std::vector<HighStmt> &Arm) {
  return std::all_of(Arm.begin(), Arm.end(), [](const HighStmt &S) {
    if (S.Kind == StmtKind::Nop || S.Kind == StmtKind::Block)
      return true;
    return S.Kind == StmtKind::Assign && exprIsScalarMove(S.Val.get(), false);
  });
}

/// `if (c) { clobbers; goto L; }` — PHI copies, undefs, or scalar moves.
static bool bodyIsSkipGoto(const std::vector<HighStmt> &Body) {
  return !Body.empty() && Body.back().Kind == StmtKind::Goto &&
         Body.back().GotoTarget && Body.back().GotoTarget != InvalidVA &&
         std::all_of(Body.begin(), Body.end() - 1, [](const HighStmt &S) {
           if (S.Kind == StmtKind::Nop || S.Kind == StmtKind::Block)
             return true;
           return S.Kind == StmtKind::Assign &&
                  exprIsScalarMove(S.Val.get(), S.IsPhiCopy);
         });
}

static bool bodyWorkGotoJoin(const std::vector<HighStmt> &Body, va_t Join) {
  return !ifBodyIsOnlyGoto(Body) && !Body.empty() &&
         Body.back().Kind == StmtKind::Goto && Body.back().GotoTarget == Join &&
         Join && Join != InvalidVA;
}

static va_t uniqueNestedGoto(const HighStmt &S) {
  va_t Join = 0;
  bool Ok = true;
  bool Saw = false;
  walkStatementTree(S, [&](const HighStmt &T) {
    if (T.Kind != StmtKind::Goto)
      return;
    if (!T.GotoTarget || T.GotoTarget == InvalidVA) {
      Ok = false;
      return;
    }
    if (!Saw) {
      Join = T.GotoTarget;
      Saw = true;
    } else if (T.GotoTarget != Join) {
      Ok = false;
    }
  });
  return Ok && Saw ? Join : 0;
}

static bool sameMedVar(const MedVar &A, const MedVar &B) {
  return A.Kind == B.Kind && A.Id == B.Id && A.SSAVer == B.SSAVer;
}

static bool exprUsesVar(const HighExpr *E, const MedVar &V) {
  if (!E)
    return false;
  if (E->Kind == ExprKind::Var && sameMedVar(E->Var, V))
    return true;
  for (const auto &Op : E->Operands)
    if (exprUsesVar(Op.get(), V))
      return true;
  return false;
}

static ExprPtr replaceVarInExpr(const ExprPtr &E, const MedVar &V,
                                const ExprPtr &With) {
  if (!E)
    return E;
  if (E->Kind == ExprKind::Var && sameMedVar(E->Var, V))
    return With;
  auto Copy = std::make_shared<HighExpr>(*E);
  for (auto &Op : Copy->Operands)
    Op = replaceVarInExpr(Op, V, With);
  return Copy;
}

static bool isValueAssign(const HighStmt &S, MedVar &Dest, ExprPtr &Val) {
  if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var &&
      S.Val) {
    Dest = S.Dst->Var;
    Val = S.Val;
    return true;
  }
  return false;
}

static void replaceVarInStmt(HighStmt &S, const MedVar &V, const ExprPtr &With) {
  auto Repl = [&](ExprPtr &E) { E = replaceVarInExpr(E, V, With); };
  auto Apply = [&](HighStmt &T) {
    Repl(T.Cond);
    Repl(T.Dst);
    Repl(T.Val);
    Repl(T.CallExpr);
    Repl(T.RetVal);
    Repl(T.StoreAddr);
    Repl(T.StoreVal);
    Repl(T.SwitchExpr);
  };
  Apply(S);
  walkStmts(S.Body, Apply);
  walkStmts(S.ElseBody, Apply);
  walkStmts(S.DefaultBody, Apply);
  for (auto &C : S.Cases)
    walkStmts(C.Body, Apply);
  for (auto &Clause : S.EHClauseBodies)
    walkStmts(Clause, Apply);
}

static void replaceVarInStmts(std::vector<HighStmt> &Body, const MedVar &V,
                              const ExprPtr &With) {
  for (HighStmt &S : Body)
    replaceVarInStmt(S, V, With);
}

static bool isEmptyLabel(const HighStmt &S) {
  return (S.Kind == StmtKind::Nop || S.Kind == StmtKind::Block) &&
         S.Body.empty() && S.ElseBody.empty();
}

static bool prefixOkForChain(const HighStmt &S) {
  if (S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse ||
      S.Kind == StmtKind::While || S.Kind == StmtKind::CxxTry ||
      S.Kind == StmtKind::SEHTry || S.Kind == StmtKind::Goto ||
      S.Kind == StmtKind::Return)
    return false;
  return stmtIsAssignLike(S) || S.Kind == StmtKind::Call ||
         S.Kind == StmtKind::Store || S.Kind == StmtKind::ExprStmt;
}

static const HighExpr *peelCond(const HighExpr *E) {
  unsigned Depth = 0;
  while (E && Depth++ < 8) {
    if ((E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast) &&
        !E->Operands.empty() && E->Operands[0]) {
      E = E->Operands[0].get();
      continue;
    }
    if (E->Kind == ExprKind::UnaryOp &&
        (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT) &&
        !E->Operands.empty() && E->Operands[0]) {
      E = E->Operands[0].get();
      continue;
    }
    if (E->Kind == ExprKind::BinOp && E->Op == NdOp::SUBBYTES &&
        E->Operands.size() == 2 && E->Operands[0] && E->Operands[1] &&
        E->Operands[1]->Kind == ExprKind::Const &&
        E->Operands[1]->ConstVal == 0) {
      E = E->Operands[0].get();
      continue;
    }
    break;
  }
  return E;
}

static bool isZeroConst(const HighExpr *E) {
  E = peelCond(E);
  return E && E->Kind == ExprKind::Const && E->ConstVal == 0;
}

static bool condAsZeroTest(const HighExpr *E, const HighExpr *&Val,
                           bool &NonZero) {
  E = peelCond(E);
  bool Neg = false;
  if (E && E->Kind == ExprKind::UnaryOp && E->Op == NdOp::BOOL_NOT &&
      !E->Operands.empty() && E->Operands[0]) {
    Neg = true;
    E = peelCond(E->Operands[0].get());
  }
  if (E && E->Kind == ExprKind::Call) {
    if (!Neg)
      return false;
    Val = E;
    NonZero = false;
    return true;
  }
  if (!E || E->Kind != ExprKind::BinOp || E->Operands.size() != 2)
    return false;
  const HighExpr *Lhs = E->Operands[0].get();
  const HighExpr *Rhs = E->Operands[1].get();
  if (isZeroConst(Rhs))
    Val = peelCond(Lhs);
  else if (isZeroConst(Lhs))
    Val = peelCond(Rhs);
  else
    return false;
  if (E->Op == NdOp::INT_EQUAL)
    NonZero = Neg;
  else if (E->Op == NdOp::INT_NOTEQUAL)
    NonZero = !Neg;
  else
    return false;
  return Val != nullptr;
}

static bool condStructEq(const HighExpr *A, const HighExpr *B,
                         bool SameTempId = false) {
  A = peelCond(A);
  B = peelCond(B);
  if (A == B)
    return true;
  const HighExpr *VA = nullptr;
  const HighExpr *VB = nullptr;
  bool NonZeroA = false;
  bool NonZeroB = false;
  if (condAsZeroTest(A, VA, NonZeroA) && condAsZeroTest(B, VB, NonZeroB))
    return NonZeroA == NonZeroB &&
           condStructEq(VA, VB, SameTempId);
  if (!A || !B || A->Kind != B->Kind || A->Op != B->Op)
    return false;
  if (A->Kind == ExprKind::Var || A->Kind == ExprKind::Phi) {
    // Same Temp.Id with different SSA can be loads of different fields
    // (MedIR reuses the temp). Only match exact SSA; compose loads first.
    return sameMedVar(A->Var, B->Var);
  }
  if (A->Kind == ExprKind::Const)
    return A->ConstVal == B->ConstVal;
  if (A->Kind == ExprKind::Call) {
    if (A->CallAddr != B->CallAddr && A->CallTarget != B->CallTarget)
      return false;
    if (A->IntrinsicId != B->IntrinsicId)
      return false;
    if (A->Operands.empty() || B->Operands.empty())
      return A->Operands.empty() && B->Operands.empty();
    return condStructEq(A->Operands[0].get(), B->Operands[0].get(), SameTempId);
  }
  if (A->Operands.size() != B->Operands.size())
    return false;
  for (size_t I = 0; I < A->Operands.size(); ++I)
    if (!condStructEq(A->Operands[I].get(), B->Operands[I].get(), SameTempId))
      return false;
  return true;
}

static const HighExpr *firstCallInExpr(const HighExpr *E) {
  if (!E)
    return nullptr;
  if (E->Kind == ExprKind::Call)
    return E;
  for (const auto &Op : E->Operands)
    if (const HighExpr *Call = firstCallInExpr(Op.get()))
      return Call;
  return nullptr;
}

static bool firstVarInExpr(const HighExpr *E, MedVar &Out) {
  if (!E)
    return false;
  if (E->Kind == ExprKind::Var) {
    Out = E->Var;
    return true;
  }
  for (const auto &Op : E->Operands)
    if (firstVarInExpr(Op.get(), Out))
      return true;
  return false;
}

/// Call that feeds `if (v == 0) goto`, walking assign-like reaching defs
/// through unknown `?Op?` wrappers and leftover cmp temps.
static const HighExpr *reachingPredCall(const std::vector<HighStmt> &Body,
                                        size_t IfI) {
  if (IfI >= Body.size() || !Body[IfI].Cond)
    return nullptr;
  if (const HighExpr *Call = firstCallInExpr(Body[IfI].Cond.get()))
    return Call;
  MedVar Wanted;
  if (!firstVarInExpr(Body[IfI].Cond.get(), Wanted))
    return nullptr;
  for (size_t K = IfI; K > 0;) {
    --K;
    if (!prefixOkForChain(Body[K]))
      break;
    MedVar Dest;
    ExprPtr Assigned;
    if (!isValueAssign(Body[K], Dest, Assigned) || !Assigned ||
        !sameMedVar(Dest, Wanted))
      continue;
    if (const HighExpr *Call = firstCallInExpr(Assigned.get()))
      return Call;
    if (!firstVarInExpr(Assigned.get(), Wanted))
      break;
  }
  return nullptr;
}

/// Nearest call-assign before \p IfI. Does not require the dest SSA to
/// match the cond temp; leftover `v = (call ?Op? 0)` copies often differ.
static const HighExpr *precedingPredCall(const std::vector<HighStmt> &Body,
                                         size_t IfI) {
  for (size_t K = IfI; K > 0;) {
    --K;
    if (!prefixOkForChain(Body[K]))
      break;
    MedVar Dest;
    ExprPtr Assigned;
    if (!isValueAssign(Body[K], Dest, Assigned) || !Assigned)
      continue;
    if (Assigned->Kind == ExprKind::Call)
      return Assigned.get();
    if (const HighExpr *Call = firstCallInExpr(Assigned.get()))
      return Call;
  }
  return nullptr;
}

static bool samePredCall(const HighExpr *A, const HighExpr *B) {
  if (!A || !B || A->CallTarget.empty() || B->CallTarget.empty())
    return false;
  return A->CallTarget == B->CallTarget;
}

static bool isBoolCombo(const HighExpr *E) {
  E = peelCond(E);
  return E && E->Kind == ExprKind::BinOp &&
         (E->Op == NdOp::BOOL_AND || E->Op == NdOp::BOOL_OR ||
          E->Op == NdOp::INT_AND || E->Op == NdOp::INT_OR);
}

/// Predicate call of a single test, not the first call nested inside `&&`/`||`.
/// Matching `pred()` against `!p || !pred() || !q` would flatten the skip and
/// leave later work dead after `goto`.
static const HighExpr *atomPredCall(const HighExpr *E) {
  E = peelCond(E);
  if (isBoolCombo(E))
    return nullptr;
  return firstCallInExpr(E);
}

static const HighExpr *peelFieldPath(const HighExpr *E,
                                     std::vector<uint64_t> &Offs) {
  unsigned Depth = 0;
  while (E && Depth++ < 8) {
    E = peelCond(E);
    if (!E)
      return nullptr;
    if (E->Kind == ExprKind::Load && !E->Operands.empty() && E->Operands[0]) {
      E = E->Operands[0].get();
      continue;
    }
    if (E->Kind == ExprKind::BinOp && E->Op == NdOp::INT_ADD &&
        E->Operands.size() == 2 && E->Operands[0] && E->Operands[1]) {
      if (E->Operands[1]->Kind == ExprKind::Const) {
        Offs.push_back(E->Operands[1]->ConstVal);
        E = E->Operands[0].get();
        continue;
      }
      if (E->Operands[0]->Kind == ExprKind::Const) {
        Offs.push_back(E->Operands[0]->ConstVal);
        E = E->Operands[1].get();
        continue;
      }
    }
    break;
  }
  return peelCond(E);
}

static bool sameFieldPath(const HighExpr *A, const HighExpr *B) {
  std::vector<uint64_t> PA;
  std::vector<uint64_t> PB;
  const HighExpr *BA = peelFieldPath(A, PA);
  const HighExpr *BB = peelFieldPath(B, PB);
  if (!BA || !BB || PA.empty() || PA != PB)
    return false;
  if (condStructEq(BA, BB))
    return true;
  return (BA->Kind == ExprKind::Var || BA->Kind == ExprKind::Phi) &&
         (BB->Kind == ExprKind::Var || BB->Kind == ExprKind::Phi) &&
         BA->Var.Kind == MedVar::Param && BB->Var.Kind == MedVar::Param &&
         BA->Var.Id == BB->Var.Id;
}

using FieldLoadDefs = VarKeyMap<const HighExpr *>;

static void collectFieldLoadDefs(const std::vector<HighStmt> &Body,
                                 FieldLoadDefs &Defs) {
  for (const HighStmt &S : Body) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
        S.Val->Kind == ExprKind::Load)
      Defs[varKey(S.Dst->Var)] = S.Val.get();
    collectFieldLoadDefs(S.Body, Defs);
    collectFieldLoadDefs(S.ElseBody, Defs);
    collectFieldLoadDefs(S.DefaultBody, Defs);
    for (const auto &C : S.Cases)
      collectFieldLoadDefs(C.Body, Defs);
    for (const auto &Clause : S.EHClauseBodies)
      collectFieldLoadDefs(Clause, Defs);
  }
}

static const HighExpr *asFieldLoad(const HighExpr *E,
                                   const FieldLoadDefs *Defs) {
  E = peelCond(E);
  if (!E || !Defs)
    return E;
  if (E->Kind != ExprKind::Var && E->Kind != ExprKind::Phi)
    return E;
  auto It = Defs->find(varKey(E->Var));
  return It != Defs->end() && It->second ? It->second : E;
}

static bool impliedCondMatch(const HighExpr *Conjunct, const HighExpr *Outer,
                             bool SameTempId, const FieldLoadDefs *FieldLoads);
static std::optional<ExprPtr>
dropOrDisjunct(const ExprPtr &FalsePrefix, const ExprPtr &Inner,
               bool SameTempId = false,
               const FieldLoadDefs *FieldLoads = nullptr);

static void collectAndConjuncts(const HighExpr *E,
                                std::vector<const HighExpr *> &Out) {
  if (!E)
    return;
  if (E->Kind == ExprKind::BinOp && E->Op == NdOp::BOOL_AND &&
      E->Operands.size() == 2) {
    collectAndConjuncts(E->Operands[0].get(), Out);
    collectAndConjuncts(E->Operands[1].get(), Out);
    return;
  }
  Out.push_back(E);
}

static std::optional<ExprPtr> dropAndPrefix(const HighExpr *Prefix,
                                            const ExprPtr &Inner,
                                            bool SameTempId = false,
                                            const FieldLoadDefs *FieldLoads =
                                                nullptr) {
  if (!Prefix || !Inner)
    return std::nullopt;
  std::vector<const HighExpr *> Head;
  std::vector<const HighExpr *> All;
  collectAndConjuncts(Prefix, Head);
  collectAndConjuncts(Inner.get(), All);
  if (Head.empty() || All.empty())
    return std::nullopt;
  std::vector<const HighExpr *> Rest;
  bool Dropped = false;
  for (const HighExpr *Conjunct : All) {
    bool Match = false;
    for (const HighExpr *Outer : Head) {
      if (impliedCondMatch(Conjunct, Outer, SameTempId, FieldLoads)) {
        Match = true;
        break;
      }
    }
    if (Match)
      Dropped = true;
    else
      Rest.push_back(Conjunct);
  }
  if (!Dropped)
    return std::nullopt;
  if (Rest.empty())
    return HighExpr::makeConst(1, 1);
  ExprPtr Out = std::make_shared<HighExpr>(*Rest[0]);
  for (size_t I = 1; I < Rest.size(); ++I)
    Out = HighExpr::makeBinop(NdOp::BOOL_AND, std::move(Out),
                              std::make_shared<HighExpr>(*Rest[I]));
  return Out;
}

static std::optional<ExprPtr> dropAndPrefix(const ExprPtr &Prefix,
                                            const ExprPtr &Inner,
                                            bool SameTempId = false,
                                            const FieldLoadDefs *FieldLoads =
                                                nullptr) {
  return dropAndPrefix(Prefix.get(), Inner, SameTempId, FieldLoads);
}

static bool isConstTrue(const ExprPtr &E) {
  const HighExpr *P = peelCond(E.get());
  return P && P->Kind == ExprKind::Const && P->ConstVal != 0;
}

/// Invert a compare / AND / OR tree of `== 0` / `!= 0`. Leave `x == 0 || x < 0`
/// as `BOOL_NOT` so HighC can still print `x > 0`.
static bool isEqNeTree(const HighExpr *E) {
  E = peelCond(E);
  if (!E)
    return false;
  if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2)
    return false;
  if (E->Op == NdOp::INT_EQUAL || E->Op == NdOp::INT_NOTEQUAL)
    return true;
  if (E->Op != NdOp::BOOL_OR && E->Op != NdOp::BOOL_AND &&
      E->Op != NdOp::INT_OR && E->Op != NdOp::INT_AND)
    return false;
  return isEqNeTree(E->Operands[0].get()) && isEqNeTree(E->Operands[1].get());
}

static ExprPtr invertHighCond(ExprPtr C) {
  if (!C)
    return C;
  const HighExpr *P = peelCond(C.get());
  if (P && P->Kind == ExprKind::UnaryOp && P->Op == NdOp::BOOL_NOT &&
      !P->Operands.empty() && P->Operands[0])
    return P->Operands[0];
  if (P && P->Kind == ExprKind::BinOp && P->Operands.size() == 2 &&
      P->Operands[0] && P->Operands[1] && isEqNeTree(P)) {
    if (P->Op == NdOp::BOOL_OR || P->Op == NdOp::INT_OR)
      return HighExpr::makeBinop(NdOp::BOOL_AND,
                                 invertHighCond(P->Operands[0]),
                                 invertHighCond(P->Operands[1]));
    if (P->Op == NdOp::BOOL_AND || P->Op == NdOp::INT_AND)
      return HighExpr::makeBinop(NdOp::BOOL_OR, invertHighCond(P->Operands[0]),
                                 invertHighCond(P->Operands[1]));
    if (P->Op == NdOp::INT_EQUAL || P->Op == NdOp::INT_NOTEQUAL) {
      HighExpr Flipped = *P;
      Flipped.Op =
          P->Op == NdOp::INT_EQUAL ? NdOp::INT_NOTEQUAL : NdOp::INT_EQUAL;
      return std::make_shared<HighExpr>(std::move(Flipped));
    }
  }
  return HighExpr::makeUnary(NdOp::BOOL_NOT, std::move(C));
}

static const HighExpr *condPointerValue(const HighExpr *E) {
  const HighExpr *Val = nullptr;
  bool NonZero = false;
  if (condAsZeroTest(E, Val, NonZero))
    return peelCond(Val);
  return peelCond(E);
}

static bool condIsPointerLike(const HighExpr *E) {
  E = condPointerValue(E);
  return E && !firstCallInExpr(E) &&
         (E->Kind == ExprKind::Load || E->Kind == ExprKind::Var ||
          E->Kind == ExprKind::Phi ||
          (E->Kind == ExprKind::BinOp && E->Op == NdOp::INT_ADD));
}

static bool storeWritesCondPointer(const HighExpr *Addr, const HighExpr *Cond,
                                   const FieldLoadDefs *FieldLoads) {
  if (!Addr || !Cond)
    return false;
  const HighExpr *P = condPointerValue(Cond);
  if (!P)
    return false;
  if (sameFieldPath(Addr, P) ||
      sameFieldPath(Addr, asFieldLoad(P, FieldLoads)))
    return true;
  if (P->Kind == ExprKind::Load && !P->Operands.empty() && P->Operands[0] &&
      condStructEq(peelCond(P->Operands[0].get()), peelCond(Addr)))
    return true;
  return false;
}

static bool callArgIsCondPointer(const HighExpr *Call, const HighExpr *Cond) {
  if (!Call || Call->Kind != ExprKind::Call || !Cond)
    return false;
  const HighExpr *P = condPointerValue(Cond);
  if (!P)
    return false;
  for (const auto &Op : Call->Operands) {
    if (!Op)
      continue;
    const HighExpr *Arg = peelCond(Op.get());
    if (condStructEq(Arg, P) || sameFieldPath(Arg, P))
      return true;
  }
  return false;
}

static const HighExpr *stmtCallExpr(const HighStmt &S) {
  if (S.Kind == StmtKind::Call && S.CallExpr)
    return S.CallExpr.get();
  if (S.Val && S.Val->Kind == ExprKind::Call)
    return S.Val.get();
  if (S.CallExpr && S.CallExpr->Kind == ExprKind::Call)
    return S.CallExpr.get();
  return firstCallInExpr(S.Val.get());
}

static bool stmtInvalidatesCond(const HighStmt &S, const HighExpr *Cond,
                                const FieldLoadDefs *FieldLoads) {
  if (!Cond)
    return false;
  MedVar Dest;
  ExprPtr Val;
  if (isValueAssign(S, Dest, Val) && exprUsesVar(Cond, Dest))
    return true;
  if (S.Kind == StmtKind::Store && S.StoreAddr &&
      storeWritesCondPointer(S.StoreAddr.get(), Cond, FieldLoads))
    return true;
  if (!condIsPointerLike(Cond))
    return false;
  return callArgIsCondPointer(stmtCallExpr(S), Cond);
}

static void filterLiveConds(std::vector<const HighExpr *> &Live,
                            const HighStmt &S,
                            const FieldLoadDefs *FieldLoads) {
  Live.erase(std::remove_if(Live.begin(), Live.end(),
                            [&](const HighExpr *C) {
                              return stmtInvalidatesCond(S, C, FieldLoads);
                            }),
             Live.end());
}

enum class ImpliedAct { None, Partial, FlattenThen, FlattenElse };

static ImpliedAct peelAgainstOuters(HighStmt &S,
                                    const std::vector<const HighExpr *> &Outers,
                                    const FieldLoadDefs *FieldLoads) {
  if ((S.Kind != StmtKind::If && S.Kind != StmtKind::IfElse) || !S.Cond ||
      Outers.empty())
    return ImpliedAct::None;
  ExprPtr Cur = S.Cond;
  bool Partial = false;
  for (const HighExpr *Outer : Outers) {
    if (auto Rest = dropAndPrefix(Outer, Cur, false, FieldLoads)) {
      if (isConstTrue(*Rest)) {
        if (S.ElseBody.empty())
          return ImpliedAct::FlattenThen;
        continue;
      }
      Cur = std::move(*Rest);
      Partial = true;
      continue;
    }
    if (auto OrRest = dropOrDisjunct(
            invertHighCond(std::make_shared<HighExpr>(*Outer)), Cur, false,
            FieldLoads)) {
      Cur = std::move(*OrRest);
      Partial = true;
      continue;
    }
    if (auto InvRest = dropAndPrefix(
            Outer, invertHighCond(std::make_shared<HighExpr>(*Cur)), false,
            FieldLoads)) {
      if (isConstTrue(*InvRest))
        return ImpliedAct::FlattenElse;
    }
  }
  if (Partial) {
    S.Cond = std::move(Cur);
    return ImpliedAct::Partial;
  }
  return ImpliedAct::None;
}

static bool flattenInList(std::vector<HighStmt> &List,
                          const std::vector<const HighExpr *> &Outers,
                          const FieldLoadDefs *FieldLoads) {
  bool Changed = false;
  for (size_t J = 0; J < List.size();) {
    std::vector<const HighExpr *> Live = Outers;
    for (size_t P = 0; P < J; ++P)
      filterLiveConds(Live, List[P], FieldLoads);
    const ImpliedAct Act = peelAgainstOuters(List[J], Live, FieldLoads);
    if (Act == ImpliedAct::FlattenThen || Act == ImpliedAct::FlattenElse) {
      HighStmt Taken = std::move(List[J]);
      auto &Src =
          Act == ImpliedAct::FlattenThen ? Taken.Body : Taken.ElseBody;
      List.erase(List.begin() + static_cast<long>(J));
      List.insert(List.begin() + static_cast<long>(J),
                  std::make_move_iterator(Src.begin()),
                  std::make_move_iterator(Src.end()));
      Changed = true;
      continue;
    }
    if (Act == ImpliedAct::Partial)
      Changed = true;
    ++J;
  }
  return Changed;
}

static bool dropImpliedInnerConds(
    std::vector<HighStmt> &Body, const FieldLoadDefs *FieldLoads,
    const std::vector<const HighExpr *> &Ancestors) {
  return flattenInList(Body, Ancestors, FieldLoads);
}

static void dropImpliedInnerCondsIn(
    std::vector<HighStmt> &Body, const FieldLoadDefs *FieldLoads,
    const std::vector<const HighExpr *> &Ancestors) {
  dropImpliedInnerConds(Body, FieldLoads, Ancestors);
  for (size_t I = 0; I < Body.size(); ++I) {
    HighStmt &S = Body[I];
    std::vector<const HighExpr *> Live = Ancestors;
    for (size_t P = 0; P < I; ++P)
      filterLiveConds(Live, Body[P], FieldLoads);
    std::vector<const HighExpr *> ThenLive = Live;
    if ((S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse) && S.Cond)
      ThenLive.push_back(S.Cond.get());
    dropImpliedInnerCondsIn(S.Body, FieldLoads, ThenLive);
    dropImpliedInnerCondsIn(S.ElseBody, FieldLoads, Live);
    dropImpliedInnerCondsIn(S.DefaultBody, FieldLoads, Live);
    for (auto &C : S.Cases)
      dropImpliedInnerCondsIn(C.Body, FieldLoads, Live);
    for (auto &Clause : S.EHClauseBodies)
      dropImpliedInnerCondsIn(Clause, FieldLoads, Live);
  }
}

static void dropImpliedInnerCondsNested(std::vector<HighStmt> &Body) {
  FieldLoadDefs FieldLoads;
  collectFieldLoadDefs(Body, FieldLoads);
  dropImpliedInnerCondsIn(Body, &FieldLoads, {});
}

static void collectOrDisjuncts(const HighExpr *E,
                               std::vector<const HighExpr *> &Out) {
  if (!E)
    return;
  if (E->Kind == ExprKind::BinOp &&
      (E->Op == NdOp::BOOL_OR || E->Op == NdOp::INT_OR) &&
      E->Operands.size() == 2) {
    collectOrDisjuncts(E->Operands[0].get(), Out);
    collectOrDisjuncts(E->Operands[1].get(), Out);
    return;
  }
  Out.push_back(E);
}

static bool impliedCondMatch(const HighExpr *Conjunct, const HighExpr *Outer,
                             bool SameTempId, const FieldLoadDefs *FieldLoads) {
  if (!Conjunct || !Outer)
    return false;
  if (condStructEq(Conjunct, Outer, SameTempId))
    return true;
  const HighExpr *VA = nullptr;
  const HighExpr *VB = nullptr;
  bool NA = false;
  bool NB = false;
  if (condAsZeroTest(Conjunct, VA, NA) && condAsZeroTest(Outer, VB, NB) &&
      NA == NB &&
      (samePredCall(atomPredCall(VA), atomPredCall(VB)) ||
       sameFieldPath(VA, VB) ||
       sameFieldPath(asFieldLoad(VA, FieldLoads),
                     asFieldLoad(VB, FieldLoads))))
    return true;
  if (!samePredCall(atomPredCall(Conjunct), atomPredCall(Outer)))
    return false;
  const bool TA = condAsZeroTest(Conjunct, VA, NA);
  const bool TB = condAsZeroTest(Outer, VB, NB);
  if (TA && TB)
    return NA == NB;
  if (!TA && !TB)
    return true;
  return (TA && !TB && NA) || (!TA && TB && NB);
}

/// Drop OR disjuncts that match \p FalsePrefix. Fallthrough after
/// `if (P) { always exit }` proves P is false, so `P || rest` is `rest`.
static std::optional<ExprPtr>
dropOrDisjunct(const ExprPtr &FalsePrefix, const ExprPtr &Inner,
               bool SameTempId, const FieldLoadDefs *FieldLoads) {
  if (!FalsePrefix || !Inner)
    return std::nullopt;
  std::vector<const HighExpr *> Head;
  std::vector<const HighExpr *> All;
  collectOrDisjuncts(FalsePrefix.get(), Head);
  collectOrDisjuncts(Inner.get(), All);
  if (Head.empty() || All.empty() || All.size() < 2)
    return std::nullopt;
  std::vector<const HighExpr *> Rest;
  bool Dropped = false;
  for (const HighExpr *Disjunct : All) {
    bool Match = false;
    for (const HighExpr *Outer : Head) {
      if (impliedCondMatch(Disjunct, Outer, SameTempId, FieldLoads)) {
        Match = true;
        break;
      }
    }
    if (Match)
      Dropped = true;
    else
      Rest.push_back(Disjunct);
  }
  if (!Dropped || Rest.empty())
    return std::nullopt;
  ExprPtr Out = std::make_shared<HighExpr>(*Rest[0]);
  for (size_t I = 1; I < Rest.size(); ++I)
    Out = HighExpr::makeBinop(NdOp::BOOL_OR, std::move(Out),
                              std::make_shared<HighExpr>(*Rest[I]));
  return Out;
}

static bool onlyScalarAssigns(const std::vector<HighStmt> &Body) {
  if (Body.size() > 8)
    return false;
  for (const HighStmt &S : Body) {
    if (S.Kind == StmtKind::Nop ||
        (S.Kind == StmtKind::Block && S.Body.empty()))
      continue;
    MedVar Dest;
    ExprPtr Val;
    if (!isValueAssign(S, Dest, Val) || !Val || exprHasCall(Val.get()))
      return false;
  }
  return true;
}

static bool fallthroughWorkOk(const HighStmt &S) {
  if (S.Kind == StmtKind::Nop)
    return true;
  if (S.Kind == StmtKind::Block && S.Body.empty())
    return true;
  if (S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse)
    return onlyScalarAssigns(S.Body) && onlyScalarAssigns(S.ElseBody);
  return S.Kind == StmtKind::Assign || S.Kind == StmtKind::Store ||
         S.Kind == StmtKind::Call || S.Kind == StmtKind::ExprStmt;
}

static bool listAlwaysTransfers(const std::vector<HighStmt> &Body) {
  if (Body.empty())
    return false;
  for (size_t I = Body.size(); I > 0; --I) {
    const HighStmt &S = Body[I - 1];
    if (S.Kind == StmtKind::Nop ||
        (S.Kind == StmtKind::Block && S.Body.empty()))
      continue;
    return !stmtFallsThrough(S);
  }
  return false;
}

static ExprPtr composeWorkAssigns(const std::vector<HighStmt> &Body,
                                  size_t Begin, size_t End, ExprPtr Cond) {
  if (!Cond || Begin > End)
    return Cond;
  for (size_t N = End; N > Begin; --N) {
    const HighStmt &S = Body[N - 1];
    if (S.Kind == StmtKind::Nop ||
        (S.Kind == StmtKind::Block && S.Body.empty()))
      continue;
    MedVar Dest;
    ExprPtr Val;
    if (isValueAssign(S, Dest, Val) && exprUsesVar(Cond.get(), Dest))
      Cond = replaceVarInExpr(Cond, Dest, Val);
  }
  return Cond;
}

static bool dropImpliedFallthroughConds(std::vector<HighStmt> &Body,
                                        const FieldLoadDefs *FieldLoads) {
  bool Changed = false;
  for (size_t I = 0; I < Body.size(); ++I) {
    HighStmt &S = Body[I];
    if ((S.Kind != StmtKind::If && S.Kind != StmtKind::IfElse) || !S.Cond ||
        S.Body.empty() || !S.ElseBody.empty() || !listAlwaysTransfers(S.Body))
      continue;
    size_t J = I + 1;
    unsigned Skipped = 0;
    while (J < Body.size() && Skipped < 32 && fallthroughWorkOk(Body[J])) {
      ++J;
      ++Skipped;
    }
    if (J >= Body.size())
      continue;
    HighStmt &Next = Body[J];
    if ((Next.Kind != StmtKind::If && Next.Kind != StmtKind::IfElse) ||
        !Next.Cond)
      continue;
    ExprPtr Head = composeWorkAssigns(
        Body, 0, I, std::make_shared<HighExpr>(*S.Cond));
    ExprPtr Inner = composeWorkAssigns(
        Body, 0, J, std::make_shared<HighExpr>(*Next.Cond));
    auto Rest = dropOrDisjunct(Head, Inner, false, FieldLoads);
    if (!Rest) {
      const HighExpr *P = peelCond(Inner.get());
      if (P && P->Kind == ExprKind::UnaryOp && P->Op == NdOp::BOOL_NOT &&
          !P->Operands.empty() && P->Operands[0]) {
        ExprPtr Implied = invertHighCond(Head);
        auto AndRest =
            dropAndPrefix(Implied, P->Operands[0], false, FieldLoads);
        if (AndRest && !isConstTrue(*AndRest))
          Rest = HighExpr::makeUnary(NdOp::BOOL_NOT, std::move(*AndRest));
      }
    }
    if (!Rest)
      continue;
    Next.Cond = std::move(*Rest);
    Changed = true;
  }
  return Changed;
}

static void dropImpliedFallthroughCondsIn(std::vector<HighStmt> &Body,
                                          const FieldLoadDefs *FieldLoads) {
  dropImpliedFallthroughConds(Body, FieldLoads);
  for (HighStmt &S : Body) {
    dropImpliedFallthroughCondsIn(S.Body, FieldLoads);
    dropImpliedFallthroughCondsIn(S.ElseBody, FieldLoads);
    dropImpliedFallthroughCondsIn(S.DefaultBody, FieldLoads);
    for (auto &C : S.Cases)
      dropImpliedFallthroughCondsIn(C.Body, FieldLoads);
    for (auto &Clause : S.EHClauseBodies)
      dropImpliedFallthroughCondsIn(Clause, FieldLoads);
  }
}

static void dropImpliedFallthroughCondsNested(std::vector<HighStmt> &Body) {
  FieldLoadDefs FieldLoads;
  collectFieldLoadDefs(Body, FieldLoads);
  dropImpliedFallthroughCondsIn(Body, &FieldLoads);
}

static bool isNegatedCallCond(const HighExpr *E) {
  E = peelCond(E);
  if (!E)
    return false;
  if (E->Kind == ExprKind::UnaryOp && E->Op == NdOp::BOOL_NOT &&
      !E->Operands.empty())
    return firstCallInExpr(E->Operands[0].get()) != nullptr;
  const HighExpr *Val = nullptr;
  bool NonZero = false;
  return condAsZeroTest(E, Val, NonZero) && !NonZero &&
         firstCallInExpr(Val) != nullptr;
}

static bool armIsOnlyTransfer(const std::vector<HighStmt> &Body) {
  if (Body.empty())
    return true;
  return ifBodyIsOnlyGoto(Body);
}

static bool isNegatedCallCondFromPrefix(const HighExpr *E,
                                        const std::vector<HighStmt> &Body,
                                        size_t IfIdx) {
  if (isNegatedCallCond(E))
    return true;
  const HighExpr *Val = nullptr;
  bool NonZero = false;
  if (!condAsZeroTest(E, Val, NonZero) || NonZero || !Val)
    return false;
  Val = peelCond(Val);
  if (!Val || Val->Kind != ExprKind::Var)
    return false;
  MedVar Wanted = Val->Var;
  for (int J = static_cast<int>(IfIdx) - 1, Steps = 0; J >= 0 && Steps < 8;
       --J) {
    const HighStmt &P = Body[static_cast<size_t>(J)];
    if (stmtIsSkipResidue(P))
      continue;
    ++Steps;
    MedVar Dest;
    ExprPtr Av;
    if (!isValueAssign(P, Dest, Av) || !Av || !sameMedVar(Dest, Wanted))
      break;
    if (firstCallInExpr(Av.get()))
      return true;
    const HighExpr *Inner = peelCond(Av.get());
    if (Inner && Inner->Kind == ExprKind::Var) {
      Wanted = Inner->Var;
      continue;
    }
    if (!Inner)
      break;
    unsigned Vars = 0;
    MedVar Next;
    std::function<void(const HighExpr *)> Count = [&](const HighExpr *N) {
      if (!N)
        return;
      N = peelCond(N);
      if (!N)
        return;
      if (N->Kind == ExprKind::Var) {
        ++Vars;
        Next = N->Var;
        return;
      }
      if (N->Kind == ExprKind::Const)
        return;
      for (const auto &Op : N->Operands)
        Count(Op.get());
    };
    Count(Inner);
    if (Vars != 1)
      break;
    Wanted = Next;
  }
  return false;
}

static bool preferPositiveIfElse(std::vector<HighStmt> &Body) {
  bool Changed = false;
  for (size_t I = 0; I < Body.size(); ++I) {
    HighStmt &S = Body[I];
    if (S.Kind != StmtKind::IfElse || !S.Cond || S.Body.empty() ||
        S.ElseBody.empty() ||
        !isNegatedCallCondFromPrefix(S.Cond.get(), Body, I) ||
        armIsOnlyTransfer(S.Body) || armIsOnlyTransfer(S.ElseBody))
      continue;
    S.Cond = invertHighCond(std::move(S.Cond));
    std::swap(S.Body, S.ElseBody);
    Changed = true;
  }
  return Changed;
}

static void preferPositiveIfElseNested(std::vector<HighStmt> &Body) {
  preferPositiveIfElse(Body);
  for (HighStmt &S : Body) {
    preferPositiveIfElseNested(S.Body);
    preferPositiveIfElseNested(S.ElseBody);
    preferPositiveIfElseNested(S.DefaultBody);
    for (auto &C : S.Cases)
      preferPositiveIfElseNested(C.Body);
    for (auto &Clause : S.EHClauseBodies)
      preferPositiveIfElseNested(Clause);
  }
}

static bool condIsLogicalCombo(const HighExpr *E) {
  E = peelCond(E);
  return E && E->Kind == ExprKind::BinOp &&
         (E->Op == NdOp::BOOL_AND || E->Op == NdOp::BOOL_OR ||
          E->Op == NdOp::INT_AND || E->Op == NdOp::INT_OR);
}

/// Dest of `v = call(...)` that feeds `if (v)` / `if (v == 0)`, walking
/// leftover view copies. AND/OR guards are not a single call result.
static bool callResultDestFromPrefix(const std::vector<HighStmt> &Body,
                                     size_t IfIdx, MedVar &Out) {
  if (IfIdx >= Body.size() || !Body[IfIdx].Cond)
    return false;
  const HighExpr *E = peelCond(Body[IfIdx].Cond.get());
  if (!E || condIsLogicalCombo(E))
    return false;
  const HighExpr *Val = nullptr;
  bool NonZero = false;
  if (condAsZeroTest(E, Val, NonZero))
    Val = peelCond(Val);
  else
    Val = peelCond(E);
  if (!Val)
    return false;

  auto TakeCallAssign = [&](const MedVar &Dest, const HighExpr *Assigned) {
    const HighExpr *Call = firstCallInExpr(Assigned);
    if (!Call || Dest.Kind == MedVar::Param)
      return false;
    if (Call->Type && Call->Type->Kind == NdTypeKind::Ptr)
      return false;
    Out = Dest;
    return true;
  };

  if (Val->Kind == ExprKind::Call) {
    for (int J = static_cast<int>(IfIdx) - 1, Steps = 0; J >= 0 && Steps < 8;
         --J) {
      const HighStmt &P = Body[static_cast<size_t>(J)];
      if (stmtIsSkipResidue(P))
        continue;
      ++Steps;
      MedVar Dest;
      ExprPtr Av;
      if (!isValueAssign(P, Dest, Av) || !Av)
        break;
      const HighExpr *Call = firstCallInExpr(Av.get());
      if (Call && samePredCall(Call, Val) && TakeCallAssign(Dest, Av.get()))
        return true;
      break;
    }
    return false;
  }

  if (Val->Kind != ExprKind::Var && Val->Kind != ExprKind::Phi)
    return false;
  MedVar Wanted = Val->Var;
  for (int J = static_cast<int>(IfIdx) - 1, Steps = 0; J >= 0 && Steps < 8;
       --J) {
    const HighStmt &P = Body[static_cast<size_t>(J)];
    if (stmtIsSkipResidue(P))
      continue;
    ++Steps;
    MedVar Dest;
    ExprPtr Av;
    if (!isValueAssign(P, Dest, Av) || !Av || !sameMedVar(Dest, Wanted))
      break;
    if (TakeCallAssign(Dest, Av.get()))
      return true;
    const HighExpr *Inner = peelCond(Av.get());
    if (Inner && (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi)) {
      Wanted = Inner->Var;
      continue;
    }
    if (!Inner)
      break;
    unsigned Vars = 0;
    MedVar Next;
    std::function<void(const HighExpr *)> Count = [&](const HighExpr *N) {
      if (!N)
        return;
      N = peelCond(N);
      if (!N)
        return;
      if (N->Kind == ExprKind::Var || N->Kind == ExprKind::Phi) {
        ++Vars;
        Next = N->Var;
        return;
      }
      if (N->Kind == ExprKind::Const)
        return;
      for (const auto &Op : N->Operands)
        Count(Op.get());
    };
    Count(Inner);
    if (Vars != 1)
      break;
    Wanted = Next;
  }
  return false;
}

static std::vector<HighStmt> *callResultTrueArm(HighStmt &S) {
  const HighExpr *Val = nullptr;
  bool NonZero = false;
  if (condAsZeroTest(S.Cond.get(), Val, NonZero)) {
    if (NonZero)
      return &S.Body;
    if (S.Kind == StmtKind::IfElse && !S.ElseBody.empty())
      return &S.ElseBody;
    return nullptr;
  }
  const HighExpr *E = peelCond(S.Cond.get());
  if (E && (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi ||
            E->Kind == ExprKind::Call))
    return &S.Body;
  return nullptr;
}

/// MSVC `test eax; jz` leaves the call result in EAX. HighIR often stores
/// that leftover as integer undef after packing other call args (fmt Value
/// blobs, CString ctor homes). Restore the first such store. Const 0 stays.
static bool isLeftoverCallResultUndef(const HighExpr *Val, TypeRef &Ty) {
  const HighExpr *Inner = peelCond(Val);
  if (!Inner || Inner->Kind != ExprKind::Undef)
    return false;
  Ty = Val->Type ? Val->Type : Inner->Type;
  if (Ty && Ty->Kind == NdTypeKind::Ptr)
    return false;
  if (Ty && Ty->Kind != NdTypeKind::Int)
    return false;
  if (Ty && Ty->Size > 16)
    return false;
  return true;
}

static bool isLoadOfAddr(const HighExpr *E, const HighExpr *Addr) {
  E = peelCond(E);
  if (!E || E->Kind != ExprKind::Load || E->Operands.empty() ||
      !E->Operands[0] || !Addr)
    return false;
  return E->Operands[0]->structuralEq(*Addr);
}

/// MSVC packs leftover EAX into the next same-size Value after
/// `&first`. Keep the sibling store; drop the leftover home so it
/// cannot overlay a different named slot (`record.types_`).
static bool retargetLeftoverUndefToPackedCopy(std::vector<HighStmt> &Stmts,
                                              size_t HomeIdx,
                                              const MedVar &Dest,
                                              const TypeRef &Ty) {
  if (HomeIdx >= Stmts.size() || !Stmts[HomeIdx].StoreAddr)
    return false;
  const HighExpr *HomeAddr = Stmts[HomeIdx].StoreAddr.get();
  MedVar Temp;
  bool HaveTemp = false;
  size_t AssignIdx = static_cast<size_t>(-1);
  size_t PackedIdx = static_cast<size_t>(-1);
  unsigned Looked = 0;
  for (size_t I = HomeIdx + 1; I < Stmts.size() && Looked < 8; ++I) {
    HighStmt &S = Stmts[I];
    if (stmtIsSkipResidue(S))
      continue;
    ++Looked;
    if (!HaveTemp && S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
        isLoadOfAddr(S.Val.get(), HomeAddr)) {
      Temp = S.Dst->Var;
      HaveTemp = true;
      AssignIdx = I;
      continue;
    }
    const HighExpr *Val = nullptr;
    if (S.Kind == StmtKind::Store && S.StoreAddr && S.StoreVal)
      Val = S.StoreVal.get();
    else if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
             S.Dst->Kind == ExprKind::Load && !S.Dst->Operands.empty())
      Val = S.Val.get();
    if (!Val)
      break;
    const bool FromHome = isLoadOfAddr(Val, HomeAddr);
    const bool FromTemp = HaveTemp && (Val->Kind == ExprKind::Var ||
                                       Val->Kind == ExprKind::Phi) &&
                          sameMedVar(Val->Var, Temp);
    if (!FromHome && !FromTemp)
      break;
    const HighExpr *PackedAddr =
        S.Kind == StmtKind::Store ? S.StoreAddr.get() : S.Dst->Operands[0].get();
    if (!PackedAddr || PackedAddr->structuralEq(*HomeAddr))
      break;
    PackedIdx = I;
    break;
  }
  if (PackedIdx == static_cast<size_t>(-1))
    return false;
  TypeRef FillTy = Ty;
  const HighExpr *PackedVal = Stmts[PackedIdx].Kind == StmtKind::Store
                                  ? Stmts[PackedIdx].StoreVal.get()
                                  : Stmts[PackedIdx].Val.get();
  if (PackedVal && PackedVal->Type && PackedVal->Type->Size >= 8)
    FillTy = PackedVal->Type;
  else if (!FillTy || FillTy->Size < 8)
    FillTy = NdType::makeInt(16, false);
  auto Fill = HighExpr::makeVar(Dest, FillTy);
  if (Stmts[PackedIdx].Kind == StmtKind::Store)
    Stmts[PackedIdx].StoreVal = Fill;
  else
    Stmts[PackedIdx].Val = Fill;
  if (AssignIdx != static_cast<size_t>(-1))
    Stmts[AssignIdx].Val = HighExpr::makeVar(Dest, Ty);
  Stmts.erase(Stmts.begin() + static_cast<std::ptrdiff_t>(HomeIdx));
  return true;
}

static bool fillI32UndefFromCallResult(std::vector<HighStmt> &Arm,
                                       const MedVar &Dest) {
  unsigned Walked = 0;
  std::function<bool(std::vector<HighStmt> &)> Walk =
      [&](std::vector<HighStmt> &Stmts) {
        for (size_t I = 0; I < Stmts.size(); ++I) {
          HighStmt &T = Stmts[I];
          if (T.Kind == StmtKind::CxxTry || T.Kind == StmtKind::Block ||
              T.Kind == StmtKind::SEHTry) {
            if (Walk(T.Body))
              return true;
            continue;
          }
          if (stmtIsSkipResidue(T))
            continue;
          if (++Walked > 32)
            return false;
          if (T.Kind != StmtKind::Store || !T.StoreVal)
            continue;
          TypeRef Ty;
          if (!isLeftoverCallResultUndef(T.StoreVal.get(), Ty))
            continue;
          if (!Ty)
            Ty = NdType::makeInt(Dest.Size ? Dest.Size : 4, true);
          if (retargetLeftoverUndefToPackedCopy(Stmts, I, Dest, Ty))
            return true;
          T.StoreVal = HighExpr::makeVar(Dest, Ty);
          return true;
        }
        return false;
      };
  return Walk(Arm);
}

static bool rewriteCallResultUndefStores(std::vector<HighStmt> &Body) {
  bool Changed = false;
  for (HighStmt &S : Body) {
    Changed |= rewriteCallResultUndefStores(S.Body);
    Changed |= rewriteCallResultUndefStores(S.ElseBody);
    Changed |= rewriteCallResultUndefStores(S.DefaultBody);
    for (auto &C : S.Cases)
      Changed |= rewriteCallResultUndefStores(C.Body);
    for (auto &Clause : S.EHClauseBodies)
      Changed |= rewriteCallResultUndefStores(Clause);
  }
  for (size_t I = 0; I < Body.size(); ++I) {
    HighStmt &S = Body[I];
    if ((S.Kind != StmtKind::If && S.Kind != StmtKind::IfElse) || !S.Cond)
      continue;
    MedVar Dest;
    if (!callResultDestFromPrefix(Body, I, Dest))
      continue;
    std::vector<HighStmt> *Arm = callResultTrueArm(S);
    if (!Arm)
      continue;
    Changed |= fillI32UndefFromCallResult(*Arm, Dest);
  }
  return Changed;
}

static const HighExpr *guardPtrFromCond(const HighExpr *E) {
  const HighExpr *Val = nullptr;
  bool NonZero = false;
  if (condAsZeroTest(E, Val, NonZero) && NonZero && Val) {
    Val = peelCond(Val);
    if (Val && Val->Kind != ExprKind::Call)
      return Val;
    return nullptr;
  }
  E = peelCond(E);
  if (E && (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi ||
            E->Kind == ExprKind::Load))
    return E;
  return nullptr;
}

static bool isParentThisExpr(const HighExpr *E) {
  E = peelCond(E);
  return E && (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) &&
         E->Var.Kind == MedVar::Param && E->Var.Id == 0;
}

static HighExpr *stmtCallExpr(HighStmt &S) {
  if (S.Kind == StmtKind::Call && S.CallExpr)
    return S.CallExpr.get();
  if (S.Kind == StmtKind::Assign && S.Val && S.Val->Kind == ExprKind::Call)
    return S.Val.get();
  return nullptr;
}

static bool retargetParentThisTo(HighExpr *C, const HighExpr *P) {
  if (!C || C->Operands.empty() || !C->Operands[0] || !P)
    return false;
  if (!isParentThisExpr(C->Operands[0].get()))
    return false;
  C->Operands[0] = std::make_shared<HighExpr>(*P);
  return true;
}

static bool rewriteThenGuardedCalls(std::vector<HighStmt> &Then,
                                    const HighExpr *P) {
  bool Changed = false;
  bool SeenCall = false;
  std::function<void(std::vector<HighStmt> &)> Walk =
      [&](std::vector<HighStmt> &Stmts) {
        for (HighStmt &T : Stmts) {
          if (HighExpr *C = stmtCallExpr(T)) {
            if (SeenCall)
              Changed |= retargetParentThisTo(C, P);
            SeenCall = true;
          }
          Walk(T.Body);
          Walk(T.ElseBody);
          Walk(T.DefaultBody);
          for (auto &Case : T.Cases)
            Walk(Case.Body);
          for (auto &Clause : T.EHClauseBodies)
            Walk(Clause);
        }
      };
  Walk(Then);
  return Changed;
}

static bool rewriteGuardedCallThis(std::vector<HighStmt> &Body) {
  bool Changed = false;
  for (size_t I = 0; I < Body.size(); ++I) {
    HighStmt &S = Body[I];
    if ((S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse) && S.Cond) {
      const HighExpr *Val = nullptr;
      bool NonZero = false;
      const bool NullThen =
          condAsZeroTest(S.Cond.get(), Val, NonZero) && Val &&
          Val->Kind != ExprKind::Call && !NonZero &&
          !stmtListFallsThrough(S.Body);
      if (NullThen) {
        // `if (!p) { ...; return; } vcall(parent_this)` is still guarded by
        // `p`. Retarget only the first fallthrough indirect call so a later
        // parent method is not stolen.
        if (S.Kind == StmtKind::IfElse) {
          for (HighStmt &T : S.ElseBody) {
            if (stmtIsSkipResidue(T))
              continue;
            if (HighExpr *C = stmtCallExpr(T)) {
              if (C->IsIndirectCall)
                Changed |= retargetParentThisTo(C, Val);
            }
            break;
          }
        } else {
          for (size_t J = I + 1; J < Body.size(); ++J) {
            if (stmtIsSkipResidue(Body[J]))
              continue;
            if (HighExpr *C = stmtCallExpr(Body[J])) {
              if (C->IsIndirectCall)
                Changed |= retargetParentThisTo(C, Val);
            }
            break;
          }
        }
      } else if (const HighExpr *P = guardPtrFromCond(S.Cond.get())) {
        Changed |= rewriteThenGuardedCalls(S.Body, P);
      }
    }
    Changed |= rewriteGuardedCallThis(S.Body);
    Changed |= rewriteGuardedCallThis(S.ElseBody);
    Changed |= rewriteGuardedCallThis(S.DefaultBody);
    for (auto &C : S.Cases)
      Changed |= rewriteGuardedCallThis(C.Body);
    for (auto &Clause : S.EHClauseBodies)
      Changed |= rewriteGuardedCallThis(Clause);
  }
  return Changed;
}

static bool hoistIdenticalIfElseSuffixes(std::vector<HighStmt> &Body) {
  bool Changed = false;
  for (int I = static_cast<int>(Body.size()) - 1; I >= 0; --I) {
    HighStmt &S = Body[static_cast<size_t>(I)];
    if (S.Kind != StmtKind::IfElse)
      continue;
    std::vector<HighStmt> Joins;
    while (!S.Body.empty() && !S.ElseBody.empty()) {
      MedVar DestA, DestB;
      ExprPtr ValA, ValB;
      if (!isValueAssign(S.Body.back(), DestA, ValA) ||
          !isValueAssign(S.ElseBody.back(), DestB, ValB) || !ValA || !ValB ||
          DestA.Kind != DestB.Kind || DestA.Id != DestB.Id ||
          !ValA->structuralEq(*ValB))
        break;
      Joins.push_back(std::move(S.ElseBody.back()));
      S.Body.pop_back();
      S.ElseBody.pop_back();
    }
    if (Joins.empty())
      continue;
    size_t At = static_cast<size_t>(I + 1);
    for (auto It = Joins.rbegin(); It != Joins.rend(); ++It)
      Body.insert(Body.begin() + static_cast<long>(At++), std::move(*It));
    Changed = true;
  }
  return Changed;
}

static void hoistIdenticalIfElseSuffixesNested(std::vector<HighStmt> &Body) {
  for (HighStmt &S : Body) {
    hoistIdenticalIfElseSuffixesNested(S.Body);
    hoistIdenticalIfElseSuffixesNested(S.ElseBody);
    hoistIdenticalIfElseSuffixesNested(S.DefaultBody);
    for (auto &C : S.Cases)
      hoistIdenticalIfElseSuffixesNested(C.Body);
    for (auto &Clause : S.EHClauseBodies)
      hoistIdenticalIfElseSuffixesNested(Clause);
  }
  hoistIdenticalIfElseSuffixes(Body);
}

static ExprPtr composeTrailingAssigns(const std::vector<HighStmt> &Body,
                                      size_t Begin, size_t End, ExprPtr Cond) {
  if (!Cond || Begin > End)
    return Cond;
  for (size_t N = End; N > Begin; --N) {
    const HighStmt &S = Body[N - 1];
    if (!prefixOkForChain(S))
      break;
    MedVar Dest;
    ExprPtr Val;
    if (!isValueAssign(S, Dest, Val) || !exprUsesVar(Cond.get(), Dest))
      continue;
    Cond = replaceVarInExpr(Cond, Dest, Val);
  }
  return Cond;
}

static ExprPtr outerCondFromSiblings(const std::vector<HighStmt> &Body,
                                     size_t OuterI, ExprPtr Cond) {
  if (!Cond)
    return Cond;
  for (size_t K = OuterI; K > 0; --K) {
    const HighStmt &S = Body[K - 1];
    if (!prefixOkForChain(S))
      break;
    MedVar Dest;
    ExprPtr Val;
    if (!isValueAssign(S, Dest, Val) || !exprUsesVar(Cond.get(), Dest))
      continue;
    Cond = replaceVarInExpr(Cond, Dest, Val);
  }
  return Cond;
}

static bool prefixesAllowSameTempId(const std::vector<HighStmt> &Body,
                                    size_t Begin, size_t End) {
  bool AnyReload = false;
  for (size_t K = Begin; K < End; ++K) {
    if (armIsSkippable({Body[K]}))
      continue;
    MedVar Dest;
    ExprPtr Val;
    if (!isValueAssign(Body[K], Dest, Val) || !Val)
      continue;
    if (Val->Kind == ExprKind::Load)
      AnyReload = true;
    else if (Val->Kind != ExprKind::Call)
      return false;
  }
  return AnyReload;
}

static bool bodyIsNestedIf(const std::vector<HighStmt> &Body, size_t &IfIdx) {
  IfIdx = SIZE_MAX;
  for (size_t I = 0; I < Body.size(); ++I) {
    if (Body[I].Kind == StmtKind::If && Body[I].Cond &&
        Body[I].ElseBody.empty()) {
      if (IfIdx != SIZE_MAX)
        return false;
      IfIdx = I;
      continue;
    }
    if (IfIdx == SIZE_MAX && prefixOkForChain(Body[I]))
      continue;
    if (!armIsSkippable({Body[I]}))
      return false;
  }
  return IfIdx != SIZE_MAX;
}

static bool stmtUsesVar(const HighStmt &S, const MedVar &V) {
  return exprUsesVar(S.Cond.get(), V) || exprUsesVar(S.Dst.get(), V) ||
         exprUsesVar(S.Val.get(), V) || exprUsesVar(S.CallExpr.get(), V) ||
         exprUsesVar(S.RetVal.get(), V) || exprUsesVar(S.StoreAddr.get(), V) ||
         exprUsesVar(S.StoreVal.get(), V) || exprUsesVar(S.SwitchExpr.get(), V);
}

static bool stmtsUseVar(const std::vector<HighStmt> &Stmts, const MedVar &V) {
  bool Used = false;
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (stmtUsesVar(S, V))
      Used = true;
  });
  return Used;
}

static bool exprHasSameCall(const HighExpr *E, va_t Addr,
                            const std::string &Target) {
  if (!E)
    return false;
  if (E->Kind == ExprKind::Call &&
      ((Addr && E->CallAddr == Addr) ||
       (!Target.empty() && E->CallTarget == Target)))
    return true;
  for (const auto &Op : E->Operands)
    if (exprHasSameCall(Op.get(), Addr, Target))
      return true;
  return false;
}

static bool isPredicateOrLoadAssign(const HighStmt &S, const ExprPtr &Pred) {
  MedVar Dest;
  ExprPtr Val;
  if (!isValueAssign(S, Dest, Val) || !Val)
    return false;
  if (Val->Kind == ExprKind::Load)
    return true;
  return Val->Kind == ExprKind::Call &&
         exprHasSameCall(Pred.get(), Val->CallAddr, Val->CallTarget);
}

static bool exprReadsVarReal(const HighExpr *E, const MedVar &V) {
  if (!E)
    return false;
  if (E->Kind == ExprKind::Call) {
    if (E->Operands.empty())
      return false;
    return exprUsesVar(E->Operands[0].get(), V);
  }
  if (E->Kind == ExprKind::Var && sameMedVar(E->Var, V))
    return true;
  for (const auto &Op : E->Operands)
    if (exprReadsVarReal(Op.get(), V))
      return true;
  return false;
}

static bool stmtReadsVarReal(const HighStmt &S, const MedVar &V) {
  if (exprReadsVarReal(S.Cond.get(), V) || exprReadsVarReal(S.RetVal.get(), V) ||
      exprReadsVarReal(S.StoreAddr.get(), V) ||
      exprReadsVarReal(S.StoreVal.get(), V) ||
      exprReadsVarReal(S.SwitchExpr.get(), V))
    return true;
  if (S.CallExpr)
    return exprReadsVarReal(S.CallExpr.get(), V);
  MedVar Dest;
  ExprPtr Val;
  if (isValueAssign(S, Dest, Val) && sameMedVar(Dest, V)) {
    if (!Val || Val->Kind == ExprKind::Undef)
      return false;
    if (Val->Kind == ExprKind::Call)
      return exprReadsVarReal(Val.get(), V);
    return exprUsesVar(Val.get(), V);
  }
  return exprReadsVarReal(S.Val.get(), V) || exprReadsVarReal(S.Dst.get(), V);
}

static bool destHasRealUse(const std::vector<HighStmt> &Body, size_t Skip,
                           const MedVar &Dest) {
  bool Used = false;
  for (size_t I = 0; I < Body.size() && !Used; ++I) {
    if (I == Skip)
      continue;
    walkStatementTree(Body[I], [&](const HighStmt &S) {
      if (stmtReadsVarReal(S, Dest))
        Used = true;
    });
  }
  return Used;
}

static void dropUnusedTrailingAssigns(std::vector<HighStmt> &Body, size_t End,
                                      const ExprPtr &Pred) {
  size_t I = End;
  while (I > 0) {
    --I;
    if (Body[I].Kind == StmtKind::Call && Body[I].CallExpr &&
        exprHasSameCall(Pred.get(), Body[I].CallExpr->CallAddr,
                        Body[I].CallExpr->CallTarget)) {
      Body.erase(Body.begin() + static_cast<long>(I));
      if (End > I)
        --End;
      continue;
    }
    MedVar Dest;
    ExprPtr Val;
    if (!isValueAssign(Body[I], Dest, Val) || !Val) {
      if (armIsSkippable({Body[I]}) || isEmptyLabel(Body[I]))
        continue;
      break;
    }
    const bool RealUse = destHasRealUse(Body, I, Dest);
    const bool PredCall = Val->Kind == ExprKind::Call &&
                          exprHasSameCall(Pred.get(), Val->CallAddr, Val->CallTarget);
    if (PredCall && !RealUse) {
      Body.erase(Body.begin() + static_cast<long>(I));
      if (End > I)
        --End;
      continue;
    }
    if (Val->Kind == ExprKind::Load && RealUse)
      continue;
    if (Val->Kind == ExprKind::Load && !RealUse) {
      Body.erase(Body.begin() + static_cast<long>(I));
      if (End > I)
        --End;
      continue;
    }
    if (armIsSkippable({Body[I]}) || isEmptyLabel(Body[I]))
      continue;
    break;
  }
}

static std::optional<ExprPtr> peelInnerAgainstOuter(
    const HighStmt &Stmt, size_t OuterI, const std::vector<HighStmt> &Parent,
    size_t J) {
  if (J >= Stmt.Body.size())
    return std::nullopt;
  const HighStmt &Inner = Stmt.Body[J];
  if ((Inner.Kind != StmtKind::If && Inner.Kind != StmtKind::IfElse) ||
      !Inner.Cond)
    return std::nullopt;
  const bool SameTempId = prefixesAllowSameTempId(Stmt.Body, 0, J);
  if (auto Rest = dropAndPrefix(Stmt.Cond, Inner.Cond, SameTempId))
    return Rest;
  return dropAndPrefix(outerCondFromSiblings(Parent, OuterI, Stmt.Cond),
                       composeTrailingAssigns(Stmt.Body, 0, J, Inner.Cond),
                       SameTempId);
}

static std::optional<ExprPtr>
composePrefixesIntoCond(const std::vector<HighStmt> &Body, size_t Begin,
                        size_t End, ExprPtr Cond) {
  if (!Cond || Begin > End)
    return std::nullopt;
  for (size_t I = Begin; I < End; ++I)
    if (!prefixOkForChain(Body[I]))
      return std::nullopt;
  for (size_t N = End; N > Begin; --N) {
    const size_t I = N - 1;
    MedVar Dest;
    ExprPtr Val;
    if (!isValueAssign(Body[I], Dest, Val) ||
        !exprUsesVar(Cond.get(), Dest))
      continue;
    Cond = replaceVarInExpr(Cond, Dest, Val);
  }
  for (size_t I = Begin; I < End; ++I) {
    MedVar Dest;
    ExprPtr Val;
    if (isValueAssign(Body[I], Dest, Val) &&
        exprUsesVar(Cond.get(), Dest))
      return std::nullopt;
    if (Body[I].Kind == StmtKind::Assign && Body[I].Dst &&
        Body[I].Dst->Kind == ExprKind::Var &&
        exprUsesVar(Cond.get(), Body[I].Dst->Var))
      return std::nullopt;
  }
  return Cond;
}

struct PredChain {
  ExprPtr Cond;
  std::vector<HighStmt> Work;
  std::vector<HighStmt> SkipPrefix;
};

static std::optional<PredChain> collectPredChain(const HighStmt &S, va_t Join,
                                                 ExprPtr CondOverride);

static std::optional<PredChain>
collectPredChainFromList(const std::vector<HighStmt> &Body, va_t Join,
                         ExprPtr ExtraCond) {
  if (Body.empty())
    return std::nullopt;
  size_t LastI = Body.size();
  while (LastI > 0 && isEmptyLabel(Body[LastI - 1]))
    --LastI;
  if (LastI == 0)
    return std::nullopt;
  const HighStmt &Last = Body[LastI - 1];
  if ((Last.Kind != StmtKind::If && Last.Kind != StmtKind::IfElse) ||
      !Last.Cond)
    return std::nullopt;
  for (size_t I = 0; I + 1 < LastI; ++I)
    if (!prefixOkForChain(Body[I]))
      return std::nullopt;

  ExprPtr LastCond = Last.Cond;
  std::vector<std::pair<MedVar, ExprPtr>> Composed;
  std::vector<char> Folded(LastI, 0);
  // Nearest assign to the if is the reaching def. ABI copies between a
  // predicate load and the compare must not hide that def.
  for (size_t N = LastI - 1; N > 0; --N) {
    const size_t I = N - 1;
    MedVar Dest;
    ExprPtr Val;
    if (!isValueAssign(Body[I], Dest, Val) ||
        !exprUsesVar(LastCond.get(), Dest))
      continue;
    LastCond = replaceVarInExpr(LastCond, Dest, Val);
    Composed.push_back({Dest, std::move(Val)});
    Folded[I] = 1;
  }
  std::vector<HighStmt> KeptPrefix;
  for (size_t I = 0; I + 1 < LastI; ++I) {
    if (Folded[I])
      continue;
    MedVar Dest;
    ExprPtr Val;
    if (isValueAssign(Body[I], Dest, Val) &&
        exprUsesVar(LastCond.get(), Dest))
      return std::nullopt;
    if (Body[I].Kind == StmtKind::Assign && Body[I].Dst &&
        Body[I].Dst->Kind == ExprKind::Var &&
        exprUsesVar(LastCond.get(), Body[I].Dst->Var))
      return std::nullopt;
    KeptPrefix.push_back(Body[I]);
  }

  auto Inner = collectPredChain(Last, Join, LastCond);
  if (!Inner)
    return std::nullopt;
  for (const auto &[Dest, Val] : Composed)
    replaceVarInStmts(Inner->Work, Dest, Val);
  std::vector<HighStmt> Work;
  Work.reserve(KeptPrefix.size() + Inner->Work.size());
  for (HighStmt &S : KeptPrefix)
    Work.push_back(std::move(S));
  for (HighStmt &S : Inner->Work)
    Work.push_back(std::move(S));
  Inner->Work = std::move(Work);
  if (ExtraCond)
    Inner->Cond =
        HighExpr::makeBinop(NdOp::BOOL_AND, std::move(ExtraCond), Inner->Cond);
  return Inner;
}

static std::optional<PredChain> collectPredChain(const HighStmt &S, va_t Join,
                                                 ExprPtr CondOverride) {
  ExprPtr Cond = CondOverride ? std::move(CondOverride) : S.Cond;
  if (!Cond || !Join || Join == InvalidVA)
    return std::nullopt;
  auto WithSkip = [&](PredChain C, const std::vector<HighStmt> &Skip) {
    C.SkipPrefix.insert(C.SkipPrefix.begin(), Skip.begin(), Skip.end());
    return C;
  };
  if (S.Kind == StmtKind::If && bodyWorkGotoJoin(S.Body, Join)) {
    PredChain C;
    C.Cond = std::move(Cond);
    C.Work.assign(S.Body.begin(), S.Body.end() - 1);
    return C;
  }
  if (S.Kind == StmtKind::IfElse && armIsSkippable(S.ElseBody) &&
      bodyWorkGotoJoin(S.Body, Join)) {
    PredChain C;
    C.Cond = std::move(Cond);
    C.Work.assign(S.Body.begin(), S.Body.end() - 1);
    C.SkipPrefix = S.ElseBody;
    return C;
  }
  if (S.Kind == StmtKind::IfElse && armIsSkippable(S.Body) &&
      bodyWorkGotoJoin(S.ElseBody, Join)) {
    PredChain C;
    C.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, std::move(Cond));
    C.Work.assign(S.ElseBody.begin(), S.ElseBody.end() - 1);
    C.SkipPrefix = S.Body;
    return C;
  }
  if (S.Kind == StmtKind::If)
    return collectPredChainFromList(S.Body, Join, std::move(Cond));
  if (S.Kind == StmtKind::IfElse && armIsSkippable(S.Body)) {
    auto Inner = collectPredChainFromList(
        S.ElseBody, Join, HighExpr::makeUnary(NdOp::BOOL_NOT, std::move(Cond)));
    return Inner ? std::optional(WithSkip(std::move(*Inner), S.Body))
                 : std::nullopt;
  }
  if (S.Kind == StmtKind::IfElse && armIsSkippable(S.ElseBody)) {
    auto Inner = collectPredChainFromList(S.Body, Join, std::move(Cond));
    return Inner ? std::optional(WithSkip(std::move(*Inner), S.ElseBody))
                 : std::nullopt;
  }
  return std::nullopt;
}

static bool isSkippablePad(const HighStmt &S) {
  return S.Kind == StmtKind::Nop ||
         (S.Kind == StmtKind::Block && S.Body.empty());
}

static bool stmtHasStructuredRegion(const HighStmt &S) {
  bool Has = false;
  walkStatementTree(S, [&](const HighStmt &T) {
    if (T.Kind == StmtKind::CxxTry || T.Kind == StmtKind::SEHTry ||
        T.Kind == StmtKind::ItaniumTry || T.Kind == StmtKind::While ||
        T.Kind == StmtKind::DoWhile || T.Kind == StmtKind::For ||
        T.Kind == StmtKind::Switch)
      Has = true;
  });
  return Has;
}

static bool sameJoinDest(const MedVar &A, const MedVar &B) {
  if (A.RenameTag >= 0 && B.RenameTag >= 0)
    return A.RenameTag == B.RenameTag;
  if (A.Kind != B.Kind)
    return false;
  if (A.Kind == MedVar::Reg)
    return A.RegOff == B.RegOff && A.Size == B.Size;
  if (A.Kind == MedVar::Stack)
    return A.StackOff == B.StackOff && A.Size == B.Size;
  return A.Id == B.Id;
}

static bool isJoinClutter(const HighStmt &S, const MedVar &V) {
  if (isSkippablePad(S) || S.IsPhiCopy)
    return true;
  MedVar Dest;
  ExprPtr Val;
  if (!isValueAssign(S, Dest, Val) || !Val)
    return false;
  if (sameJoinDest(Dest, V))
    return Val->Kind == ExprKind::Undef || Val->Kind == ExprKind::Const;
  return Val->Kind == ExprKind::Undef || Val->Kind == ExprKind::Load ||
         exprIsScalarMove(Val.get(), true);
}

static bool exprUsesJoinDest(const HighExpr *E, const MedVar &Dest) {
  if (!E)
    return false;
  if ((E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) &&
      sameJoinDest(E->Var, Dest))
    return true;
  for (const auto &Op : E->Operands)
    if (exprUsesJoinDest(Op.get(), Dest))
      return true;
  return false;
}

static bool stmtUsesJoinDest(const HighStmt &S, const MedVar &Dest) {
  return exprUsesJoinDest(S.Val.get(), Dest) ||
         exprUsesJoinDest(S.CallExpr.get(), Dest) ||
         exprUsesJoinDest(S.Dst.get(), Dest);
}

static bool stmtsUseJoinDest(const std::vector<HighStmt> &Stmts,
                             const MedVar &V) {
  bool Used = false;
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (stmtUsesJoinDest(S, V))
      Used = true;
  });
  return Used;
}

static bool destBeforeJoinGoto(const std::vector<HighStmt> &Stmts, size_t GotoI,
                               MedVar &V, size_t *AssignI = nullptr,
                               const MedVar *Wanted = nullptr);

static bool destBeforeJoinGoto(const std::vector<HighStmt> &Stmts, size_t GotoI,
                               MedVar &V, size_t *AssignI, const MedVar *Wanted) {
  for (size_t J = GotoI; J > 0;) {
    --J;
    if (isSkippablePad(Stmts[J]) || Stmts[J].Kind == StmtKind::Goto)
      continue;
    if (Stmts[J].Kind == StmtKind::If || Stmts[J].Kind == StmtKind::IfElse) {
      if (destBeforeJoinGoto(Stmts[J].Body, Stmts[J].Body.size(), V, nullptr,
                             Wanted))
        return true;
      if (Stmts[J].Kind == StmtKind::IfElse &&
          destBeforeJoinGoto(Stmts[J].ElseBody, Stmts[J].ElseBody.size(), V,
                             nullptr, Wanted))
        return true;
      continue;
    }
    MedVar Dest;
    ExprPtr Val;
    if (!isValueAssign(Stmts[J], Dest, Val) || !Val)
      return false;
    if (Val->Kind == ExprKind::Undef || Val->Kind == ExprKind::Load ||
        Val->Kind == ExprKind::Const)
      continue;
    if (Stmts[J].IsPhiCopy && Val->Kind == ExprKind::Var) {
      if (!Wanted || !sameJoinDest(Dest, *Wanted))
        continue;
    }
    V = Dest;
    if (AssignI)
      *AssignI = J;
    return true;
  }
  return false;
}

static bool prefixIsJoinSkip(const std::vector<HighStmt> &Stmts, size_t GotoI) {
  for (size_t J = GotoI; J-- > 0;) {
    if (isSkippablePad(Stmts[J]) || Stmts[J].IsPhiCopy)
      continue;
    MedVar Dest;
    ExprPtr Val;
    if (isValueAssign(Stmts[J], Dest, Val) && Val &&
        (Val->Kind == ExprKind::Undef || Val->Kind == ExprKind::Load))
      continue;
    if (Stmts[J].Kind == StmtKind::If || Stmts[J].Kind == StmtKind::IfElse)
      continue;
    return false;
  }
  return true;
}

static size_t countStmtTree(const HighStmt &S) {
  size_t N = 0;
  walkStatementTree(S, [&](const HighStmt &T) {
    if (isSkippablePad(T) || T.IsPhiCopy)
      return;
    MedVar Dest;
    ExprPtr Val;
    if (isValueAssign(T, Dest, Val) && Val &&
        (Val->Kind == ExprKind::Undef || Val->Kind == ExprKind::Load))
      return;
    ++N;
  });
  return N;
}

static bool everyJoinGotoHasIncoming(const HighStmt &S, va_t Join,
                                     const MedVar *Wanted = nullptr) {
  bool Ok = true;
  bool Saw = false;
  bool AnyIncoming = false;
  std::function<void(const std::vector<HighStmt> &)> Walk =
      [&](const std::vector<HighStmt> &Stmts) {
        for (size_t I = 0; I < Stmts.size(); ++I) {
          if (Stmts[I].Kind == StmtKind::Goto && Stmts[I].GotoTarget == Join) {
            Saw = true;
            MedVar Incoming;
            if (destBeforeJoinGoto(Stmts, I, Incoming, nullptr, Wanted))
              AnyIncoming = true;
            else if (!prefixIsJoinSkip(Stmts, I))
              Ok = false;
          }
          Walk(Stmts[I].Body);
          Walk(Stmts[I].ElseBody);
          for (const auto &C : Stmts[I].Cases)
            Walk(C.Body);
          Walk(Stmts[I].DefaultBody);
          for (const auto &Clause : Stmts[I].EHClauseBodies)
            Walk(Clause);
        }
      };
  Walk(S.Body);
  Walk(S.ElseBody);
  return Ok && Saw && AnyIncoming;
}

static void retargetFallthroughIncomings(std::vector<HighStmt> &Stmts,
                                         const MedVar &Dest) {
  for (size_t J = Stmts.size(); J > 0;) {
    --J;
    if (isSkippablePad(Stmts[J]) || Stmts[J].Kind == StmtKind::Goto)
      continue;
    if (Stmts[J].Kind == StmtKind::If || Stmts[J].Kind == StmtKind::IfElse) {
      retargetFallthroughIncomings(Stmts[J].Body, Dest);
      if (Stmts[J].Kind == StmtKind::IfElse)
        retargetFallthroughIncomings(Stmts[J].ElseBody, Dest);
      return;
    }
    MedVar Incoming;
    ExprPtr Val;
    if (!isValueAssign(Stmts[J], Incoming, Val) || !Val)
      return;
    if (Val->Kind == ExprKind::Undef || Val->Kind == ExprKind::Load ||
        Val->Kind == ExprKind::Const)
      continue;
    if (Stmts[J].IsPhiCopy && Val->Kind == ExprKind::Var &&
        !sameJoinDest(Incoming, Dest))
      continue;
    Stmts[J].Dst =
        HighExpr::makeVar(Dest, Stmts[J].Dst ? Stmts[J].Dst->Type : nullptr);
    return;
  }
}

static void retargetIncomingBeforeJoinGotos(std::vector<HighStmt> &Stmts,
                                            va_t Join, const MedVar &Dest) {
  for (HighStmt &S : Stmts) {
    retargetIncomingBeforeJoinGotos(S.Body, Join, Dest);
    retargetIncomingBeforeJoinGotos(S.ElseBody, Join, Dest);
    for (auto &C : S.Cases)
      retargetIncomingBeforeJoinGotos(C.Body, Join, Dest);
    retargetIncomingBeforeJoinGotos(S.DefaultBody, Join, Dest);
    for (auto &Clause : S.EHClauseBodies)
      retargetIncomingBeforeJoinGotos(Clause, Join, Dest);
  }
  for (size_t I = 0; I < Stmts.size(); ++I) {
    if (Stmts[I].Kind != StmtKind::Goto || Stmts[I].GotoTarget != Join)
      continue;
    MedVar Incoming;
    size_t AssignI = SIZE_MAX;
    if (destBeforeJoinGoto(Stmts, I, Incoming, &AssignI, &Dest) &&
        AssignI < Stmts.size()) {
      Stmts[AssignI].Dst =
          HighExpr::makeVar(Dest, Stmts[AssignI].Dst ? Stmts[AssignI].Dst->Type
                                                     : nullptr);
      continue;
    }
    retargetFallthroughIncomings(Stmts, Dest);
  }
}

static HighStmt makeVarAssign(const MedVar &V, const ExprPtr &Val) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Dst = HighExpr::makeVar(V, Val ? Val->Type : nullptr);
  S.Val = Val;
  return S;
}

static void retargetJoinAssigns(std::vector<HighStmt> &Body, const MedVar &V) {
  for (HighStmt &S : Body) {
    MedVar Dest;
    ExprPtr Val;
    if (isValueAssign(S, Dest, Val) && sameJoinDest(Dest, V) &&
        !sameMedVar(Dest, V)) {
      S.Dst = HighExpr::makeVar(V, S.Dst->Type ? S.Dst->Type : nullptr);
    }
    retargetJoinAssigns(S.Body, V);
    retargetJoinAssigns(S.ElseBody, V);
    for (auto &C : S.Cases)
      retargetJoinAssigns(C.Body, V);
    retargetJoinAssigns(S.DefaultBody, V);
    for (auto &Clause : S.EHClauseBodies)
      retargetJoinAssigns(Clause, V);
  }
}

static void stripAssignGotoJoin(std::vector<HighStmt> &Body, const MedVar &V,
                                va_t Join) {
  for (HighStmt &S : Body) {
    stripAssignGotoJoin(S.Body, V, Join);
    stripAssignGotoJoin(S.ElseBody, V, Join);
    for (auto &C : S.Cases)
      stripAssignGotoJoin(C.Body, V, Join);
    stripAssignGotoJoin(S.DefaultBody, V, Join);
    for (auto &Clause : S.EHClauseBodies)
      stripAssignGotoJoin(Clause, V, Join);
  }
  std::vector<HighStmt> Out;
  Out.reserve(Body.size());
  for (size_t I = 0; I < Body.size(); ++I) {
    if (Body[I].Kind == StmtKind::Goto && Body[I].GotoTarget == Join)
      continue;
    Out.push_back(std::move(Body[I]));
  }
  Body = std::move(Out);
}

static bool treeWritesJoinDest(const HighStmt &S, const MedVar &V) {
  bool Writes = false;
  walkStatementTree(S, [&](const HighStmt &T) {
    MedVar Dest;
    ExprPtr Val;
    if (isValueAssign(T, Dest, Val) && Val && sameJoinDest(Dest, V) &&
        Val->Kind != ExprKind::Undef && Val->Kind != ExprKind::Load &&
        Val->Kind != ExprKind::Const)
      Writes = true;
  });
  return Writes;
}

static bool treeHasSameAssign(const HighStmt &S, const MedVar &V,
                              const ExprPtr &Def) {
  bool Hit = false;
  if (!Def)
    return false;
  walkStatementTree(S, [&](const HighStmt &T) {
    MedVar Dest;
    ExprPtr Val;
    if (isValueAssign(T, Dest, Val) && Val && sameJoinDest(Dest, V) &&
        Val->structuralEq(*Def))
      Hit = true;
  });
  return Hit;
}

static bool stmtIsJoinClobber(const HighStmt &S) {
  if (isSkippablePad(S) || S.Kind == StmtKind::Goto)
    return true;
  MedVar Dest;
  ExprPtr Val;
  if (!isValueAssign(S, Dest, Val) || !Val)
    return false;
  return Val->Kind == ExprKind::Undef || Val->Kind == ExprKind::Const ||
         Val->Kind == ExprKind::Load || Val->Kind == ExprKind::Var ||
         S.IsPhiCopy || exprIsScalarMove(Val.get(), true);
}

static bool armIsJoinClobber(const std::vector<HighStmt> &Arm) {
  return std::all_of(Arm.begin(), Arm.end(), stmtIsJoinClobber);
}

static void coverVarFallthrough(std::vector<HighStmt> &Body, const MedVar &V,
                                const ExprPtr &Def, va_t Join = 0) {
  size_t End = Body.size();
  while (End > 0 &&
         (isSkippablePad(Body[End - 1]) || isJoinClutter(Body[End - 1], V) ||
          (Join && Body[End - 1].Kind == StmtKind::Goto &&
           Body[End - 1].GotoTarget == Join)))
    --End;
  if (End == 0) {
    Body.push_back(makeVarAssign(V, Def));
    return;
  }
  HighStmt &Last = Body[End - 1];
  bool PrefixWrites = false;
  for (size_t K = 0; K + 1 < End; ++K)
    if (treeWritesJoinDest(Body[K], V))
      PrefixWrites = true;
  if (Last.Kind == StmtKind::If) {
    if (PrefixWrites) {
      if (armIsJoinClobber(Last.Body) || bodyIsSkipGoto(Last.Body)) {
        Last.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Last.Cond);
        Last.Body = {makeVarAssign(V, Def)};
        Last.ElseBody.clear();
        Last.Kind = StmtKind::If;
      }
      return;
    }
    if (Last.Body.empty() || armIsJoinClobber(Last.Body) ||
        bodyIsSkipGoto(Last.Body))
      return;
    coverVarFallthrough(Last.Body, V, Def, Join);
    Last.Kind = StmtKind::IfElse;
    Last.ElseBody = {makeVarAssign(V, Def)};
    return;
  }
  if (Last.Kind == StmtKind::IfElse) {
    if (PrefixWrites)
      return;
    coverVarFallthrough(Last.Body, V, Def, Join);
    coverVarFallthrough(Last.ElseBody, V, Def, Join);
    return;
  }
  MedVar Dest;
  ExprPtr Val;
  if (isValueAssign(Last, Dest, Val) && sameJoinDest(Dest, V))
    return;
  for (size_t K = 0; K < End; ++K)
    if (treeWritesJoinDest(Body[K], V))
      return;
  Body.push_back(makeVarAssign(V, Def));
}

static bool stmtHasCallOrStore(const HighStmt &S) {
  if (S.Kind == StmtKind::Call || S.Kind == StmtKind::Store)
    return true;
  MedVar Dest;
  ExprPtr Val;
  return isValueAssign(S, Dest, Val) && Val && Val->Kind == ExprKind::Call;
}

static bool isRealJoinValue(const HighExpr *V) {
  if (!V || V->Kind == ExprKind::Undef || V->Kind == ExprKind::Load ||
      V->Kind == ExprKind::Var || V->Kind == ExprKind::Phi)
    return false;
  if (V->Kind == ExprKind::Const)
    return true;
  return !exprIsScalarMove(V, false);
}

static bool peelJoinDefaultAssign(const HighStmt &S, MedVar &Dest,
                                  ExprPtr &Val) {
  if (isValueAssign(S, Dest, Val) && isRealJoinValue(Val.get()))
    return true;
  if (S.Kind != StmtKind::Block && S.Kind != StmtKind::Nop)
    return false;
  bool Found = false;
  for (const HighStmt &T : S.Body) {
    if (isSkippablePad(T))
      continue;
    MedVar InnerDest;
    ExprPtr InnerVal;
    if (!isValueAssign(T, InnerDest, InnerVal) || !InnerVal)
      return false;
    if (!isRealJoinValue(InnerVal.get()))
      continue;
    if (Found)
      return false;
    Dest = InnerDest;
    Val = InnerVal;
    Found = true;
  }
  return Found;
}

static bool isJoinValueGotoIf(const HighStmt &S, va_t Join, const MedVar *Wanted,
                              MedVar *Dest, std::vector<HighStmt> *ThenWork) {
  if (S.Kind != StmtKind::If || !S.Cond || !S.ElseBody.empty() ||
      S.Body.empty() || S.Body.back().Kind != StmtKind::Goto)
    return false;
  const va_t Target = S.Body.back().GotoTarget;
  if (!Target || Target == InvalidVA || (Join && Target != Join))
    return false;
  MedVar Found;
  size_t AssignI = SIZE_MAX;
  if (!destBeforeJoinGoto(S.Body, S.Body.size() - 1, Found, &AssignI, Wanted))
    return false;
  if (Wanted && !sameJoinDest(Found, *Wanted))
    return false;
  if (AssignI >= S.Body.size())
    return false;
  if (Dest)
    *Dest = Found;
  if (ThenWork) {
    ThenWork->clear();
    for (size_t I = 0; I < S.Body.size(); ++I) {
      if (S.Body[I].Kind == StmtKind::Goto && S.Body[I].GotoTarget == Target)
        continue;
      MedVar ClutterDest;
      ExprPtr ClutterVal;
      const bool LoadForDest =
          isValueAssign(S.Body[I], ClutterDest, ClutterVal) && ClutterVal &&
          ClutterVal->Kind == ExprKind::Load;
      if (I != AssignI && isJoinClutter(S.Body[I], Found) && !LoadForDest)
        continue;
      ThenWork->push_back(S.Body[I]);
      if (I == AssignI && Wanted)
        ThenWork->back().Dst = HighExpr::makeVar(
            *Wanted, S.Body[I].Dst ? S.Body[I].Dst->Type : nullptr);
    }
    if (ThenWork->empty())
      return false;
  }
  return true;
}

/// `if (c1) { dest=a; goto L; } if (c2) { dest=b; goto L; } dest=def; use(dest)`
/// plus an optional skip-goto to the default label. Turns the join into
/// if/else-if/else so later invert-skip does not see sibling gotos.
static bool foldJoinValueGotoChain(std::vector<HighStmt> &Body) {
  for (size_t I = 0; I < Body.size(); ++I) {
    va_t Join = 0;
    MedVar Dest;
    std::vector<HighStmt> FirstWork;
    if (!isJoinValueGotoIf(Body[I], 0, nullptr, &Dest, &FirstWork))
      continue;
    Join = Body[I].Body.back().GotoTarget;

    struct Arm {
      size_t Index = 0;
      ExprPtr Cond;
      std::vector<HighStmt> ThenWork;
      std::vector<HighStmt> Leading;
    };
    std::vector<Arm> Arms;
    Arms.push_back({I, Body[I].Cond, std::move(FirstWork), {}});

    size_t J = I + 1;
    while (J < Body.size() && Arms.size() < 8) {
      if (prefixOkForChain(Body[J])) {
        ++J;
        continue;
      }
      std::vector<HighStmt> Work;
      MedVar ArmDest;
      if (!isJoinValueGotoIf(Body[J], Join, &Dest, &ArmDest, &Work))
        break;
      Arm Next;
      Next.Index = J;
      Next.Cond = Body[J].Cond;
      Next.ThenWork = std::move(Work);
      Arms.push_back(std::move(Next));
      ++J;
    }

    size_t LeadFrom = I + 1;
    for (size_t A = 1; A < Arms.size(); ++A) {
      Arms[A].Leading.assign(Body.begin() + static_cast<long>(LeadFrom),
                             Body.begin() + static_cast<long>(Arms[A].Index));
      LeadFrom = Arms[A].Index + 1;
    }

    size_t DefaultI = LeadFrom;
    while (DefaultI < Body.size() &&
           (isSkippablePad(Body[DefaultI]) ||
            (Body[DefaultI].Kind == StmtKind::Assign && Body[DefaultI].Val &&
             Body[DefaultI].Val->Kind == ExprKind::Undef)))
      ++DefaultI;
    if (DefaultI >= Body.size())
      continue;
    MedVar DefaultDest;
    ExprPtr DefaultVal;
    if (!peelJoinDefaultAssign(Body[DefaultI], DefaultDest, DefaultVal) ||
        !DefaultVal || !sameJoinDest(DefaultDest, Dest))
      continue;

    size_t UseI = DefaultI + 1;
    while (UseI < Body.size() && isSkippablePad(Body[UseI]))
      ++UseI;
    if (UseI >= Body.size() ||
        !((Body[UseI].Kind == StmtKind::Call ||
           Body[UseI].Kind == StmtKind::Return ||
           (Body[UseI].Kind == StmtKind::Assign && Body[UseI].Val &&
            Body[UseI].Val->Kind == ExprKind::Call)) &&
          stmtUsesJoinDest(Body[UseI], Dest)))
      continue;

    size_t SkipI = SIZE_MAX;
    ExprPtr SkipCond;
    std::vector<HighStmt> AfterSkip;
    const va_t DefaultAddr = Body[DefaultI].Addr;
    if (DefaultAddr && DefaultAddr != InvalidVA && I > 0) {
      size_t P = I;
      while (P > 0 && prefixOkForChain(Body[P - 1]))
        --P;
      if (P > 0 && Body[P - 1].Kind == StmtKind::If && Body[P - 1].Cond &&
          bodyIsSkipGoto(Body[P - 1].Body) &&
          Body[P - 1].Body.back().GotoTarget == DefaultAddr) {
        SkipI = P - 1;
        SkipCond = Body[P - 1].Cond;
        AfterSkip.assign(Body.begin() + static_cast<long>(P),
                         Body.begin() + static_cast<long>(I));
      }
    }

    HighStmt Chain = makeVarAssign(Dest, DefaultVal);
    // The null path still gotos this address from outside the try. Keep it
    // on the synthesized else so that edge can join the same value.
    Chain.Addr = DefaultAddr;
    for (int A = static_cast<int>(Arms.size()) - 1; A >= 0; --A) {
      std::vector<HighStmt> ElseBody;
      if (A + 1 < static_cast<int>(Arms.size()))
        ElseBody = Arms[static_cast<size_t>(A) + 1].Leading;
      ElseBody.push_back(std::move(Chain));
      HighStmt IfE;
      IfE.Kind = StmtKind::IfElse;
      IfE.Cond = Arms[static_cast<size_t>(A)].Cond;
      IfE.Body = std::move(Arms[static_cast<size_t>(A)].ThenWork);
      IfE.ElseBody = std::move(ElseBody);
      Chain = std::move(IfE);
    }
    std::vector<HighStmt> Built;
    if (SkipI != SIZE_MAX && SkipCond) {
      HighStmt Outer;
      Outer.Kind = StmtKind::IfElse;
      Outer.Cond = std::move(SkipCond);
      Outer.Body = {makeVarAssign(Dest, DefaultVal)};
      Outer.ElseBody = std::move(AfterSkip);
      Outer.ElseBody.push_back(std::move(Chain));
      Built.push_back(std::move(Outer));
    } else {
      Built.push_back(std::move(Chain));
    }

    const size_t ReplaceBegin = SkipI != SIZE_MAX ? SkipI : I;
    Body.erase(Body.begin() + static_cast<long>(ReplaceBegin),
               Body.begin() + static_cast<long>(DefaultI + 1));
    Body.insert(Body.begin() + static_cast<long>(ReplaceBegin),
                std::make_move_iterator(Built.begin()),
                std::make_move_iterator(Built.end()));
    return true;
  }
  return false;
}

static bool sinkJoinDefaultAssign(std::vector<HighStmt> &Body) {
  bool Changed = false;
  for (size_t P = 0; P + 1 < Body.size(); ++P) {
    HighStmt &Prev = Body[P];
    if ((Prev.Kind != StmtKind::If && Prev.Kind != StmtKind::IfElse) ||
        stmtHasStructuredRegion(Prev))
      continue;
    const va_t Join = uniqueNestedGoto(Prev);
    if (countStmtTree(Prev) > 96)
      continue;
    size_t I = P + 1;
    size_t DefI = SIZE_MAX;
    MedVar Dest;
    ExprPtr Def;
    while (I < Body.size()) {
      MedVar Cur;
      ExprPtr Val;
      if (isValueAssign(Body[I], Cur, Val) && Val &&
          Val->Kind != ExprKind::Undef && Val->Kind != ExprKind::Load &&
          Val->Kind != ExprKind::Const) {
        size_t J = I + 1;
        while (J < Body.size() && isJoinClutter(Body[J], Cur) &&
               (!Join || Body[J].Addr != Join))
          ++J;
        if (J < Body.size() &&
            ((Join && Body[J].Addr == Join) ||
             ((Body[J].Kind == StmtKind::Call ||
               Body[J].Kind == StmtKind::Return) &&
              stmtUsesJoinDest(Body[J], Cur)))) {
          // Keep the last join-used assign. An earlier same-RegOff copy
          // (a fallthrough `lea` that still feeds another PHI) is not the
          // default; retargeting incoming onto that SSA leaves the call
          // reading a different version and DCE drops the real incoming.
          Dest = Cur;
          Def = Val;
          DefI = I;
        }
        ++I;
        continue;
      }
      if (Body[I].Kind == StmtKind::Call || Body[I].Kind == StmtKind::Store)
        break;
      if (isSkippablePad(Body[I])) {
        ++I;
        continue;
      }
      break;
    }
    if (!Def || DefI >= Body.size()) {
      continue;
    }
    const bool HasJoinIncoming =
        Join && everyJoinGotoHasIncoming(Prev, Join, &Dest);
    const bool WritesDest = treeWritesJoinDest(Prev, Dest);
    if (!HasJoinIncoming && !WritesDest)
      continue;
    if (treeHasSameAssign(Prev, Dest, Def))
      continue;
    if (HasJoinIncoming) {
      retargetIncomingBeforeJoinGotos(Prev.Body, Join, Dest);
      retargetIncomingBeforeJoinGotos(Prev.ElseBody, Join, Dest);
      retargetJoinAssigns(Prev.Body, Dest);
      retargetJoinAssigns(Prev.ElseBody, Dest);
      stripAssignGotoJoin(Prev.Body, Dest, Join);
      stripAssignGotoJoin(Prev.ElseBody, Dest, Join);
    }
    if (Prev.Kind == StmtKind::If) {
      coverVarFallthrough(Prev.Body, Dest, Def, Join);
      Prev.Kind = StmtKind::IfElse;
      HighStmt Else = makeVarAssign(Dest, Def);
      Else.Addr = Body[DefI].Addr;
      Prev.ElseBody = {std::move(Else)};
    } else {
      coverVarFallthrough(Prev.Body, Dest, Def, Join);
      coverVarFallthrough(Prev.ElseBody, Dest, Def, Join);
    }
    Body.erase(Body.begin() + static_cast<long>(DefI));
    Changed = true;
    break;
  }
  return Changed;
}

//===----------------------------------------------------------------------===//
// structureIfElse — fold if(cond){goto} patterns into if/else trees
//===----------------------------------------------------------------------===//

static void structureIfElseList(std::vector<HighStmt> &Body, int MaxPasses,
                                const MedFunc *Med);

static bool dropDuplicateSkipGotos(std::vector<HighStmt> &Body) {
  bool Changed = false;
  for (int I = 0; I < static_cast<int>(Body.size()); ++I) {
    HighStmt &Stmt = Body[I];
    if (Stmt.Kind != StmtKind::If || !Stmt.Cond || !bodyIsSkipGoto(Stmt.Body))
      continue;
    const va_t SkipTo = Stmt.Body.back().GotoTarget;
    size_t J = static_cast<size_t>(I) + 1;
    while (J < Body.size() && prefixOkForChain(Body[J]))
      ++J;
    if (J >= Body.size() || Body[J].Kind != StmtKind::If || !Body[J].Cond ||
        !bodyIsSkipGoto(Body[J].Body) ||
        Body[J].Body.back().GotoTarget != SkipTo)
      continue;
    auto NextCond = composePrefixesIntoCond(
        Body, static_cast<size_t>(I) + 1, J, Body[J].Cond);
    bool Same = NextCond && condStructEq(Stmt.Cond.get(), NextCond->get());
    if (!Same) {
      const HighExpr *A = reachingPredCall(Body, static_cast<size_t>(I));
      const HighExpr *B = reachingPredCall(Body, J);
      Same = samePredCall(A, B);
    }
    if (!Same)
      Same = samePredCall(precedingPredCall(Body, static_cast<size_t>(I)),
                          precedingPredCall(Body, J));
    if (Same) {
      Body.erase(Body.begin() + static_cast<long>(I) + 1,
                 Body.begin() + static_cast<long>(J) + 1);
      Changed = true;
      --I;
      continue;
    }
    if (!NextCond)
      continue;
    Stmt.Cond = HighExpr::makeBinop(NdOp::BOOL_OR, Stmt.Cond,
                                    std::move(*NextCond));
    Body.erase(Body.begin() + static_cast<long>(I) + 1,
               Body.begin() + static_cast<long>(J) + 1);
    Changed = true;
    --I;
  }
  return Changed;
}

static void dropDuplicateSkipGotosNested(std::vector<HighStmt> &Body) {
  for (HighStmt &S : Body) {
    dropDuplicateSkipGotosNested(S.Body);
    dropDuplicateSkipGotosNested(S.ElseBody);
    dropDuplicateSkipGotosNested(S.DefaultBody);
    for (auto &C : S.Cases)
      dropDuplicateSkipGotosNested(C.Body);
    for (auto &Clause : S.EHClauseBodies)
      dropDuplicateSkipGotosNested(Clause);
  }
  dropDuplicateSkipGotos(Body);
}

static void foldJoinValueGotoChainNested(std::vector<HighStmt> &Body) {
  for (HighStmt &S : Body) {
    foldJoinValueGotoChainNested(S.Body);
    foldJoinValueGotoChainNested(S.ElseBody);
    foldJoinValueGotoChainNested(S.DefaultBody);
    for (auto &C : S.Cases)
      foldJoinValueGotoChainNested(C.Body);
    for (auto &Clause : S.EHClauseBodies)
      foldJoinValueGotoChainNested(Clause);
  }
  while (foldJoinValueGotoChain(Body))
    ;
}

static bool invertEmptyThenAssignCall(std::vector<HighStmt> &Body) {
  bool Changed = false;
  for (int I = 0; I < static_cast<int>(Body.size()); ++I) {
    HighStmt &Stmt = Body[I];
    if ((Stmt.Kind != StmtKind::If && Stmt.Kind != StmtKind::IfElse) ||
        !Stmt.Cond || exprHasCall(Stmt.Cond.get()) || !Stmt.Body.empty() ||
        !Stmt.ElseBody.empty())
      continue;
    const size_t NextI = static_cast<size_t>(I) + 1;
    size_t K = NextI;
    while (K < Body.size() &&
           (Body[K].Kind == StmtKind::Nop ||
            (Body[K].Kind == StmtKind::Block && Body[K].Body.empty())))
      ++K;
    if (K >= Body.size() || K - NextI > 8 ||
        Body[K].Kind != StmtKind::Assign || !Body[K].Val ||
        Body[K].Val->Kind != ExprKind::Call)
      continue;
    const size_t TargetIndex = K + 1;
    Stmt.Kind = StmtKind::If;
    Stmt.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Stmt.Cond);
    Stmt.Body.clear();
    for (size_t J = NextI; J < TargetIndex; ++J)
      Stmt.Body.push_back(std::move(Body[J]));
    Body.erase(Body.begin() + static_cast<long>(NextI),
               Body.begin() + static_cast<long>(TargetIndex));
    Changed = true;
  }
  return Changed;
}

static void invertEmptyThenAssignCallNested(std::vector<HighStmt> &Body) {
  for (HighStmt &S : Body) {
    invertEmptyThenAssignCallNested(S.Body);
    invertEmptyThenAssignCallNested(S.ElseBody);
    invertEmptyThenAssignCallNested(S.DefaultBody);
    for (auto &C : S.Cases)
      invertEmptyThenAssignCallNested(C.Body);
    for (auto &Clause : S.EHClauseBodies)
      invertEmptyThenAssignCallNested(Clause);
  }
  invertEmptyThenAssignCall(Body);
}

/// `if (c) goto L_out; work; goto L_cleanup;` when L_out is not inside the
/// work. Cleanup is the next top-level goto, not the list tail, so a later
/// sibling label stays put. Cond may contain a call.
static bool invertExternalSkipGoto(std::vector<HighStmt> &Body) {
  bool Changed = false;
  for (int I = 0; I < static_cast<int>(Body.size()); ++I) {
    HighStmt &Stmt = Body[I];
    if (Stmt.Kind != StmtKind::If || !Stmt.Cond || !bodyIsSkipGoto(Stmt.Body))
      continue;
    const va_t Target = Stmt.Body.back().GotoTarget;
    const size_t NextI = static_cast<size_t>(I) + 1;
    if (NextI >= Body.size())
      continue;
    size_t CleanupI = SIZE_MAX;
    for (size_t K = NextI; K < Body.size() && K - NextI < 32; ++K) {
      if (Body[K].Kind == StmtKind::Goto && Body[K].GotoTarget &&
          Body[K].GotoTarget != InvalidVA) {
        CleanupI = K;
        break;
      }
    }
    if (CleanupI == SIZE_MAX || Body[CleanupI].GotoTarget == Target)
      continue;
    const size_t TargetInRest = findSkipTargetInList(Body, NextI, Target);
    if (TargetInRest != SIZE_MAX && TargetInRest <= CleanupI)
      continue;
    HighStmt ElseGoto;
    ElseGoto.Kind = StmtKind::Goto;
    ElseGoto.GotoTarget = Target;
    Stmt.Kind = StmtKind::IfElse;
    Stmt.Cond = invertHighCond(Stmt.Cond);
    Stmt.Body.clear();
    for (size_t K = NextI; K <= CleanupI; ++K)
      Stmt.Body.push_back(std::move(Body[K]));
    Stmt.ElseBody = {std::move(ElseGoto)};
    Body.erase(Body.begin() + static_cast<long>(NextI),
               Body.begin() + static_cast<long>(CleanupI + 1));
    Changed = true;
  }
  return Changed;
}

static void invertExternalSkipGotoNested(std::vector<HighStmt> &Body) {
  for (HighStmt &S : Body) {
    invertExternalSkipGotoNested(S.Body);
    invertExternalSkipGotoNested(S.ElseBody);
    invertExternalSkipGotoNested(S.DefaultBody);
    for (auto &C : S.Cases)
      invertExternalSkipGotoNested(C.Body);
    for (auto &Clause : S.EHClauseBodies)
      invertExternalSkipGotoNested(Clause);
  }
  invertExternalSkipGoto(Body);
}

/// `if (c) goto L; work; goto L` is `if (!c) { work } goto L`. invertExternal
/// leaves this shape because both gotos share a target. Do not steal a later
/// sibling skip.
static bool foldSameTargetSkipGoto(std::vector<HighStmt> &Body) {
  bool Changed = false;
  for (int I = 0; I < static_cast<int>(Body.size()); ++I) {
    HighStmt &Stmt = Body[I];
    if (Stmt.Kind != StmtKind::If || !Stmt.Cond || !bodyIsSkipGoto(Stmt.Body))
      continue;
    const va_t Target = Stmt.Body.back().GotoTarget;
    const size_t NextI = static_cast<size_t>(I) + 1;
    if (NextI >= Body.size())
      continue;
    size_t CleanupI = SIZE_MAX;
    for (size_t K = NextI; K < Body.size() && K - NextI < 32; ++K) {
      if (Body[K].Kind == StmtKind::Goto && Body[K].GotoTarget &&
          Body[K].GotoTarget != InvalidVA) {
        CleanupI = K;
        break;
      }
    }
    if (CleanupI == SIZE_MAX || Body[CleanupI].GotoTarget != Target)
      continue;
    const size_t TargetInRest = findSkipTargetInList(Body, NextI, Target);
    if (TargetInRest != SIZE_MAX && TargetInRest < CleanupI)
      continue;
    if (!rangeHasObservableWork(Body, NextI, CleanupI))
      continue;
    Stmt.Cond = invertHighCond(std::move(Stmt.Cond));
    Stmt.Body.clear();
    Stmt.Body.reserve(CleanupI - NextI);
    for (size_t K = NextI; K < CleanupI; ++K)
      Stmt.Body.push_back(std::move(Body[K]));
    Body.erase(Body.begin() + static_cast<long>(NextI),
               Body.begin() + static_cast<long>(CleanupI));
    Changed = true;
  }
  return Changed;
}

static void foldSameTargetSkipGotoNested(std::vector<HighStmt> &Body) {
  for (HighStmt &S : Body) {
    foldSameTargetSkipGotoNested(S.Body);
    foldSameTargetSkipGotoNested(S.ElseBody);
    foldSameTargetSkipGotoNested(S.DefaultBody);
    for (auto &C : S.Cases)
      foldSameTargetSkipGotoNested(C.Body);
    for (auto &Clause : S.EHClauseBodies)
      foldSameTargetSkipGotoNested(Clause);
  }
  foldSameTargetSkipGoto(Body);
}

static size_t nextNonNopIndex(const std::vector<HighStmt> &Body, size_t I) {
  size_t Next = I;
  while (Next < Body.size() &&
         (Body[Next].Kind == StmtKind::Nop ||
          (Body[Next].Kind == StmtKind::Block && Body[Next].Body.empty())))
    ++Next;
  return Next;
}

static bool sameFallthroughLabel(va_t Target, va_t Fallthrough) {
  if (!Target || !Fallthrough || Target == InvalidVA ||
      Fallthrough == InvalidVA)
    return false;
  return Target == Fallthrough ||
         (Fallthrough > Target && Fallthrough - Target <= 16) ||
         (Target > Fallthrough && Target - Fallthrough <= 16);
}

/// `if (c) { T } else { copies; goto L; } L:` can fall through after the
/// copies. A nested last IfElse may fall through the parent into L
/// (`FallthroughTarget`). A distant L stays an else-goto.
static bool flattenElseGotoNextLabel(std::vector<HighStmt> &Body,
                                     va_t FallthroughTarget) {
  bool Changed = false;
  AddrMap AM;
  for (int I = 0; I < static_cast<int>(Body.size()); ++I) {
    HighStmt &Stmt = Body[I];
    if (Stmt.Kind != StmtKind::IfElse || !ifBodyIsOnlyGoto(Stmt.ElseBody))
      continue;
    const va_t Target = uniqueExitGoto(Stmt.ElseBody);
    if (!Target || Target == InvalidVA)
      continue;
    AM.rebuild(Body);
    const size_t TargetIndex = findTargetIndex(AM, Target);
    const size_t NextI = nextNonNopIndex(Body, static_cast<size_t>(I) + 1);
    const bool ParentFallthrough =
        TargetIndex == SIZE_MAX && NextI >= Body.size() &&
        sameFallthroughLabel(Target, FallthroughTarget);
    if (TargetIndex != NextI && !ParentFallthrough)
      continue;
    Stmt.ElseBody.pop_back();
    if (Stmt.ElseBody.empty())
      Stmt.Kind = StmtKind::If;
    Changed = true;
  }
  return Changed;
}

static void flattenElseGotoNextLabelNested(std::vector<HighStmt> &Body,
                                           va_t FallthroughTarget) {
  for (size_t I = 0; I < Body.size(); ++I) {
    HighStmt &S = Body[I];
    // Keep a labeled empty block; skipping it would lose L and miss a nested
    // `else goto L` that falls through the parent into that label.
    const size_t NextRaw = I + 1;
    va_t ChildFall = FallthroughTarget;
    if (NextRaw < Body.size()) {
      ChildFall = AddrMap::entryAddress(Body[NextRaw]);
      if (!ChildFall) {
        const size_t NextI = nextNonNopIndex(Body, NextRaw);
        if (NextI < Body.size())
          ChildFall = AddrMap::entryAddress(Body[NextI]);
        if (!ChildFall)
          ChildFall = FallthroughTarget;
      }
    }
    flattenElseGotoNextLabelNested(S.Body, ChildFall);
    flattenElseGotoNextLabelNested(S.ElseBody, FallthroughTarget);
    flattenElseGotoNextLabelNested(S.DefaultBody, FallthroughTarget);
    for (auto &C : S.Cases)
      flattenElseGotoNextLabelNested(C.Body, FallthroughTarget);
    for (auto &Clause : S.EHClauseBodies)
      flattenElseGotoNextLabelNested(Clause, FallthroughTarget);
  }
  flattenElseGotoNextLabel(Body, FallthroughTarget);
}

/// `if (c) { work; goto Join; } elsework; Join:` is if/else when elsework is
/// straight-line. Sibling If/IfElse after the then stays put.
/// A nested `if (outer) { if (c) { work; goto Join; } } elsework; Join` sinks
/// elsework into the inner If.
static bool foldThenSkipOver(std::vector<HighStmt> &Body) {
  bool Changed = false;
  AddrMap AM;
  for (int I = 0; I < static_cast<int>(Body.size()); ++I) {
    HighStmt &Stmt = Body[I];
    HighStmt *Inner = nullptr;
    if (Stmt.Kind == StmtKind::If && Stmt.Cond && !Stmt.Body.empty() &&
        !ifBodyIsOnlyGoto(Stmt.Body) && Stmt.Body.back().Kind == StmtKind::Goto)
      Inner = &Stmt;
    else if (Stmt.Kind == StmtKind::IfElse && !Stmt.Body.empty() &&
             Stmt.Body.back().Kind == StmtKind::If && Stmt.Body.back().Cond &&
             !Stmt.Body.back().Body.empty() &&
             !ifBodyIsOnlyGoto(Stmt.Body.back().Body) &&
             Stmt.Body.back().Body.back().Kind == StmtKind::Goto)
      Inner = &Stmt.Body.back();
    if (!Inner)
      continue;
    const va_t Join = Inner->Body.back().GotoTarget;
    if (!Join || Join == InvalidVA)
      continue;
    AM.rebuild(Body);
    const size_t JoinIdx = findTargetIndex(AM, Join);
    const size_t NextI = static_cast<size_t>(I) + 1;
    if (JoinIdx == SIZE_MAX || JoinIdx <= NextI)
      continue;
    const size_t ElseEnd = findSkipOverElseEnd(Body, NextI, JoinIdx, Join);
    if (ElseEnd == SIZE_MAX || ElseEnd <= NextI || ElseEnd - NextI > 16)
      continue;
    if (!rangeHasObservableWork(Body, NextI, ElseEnd))
      continue;
    if (!ownsRun(Body, AM, {NextI, ElseEnd}, static_cast<size_t>(I), nullptr))
      continue;
    const bool JoinFollowsElse = ElseEnd == JoinIdx;
    size_t ElseTake = ElseEnd;
    if (JoinFollowsElse && ElseTake > NextI &&
        Body[ElseTake - 1].Kind == StmtKind::Goto &&
        Body[ElseTake - 1].GotoTarget == Join)
      --ElseTake;
    if (ElseTake <= NextI)
      continue;
    Inner->Kind = StmtKind::IfElse;
    if (JoinFollowsElse)
      Inner->Body.pop_back();
    Inner->ElseBody.clear();
    Inner->ElseBody.reserve(ElseTake - NextI);
    for (size_t K = NextI; K < ElseTake; ++K)
      Inner->ElseBody.push_back(std::move(Body[K]));
    Body.erase(Body.begin() + static_cast<long>(NextI),
               Body.begin() + static_cast<long>(ElseTake));
    Changed = true;
  }
  return Changed;
}

static void foldThenSkipOverNested(std::vector<HighStmt> &Body) {
  for (HighStmt &S : Body) {
    foldThenSkipOverNested(S.Body);
    foldThenSkipOverNested(S.ElseBody);
    foldThenSkipOverNested(S.DefaultBody);
    for (auto &C : S.Cases)
      foldThenSkipOverNested(C.Body);
    for (auto &Clause : S.EHClauseBodies)
      foldThenSkipOverNested(Clause);
  }
  foldThenSkipOver(Body);
}

static bool isSmallJoinWork(const HighStmt &S) {
  if (S.IsPhiCopy || S.Kind == StmtKind::Nop)
    return true;
  return S.Kind == StmtKind::Call || S.Kind == StmtKind::Assign ||
         S.Kind == StmtKind::Store;
}

/// DCE-retained join entry. In this shape, the label sits
/// on an empty Block and the ctor lives on the next Assign.
static bool isEmptyEntryLabel(const HighStmt &S) {
  return S.Kind == StmtKind::Block && S.Body.empty() && S.ElseBody.empty() &&
         S.Cases.empty() && S.DefaultBody.empty() && S.EHClauseBodies.empty();
}

/// One observable call/assign/store plus `goto Cleanup`. A `return` tail
/// is a shared `__wind` cleanup (L_7D6 / L_7DD) and stays put. The first
/// work insn may carry a later VA than the empty entry label.
static bool isSmallExclusiveJoin(const std::vector<HighStmt> &Body, size_t I,
                                 size_t &End) {
  if (I >= Body.size())
    return false;
  const va_t Label = AddrMap::entryAddress(Body[I]);
  if (!Label || Label == InvalidVA)
    return false;
  End = I;
  size_t Work = 0;
  for (; End < Body.size(); ++End) {
    const HighStmt &S = Body[End];
    if (isEmptyEntryLabel(S)) {
      const va_t Next = AddrMap::entryAddress(S);
      if (Next && Next != InvalidVA && Next != Label)
        return false;
      continue;
    }
    if (S.Kind == StmtKind::Goto) {
      if (!S.GotoTarget || S.GotoTarget == InvalidVA || S.GotoTarget == Label)
        return false;
      ++End;
      return Work == 1;
    }
    if (S.Kind == StmtKind::Return)
      return false;
    if (!isSmallJoinWork(S))
      return false;
    if (!S.IsPhiCopy && S.Kind != StmtKind::Nop) {
      ++Work;
      if (Work > 1)
        return false;
    }
  }
  return false;
}

static bool hasFallthroughPred(const std::vector<HighStmt> &Body, size_t I) {
  size_t P = I;
  while (P > 0) {
    --P;
    if (Body[P].Kind == StmtKind::Nop)
      continue;
    return stmtFallsThrough(Body[P]);
  }
  return true;
}

static void countGotosTo(const std::vector<HighStmt> &Body, va_t Target,
                         size_t &All, size_t &ElseOnly) {
  for (const HighStmt &S : Body) {
    if (S.Kind == StmtKind::IfElse && ifBodyIsOnlyGoto(S.ElseBody) &&
        S.ElseBody.back().GotoTarget == Target)
      ++ElseOnly;
    if (S.Kind == StmtKind::Goto && S.GotoTarget == Target)
      ++All;
    countGotosTo(S.Body, Target, All, ElseOnly);
    countGotosTo(S.ElseBody, Target, All, ElseOnly);
    countGotosTo(S.DefaultBody, Target, All, ElseOnly);
    for (const auto &C : S.Cases)
      countGotosTo(C.Body, Target, All, ElseOnly);
    for (const auto &Clause : S.EHClauseBodies)
      countGotosTo(Clause, Target, All, ElseOnly);
  }
}

static bool replaceElseOnlyGotos(std::vector<HighStmt> &Body, va_t Target,
                                 const std::vector<HighStmt> &Fill) {
  bool Changed = false;
  for (HighStmt &S : Body) {
    if (S.Kind == StmtKind::IfElse && ifBodyIsOnlyGoto(S.ElseBody) &&
        S.ElseBody.back().GotoTarget == Target) {
      // The goto is redundant after inlining the exclusive join, but its
      // predecessor's PHI copies still have to execute before the join work.
      S.ElseBody.pop_back();
      S.ElseBody.insert(S.ElseBody.end(), Fill.begin(), Fill.end());
      Changed = true;
    }
    Changed |= replaceElseOnlyGotos(S.Body, Target, Fill);
    Changed |= replaceElseOnlyGotos(S.ElseBody, Target, Fill);
    Changed |= replaceElseOnlyGotos(S.DefaultBody, Target, Fill);
    for (auto &C : S.Cases)
      Changed |= replaceElseOnlyGotos(C.Body, Target, Fill);
    for (auto &Clause : S.EHClauseBodies)
      Changed |= replaceElseOnlyGotos(Clause, Target, Fill);
  }
  return Changed;
}

/// `if (c) { work; goto CleanupA; } else goto L; L: ctor(); goto CleanupB`
/// inlines L into every else-only goto when nothing falls through into L.
/// Then-arm gotos to a shared wind tail keep L_7D6 / L_7DD.
static bool inlineSmallExclusiveJoin(std::vector<HighStmt> &Body,
                                     std::vector<HighStmt> &Root) {
  bool Changed = false;
  for (size_t I = 0; I < Body.size();) {
    size_t End = 0;
    if (!isSmallExclusiveJoin(Body, I, End) || hasFallthroughPred(Body, I)) {
      ++I;
      continue;
    }
    const va_t Label = AddrMap::entryAddress(Body[I]);
    size_t All = 0;
    size_t ElseOnly = 0;
    countGotosTo(Root, Label, All, ElseOnly);
    if (!All || All != ElseOnly) {
      ++I;
      continue;
    }
    std::vector<HighStmt> Fill;
    Fill.reserve(End - I);
    for (size_t J = I; J < End; ++J) {
      if (isEmptyEntryLabel(Body[J]))
        continue;
      Fill.push_back(Body[J]);
      if (Fill.back().Addr == Label)
        Fill.back().Addr = 0;
    }
    if (Fill.empty() || !replaceElseOnlyGotos(Root, Label, Fill)) {
      ++I;
      continue;
    }
    Body.erase(Body.begin() + static_cast<long>(I),
               Body.begin() + static_cast<long>(End));
    Changed = true;
  }
  return Changed;
}

static void inlineSmallExclusiveJoinNested(std::vector<HighStmt> &Body,
                                           std::vector<HighStmt> &Root) {
  for (HighStmt &S : Body) {
    inlineSmallExclusiveJoinNested(S.Body, Root);
    inlineSmallExclusiveJoinNested(S.ElseBody, Root);
    inlineSmallExclusiveJoinNested(S.DefaultBody, Root);
    for (auto &C : S.Cases)
      inlineSmallExclusiveJoinNested(C.Body, Root);
    for (auto &Clause : S.EHClauseBodies)
      inlineSmallExclusiveJoinNested(Clause, Root);
  }
  inlineSmallExclusiveJoin(Body, Root);
}

static void structureIfElseNested(std::vector<HighStmt> &Stmts, int MaxPasses,
                                  const MedFunc *Med) {
  for (HighStmt &S : Stmts) {
    structureIfElseNested(S.Body, MaxPasses, Med);
    structureIfElseNested(S.ElseBody, MaxPasses, Med);
    for (auto &C : S.Cases)
      structureIfElseNested(C.Body, MaxPasses, Med);
    structureIfElseNested(S.DefaultBody, MaxPasses, Med);
    for (auto &Clause : S.EHClauseBodies)
      structureIfElseNested(Clause, MaxPasses, Med);
  }
  structureIfElseList(Stmts, MaxPasses, Med);
}

void structureIfElse(HighFunc &Func, int MaxPasses, const MedFunc *Med) {
  structureIfElseNested(Func.Body, MaxPasses, Med);
}

static void structureIfElseList(std::vector<HighStmt> &Body, int MaxPasses,
                                const MedFunc *Med) {
  const char *Detail = std::getenv("NEVERD_HIGHIR_DETAIL");
  const bool WantDetail = Detail && Detail[0] == '1' && Detail[1] == '\0';
  auto T0 = std::chrono::steady_clock::now();
  AddrMap AM;
  bool Changed = true;
  int Pass = 0;
  while (Changed && Pass++ < MaxPasses) {
    Changed = false;
    AM.rebuild(Body);
    for (int Sinks = 0;
         Sinks < 32 &&
         (sinkJoinDefaultAssign(Body) || foldJoinValueGotoChain(Body));
         ++Sinks) {
      Changed = true;
      AM.rebuild(Body);
    }
    std::set<va_t> LiveTargets;
    walkStmts(Body, [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Goto)
        LiveTargets.insert(S.GotoTarget);
    });
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

    auto isThrowStmt = [](const HighStmt &S) {
      const HighExpr *E = nullptr;
      if (S.Kind == StmtKind::Call)
        E = S.CallExpr.get();
      else if ((S.Kind == StmtKind::Assign || S.Kind == StmtKind::ExprStmt) &&
               S.Val)
        E = S.Val.get();
      return E && E->Kind == ExprKind::Call &&
             isMsvcCxxThrowCallName(E->CallTarget);
    };
    auto armHasThrow = [&](const std::vector<HighStmt> &Arm) {
      return std::any_of(Arm.begin(), Arm.end(), isThrowStmt);
    };
    auto sameAssignDest = [](const HighStmt &A, const HighStmt &B) {
      return A.Kind == StmtKind::Assign && B.Kind == StmtKind::Assign &&
             A.Dst && B.Dst && A.Dst->Kind == ExprKind::Var &&
             B.Dst->Kind == ExprKind::Var && A.Dst->Var.Id == B.Dst->Var.Id &&
             A.Dst->Var.Kind == B.Dst->Var.Kind;
    };

    for (int I = static_cast<int>(Body.size()) - 1; I >= 0; --I) {
      auto &Stmt = Body[I];
      // Identical if/else suffixes are one statement. MSVC `jle` often copies
      // the same call onto both edges of a join PHI, with undef pads on one
      // arm.
      if (Stmt.Kind == StmtKind::IfElse) {
        auto deadPad = [](const HighStmt &S) {
          if (isSkippablePad(S))
            return true;
          MedVar Dest;
          ExprPtr Val;
          return isValueAssign(S, Dest, Val) && Val &&
                 Val->Kind == ExprKind::Undef;
        };
        auto stripDead = [&](std::vector<HighStmt> &Arm) {
          while (!Arm.empty() && deadPad(Arm.front()))
            Arm.erase(Arm.begin());
          while (!Arm.empty() && deadPad(Arm.back()))
            Arm.pop_back();
        };
        stripDead(Stmt.Body);
        stripDead(Stmt.ElseBody);
        std::vector<HighStmt> Joins;
        while (!Stmt.Body.empty() && !Stmt.ElseBody.empty()) {
          MedVar DestA, DestB;
          ExprPtr ValA, ValB;
          if (!isValueAssign(Stmt.Body.back(), DestA, ValA) ||
              !isValueAssign(Stmt.ElseBody.back(), DestB, ValB) || !ValA ||
              !ValB || !sameJoinDest(DestA, DestB) ||
              !ValA->structuralEq(*ValB))
            break;
          Joins.push_back(std::move(Stmt.ElseBody.back()));
          Stmt.Body.pop_back();
          Stmt.ElseBody.pop_back();
          stripDead(Stmt.Body);
          stripDead(Stmt.ElseBody);
        }
        if (!Joins.empty()) {
          const bool Drop = Stmt.Body.empty() && Stmt.ElseBody.empty();
          if (Drop)
            Body.erase(Body.begin() + I);
          size_t At = Drop ? static_cast<size_t>(I) : static_cast<size_t>(I + 1);
          for (auto It = Joins.rbegin(); It != Joins.rend(); ++It)
            Body.insert(Body.begin() + static_cast<long>(At++),
                        std::move(*It));
          Changed = true;
          continue;
        }
      }
      if ((Stmt.Kind == StmtKind::If || Stmt.Kind == StmtKind::IfElse) &&
          Stmt.Body.empty() && Stmt.ElseBody.empty()) {
        Body.erase(Body.begin() + I);
        Changed = true;
        continue;
      }
      // `if (!c) { v = 0; } else { throw; v = 0; }` → `if (c) throw; v = 0;`
      if (Stmt.Kind == StmtKind::IfElse && Stmt.Cond &&
          armHasThrow(Stmt.Body) != armHasThrow(Stmt.ElseBody)) {
        if (!armHasThrow(Stmt.Body)) {
          Stmt.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Stmt.Cond);
          std::swap(Stmt.Body, Stmt.ElseBody);
        }
        while (!Stmt.Body.empty() && !Stmt.ElseBody.empty() &&
               sameAssignDest(Stmt.Body.back(), Stmt.ElseBody.back())) {
          HighStmt Join = std::move(Stmt.ElseBody.back());
          Stmt.Body.pop_back();
          Stmt.ElseBody.pop_back();
          Body.insert(Body.begin() + I + 1, std::move(Join));
        }
        if (Stmt.ElseBody.empty() ||
            std::all_of(Stmt.ElseBody.begin(), Stmt.ElseBody.end(),
                        [](const HighStmt &S) {
                          return S.Kind == StmtKind::Nop ||
                                 S.Kind == StmtKind::Block;
                        })) {
          Stmt.Kind = StmtKind::If;
          Stmt.ElseBody.clear();
        }
        Changed = true;
        continue;
      }
      // `goto Join` along the trailing spine is redundant when Join is next.
      if (Stmt.Kind == StmtKind::IfElse && Stmt.Cond) {
        va_t SpineJoin = 0;
        auto SpineGoto = [&](auto &&Self, const std::vector<HighStmt> &Arm) -> va_t {
          if (Arm.empty())
            return 0;
          const HighStmt &Last = Arm.back();
          if (Last.Kind == StmtKind::Goto)
            return Last.GotoTarget;
          if (Last.Kind == StmtKind::IfElse) {
            const va_t ThenJ = Self(Self, Last.Body);
            const va_t ElseJ = Self(Self, Last.ElseBody);
            return ThenJ && ThenJ == ElseJ ? ThenJ : ThenJ;
          }
          if (Last.Kind == StmtKind::If)
            return Self(Self, Last.Body);
          return 0;
        };
        SpineJoin = SpineGoto(SpineGoto, Stmt.Body);
        if (SpineJoin && SpineJoin != InvalidVA) {
          AM.rebuild(Body);
          const size_t JoinIdx =
              findTargetIndexOrBlock(AM, SpineJoin, Body, Med);
          if (JoinIdx == static_cast<size_t>(I) + 1) {
            size_t Before = 0;
            walkStatementTree(Stmt, [&](const HighStmt &S) {
              if (S.Kind == StmtKind::Goto && S.GotoTarget == SpineJoin)
                ++Before;
            });
            trimJoinGotos(Stmt.Body, SpineJoin);
            trimJoinGotos(Stmt.ElseBody, SpineJoin);
            size_t After = 0;
            walkStatementTree(Stmt, [&](const HighStmt &S) {
              if (S.Kind == StmtKind::Goto && S.GotoTarget == SpineJoin)
                ++After;
            });
            if (After < Before) {
              Changed = true;
              continue;
            }
          }
        }
      }
      if (Stmt.Kind == StmtKind::IfElse && !Stmt.Body.empty() &&
          !Stmt.ElseBody.empty() && Stmt.Body.back().Kind == StmtKind::Goto &&
          Stmt.ElseBody.back().Kind == StmtKind::Goto) {
        const va_t Target = Stmt.Body.back().GotoTarget;
        if (!Target || Target == InvalidVA ||
            Stmt.ElseBody.back().GotoTarget != Target)
          continue;
        AM.rebuild(Body);
        const size_t TargetIndex = findTargetIndex(AM, Target);
        if (TargetIndex == SIZE_MAX || TargetIndex <= static_cast<size_t>(I))
          continue;
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
          Body.insert(Body.begin() + I + 1, std::move(Transfer));
        }
        Changed = true;
        continue;
      }
      // `if (p == 0) {} else { v = id; goto L; } v = 0; L:` is a join PHI.
      // Sink the fallthrough copies into the empty arm and drop the goto.
      if (Stmt.Kind == StmtKind::IfElse) {
        auto ArmEmpty = [](const std::vector<HighStmt> &Arm) {
          return std::all_of(Arm.begin(), Arm.end(), [](const HighStmt &S) {
            return S.Kind == StmtKind::Nop || S.Kind == StmtKind::Block;
          });
        };
        const bool EmptyThen = ArmEmpty(Stmt.Body) && !Stmt.ElseBody.empty() &&
                               Stmt.ElseBody.back().Kind == StmtKind::Goto;
        const bool EmptyElse = ArmEmpty(Stmt.ElseBody) && !Stmt.Body.empty() &&
                               Stmt.Body.back().Kind == StmtKind::Goto;
        if (EmptyThen || EmptyElse) {
          auto &Taken = EmptyThen ? Stmt.ElseBody : Stmt.Body;
          const va_t Target = Taken.back().GotoTarget;
          if (Target && Target != InvalidVA) {
            AM.rebuild(Body);
            const size_t TargetIndex = findTargetIndex(AM, Target);
            const size_t NextI = static_cast<size_t>(I) + 1;
            if (TargetIndex != SIZE_MAX && TargetIndex > static_cast<size_t>(I) &&
                NextI <= TargetIndex) {
              size_t PrefixEnd = NextI;
              size_t PhiCount = 0;
              while (PrefixEnd < Body.size()) {
                const HighStmt &P = Body[PrefixEnd];
                if (P.Kind == StmtKind::Nop || P.Kind == StmtKind::Block) {
                  ++PrefixEnd;
                  continue;
                }
                if (P.IsPhiCopy) {
                  ++PhiCount;
                  ++PrefixEnd;
                  continue;
                }
                break;
              }
              if (PhiCount > 0 && TargetIndex <= PrefixEnd &&
                  ownsRun(Body, AM, {NextI, PrefixEnd},
                          static_cast<size_t>(I), Med)) {
                bool GotoIsEntry = LiveTargets.count(Taken.back().Addr) != 0;
                if (Med && Taken.back().Addr) {
                  for (const auto &Block : Med->Blocks) {
                    const va_t Start =
                        Block.StartAddr
                            ? Block.StartAddr
                            : (Block.Ops.empty() ? 0 : Block.Ops.front().Addr);
                    if (Start == Taken.back().Addr)
                      GotoIsEntry = true;
                  }
                }
                if (!GotoIsEntry) {
                  Taken.pop_back();
                  std::vector<HighStmt> Prefix;
                  Prefix.reserve(PrefixEnd - NextI);
                  for (size_t K = NextI; K < PrefixEnd; ++K)
                    Prefix.push_back(std::move(Body[K]));
                  if (EmptyThen)
                    Stmt.Body = std::move(Prefix);
                  else
                    Stmt.ElseBody = std::move(Prefix);
                  Body.erase(Body.begin() +
                                      static_cast<long>(NextI),
                                  Body.begin() +
                                      static_cast<long>(PrefixEnd));
                  Changed = true;
                  continue;
                }
              }
            }
          }
        }
      }
      // `if (c) {} else { work; goto Join; }` is the taken arm of an inverted
      // if. Rewrite so the work+goto fold below can attach the sibling else.
      if (Stmt.Kind == StmtKind::IfElse && Stmt.ElseBody.empty()) {
        Stmt.Kind = StmtKind::If;
        Changed = true;
      }
      if (Stmt.Kind == StmtKind::IfElse && Stmt.Cond) {
        if (armIsSkippable(Stmt.Body) && !Stmt.ElseBody.empty() &&
            !ifBodyIsOnlyGoto(Stmt.ElseBody)) {
          bool ThenUsed = false;
          bool ThenHasIncoming = false;
          walkStmts(Stmt.Body, [&](const HighStmt &S) {
            MedVar Dest;
            ExprPtr Val;
            if (!isValueAssign(S, Dest, Val) || !Val)
              return;
            if (stmtsUseJoinDest(Stmt.ElseBody, Dest))
              ThenUsed = true;
            if (Val->Kind != ExprKind::Undef && Val->Kind != ExprKind::Var &&
                Val->Kind != ExprKind::Const && Val->Kind != ExprKind::Load)
              ThenHasIncoming = true;
            for (size_t J = static_cast<size_t>(I) + 1; J < Body.size(); ++J)
              if (stmtUsesJoinDest(Body[J], Dest))
                ThenHasIncoming = true;
          });
          if (!ThenUsed && !ThenHasIncoming) {
            Stmt.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Stmt.Cond);
            Stmt.Kind = StmtKind::If;
            Stmt.Body = std::move(Stmt.ElseBody);
            Stmt.ElseBody.clear();
            Changed = true;
          }
        } else if (armIsSkippable(Stmt.ElseBody) && uniqueExitGoto(Stmt.Body) &&
                   !ifBodyIsOnlyGoto(Stmt.Body)) {
          Stmt.Kind = StmtKind::If;
          Stmt.ElseBody.clear();
          Changed = true;
        }
      }
      // `if (c) { if (d) S }` → `if (c && d) S` when d is independent.
      // If d already starts with c, drop those conjuncts (or flatten when
      // the inner cond is implied). Do not pull an inner IfElse into the
      // else (that would run the else when `!c`).
      if (Stmt.Kind == StmtKind::If && Stmt.Cond && Stmt.ElseBody.empty()) {
        size_t InnerI = SIZE_MAX;
        if (bodyIsNestedIf(Stmt.Body, InnerI)) {
          HighStmt Inner = std::move(Stmt.Body[InnerI]);
          bool FoldPrefix = InnerI > 0;
          bool DidCompose = false;
          bool SideEffectPrefix = false;
          for (size_t K = 0; K < InnerI; ++K)
            if (Stmt.Body[K].Kind == StmtKind::Call ||
                Stmt.Body[K].Kind == StmtKind::Store)
              SideEffectPrefix = true;
          if (FoldPrefix) {
            bool UsedLater = false;
            for (size_t K = 0; K < InnerI; ++K) {
              MedVar Dest;
              ExprPtr Val;
              if (isValueAssign(Stmt.Body[K], Dest, Val) &&
                  stmtsUseVar(Inner.Body, Dest))
                UsedLater = true;
            }
            ExprPtr Before = Inner.Cond;
            auto Folded = composePrefixesIntoCond(Stmt.Body, 0, InnerI,
                                                  Inner.Cond);
            if (!Folded || UsedLater)
              FoldPrefix = false;
            else {
              Inner.Cond = std::move(*Folded);
              DidCompose = !condStructEq(Before.get(), Inner.Cond.get());
            }
          }
          const bool SameTempId =
              prefixesAllowSameTempId(Stmt.Body, 0, InnerI);
          ExprPtr OuterForPeel =
              outerCondFromSiblings(Body, static_cast<size_t>(I), Stmt.Cond);
          ExprPtr PredForDce = OuterForPeel ? OuterForPeel : Stmt.Cond;
          if (auto Rest = dropAndPrefix(OuterForPeel, Inner.Cond, SameTempId)) {
            if (isConstTrue(*Rest)) {
              std::vector<HighStmt> Prefix;
              Prefix.reserve(Stmt.Body.size() - 1 + Inner.Body.size());
              for (size_t K = 0; K < Stmt.Body.size(); ++K) {
                if (K == InnerI)
                  continue;
                if (DidCompose && K < InnerI && prefixOkForChain(Stmt.Body[K]) &&
                    isPredicateOrLoadAssign(Stmt.Body[K], PredForDce))
                  continue;
                Prefix.push_back(std::move(Stmt.Body[K]));
              }
              for (HighStmt &Nested : Inner.Body)
                Prefix.push_back(std::move(Nested));
              Stmt.Body = std::move(Prefix);
            } else {
              Inner.Cond = std::move(*Rest);
              Stmt.Body[InnerI] = std::move(Inner);
              dropUnusedTrailingAssigns(Stmt.Body, InnerI, PredForDce);
            }
            Changed = true;
          } else if (!SideEffectPrefix && (InnerI == 0 || FoldPrefix)) {
            std::vector<HighStmt> Prefix;
            Prefix.reserve(Stmt.Body.size() - 1 + Inner.Body.size());
            for (size_t K = 0; K < Stmt.Body.size(); ++K) {
              if (K == InnerI)
                continue;
              if (DidCompose && K < InnerI && prefixOkForChain(Stmt.Body[K]) &&
                  isPredicateOrLoadAssign(Stmt.Body[K], Inner.Cond))
                continue;
              Prefix.push_back(std::move(Stmt.Body[K]));
            }
            Stmt.Cond = HighExpr::makeBinop(NdOp::BOOL_AND, std::move(Stmt.Cond),
                                            std::move(Inner.Cond));
            for (HighStmt &Nested : Inner.Body)
              Prefix.push_back(std::move(Nested));
            Stmt.Body = std::move(Prefix);
            Changed = true;
          } else {
            Stmt.Body[InnerI] = std::move(Inner);
            dropUnusedTrailingAssigns(Stmt.Body, InnerI, PredForDce);
          }
        } else {
          ExprPtr PredForDce =
              outerCondFromSiblings(Body, static_cast<size_t>(I), Stmt.Cond);
          for (size_t J = 0; J < Stmt.Body.size();) {
            auto Rest = peelInnerAgainstOuter(Stmt, static_cast<size_t>(I),
                                              Body, J);
            if (!Rest) {
              dropUnusedTrailingAssigns(Stmt.Body, J, PredForDce);
              ++J;
              continue;
            }
            if (isConstTrue(*Rest) && Stmt.Body[J].ElseBody.empty()) {
              HighStmt Inner = std::move(Stmt.Body[J]);
              const size_t Inserted = Inner.Body.size();
              Stmt.Body.erase(Stmt.Body.begin() + static_cast<long>(J));
              Stmt.Body.insert(Stmt.Body.begin() + static_cast<long>(J),
                               std::make_move_iterator(Inner.Body.begin()),
                               std::make_move_iterator(Inner.Body.end()));
              size_t DropAt = J + Inserted;
              for (size_t K = J; K < J + Inserted; ++K) {
                if (Stmt.Body[K].Kind == StmtKind::If ||
                    Stmt.Body[K].Kind == StmtKind::IfElse) {
                  DropAt = K;
                  break;
                }
              }
              dropUnusedTrailingAssigns(Stmt.Body, DropAt, PredForDce);
            } else if (isConstTrue(*Rest)) {
              ++J;
            } else {
              Stmt.Body[J].Cond = std::move(*Rest);
              dropUnusedTrailingAssigns(Stmt.Body, J, PredForDce);
              ++J;
            }
            Changed = true;
          }
          for (size_t J = 0; J < Stmt.Body.size(); ++J) {
            if (Stmt.Body[J].Kind == StmtKind::If ||
                Stmt.Body[J].Kind == StmtKind::IfElse)
              dropUnusedTrailingAssigns(Stmt.Body, J, PredForDce);
          }
        }
      }
      // `if (c) { work; goto Join; } elseWork; Join:` and the title shape
      // where every then-exit is `goto Join` (nested ifs included). Invert-skip
      // still owns `if (c) goto L` with only PHI copies in the body.
      if (Stmt.Kind == StmtKind::If && Stmt.Cond &&
          !Stmt.Body.empty() && !ifBodyIsOnlyGoto(Stmt.Body)) {
        const va_t Join = uniqueExitGoto(Stmt.Body);
        if (Join) {
          bool HasEnteredTransfer = false;
          walkStmts(Stmt.Body, [&](const HighStmt &S) {
            if (S.Kind == StmtKind::Goto && HasEntry(S.Addr))
              HasEnteredTransfer = true;
          });
          if (HasEnteredTransfer)
            continue;
          AM.rebuild(Body);
          const size_t NextI = static_cast<size_t>(I) + 1;
          const size_t JoinIdx =
              findTargetIndexOrBlock(AM, Join, Body, Med);
          if (JoinIdx != SIZE_MAX && JoinIdx < NextI) {
            // Join is above this if; leave the gotos in place.
          } else if (JoinIdx == NextI) {
            trimJoinGotos(Stmt.Body, Join);
            Changed = true;
            continue;
          } else {
            const bool JoinInList =
                JoinIdx != SIZE_MAX && JoinIdx > NextI;
            const size_t ElseEnd = JoinInList ? JoinIdx : Body.size();
            if (ElseEnd > NextI && !rangeHasLoop(Body, NextI, ElseEnd)) {
              bool RestOk = JoinInList;
              if (!JoinInList) {
                std::vector<HighStmt> Rest(Body.begin() +
                                               static_cast<long>(NextI),
                                           Body.end());
                const va_t RestJoin = uniqueExitGoto(Rest);
                RestOk = RestJoin == Join || stmtListFallsThrough(Rest);
              }
              if (RestOk && ownsRunHighIR(Body, AM, {NextI, ElseEnd},
                                          static_cast<size_t>(I))) {
                Stmt.Kind = StmtKind::IfElse;
                Stmt.ElseBody.clear();
                Stmt.ElseBody.reserve(ElseEnd - NextI);
                for (size_t K = NextI; K < ElseEnd; ++K)
                  Stmt.ElseBody.push_back(std::move(Body[K]));
                Body.erase(Body.begin() + static_cast<long>(NextI),
                           Body.begin() + static_cast<long>(ElseEnd));
                if (JoinInList) {
                  trimJoinGotos(Stmt.Body, Join);
                  trimJoinGotos(Stmt.ElseBody, Join);
                }
                structureIfElseList(Stmt.Body, limits::kIfElseNestedArmPasses,
                                    Med);
                structureIfElseList(Stmt.ElseBody,
                                    limits::kIfElseNestedArmPasses, Med);
                Changed = true;
                continue;
              }
            }
          }
        }
      }
      // Fallthrough if-chain whose only transfer is `goto Join`, then elseWork
      // and Join: `if (c1 && c2 && c3) work; else elseWork;`.
      if ((Stmt.Kind == StmtKind::If || Stmt.Kind == StmtKind::IfElse) &&
          Stmt.Cond && stmtFallsThrough(Stmt)) {
        const va_t Join = uniqueNestedGoto(Stmt);
        if (Join) {
          auto Chain = collectPredChain(Stmt, Join, nullptr);
          if (Chain && Chain->Cond && !Chain->Work.empty()) {
            AM.rebuild(Body);
            const size_t NextI = static_cast<size_t>(I) + 1;
            size_t JoinIdx = findTargetIndexOrBlock(AM, Join, Body, Med);
            // AddrMap keeps the first top-level owner. A later join label
            // after elseWork must win over an earlier copy of the same VA.
            if (JoinIdx == SIZE_MAX || JoinIdx <= NextI) {
              for (size_t K = NextI + 1; K < Body.size(); ++K) {
                if (AddrMap::entryAddress(Body[K]) == Join) {
                  JoinIdx = K;
                  break;
                }
              }
            }
            if (JoinIdx != SIZE_MAX && JoinIdx > NextI &&
                !rangeHasLoop(Body, NextI, JoinIdx)) {
              bool SharedLabel = false;
              for (size_t K = NextI; K < JoinIdx; ++K) {
                const va_t Address = AddrMap::entryAddress(Body[K]);
                if (Address && Address != InvalidVA &&
                    LiveTargets.count(Address))
                  SharedLabel = true;
              }
              if (!SharedLabel ||
                  ownsRunHighIR(Body, AM, {NextI, JoinIdx},
                                static_cast<size_t>(I))) {
                Stmt.Kind = StmtKind::IfElse;
                Stmt.Cond = std::move(Chain->Cond);
                Stmt.Body = std::move(Chain->Work);
                Stmt.ElseBody.clear();
                Stmt.ElseBody.reserve(Chain->SkipPrefix.size() + JoinIdx -
                                      NextI);
                for (HighStmt &Skip : Chain->SkipPrefix)
                  Stmt.ElseBody.push_back(std::move(Skip));
                for (size_t K = NextI; K < JoinIdx; ++K)
                  Stmt.ElseBody.push_back(std::move(Body[K]));
                Body.erase(Body.begin() + static_cast<long>(NextI),
                           Body.begin() + static_cast<long>(JoinIdx));
                trimJoinGotos(Stmt.Body, Join);
                trimJoinGotos(Stmt.ElseBody, Join);
                Changed = true;
                continue;
              }
            }
          }
        }
      }
      if (Stmt.Kind != StmtKind::If)
        continue;
      if (!bodyIsSkipGoto(Stmt.Body))
        continue;
      std::vector<HighStmt> TakenCopies(Stmt.Body.begin(), Stmt.Body.end() - 1);

      // Earlier folds in this same pass may erase statements after I.
      // Address ownership must describe the current list, not stale indices.
      AM.rebuild(Body);
      va_t IfTarget = Stmt.Body.back().GotoTarget;
      if (IfTarget == 0 || IfTarget == InvalidVA)
        continue;

      size_t NextI = static_cast<size_t>(I) + 1;
      if (NextI >= Body.size())
        continue;

      // A block can start before its first surviving HighIR statement (for
      // example COPY return-register, RET). Its own return is never part of
      // the conditional's fallthrough arm. Peeled PHI copies leave the
      // COND_BR target without an exact HighIR address — look inside the
      // Med block when the slack window misses.
      const size_t TargetIndex =
          findTargetIndexOrBlock(AM, IfTarget, Body, Med);
      if (TargetIndex == SIZE_MAX || TargetIndex <= static_cast<size_t>(I))
        continue;

      // A later `goto Join` that lands inside (NextI, TargetIndex) is the
      // else arm of `if (c) goto L_else; then; Join: ...; L_else: else; goto Join`.
      size_t BackEdgeGoto = SIZE_MAX;
      va_t JoinAddr = 0;
      if (TakenCopies.empty()) {
        for (size_t K = TargetIndex; K < Body.size(); ++K) {
          const HighStmt &S = Body[K];
          if (K > TargetIndex &&
              (S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse ||
               S.Kind == StmtKind::While || S.Kind == StmtKind::CxxTry ||
               S.Kind == StmtKind::SEHTry))
            break;
          if (S.Kind != StmtKind::Goto)
            continue;
          const size_t JoinIdx = findTargetIndex(AM, S.GotoTarget);
          if (JoinIdx != SIZE_MAX && JoinIdx > static_cast<size_t>(I) &&
              JoinIdx < TargetIndex) {
            BackEdgeGoto = K;
            JoinAddr = S.GotoTarget;
          }
          break;
        }
      }
      if (BackEdgeGoto != SIZE_MAX) {
        const size_t JoinIdx = findTargetIndex(AM, JoinAddr);
        if (JoinIdx != SIZE_MAX && JoinIdx >= NextI &&
            ownsRun(Body, AM, {NextI, JoinIdx}, I, Med) &&
            ownsRun(Body, AM, {TargetIndex, BackEdgeGoto + 1}, I, Med)) {
          Stmt.Kind = StmtKind::IfElse;
          Stmt.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Stmt.Cond);
          std::vector<HighStmt> ThenBody;
          for (size_t K = NextI; K < JoinIdx; ++K)
            ThenBody.push_back(std::move(Body[K]));
          std::vector<HighStmt> ElseBody;
          for (size_t K = TargetIndex; K < BackEdgeGoto; ++K)
            ElseBody.push_back(std::move(Body[K]));
          Stmt.Body = std::move(ThenBody);
          Stmt.ElseBody = std::move(ElseBody);
          Body.erase(Body.begin() + static_cast<long>(TargetIndex),
                     Body.begin() + static_cast<long>(BackEdgeGoto + 1));
          Body.erase(Body.begin() + static_cast<long>(NextI),
                     Body.begin() + static_cast<long>(JoinIdx));
          Changed = true;
          continue;
        }
      }

      // `if (c) goto Else; then; goto Join; Else: else; Join:` is if/else.
      // Assign-only diamonds (id PHI) move without ownsRun; richer arms still
      // need exclusive ownership.
      if (TakenCopies.empty() && BackEdgeGoto == SIZE_MAX &&
          TargetIndex > NextI) {
        size_t ThenGoto = SIZE_MAX;
        va_t JoinAddr = 0;
        for (size_t K = NextI; K < TargetIndex; ++K) {
          if (Body[K].Kind == StmtKind::Goto && Body[K].GotoTarget &&
              Body[K].GotoTarget != InvalidVA) {
            ThenGoto = K;
            JoinAddr = Body[K].GotoTarget;
          }
        }
        const size_t JoinIdx =
            findTargetIndexOrBlock(AM, JoinAddr, Body, Med);
        bool AdjacentElse = ThenGoto != SIZE_MAX && ThenGoto + 1 == TargetIndex;
        if (!AdjacentElse && ThenGoto != SIZE_MAX && ThenGoto + 1 < TargetIndex) {
          AdjacentElse = true;
          for (size_t K = ThenGoto + 1; K < TargetIndex; ++K) {
            if (Body[K].Kind != StmtKind::Nop &&
                Body[K].Kind != StmtKind::Block) {
              AdjacentElse = false;
              break;
            }
          }
        }
        if (ThenGoto != SIZE_MAX && JoinIdx != SIZE_MAX &&
            JoinIdx >= TargetIndex && ThenGoto > NextI && AdjacentElse) {
          auto RangeAssignLike = [&](size_t Begin, size_t End) {
            for (size_t K = Begin; K < End; ++K)
              if (!stmtIsAssignLike(Body[K]))
                return false;
            return Begin < End;
          };
          const bool AssignDiamond =
              RangeAssignLike(NextI, ThenGoto) &&
              RangeAssignLike(TargetIndex, JoinIdx);
          if (AssignDiamond) {
            Stmt.Kind = StmtKind::IfElse;
            Stmt.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Stmt.Cond);
            std::vector<HighStmt> ThenBody;
            for (size_t K = NextI; K < ThenGoto; ++K)
              ThenBody.push_back(std::move(Body[K]));
            std::vector<HighStmt> ElseBody;
            for (size_t K = TargetIndex; K < JoinIdx; ++K)
              ElseBody.push_back(std::move(Body[K]));
            Stmt.Body = std::move(ThenBody);
            Stmt.ElseBody = std::move(ElseBody);
            Body.erase(Body.begin() + static_cast<long>(TargetIndex),
                       Body.begin() + static_cast<long>(JoinIdx));
            Body.erase(Body.begin() + static_cast<long>(NextI),
                       Body.begin() + static_cast<long>(ThenGoto + 1));
            Changed = true;
            continue;
          }
          // Call / store arms of the same diamond. A later sibling (Format)
          // between Else and Join is not the else arm; keep `goto Join` so
          // the then-arm does not fall into it.
          const size_t ElseEnd =
              findDiamondElseEnd(Body, TargetIndex, JoinIdx, JoinAddr);
          if (ElseEnd != SIZE_MAX && ElseEnd > TargetIndex &&
              rangeHasObservableWork(Body, NextI, ThenGoto) &&
              rangeHasObservableWork(Body, TargetIndex, ElseEnd) &&
              ownsRun(Body, AM, {NextI, ThenGoto},
                      static_cast<size_t>(I), Med) &&
              ownsRun(Body, AM, {TargetIndex, ElseEnd},
                      static_cast<size_t>(I), Med)) {
            const bool JoinFollowsElse = ElseEnd == JoinIdx;
            size_t ElseTake = ElseEnd;
            if (JoinFollowsElse && ElseTake > TargetIndex &&
                Body[ElseTake - 1].Kind == StmtKind::Goto &&
                Body[ElseTake - 1].GotoTarget == JoinAddr)
              --ElseTake;
            Stmt.Kind = StmtKind::IfElse;
            Stmt.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Stmt.Cond);
            std::vector<HighStmt> ThenBody;
            const size_t ThenTake =
                JoinFollowsElse ? ThenGoto : ThenGoto + 1;
            for (size_t K = NextI; K < ThenTake; ++K)
              ThenBody.push_back(std::move(Body[K]));
            std::vector<HighStmt> ElseBody;
            for (size_t K = TargetIndex; K < ElseTake; ++K)
              ElseBody.push_back(std::move(Body[K]));
            Stmt.Body = std::move(ThenBody);
            Stmt.ElseBody = std::move(ElseBody);
            Body.erase(Body.begin() + static_cast<long>(TargetIndex),
                       Body.begin() + static_cast<long>(ElseEnd));
            Body.erase(Body.begin() + static_cast<long>(NextI),
                       Body.begin() + static_cast<long>(ThenGoto + 1));
            Changed = true;
            continue;
          }
        }
      }

      // `if (c) goto L; <fallthrough>; L:` with no else back-edge is
      // `if (!c) { fallthrough; } L:`. Skip assign-only prefixes (throw
      // joins / PHI copies) so `if (arg0 == 7) throw` stays that shape.
      // A trailing `goto Join` in the skip range is real work: hiding it
      // leaves the else arm dead after an unconditional jump.
      auto RangeHasWork = [&](size_t Begin, size_t End) {
        for (size_t K = Begin; K < End; ++K) {
          const HighStmt &S = Body[K];
          if (S.Kind == StmtKind::Call || S.Kind == StmtKind::If ||
              S.Kind == StmtKind::IfElse || S.Kind == StmtKind::Store ||
              S.Kind == StmtKind::CxxTry || S.Kind == StmtKind::SEHTry ||
              S.Kind == StmtKind::While)
            return true;
          if (S.Kind == StmtKind::Assign && S.Val &&
              S.Val->Kind == ExprKind::Call)
            return true;
        }
        return false;
      };
      bool SkipIsElseGoto = false;
      bool SkipHasLoop = false;
      for (size_t K = NextI; K < TargetIndex; ++K) {
        const HighStmt &S = Body[K];
        if (S.Kind == StmtKind::While)
          SkipHasLoop = true;
        if (S.Kind == StmtKind::Goto && S.GotoTarget &&
            S.GotoTarget != InvalidVA && S.GotoTarget != IfTarget)
          SkipIsElseGoto = true;
      }
      bool TakenUsedAtJoin = false;
      if (!TakenCopies.empty() && TargetIndex < Body.size()) {
        for (const HighStmt &Copy : TakenCopies) {
          MedVar Dest;
          ExprPtr Val;
          if (!isValueAssign(Copy, Dest, Val) || !Val ||
              Val->Kind == ExprKind::Undef || Val->Kind == ExprKind::Load)
            continue;
          for (size_t K = TargetIndex;
               K < Body.size() && K < TargetIndex + 8; ++K) {
            if (stmtUsesJoinDest(Body[K], Dest))
              TakenUsedAtJoin = true;
          }
        }
      }
      if ((TakenCopies.empty() ||
           (armIsSkippable(TakenCopies) && !TakenUsedAtJoin)) &&
          BackEdgeGoto == SIZE_MAX && TargetIndex > NextI &&
          !SkipIsElseGoto && !SkipHasLoop &&
          RangeHasWork(NextI, TargetIndex) &&
          ownsRun(Body, AM, {NextI, TargetIndex}, I, Med)) {
        Stmt.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Stmt.Cond);
        Stmt.Body.clear();
        for (size_t K = NextI; K < TargetIndex; ++K)
          Stmt.Body.push_back(std::move(Body[K]));
        Body.erase(Body.begin() + static_cast<long>(NextI),
                   Body.begin() + static_cast<long>(TargetIndex));
        Changed = true;
        continue;
      }

      auto Else = findElseTarget(Body, NextI, IfTarget, TargetIndex);

      // Early-return fold.
      if (TakenCopies.empty() && Else.HasEarlyReturn && Else.Target == 0 &&
          Else.ReturnIdx != SIZE_MAX &&
          Else.FallthroughIndices.size() == Else.ReturnIdx - NextI + 1 &&
          ownsRun(Body, AM, {NextI, Else.ReturnIdx + 1}, I, Med)) {
        // The complete false-edge prefix, including its PHI copies, belongs
        // before this return. Shared entries must retain their original scope.
        Stmt.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Stmt.Cond);
        Stmt.Body.clear();
        for (size_t Idx : Else.FallthroughIndices)
          Stmt.Body.push_back(std::move(Body[Idx]));
        for (auto It = Else.FallthroughIndices.rbegin();
             It != Else.FallthroughIndices.rend(); ++It)
          Body.erase(Body.begin() + static_cast<long>(*It));
        Changed = true;
        continue;
      }

      // Same-target fold: both branches goto the same address.
      if (IfTarget == Else.Target) {
        if (!ownsRun(Body, AM, {NextI, Else.GotoIdx + 1}, I, Med))
          continue;
        // Both arms reach this target, but it need not be their physical
        // continuation: another branch can own statements between the common
        // transfer and its target. Keep that transfer unless the next emitted
        // statement is exactly the target; never fall through the intervening
        // code merely because both arms agree where to jump.
        const bool TargetFollows =
            Else.GotoIdx + 1 < Body.size() &&
            AddrMap::entryAddress(Body[Else.GotoIdx + 1]) == IfTarget;
        if (TakenCopies.empty()) {
          Stmt.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, Stmt.Cond);
          Stmt.Body.clear();
          for (size_t K = NextI; K < Else.GotoIdx; ++K)
            Stmt.Body.push_back(std::move(Body[K]));
        } else {
          Stmt.Kind = StmtKind::IfElse;
          Stmt.Body = std::move(TakenCopies);
          for (size_t K = NextI; K < Else.GotoIdx; ++K)
            Stmt.ElseBody.push_back(std::move(Body[K]));
        }
        Body.erase(Body.begin() + static_cast<long>(NextI),
                        Body.begin() +
                            static_cast<long>(Else.GotoIdx + TargetFollows));
        Changed = true;
        continue;
      }

      // General if/else structuring with merge-point inference.
      va_t IfDest = findGotoDestination(Body, AM, IfTarget);
      va_t ElseDest = Else.Target != 0
                          ? findGotoDestination(Body, AM, Else.Target)
                          : 0;
      va_t MergeTarget =
          inferMergeTarget(IfTarget, IfDest, Else.Target, ElseDest);

      auto IfResult =
          collectStmtsForTarget(Body, AM, IfTarget, MergeTarget);
      const size_t InlineEnd = Else.Target != 0 ? Else.GotoIdx + 1 : NextI;
      auto FoldSharedTail = [&]() {
        if (TargetIndex > NextI &&
            !ownsRun(Body, AM, {NextI, TargetIndex}, I, Med))
          return false;
        // Both arms now continue at the original target. Only the exclusive
        // false prefix moves; shared tail labels and statements stay in place.
        std::vector<HighStmt> FalseBody;
        for (size_t K = NextI; K < TargetIndex; ++K)
          FalseBody.push_back(std::move(Body[K]));
        Stmt.Kind = FalseBody.empty() ? StmtKind::If : StmtKind::IfElse;
        Stmt.Body = std::move(TakenCopies);
        Stmt.ElseBody = std::move(FalseBody);
        Body.erase(Body.begin() + static_cast<long>(NextI),
                        Body.begin() + static_cast<long>(TargetIndex));
        return true;
      };
      // The immediately following target also belongs to the false edge.
      if (IfResult.Start == NextI ||
          !ownsRun(Body, AM, IfResult, I, Med) ||
          (InlineEnd > NextI &&
           (!ownsRun(Body, AM, {NextI, InlineEnd}, I, Med) ||
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
          !endsWithTransfer(Body[IfResult.End - 1]);
      const va_t Successor =
          IfResult.End < Body.size()
              ? AddrMap::entryAddress(Body[IfResult.End])
              : 0;
      if (NeedsSuccessor && (!Successor || Body[IfResult.End].IsPhiCopy)) {
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
          MergeTarget != 0 && Continuation < Body.size() &&
          findTargetIndex(AM, MergeTarget) == Continuation;

      std::vector<HighStmt> IfBody = std::move(TakenCopies);
      for (size_t K = IfResult.Start; K < IfResult.End; ++K)
        IfBody.push_back(std::move(Body[K]));
      if (NeedsSuccessor) {
        HighStmt Transfer;
        Transfer.Kind = StmtKind::Goto;
        Transfer.GotoTarget = Successor;
        LiveTargets.insert(Transfer.GotoTarget);
        IfBody.push_back(std::move(Transfer));
      }
      std::vector<HighStmt> ElseBody;
      for (size_t K = NextI; K < InlineEnd; ++K)
        ElseBody.push_back(std::move(Body[K]));
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
        if (End <= Body.size())
          Body.erase(Body.begin() + static_cast<long>(Start),
                          Body.begin() + static_cast<long>(End));
      }

      Changed = true;
    }
  }
  dropDuplicateSkipGotos(Body);
  for (size_t I = 0; I < Body.size(); ++I) {
    HighStmt &S = Body[I];
    if ((S.Kind != StmtKind::If && S.Kind != StmtKind::IfElse) || !S.Cond)
      continue;
    ExprPtr Pred = outerCondFromSiblings(Body, I, S.Cond);
    for (size_t J = 0; J < S.Body.size(); ++J)
      if (S.Body[J].Kind == StmtKind::If ||
          S.Body[J].Kind == StmtKind::IfElse)
        dropUnusedTrailingAssigns(S.Body, J, Pred);
  }
  for (HighStmt &S : Body) {
    structureIfElseNested(S.Body, limits::kIfElseNestedArmPasses, Med);
    structureIfElseNested(S.ElseBody, limits::kIfElseNestedArmPasses, Med);
    for (auto &C : S.Cases)
      structureIfElseNested(C.Body, limits::kIfElseNestedArmPasses, Med);
    structureIfElseNested(S.DefaultBody, limits::kIfElseNestedArmPasses, Med);
    for (auto &Clause : S.EHClauseBodies)
      structureIfElseNested(Clause, limits::kIfElseNestedArmPasses, Med);
  }
  if (WantDetail) {
    auto Ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - T0)
                  .count();
    if (Body.size() >= 64 || Ms >= 2)
      syncWarning() << "m2h-ifelse-list: size=" << Body.size()
                    << " passes=" << Pass << " cap=" << MaxPasses
                    << " ms=" << Ms << "\n";
  }
}

} // namespace neverd
