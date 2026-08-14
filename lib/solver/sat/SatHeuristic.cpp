//===- SatHeuristic.cpp - Decisions, restarts, database reduction ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The parts of the search that are choices rather than deductions: which
/// variable to branch on, which value to try first, when to start over, and
/// which learned clauses to forget.
///
/// None of these can be wrong, only expensive — every one of them is a guess
/// the deductive machinery is free to overrule.  That is exactly why they are
/// gathered here: a reader looking for what the engine *proves* never has to
/// come into this file, and a reader tuning what it *tries* never has to leave
/// it.
///
/// Every choice breaks its ties by index, so a formula built the same way is
/// searched the same way.  That is not a detail for a decompiler: a
/// simplification that appears only on some runs is worse than one that never
/// appears, because a user cannot tell it apart from a fault elsewhere.
///
//===----------------------------------------------------------------------===//

#include "SatDetail.h"

#include "neverd/solver/SatTypes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace neverd::solver::detail {

namespace {

/// Activities are rescaled rather than allowed to overflow.  Scaling every
/// activity by the same factor leaves their order — which is all the heap
/// reads — untouched.
constexpr double kVarActivityCeiling = 1e100;
constexpr double kVarActivityScale = 1e-100;
constexpr float kClauseActivityCeiling = 1e20f;
constexpr float kClauseActivityScale = 1e-20f;

} // namespace

//===----------------------------------------------------------------------===//
// The activity queue
//===----------------------------------------------------------------------===//

void ActivityQueue::insert(SatVar V) {
  grow(static_cast<size_t>(V) + 1);
  if (Position[V] >= 0)
    return;
  Position[V] = static_cast<int32_t>(Heap.size());
  Heap.push_back(V);
  siftUp(Heap.size() - 1);
}

void ActivityQueue::increased(SatVar V) {
  if (V < Position.size() && Position[V] >= 0)
    siftUp(static_cast<size_t>(Position[V]));
}

SatVar ActivityQueue::removeMax() {
  SatVar Top = Heap.front();
  Position[Top] = -1;

  SatVar Last = Heap.back();
  Heap.pop_back();
  if (!Heap.empty()) {
    Heap[0] = Last;
    Position[Last] = 0;
    siftDown(0);
  }
  return Top;
}

void ActivityQueue::clear() {
  Heap.clear();
  std::fill(Position.begin(), Position.end(), -1);
}

void ActivityQueue::siftUp(size_t I) {
  SatVar V = Heap[I];
  while (I > 0) {
    size_t Parent = (I - 1) / 2;
    if (!higher(V, Heap[Parent]))
      break;
    Heap[I] = Heap[Parent];
    Position[Heap[I]] = static_cast<int32_t>(I);
    I = Parent;
  }
  Heap[I] = V;
  Position[V] = static_cast<int32_t>(I);
}

void ActivityQueue::siftDown(size_t I) {
  SatVar V = Heap[I];
  const size_t N = Heap.size();
  for (;;) {
    size_t Child = 2 * I + 1;
    if (Child >= N)
      break;
    if (Child + 1 < N && higher(Heap[Child + 1], Heap[Child]))
      ++Child;
    if (!higher(Heap[Child], V))
      break;
    Heap[I] = Heap[Child];
    Position[Heap[I]] = static_cast<int32_t>(I);
    I = Child;
  }
  Heap[I] = V;
  Position[V] = static_cast<int32_t>(I);
}

//===----------------------------------------------------------------------===//
// Restart schedule
//===----------------------------------------------------------------------===//

uint64_t restartTerm(uint64_t I) {
  // Every term is a power of two, and the term index sits inside a run of
  // length 2^k - 1.  Find the smallest run containing the index, then descend
  // into it: at each step the index either names the run's last term, which
  // fixes the exponent, or falls inside one of the two half-runs before it.
  uint64_t Size = 1;
  uint64_t Exponent = 0;
  while (Size < I + 1 && Exponent < 62) {
    ++Exponent;
    Size = 2 * Size + 1;
  }
  while (Size - 1 != I && Exponent > 0) {
    Size = (Size - 1) / 2;
    --Exponent;
    I %= Size;
  }
  return uint64_t(1) << Exponent;
}

//===----------------------------------------------------------------------===//
// Decisions
//===----------------------------------------------------------------------===//

SatLit SatEngine::pickBranchLit() {
  while (!Order.empty()) {
    SatVar V = Order.removeMax();
    // The queue is a superset of the unassigned variables: backtracking puts
    // a variable back without checking whether it is already there, and a
    // variable can be assigned while queued.  Filtering on the way out is
    // cheaper than keeping the queue exact.
    if (Value[V] != 0 || !Decidable[V])
      continue;

    bool Positive = Opts.PhaseSaving ? Phase[V] != 0 : Opts.DefaultPhase;
    return SatLit::mk(V, !Positive);
  }
  return SatLit();
}

void SatEngine::bumpVarActivity(SatVar V) {
  Activity[V] += VarInc;
  if (Activity[V] > kVarActivityCeiling) {
    for (double &A : Activity)
      A *= kVarActivityScale;
    VarInc *= kVarActivityScale;
  }
  Order.increased(V);
}

void SatEngine::bumpClauseActivity(ClauseRef Ref) {
  ClauseView C = clause(Ref);
  float Updated = C.activity() + static_cast<float>(ClauseInc);
  C.setActivity(Updated);
  if (Updated <= kClauseActivityCeiling)
    return;

  for (ClauseRef Other : Learnt) {
    ClauseView D = clause(Other);
    D.setActivity(D.activity() * kClauseActivityScale);
  }
  ClauseInc *= static_cast<double>(kClauseActivityScale);
}

//===----------------------------------------------------------------------===//
// Forgetting
//===----------------------------------------------------------------------===//

void SatEngine::reduceDatabase() {
  // Worst first.  A clause spanning many decision levels records one path
  // through the search rather than a fact about the formula, so it is the
  // first to go; among clauses that look equally circumstantial, the one least
  // used recently goes first; and the remaining ties are broken by position so
  // that the same run always forgets the same clauses.
  std::stable_sort(Learnt.begin(), Learnt.end(),
                   [this](ClauseRef A, ClauseRef B) {
                     ClauseView CA = clause(A);
                     ClauseView CB = clause(B);
                     if (CA.levelSpan() != CB.levelSpan())
                       return CA.levelSpan() > CB.levelSpan();
                     if (CA.activity() != CB.activity())
                       return CA.activity() < CB.activity();
                     return A > B;
                   });

  const size_t Half = Learnt.size() / 2;
  size_t Kept = 0;

  for (size_t I = 0, E = Learnt.size(); I < E; ++I) {
    ClauseRef Ref = Learnt[I];
    ClauseView C = clause(Ref);

    // Three clauses are never discarded: a binary one, because it costs almost
    // nothing and propagates constantly; one tying together very few decision
    // levels, because that is close to a fact about the formula; and one that
    // is currently the reason for an assignment, because conflict analysis
    // will need to read it.
    bool Discard =
        I < Half && C.size() > 2 && C.levelSpan() > 2 && !isLocked(Ref);
    if (!Discard) {
      Learnt[Kept++] = Ref;
      continue;
    }

    detachClause(Ref);
    C.markDeleted();
    WastedWords += C.words();
    ++Stats.DeletedClauses;
  }

  Learnt.resize(Kept);
}

} // namespace neverd::solver::detail
