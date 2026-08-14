//===- SatSolver.h - Conflict-driven clause-learning SAT core ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A propositional satisfiability engine over clauses in conjunctive normal
/// form, and the bottom layer of NeverD's reasoning stack: the bit-blaster
/// lowers a \c SymExpr into clauses, and everything a caller asks about a
/// bitvector formula is decided here.
///
/// The search is conflict-driven with clause learning.  It guesses a value for
/// a variable, propagates everything that guess forces, and when propagation
/// falsifies a clause it derives the clause that explains the failure, adds it
/// to the database, and jumps back to the level where that new clause becomes
/// unit.  Learning is the whole difference between this and enumeration: every
/// conflict rules out a region of the search space rather than one point, so
/// the same dead end is never entered twice by a different route.
///
/// Four things make it fast enough to sit under an interactive decompiler:
///
///   1. *Watched literals.*  A clause is only visited when one of the two
///      literals it watches becomes false, so a clause that stays satisfied
///      costs nothing however often the variables around it move, and
///      backtracking needs no bookkeeping at all.
///
///   2. *Activity-ordered decisions.*  Variables that took part in recent
///      conflicts are branched on first, which keeps the search inside the
///      part of the formula that is actually contested.
///
///   3. *Restarts.*  Search restarts on a rapidly-then-slowly growing
///      schedule, so a bad early decision cannot cost the whole run while a
///      run that is making progress is still allowed to finish.
///
///   4. *Phase saving.*  A restarted or backjumped search re-decides a
///      variable to the value it last held, so work below an abandoned
///      decision is recovered rather than redone.
///
/// The engine is deterministic: no clock, no randomness, and every tie —
/// between equally active variables, between equally useless learned
/// clauses — is broken by index.  The same clauses in the same order always
/// produce the same answer, which is what lets a decompilation be reproduced
/// and a failure be reduced to a test.
///
/// Nothing here throws to report a problem.  An unsatisfiable formula, an
/// exhausted budget and a clause that cannot be added are all return values,
/// because the callers are analysis passes with their own budgets rather than
/// code that can abandon a decompilation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SOLVER_SATSOLVER_H
#define NEVERD_SOLVER_SATSOLVER_H

#include "neverd/solver/SatTypes.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace neverd::solver {

namespace detail {
class SatEngine;
} // namespace detail

/// Tuning for the search.  The defaults are what NeverD's own callers use; a
/// caller with a hard latency bound sets a budget, and one measuring the
/// engine varies the rest.
struct SatOptions {
  /// Per-conflict multiplier applied to the branching activities.  Lower
  /// forgets older conflicts faster, which suits formulas whose difficulty
  /// moves around; higher suits formulas with one stable hard core.
  double VarDecay = 0.95;

  /// The same idea for learned clauses, deciding which survive a reduction.
  double ClauseDecay = 0.999;

  /// Conflicts allowed in the first restart interval.  Later intervals scale
  /// it by a sequence that revisits short runs often while still growing
  /// without bound, so no single interval has to be guessed correctly.
  uint64_t RestartInterval = 100;

  /// Learned clauses kept, as a fraction of the number of input clauses.
  double LearnedFraction = 1.0 / 3.0;

  /// Growth applied to that bound at each restart, so a long search keeps more
  /// of what it has proved.
  double LearnedGrowth = 1.1;

  /// Abandon the search after this many conflicts, reporting
  /// \c SatResult::Unknown.  Zero leaves it unbounded.
  uint64_t MaxConflicts = 0;

  /// Abandon the search after this many propagated literals.  Zero leaves it
  /// unbounded.  This bounds wall time far more tightly than a conflict count
  /// does, because a single conflict can hide an arbitrarily long propagation.
  uint64_t MaxPropagations = 0;

  /// Abandon the search after visiting this many watched-clause entries.
  /// Zero leaves it unbounded.  This complements \c MaxPropagations: one
  /// propagated literal may have arbitrarily many clauses watching it.
  uint64_t MaxWatchVisits = 0;

  /// Drop literals from a learned clause when the rest of it already implies
  /// them.  Costs a walk of the reason graph per conflict and pays for itself
  /// many times over in propagation strength.
  bool MinimizeLearned = true;

  /// Re-decide a variable to the value it last held.
  bool PhaseSaving = true;

  /// Value given to a variable that has never been assigned.  False first is
  /// the better guess for the encodings this engine is fed, where most gate
  /// outputs are false in most assignments.
  bool DefaultPhase = false;
};

