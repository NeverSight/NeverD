//===- SatSolver.cpp - The public solver and the search driver ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the engine's construction, clause entry, and the loop that
/// alternates propagation with decisions until the formula is decided; plus
/// the thin public class that forwards to it.
///
/// Two things happen here that are easy to overlook.
///
/// A clause is simplified before it is stored.  Duplicates collapse, literals
/// already false at the root of the search are dropped, and a clause holding
/// both polarities of a variable is discarded.  Doing that once at entry keeps
/// the propagation loop free of cases that can never help.
///
/// A budget is per call rather than cumulative.  The statistics accumulate for
/// the life of the solver because a caller wants to know what a whole analysis
/// cost, but a caller asking a thousand incremental questions wants each of
/// them bounded on its own.
///
//===----------------------------------------------------------------------===//

#include "neverd/solver/SatSolver.h"

#include "SatDetail.h"

#include "neverd/solver/SatTypes.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <memory>
#include <utility>

namespace neverd::solver {

namespace detail {

namespace {

uint64_t budgetEndpoint(uint64_t Used, uint64_t Allowance) {
  if (Allowance == 0)
    return 0;
  constexpr uint64_t Max = std::numeric_limits<uint64_t>::max();
  return Used > Max - Allowance ? Max : Used + Allowance;
}

} // namespace

SatEngine::SatEngine(const SatOptions &Opts) : Opts(Opts), Order(Activity) {}

//===----------------------------------------------------------------------===//
// Building the formula
//===----------------------------------------------------------------------===//

SatVar SatEngine::newVar(bool Decision) {
  if (Value.size() > kMaxSatVar)
    return kInvalidSatVar;

  auto V = static_cast<SatVar>(Value.size());
  Value.push_back(0);
  Level.push_back(0);
  Reason.push_back(kNoClause);
  Phase.push_back(Opts.DefaultPhase ? 1 : 0);
  Decidable.push_back(Decision ? 1 : 0);
  Seen.push_back(0);
  Activity.push_back(0.0);
  // One watch list per polarity, both empty until a clause watches them.
  Watches.emplace_back();
  Watches.emplace_back();

  Order.grow(Value.size());
  if (Decision)
    Order.insert(V);
  return V;
}

bool SatEngine::addClause(llvm::ArrayRef<SatLit> Lits) {
  if (Falsified || OutOfSpace)
    return false;

  cancelUntil(0);

  ClauseScratch.assign(Lits.begin(), Lits.end());
  for (SatLit L : ClauseScratch) {
    assert(L.isValid() && L.var() < Value.size() &&
           "a clause named a variable this solver does not have");
    if (!L.isValid() || L.var() >= Value.size())
      return false;
  }

  // Sorting puts duplicates next to each other and a variable's two polarities
  // next to each other, so both checks below are a look at the previous kept
  // literal rather than a search.
  std::sort(ClauseScratch.begin(), ClauseScratch.end());

  size_t Kept = 0;
  SatLit Prev;
  for (SatLit L : ClauseScratch) {
    int8_t V = litValue(L);
    if (V == 1)
      return true; // already true at the root, so the clause says nothing
    if (Prev.isValid() && L == ~Prev)
      return true; // holds both polarities, so it is trivially true
    if (V == -1 || (Prev.isValid() && L == Prev))
      continue;
    ClauseScratch[Kept++] = L;
    Prev = L;
  }
  ClauseScratch.resize(Kept);

  if (ClauseScratch.empty()) {
    Falsified = true;
    return false;
  }

  if (ClauseScratch.size() == 1) {
    enqueue(ClauseScratch[0], kNoClause);
    if (propagate() != kNoClause) {
      Falsified = true;
      return false;
    }
    return true;
  }

  ClauseRef Ref = allocClause(ClauseScratch, /*Learned=*/false);
  if (Ref == kNoClause) {
    OutOfSpace = true;
    return false;
  }
  Clauses.push_back(Ref);
  attachClause(Ref);
  return true;
}

//===----------------------------------------------------------------------===//
// Searching
//===----------------------------------------------------------------------===//

bool SatEngine::outOfPropagationBudget() const {
  return PropagationBudgetAt != 0 && Stats.Propagations >= PropagationBudgetAt;
}

bool SatEngine::outOfWatchVisitBudget() const {
  return WatchVisitBudgetAt != 0 && Stats.WatchVisits >= WatchVisitBudgetAt;
}

bool SatEngine::outOfBudget() const {
  return (ConflictBudgetAt != 0 && Stats.Conflicts >= ConflictBudgetAt) ||
         outOfPropagationBudget() || outOfWatchVisitBudget();
}

void SatEngine::captureModel() { Model.assign(Value.begin(), Value.end()); }

SatResult SatEngine::search(uint64_t ConflictBudget,
                            llvm::ArrayRef<SatLit> Assumptions) {
  uint64_t ConflictsHere = 0;

  for (;;) {
    ClauseRef Conflict = propagate();

    if (Conflict != kNoClause) {
      ++Stats.Conflicts;
      ++ConflictsHere;

      // A conflict with nothing guessed is a contradiction in the clauses
      // themselves, and no amount of searching will make it go away.
      if (decisionLevel() == 0) {
        Falsified = true;
        return SatResult::Unsat;
      }

      uint32_t BacktrackLevel = 0;
      uint32_t LevelSpan = 0;
      analyze(Conflict, LearntScratch, BacktrackLevel, LevelSpan);
      cancelUntil(BacktrackLevel);

      // The derived clause is unit at the level just reached, by construction:
      // that is what "the level to jump back to" means.  So it is asserted
      // rather than merely stored, and the search resumes with one more thing
      // known instead of with one fewer guess.
      if (LearntScratch.size() == 1) {
        enqueue(LearntScratch[0], kNoClause);
      } else {
        ClauseRef Ref = allocClause(LearntScratch, /*Learned=*/true);
        if (Ref == kNoClause) {
          OutOfSpace = true;
          return SatResult::Unknown;
        }
        clause(Ref).setLevelSpan(LevelSpan);
        Learnt.push_back(Ref);
        attachClause(Ref);
        bumpClauseActivity(Ref);
        enqueue(LearntScratch[0], Ref);
        ++Stats.LearnedClauses;
      }

      decayVarActivity();
      decayClauseActivity();
      continue;
    }

    if (outOfBudget())
      return SatResult::Unknown;

    if (ConflictBudget != 0 && ConflictsHere >= ConflictBudget) {
      cancelUntil(0);
      return SatResult::Unknown;
    }

    // Assignments already on the trail are not clauses to forget, so the bound
    // is on what is held beyond them.
    if (static_cast<double>(Learnt.size()) -
            static_cast<double>(Trail.size()) >=
        MaxLearnt)
      reduceDatabase();

    // Assumptions come first and in order, each on its own level, so that
    // undoing one undoes exactly the assumptions after it.
    SatLit Next;
    while (decisionLevel() < Assumptions.size()) {
      SatLit A = Assumptions[decisionLevel()];
      int8_t V = litValue(A);
      if (V == 1) {
        // Already implied.  It still gets a level of its own, so that the
        // level number keeps counting assumptions rather than decisions.
        newDecisionLevel();
        continue;
      }
      if (V == -1) {
        analyzeFinal(A, FailedAssumptions);
        return SatResult::Unsat;
      }
      Next = A;
      break;
    }

    if (!Next.isValid()) {
      Next = pickBranchLit();
      if (!Next.isValid()) {
        // Nothing left to decide and nothing falsified: the trail is a model.
        captureModel();
        return SatResult::Sat;
      }
      ++Stats.Decisions;
    }

    newDecisionLevel();
    enqueue(Next, kNoClause);
  }
}

SatResult SatEngine::solve(llvm::ArrayRef<SatLit> Assumptions) {
  Model.clear();
  FailedAssumptions.clear();

  for (SatLit A : Assumptions)
    if (!A.isValid() || A.var() >= Value.size())
      return SatResult::Invalid;

  if (Falsified)
    return SatResult::Unsat;
  if (OutOfSpace)
    return SatResult::Unknown;

  cancelUntil(0);

  ConflictBudgetAt = budgetEndpoint(Stats.Conflicts, Opts.MaxConflicts);
  PropagationBudgetAt =
      budgetEndpoint(Stats.Propagations, Opts.MaxPropagations);
  WatchVisitBudgetAt = budgetEndpoint(Stats.WatchVisits, Opts.MaxWatchVisits);

  // Keep learned clauses in proportion to the formula, never fewer than a
  // floor: on a small formula a fraction of it rounds to nothing, and the
  // search would then throw away everything it derived.
  MaxLearnt = std::max(static_cast<double>(Clauses.size()) *
                           std::max(Opts.LearnedFraction, 0.0),
                       64.0);

  SatResult R = SatResult::Unknown;
  for (uint64_t Restart = 0; R == SatResult::Unknown; ++Restart) {
    if (outOfBudget() || OutOfSpace)
      break;

    uint64_t Budget = Opts.RestartInterval == 0
                          ? 0
                          : Opts.RestartInterval * restartTerm(Restart);
    R = search(Budget, Assumptions);

    if (R == SatResult::Unknown) {
      ++Stats.Restarts;
      MaxLearnt *= std::max(Opts.LearnedGrowth, 1.0);
    }
  }

  cancelUntil(0);
  // Formula construction performs mandatory root propagation outside search;
  // it must never inherit an exhausted per-call budget.
  ConflictBudgetAt = 0;
  PropagationBudgetAt = 0;
  WatchVisitBudgetAt = 0;
  return R;
}

} // namespace detail

//===----------------------------------------------------------------------===//
// SatSolver
//===----------------------------------------------------------------------===//

SatSolver::SatSolver(const SatOptions &Opts)
    : Engine(std::make_unique<detail::SatEngine>(Opts)) {}

SatSolver::~SatSolver() = default;
SatSolver::SatSolver(SatSolver &&) noexcept = default;
SatSolver &SatSolver::operator=(SatSolver &&) noexcept = default;

SatVar SatSolver::newVar(bool Decision) { return Engine->newVar(Decision); }

uint32_t SatSolver::numVars() const {
  return static_cast<uint32_t>(Engine->Value.size());
}

size_t SatSolver::numClauses() const { return Engine->Clauses.size(); }

size_t SatSolver::numLearnedClauses() const { return Engine->Learnt.size(); }

bool SatSolver::addClause(llvm::ArrayRef<SatLit> Lits) {
  return Engine->addClause(Lits);
}

bool SatSolver::addClause(SatLit A) {
  const SatLit Lits[] = {A};
  return Engine->addClause(Lits);
}

bool SatSolver::addClause(SatLit A, SatLit B) {
  const SatLit Lits[] = {A, B};
  return Engine->addClause(Lits);
}

bool SatSolver::addClause(SatLit A, SatLit B, SatLit C) {
  const SatLit Lits[] = {A, B, C};
  return Engine->addClause(Lits);
}

bool SatSolver::isFalsified() const { return Engine->Falsified; }

SatResult SatSolver::solve() { return Engine->solve({}); }

SatResult SatSolver::solve(llvm::ArrayRef<SatLit> Assumptions) {
  return Engine->solve(Assumptions);
}

SatValue SatSolver::modelValue(SatVar V) const {
  if (V >= Engine->Model.size())
    return SatValue::Unknown;
  int8_t Val = Engine->Model[V];
  if (Val == 0)
    return SatValue::Unknown;
  return Val > 0 ? SatValue::True : SatValue::False;
}

SatValue SatSolver::modelValue(SatLit L) const {
  SatValue V = modelValue(L.var());
  return L.isNegated() ? negate(V) : V;
}

llvm::ArrayRef<SatLit> SatSolver::failedAssumptions() const {
  return Engine->FailedAssumptions;
}

const SatStats &SatSolver::stats() const { return Engine->Stats; }

const SatOptions &SatSolver::options() const { return Engine->Opts; }

void SatSolver::setOptions(const SatOptions &Opts) { Engine->Opts = Opts; }

} // namespace neverd::solver
