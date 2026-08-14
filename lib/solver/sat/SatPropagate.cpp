//===- SatPropagate.cpp - Unit propagation and the trail ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements clause storage, the watched-literal invariant, and the loop that
/// draws every consequence of the current assignment.  This is where a solver
/// spends most of its time, so the shape of the loop matters more here than
/// anywhere else in the engine.
///
/// The invariant is that a clause is reachable from exactly two of its
/// literals, and that neither of those two is false unless the clause is
/// already unit or falsified.  Assigning a literal therefore only requires
/// visiting the clauses watching its complement — every other clause still has
/// two non-false literals and cannot have become unit.  Undoing an assignment
/// requires no work at all, which is the property that makes backjumping over
/// many levels as cheap as backtracking over one.
///
/// Two details earn their keep in the inner loop.  Each watch entry carries a
/// second literal of its clause, so a clause that is already satisfied is
/// skipped without reading the arena — that is the common case, and it turns
/// most of propagation into a linear scan of one small array.  And when a
/// clause does propagate, the literal it forced is left in slot zero, which is
/// the invariant conflict analysis walks the reason graph on.
///
//===----------------------------------------------------------------------===//

#include "SatDetail.h"

#include "neverd/solver/SatTypes.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neverd::solver::detail {

namespace {

/// Drop the entry for \p Ref from \p List.
///
/// The last entry is moved into the hole rather than the tail being shifted
/// down.  Watch order is not part of the specification of anything, and it
/// stays a deterministic function of the operations performed, so reproducible
/// behaviour survives the reordering.
void removeWatch(std::vector<Watcher> &List, ClauseRef Ref) {
  for (size_t I = 0, E = List.size(); I < E; ++I) {
    if (List[I].Clause != Ref)
      continue;
    List[I] = List.back();
    List.pop_back();
    return;
  }
}

} // namespace

//===----------------------------------------------------------------------===//
// Clause storage
//===----------------------------------------------------------------------===//

ClauseRef SatEngine::allocClause(llvm::ArrayRef<SatLit> Lits, bool Learned) {
  size_t Need = ClauseView::kHeaderWords + Lits.size();
  if (Arena.size() + Need > kMaxArenaWords)
    return kNoClause;

  auto Ref = static_cast<ClauseRef>(Arena.size());
  Arena.resize(Arena.size() + Need);

  ClauseView C = clause(Ref);
  C.initialize(static_cast<uint32_t>(Lits.size()), Learned);
  for (uint32_t I = 0, E = static_cast<uint32_t>(Lits.size()); I < E; ++I)
    C.setLit(I, Lits[I]);
  return Ref;
}

void SatEngine::attachClause(ClauseRef Ref) {
  ClauseView C = clause(Ref);
  // A clause is visited when a watched literal becomes false, which is when
  // its complement becomes true, so it is filed under the complement.
  Watches[(~C.lit(0)).index()].push_back({Ref, C.lit(1)});
  Watches[(~C.lit(1)).index()].push_back({Ref, C.lit(0)});
}

void SatEngine::detachClause(ClauseRef Ref) {
  ClauseView C = clause(Ref);
  removeWatch(Watches[(~C.lit(0)).index()], Ref);
  removeWatch(Watches[(~C.lit(1)).index()], Ref);
}

bool SatEngine::isLocked(ClauseRef Ref) {
  ClauseView C = clause(Ref);
  SatLit First = C.lit(0);
  return litValue(First) == 1 && Reason[First.var()] == Ref;
}

//===----------------------------------------------------------------------===//
// The trail
//===----------------------------------------------------------------------===//

void SatEngine::enqueue(SatLit L, ClauseRef From) {
  SatVar V = L.var();
  Value[V] = L.isNegated() ? int8_t(-1) : int8_t(1);
  Level[V] = decisionLevel();
  Reason[V] = From;
  // Remember the value for the next time this variable is decided.  Recording
  // it here rather than while unwinding is the same thing, because it cannot
  // change while the variable stays assigned.
  Phase[V] = L.isNegated() ? 0 : 1;
  Trail.push_back(L);
}

void SatEngine::newDecisionLevel() {
  TrailLim.push_back(static_cast<uint32_t>(Trail.size()));
}

void SatEngine::cancelUntil(uint32_t Level) {
  if (decisionLevel() <= Level)
    return;

  uint32_t Bound = TrailLim[Level];
  for (size_t I = Trail.size(); I > Bound;) {
    --I;
    SatVar V = Trail[I].var();
    Value[V] = 0;
    Reason[V] = kNoClause;
    if (Decidable[V] && !Order.contains(V))
      Order.insert(V);
  }

  Trail.resize(Bound);
  TrailLim.resize(Level);
  QueueHead = Trail.size();
}

//===----------------------------------------------------------------------===//
// Propagation
//===----------------------------------------------------------------------===//

ClauseRef SatEngine::propagate() {
  ClauseRef Conflict = kNoClause;

  while (QueueHead < Trail.size()) {
    // A propagation budget stops between trail entries.  The watch-visit
    // interruption below also restores a complete list before returning.
    if (outOfPropagationBudget() || outOfWatchVisitBudget())
      break;

    SatLit P = Trail[QueueHead++];
    ++Stats.Propagations;

    // Every clause filed under P has just had one of its watched literals
    // falsified and has to be looked at.  Survivors are compacted forward as
    // the scan goes, so a clause that finds a new literal to watch is removed
    // from this list by simply not being copied.
    std::vector<Watcher> &List = Watches[P.index()];
    size_t Read = 0;
    size_t Write = 0;
    const size_t Count = List.size();

    while (Read < Count) {
      if (outOfWatchVisitBudget()) {
        // A partially compacted list is not a valid watch list.  Preserve the
        // processed survivors followed by the untouched suffix, then replay
        // this trail literal when a later solve supplies another budget.
        while (Read < Count)
          List[Write++] = List[Read++];
        --QueueHead;
        break;
      }

      Watcher W = List[Read++];
      ++Stats.WatchVisits;

      if (litValue(W.Blocker) == 1) {
        List[Write++] = W;
        continue;
      }

      ClauseView C = clause(W.Clause);
      SatLit Falsified = ~P;

      // Normalise so the falsified watch is in slot one, leaving slot zero for
      // the literal this clause would force.
      if (C.lit(0) == Falsified)
        C.swapLits(0, 1);

      SatLit First = C.lit(0);
      if (First != W.Blocker && litValue(First) == 1) {
        // Satisfied after all.  Record the literal that showed it, so the next
        // visit stops one step earlier.
        List[Write++] = {W.Clause, First};
        continue;
      }

      uint32_t Size = C.size();
      uint32_t Replacement = 2;
      for (; Replacement < Size; ++Replacement)
        if (litValue(C.lit(Replacement)) != -1)
          break;

      if (Replacement < Size) {
        // Watch the replacement instead.  It cannot be P, because P is false
        // and the replacement is not, so this never writes into the list being
        // scanned.
        C.swapLits(1, Replacement);
        Watches[(~C.lit(1)).index()].push_back({W.Clause, First});
        continue;
      }

      // Every literal but the first is false.
      List[Write++] = {W.Clause, First};
      if (litValue(First) == -1) {
        Conflict = W.Clause;
        QueueHead = Trail.size();
        while (Read < Count)
          List[Write++] = List[Read++];
        break;
      }
      enqueue(First, W.Clause);
    }

    List.resize(Write);
  }

  return Conflict;
}

} // namespace neverd::solver::detail
