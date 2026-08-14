//===- SatAnalyze.cpp - Learning a clause from a conflict -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Turns a falsified clause into a new clause that the formula implied all
/// along, and that rules out not just the assignment which failed but every
/// assignment failing for the same reason.  Learning is what separates this
/// search from enumeration, so this file decides how well the whole engine
/// scales far more than any tuning constant does.
///
/// The derivation walks backwards along the trail, replacing each literal
/// assigned at the current decision level by the clause that forced it, and
/// stops as soon as a single literal of that level is left.  That literal is
/// the point every path from the last decision to the conflict runs through,
/// so the clause built from it is the strongest one this conflict supports
/// that still asserts something the moment the search jumps back.
///
/// The result is then strengthened.  A literal whose own reason is already
/// covered by the rest of the clause adds nothing, and dropping it makes the
/// clause propagate in strictly more situations for the rest of the run.
/// Finding those literals is a walk of the reason graph, cut short by a
/// fingerprint of the decision levels the clause spans: a literal from a level
/// the clause does not mention cannot be covered by it, and that one test
/// prunes nearly all of the walk.
///
//===----------------------------------------------------------------------===//

#include "SatDetail.h"

#include "neverd/solver/SatTypes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace neverd::solver::detail {

namespace {

/// The bit a decision level contributes to the level fingerprint.
///
/// Levels are folded into 32 bits, so distinct levels can share a bit.  That
/// only ever makes the redundancy test admit a walk it could have skipped, and
/// the walk itself is exact, so the fold costs a little time and no soundness.
inline uint32_t levelBit(uint32_t Level) { return 1u << (Level & 31u); }

} // namespace

void SatEngine::analyze(ClauseRef Conflict, std::vector<SatLit> &Learnt,
                        uint32_t &BacktrackLevel, uint32_t &LevelSpan) {
  Learnt.clear();
  ToClear.clear();

  // Slot zero belongs to the literal the finished clause will assert.  Which
  // one that is only becomes known when the walk reaches the implication
  // point, so the slot is reserved and filled at the end.
  Learnt.push_back(SatLit());

  uint32_t PathCount = 0;
  SatLit Pivot;
  size_t Index = Trail.size();
  ClauseRef Current = Conflict;

  do {
    ClauseView C = clause(Current);
    if (C.isLearned())
      bumpClauseActivity(Current);

    // Every literal of the conflicting clause takes part.  For a reason clause
    // the first literal is the one that clause forced, and the walk arrived
    // here precisely by accounting for it.
    for (uint32_t I = Pivot.isValid() ? 1 : 0, E = C.size(); I < E; ++I) {
      SatLit Q = C.lit(I);
      SatVar V = Q.var();
      if (Seen[V] || Level[V] == 0)
        continue;

      Seen[V] = 1;
      ToClear.push_back(V);
      bumpVarActivity(V);

      // Literals from the current level are resolved away by continuing the
      // walk; literals from earlier levels are what the new clause is made of.
      if (Level[V] >= decisionLevel())
        ++PathCount;
      else
        Learnt.push_back(Q);
    }

    do {
      --Index;
    } while (!Seen[Trail[Index].var()]);

    Pivot = Trail[Index];
    Current = Reason[Pivot.var()];
    Seen[Pivot.var()] = 0;
    --PathCount;
  } while (PathCount > 0);

  Learnt[0] = ~Pivot;

  if (Opts.MinimizeLearned && Learnt.size() > 1) {
    uint32_t LevelMask = 0;
    for (size_t I = 1, E = Learnt.size(); I < E; ++I)
      LevelMask |= levelBit(Level[Learnt[I].var()]);

    size_t Kept = 1;
    for (size_t I = 1, E = Learnt.size(); I < E; ++I) {
      SatVar V = Learnt[I].var();
      // A decision has no reason to be covered by, so it always stays.
      if (Reason[V] == kNoClause || !isRedundant(Learnt[I], LevelMask))
        Learnt[Kept++] = Learnt[I];
    }
    Stats.MinimizedLiterals += Learnt.size() - Kept;
    Learnt.resize(Kept);
  }

  // The second slot must hold the literal assigned latest, because that is the
  // level the clause becomes unit at and therefore the level the watch has to
  // survive to.
  if (Learnt.size() == 1) {
    BacktrackLevel = 0;
  } else {
    size_t Latest = 1;
    for (size_t I = 2, E = Learnt.size(); I < E; ++I)
      if (Level[Learnt[I].var()] > Level[Learnt[Latest].var()])
        Latest = I;
    std::swap(Learnt[1], Learnt[Latest]);
    BacktrackLevel = Level[Learnt[1].var()];
  }

  LevelScratch.clear();
  for (SatLit L : Learnt)
    LevelScratch.push_back(Level[L.var()]);
  std::sort(LevelScratch.begin(), LevelScratch.end());
  LevelScratch.erase(std::unique(LevelScratch.begin(), LevelScratch.end()),
                     LevelScratch.end());
  LevelSpan = static_cast<uint32_t>(LevelScratch.size());

  for (SatVar V : ToClear)
    Seen[V] = 0;
  ToClear.clear();
}

bool SatEngine::isRedundant(SatLit L, uint32_t LevelMask) {
  // Marks made while checking this literal are provisional: if the check
  // fails they have to go, and if it succeeds they are worth keeping because
  // the same literals will come up again while checking the next one.
  const size_t Mark = ToClear.size();

  RedundancyStack.clear();
  RedundancyStack.push_back(L);

  while (!RedundancyStack.empty()) {
    SatLit Q = RedundancyStack.back();
    RedundancyStack.pop_back();

    ClauseView C = clause(Reason[Q.var()]);
    for (uint32_t I = 1, E = C.size(); I < E; ++I) {
      SatLit M = C.lit(I);
      SatVar V = M.var();

      // Already covered, or true at the root and so covered by nothing.
      if (Seen[V] || Level[V] == 0)
        continue;

      // A decision, or a level the clause does not mention: this branch of the
      // reason graph leaves the clause, so the literal is not redundant.
      if (Reason[V] == kNoClause || (levelBit(Level[V]) & LevelMask) == 0) {
        for (size_t K = Mark, N = ToClear.size(); K < N; ++K)
          Seen[ToClear[K]] = 0;
        ToClear.resize(Mark);
        return false;
      }

      Seen[V] = 1;
      ToClear.push_back(V);
      RedundancyStack.push_back(M);
    }
  }

  return true;
}

void SatEngine::analyzeFinal(SatLit Failed, std::vector<SatLit> &Out) {
  Out.clear();
  Out.push_back(Failed);

  if (decisionLevel() == 0)
    return;

  // Walk back over everything that is currently assigned, keeping the
  // decisions that contributed.  Every decision on the trail at this point is
  // an assumption, because assumptions are taken before any branching starts,
  // so what comes out is the subset of the assumptions that cannot hold
  // together with the one that failed.
  Seen[Failed.var()] = 1;

  for (size_t I = Trail.size(), Bound = TrailLim[0]; I > Bound;) {
    --I;
    SatVar V = Trail[I].var();
    if (!Seen[V])
      continue;
    Seen[V] = 0;

    ClauseRef R = Reason[V];
    if (R == kNoClause) {
      if (Level[V] > 0)
        Out.push_back(Trail[I]);
      continue;
    }

    ClauseView C = clause(R);
    for (uint32_t J = 1, E = C.size(); J < E; ++J) {
      SatVar U = C.lit(J).var();
      if (Level[U] > 0)
        Seen[U] = 1;
    }
  }

  Seen[Failed.var()] = 0;
}

} // namespace neverd::solver::detail