/// Counters describing what a search did.  Diagnostic only: no decision is
/// made from them, so a caller may read them or ignore them freely.
struct SatStats {
  uint64_t Decisions = 0;
  uint64_t Conflicts = 0;
  uint64_t Propagations = 0;
  /// Watched-clause entries examined during propagation.
  uint64_t WatchVisits = 0;
  uint64_t Restarts = 0;
  /// Clauses derived from conflicts, before any were deleted again.
  uint64_t LearnedClauses = 0;
  /// Learned clauses discarded by database reduction.
  uint64_t DeletedClauses = 0;
  /// Literals removed from learned clauses by minimization.
  uint64_t MinimizedLiterals = 0;
};

/// A CDCL satisfiability engine over a growable clause database.
///
/// The solver is incremental in two senses.  Clauses and variables may be
/// added between calls to \c solve, and everything learned from earlier calls
/// is kept.  And a single call may be made under assumptions — literals held
/// true for that call only — which is how a caller asks many related questions
/// without re-encoding the formula each time.
///
/// The clause database, the watch lists and the trail are deliberately not in
/// this header.  A caller depends on the question it is asking, not on how the
/// answer is represented.
class SatSolver {
public:
  explicit SatSolver(const SatOptions &Opts = SatOptions());
  ~SatSolver();

  SatSolver(const SatSolver &) = delete;
  SatSolver &operator=(const SatSolver &) = delete;
  SatSolver(SatSolver &&) noexcept;
  SatSolver &operator=(SatSolver &&) noexcept;

  //===--------------------------------------------------------------------===//
  // Building the formula
  //===--------------------------------------------------------------------===//

  /// Allocate a variable.  Returns \c kInvalidSatVar once \c kMaxSatVar is
  /// reached, which no reachable encoding does.
  ///
  /// A variable that is not a \p Decision variable is never branched on.  It
  /// still propagates and still appears in learned clauses; it simply is not
  /// part of what the search chooses, which is what a caller wants for a
  /// variable whose value is determined by others.
  SatVar newVar(bool Decision = true);

  uint32_t numVars() const;
  /// Input clauses currently in the database, not counting learned ones.
  size_t numClauses() const;
  /// Learned clauses currently in the database.
  size_t numLearnedClauses() const;

  /// Add a clause.  Duplicate literals are collapsed, literals already false
  /// at the root of the search are dropped, and a clause containing a variable
  /// and its complement is discarded as trivially true.
  ///
  /// Returns false once the formula is unsatisfiable on its own — either
  /// because this clause is empty after simplification, or because adding it
  /// made root-level propagation fail.  The solver then stays falsified and
  /// every later \c solve reports \c SatResult::Unsat without searching.
  bool addClause(llvm::ArrayRef<SatLit> Lits);
  bool addClause(SatLit A);
  bool addClause(SatLit A, SatLit B);
  bool addClause(SatLit A, SatLit B, SatLit C);

  /// True once the clauses alone are contradictory.
  bool isFalsified() const;

  //===--------------------------------------------------------------------===//
  // Asking
  //===--------------------------------------------------------------------===//

  SatResult solve();

  /// Solve with \p Assumptions held true for this call only.
  ///
  /// This is how an incremental caller switches parts of a formula on and off:
  /// encode each part guarded by its own literal, then assume the guards it
  /// wants.  Everything learned survives the call, so a series of related
  /// questions costs far less than the sum of the same questions asked cold.
  /// An invalid literal or one naming a variable this solver does not own
  /// returns \c SatResult::Invalid without changing the formula.  Only a
  /// finite search or storage budget returns \c SatResult::Unknown.
  SatResult solve(llvm::ArrayRef<SatLit> Assumptions);

  //===--------------------------------------------------------------------===//
  // Reading the answer
  //===--------------------------------------------------------------------===//

  /// The value of \p V in the satisfying assignment.  Only meaningful after
  /// \c solve returned \c SatResult::Sat; \c SatValue::Unknown otherwise, and
  /// also for a variable the formula never constrained.
  SatValue modelValue(SatVar V) const;
  SatValue modelValue(SatLit L) const;

  /// The subset of the last call's assumptions that were enough to make it
  /// unsatisfiable.  Empty unless the last \c solve returned
  /// \c SatResult::Unsat with a nonempty assumption list.
  ///
  /// A caller that switches parts of a formula on and off by guard literal
  /// reads this to learn *which* combination failed, which is strictly more
  /// than "one of them did" and is usually the whole reason to ask.
  llvm::ArrayRef<SatLit> failedAssumptions() const;

  const SatStats &stats() const;
  const SatOptions &options() const;
  /// Replace the tuning.  Budgets take effect on the next \c solve; the decay
  /// rates take effect immediately.
  void setOptions(const SatOptions &Opts);

private:
  std::unique_ptr<detail::SatEngine> Engine;
};

} // namespace neverd::solver

#endif // NEVERD_SOLVER_SATSOLVER_H
