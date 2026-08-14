//===- SatSolverTests.cpp - The CDCL core ---------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Holds the search to the four things everything above it assumes: that a
/// satisfiable formula comes back with an assignment that really satisfies
/// every clause, that an unsatisfiable one is refuted rather than merely not
/// solved, that assumptions constrain a single call and report which of them
/// clashed, and that the same formula is always answered the same way.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/solver/SatSolver.h"
#include "neverd/solver/SatTypes.h"

#include "llvm/ADT/ArrayRef.h"

#include <limits>
#include <vector>

using namespace neverd::solver;

namespace {

/// A formula kept alongside the solver so a model can be checked against the
/// clauses rather than against the solver's own opinion of them.
class Formula {
public:
  explicit Formula(const SatOptions &Opts = SatOptions()) : Solver(Opts) {}

  SatVar addVar(bool Decision = true) { return Solver.newVar(Decision); }

  void add(std::vector<SatLit> Lits) {
    Solver.addClause(Lits);
    Clauses.push_back(std::move(Lits));
  }

  /// True when \p Solver's model satisfies every clause that was added.
  bool modelSatisfiesEveryClause() const {
    for (const std::vector<SatLit> &Clause : Clauses) {
      bool Satisfied = false;
      for (SatLit L : Clause)
        Satisfied |= Solver.modelValue(L) == SatValue::True;
      if (!Satisfied)
        return false;
    }
    return true;
  }

  SatSolver Solver;

private:
  std::vector<std::vector<SatLit>> Clauses;
};

/// Three pigeons into two holes: the smallest formula that is unsatisfiable
/// for a reason the search has to derive rather than read off a unit clause.
void addPigeonhole(Formula &F, unsigned Pigeons, unsigned Holes) {
  std::vector<std::vector<SatVar>> In(Pigeons, std::vector<SatVar>(Holes));
  for (unsigned P = 0; P < Pigeons; ++P)
    for (unsigned H = 0; H < Holes; ++H)
      In[P][H] = F.addVar();

  for (unsigned P = 0; P < Pigeons; ++P) {
    std::vector<SatLit> Somewhere;
    for (unsigned H = 0; H < Holes; ++H)
      Somewhere.push_back(SatLit::positive(In[P][H]));
    F.add(Somewhere);
  }

  for (unsigned H = 0; H < Holes; ++H)
    for (unsigned P = 0; P < Pigeons; ++P)
      for (unsigned Q = P + 1; Q < Pigeons; ++Q)
        F.add({SatLit::negative(In[P][H]), SatLit::negative(In[Q][H])});
}

/// A satisfiable formula with enough structure that solving it takes a search
/// rather than one round of propagation.
void addChain(Formula &F, unsigned Length, std::vector<SatVar> &Vars) {
  for (unsigned I = 0; I < Length; ++I)
    Vars.push_back(F.addVar());

  for (unsigned I = 0; I + 2 < Length; ++I) {
    F.add({SatLit::positive(Vars[I]), SatLit::negative(Vars[I + 1]),
           SatLit::positive(Vars[I + 2])});
    F.add({SatLit::negative(Vars[I]), SatLit::positive(Vars[I + 1]),
           SatLit::negative(Vars[I + 2])});
  }
  F.add({SatLit::positive(Vars.front()), SatLit::positive(Vars.back())});
}

TEST(SatSolver, EmptyFormulaIsSatisfiable) {
  SatSolver S;
  EXPECT_EQ(S.solve(), SatResult::Sat);
  EXPECT_FALSE(S.isFalsified());
}

TEST(SatSolver, UnitAndItsComplementAreContradictory) {
  SatSolver S;
  SatVar A = S.newVar();
  EXPECT_TRUE(S.addClause(SatLit::positive(A)));
  EXPECT_FALSE(S.addClause(SatLit::negative(A)));
  EXPECT_TRUE(S.isFalsified());
  EXPECT_EQ(S.solve(), SatResult::Unsat);
}

TEST(SatSolver, TautologiesAndDuplicatesAreDiscarded) {
  SatSolver S;
  SatVar A = S.newVar();
  SatVar B = S.newVar();

  // A clause holding both polarities says nothing, and neither does a repeated
  // literal, so neither may reach the database.
  EXPECT_TRUE(S.addClause(SatLit::positive(A), SatLit::negative(A)));
  EXPECT_EQ(S.numClauses(), 0u);

  EXPECT_TRUE(S.addClause(
      {SatLit::positive(A), SatLit::positive(A), SatLit::positive(B)}));
  EXPECT_EQ(S.numClauses(), 1u);
  EXPECT_EQ(S.solve(), SatResult::Sat);
}

TEST(SatSolver, ModelSatisfiesEveryClause) {
  Formula F;
  std::vector<SatVar> Vars;
  addChain(F, 24, Vars);

  ASSERT_EQ(F.Solver.solve(), SatResult::Sat);
  EXPECT_TRUE(F.modelSatisfiesEveryClause());

  for (SatVar V : Vars)
    EXPECT_NE(F.Solver.modelValue(V), SatValue::Unknown);
}

TEST(SatSolver, PigeonholeIsRefuted) {
  Formula F;
  addPigeonhole(F, /*Pigeons=*/3, /*Holes=*/2);
  EXPECT_EQ(F.Solver.solve(), SatResult::Unsat);

  Formula Fits;
  addPigeonhole(Fits, /*Pigeons=*/3, /*Holes=*/3);
  EXPECT_EQ(Fits.Solver.solve(), SatResult::Sat);
  EXPECT_TRUE(Fits.modelSatisfiesEveryClause());
}

TEST(SatSolver, LearningHappensOnAFormulaThatNeedsIt) {
  Formula F;
  addPigeonhole(F, /*Pigeons=*/5, /*Holes=*/4);
  EXPECT_EQ(F.Solver.solve(), SatResult::Unsat);
  // The refutation cannot come from propagation alone, so a clause must have
  // been derived on the way to it.
  EXPECT_GT(F.Solver.stats().Conflicts, 0u);
  EXPECT_GT(F.Solver.stats().LearnedClauses, 0u);
}

TEST(SatSolver, AssumptionsHoldForOneCallOnly) {
  SatSolver S;
  SatVar X = S.newVar();
  SatVar G1 = S.newVar();
  SatVar G2 = S.newVar();

  // Each guard switches on a constraint that is fine alone and impossible
  // together, which is the shape an incremental caller actually builds.
  S.addClause(SatLit::negative(G1), SatLit::positive(X));
  S.addClause(SatLit::negative(G2), SatLit::negative(X));

  EXPECT_EQ(S.solve(), SatResult::Sat);

  const SatLit First[] = {SatLit::positive(G1)};
  EXPECT_EQ(S.solve(First), SatResult::Sat);
  EXPECT_EQ(S.modelValue(X), SatValue::True);

  const SatLit Second[] = {SatLit::positive(G2)};
  EXPECT_EQ(S.solve(Second), SatResult::Sat);
  EXPECT_EQ(S.modelValue(X), SatValue::False);

  const SatLit Both[] = {SatLit::positive(G1), SatLit::positive(G2)};
  EXPECT_EQ(S.solve(Both), SatResult::Unsat);
  EXPECT_EQ(S.failedAssumptions().size(), 2u);

  // Retracting them costs nothing and leaves the formula as it was.
  EXPECT_EQ(S.solve(), SatResult::Sat);
  EXPECT_TRUE(S.failedAssumptions().empty());
}

TEST(SatSolver, FailedAssumptionsNameTheGuardsThatClashed) {
  SatSolver S;
  SatVar X = S.newVar();
  SatVar G1 = S.newVar();
  SatVar G2 = S.newVar();
  SatVar Spare = S.newVar();

  S.addClause(SatLit::negative(G1), SatLit::positive(X));
  S.addClause(SatLit::negative(G2), SatLit::negative(X));

  const SatLit Assumptions[] = {SatLit::positive(Spare), SatLit::positive(G1),
                                SatLit::positive(G2)};
  ASSERT_EQ(S.solve(Assumptions), SatResult::Unsat);

  // The spare guard constrains nothing, so a useful core leaves it out.
  bool SawSpare = false;
  bool SawFirst = false;
  bool SawSecond = false;
  for (SatLit L : S.failedAssumptions()) {
    SawSpare |= L.var() == Spare;
    SawFirst |= L.var() == G1;
    SawSecond |= L.var() == G2;
  }
  EXPECT_TRUE(SawFirst);
  EXPECT_TRUE(SawSecond);
  EXPECT_FALSE(SawSpare);
}

TEST(SatSolver, ContradictoryAssumptionsAreRefutedWithoutClauses) {
  SatSolver S;
  SatVar A = S.newVar();

  const SatLit Both[] = {SatLit::positive(A), SatLit::negative(A)};
  EXPECT_EQ(S.solve(Both), SatResult::Unsat);
  // The clauses are still satisfiable; only the assumptions were not.
  EXPECT_FALSE(S.isFalsified());
  EXPECT_EQ(S.solve(), SatResult::Sat);
}

TEST(SatSolver, InvalidAssumptionsAreNotReportedAsBudgetExhaustion) {
  SatSolver S;
  S.newVar();

  const SatLit Assumptions[] = {SatLit()};
  EXPECT_EQ(S.solve(Assumptions), SatResult::Invalid);
  EXPECT_STREQ(satResultName(SatResult::Invalid), "invalid");

  const SatLit OutOfRange[] = {SatLit::positive(1)};
  EXPECT_EQ(S.solve(OutOfRange), SatResult::Invalid);

  // A malformed per-call question does not alter the underlying formula.
  EXPECT_EQ(S.solve(), SatResult::Sat);
}

TEST(SatSolver, APropagationBudgetIsAHardBoundForALongUnitChain) {
  SatOptions Opts;
  Opts.MaxPropagations = 1;

  Formula F(Opts);
  std::vector<SatVar> Vars;
  for (unsigned I = 0; I < 256; ++I)
    Vars.push_back(F.addVar());
  for (size_t I = 1; I < Vars.size(); ++I)
    F.add({SatLit::negative(Vars[I - 1]), SatLit::positive(Vars[I])});

  const SatLit Entry[] = {SatLit::positive(Vars.front())};
  uint64_t PropagationsBeforeSolve = F.Solver.stats().Propagations;

  // One propagated literal is not enough to decide anything, and running out
  // has to read as "no answer" rather than as either answer.
  EXPECT_EQ(F.Solver.solve(Entry), SatResult::Unknown);
  EXPECT_LE(F.Solver.stats().Propagations - PropagationsBeforeSolve, 1u);
  EXPECT_FALSE(F.Solver.isFalsified());

  SatOptions Unbounded;
  F.Solver.setOptions(Unbounded);
  EXPECT_EQ(F.Solver.solve(Entry), SatResult::Sat);
  EXPECT_TRUE(F.modelSatisfiesEveryClause());
}

TEST(SatSolver, AWatchVisitBudgetBoundsOneLiteralWithManyWatchers) {
  SatOptions Opts;
  Opts.MaxWatchVisits = 17;

  Formula F(Opts);
  SatVar Entry = F.addVar();
  for (unsigned I = 0; I < 4096; ++I) {
    SatVar Consequence = F.addVar();
    F.add({SatLit::negative(Entry), SatLit::positive(Consequence)});
  }

  const SatLit Assumptions[] = {SatLit::positive(Entry)};
  uint64_t VisitsBeforeSolve = F.Solver.stats().WatchVisits;

  // Propagating Entry reaches every binary clause through one watch list.  A
  // literal propagation budget alone cannot bound that fanout, so watcher
  // visits have their own hard per-call limit.
  EXPECT_EQ(F.Solver.solve(Assumptions), SatResult::Unknown);
  EXPECT_EQ(F.Solver.stats().WatchVisits - VisitsBeforeSolve,
            Opts.MaxWatchVisits);
  EXPECT_FALSE(F.Solver.isFalsified());

  // Statistics accumulate, but every solve receives a fresh budget.
  VisitsBeforeSolve = F.Solver.stats().WatchVisits;
  EXPECT_EQ(F.Solver.solve(Assumptions), SatResult::Unknown);
  EXPECT_EQ(F.Solver.stats().WatchVisits - VisitsBeforeSolve,
            Opts.MaxWatchVisits);

  // Interrupting a list scan must neither lose nor corrupt its watchers.  An
  // unbounded retry therefore reaches a complete satisfying assignment.
  SatOptions Unbounded;
  F.Solver.setOptions(Unbounded);
  EXPECT_EQ(F.Solver.solve(Assumptions), SatResult::Sat);
  EXPECT_TRUE(F.modelSatisfiesEveryClause());
}

TEST(SatSolver, ARootWatchInterruptionReplaysTheCurrentTrailLiteral) {
  SatOptions Opts;
  Opts.MaxWatchVisits = 19;

  Formula F(Opts);
  SatVar A = F.addVar();
  SatVar B = F.addVar();

  // False-first branching initially assigns !A.  These two clauses then
  // contradict through B, so conflict analysis learns A at the root level.
  F.add({SatLit::positive(A), SatLit::positive(B)});
  F.add({SatLit::positive(A), SatLit::negative(B)});

  for (unsigned I = 0; I < 4096; ++I) {
    SatVar Consequence = F.addVar(/*Decision=*/false);
    F.add({SatLit::negative(A), SatLit::positive(Consequence)});
  }

  uint64_t VisitsBeforeSolve = F.Solver.stats().WatchVisits;
  EXPECT_EQ(F.Solver.solve(), SatResult::Unknown);
  EXPECT_EQ(F.Solver.stats().WatchVisits - VisitsBeforeSolve,
            Opts.MaxWatchVisits);
  ASSERT_GT(F.Solver.stats().Conflicts, 0u);

  uint64_t ConflictsBeforeRetry = F.Solver.stats().Conflicts;
  SatOptions Unbounded;
  F.Solver.setOptions(Unbounded);

  // cancelUntil(0) does not remove root assignments.  The interrupted trail
  // literal must therefore remain pending so this retry visits the untouched
  // suffix and propagates every non-decision consequence.
  EXPECT_EQ(F.Solver.solve(), SatResult::Sat);
  EXPECT_EQ(F.Solver.stats().Conflicts, ConflictsBeforeRetry);
  EXPECT_TRUE(F.modelSatisfiesEveryClause());
}

TEST(SatSolver, MaximumBudgetsDoNotWrapAccumulatedStatistics) {
  SatSolver S;
  SatVar A = S.newVar();
  SatVar B = S.newVar();

  // False-first branching tries !A.  These clauses then force both values of
  // B, so the satisfying run accumulates every kind of budgeted work before
  // learning that A must be true.
  ASSERT_TRUE(S.addClause(SatLit::positive(A), SatLit::positive(B)));
  ASSERT_TRUE(S.addClause(SatLit::positive(A), SatLit::negative(B)));
  ASSERT_EQ(S.solve(), SatResult::Sat);
  ASSERT_GT(S.stats().Conflicts, 0u);
  ASSERT_GT(S.stats().Propagations, 0u);
  ASSERT_GT(S.stats().WatchVisits, 0u);

  SatOptions Opts;
  Opts.MaxConflicts = std::numeric_limits<uint64_t>::max();
  Opts.MaxPropagations = std::numeric_limits<uint64_t>::max();
  Opts.MaxWatchVisits = std::numeric_limits<uint64_t>::max();
  S.setOptions(Opts);

  // A maximum per-call allowance is effectively unbounded from any reachable
  // counter value; computing its absolute endpoint must saturate, not wrap.
  EXPECT_EQ(S.solve(), SatResult::Sat);
}

TEST(SatSolver, ASolveBudgetDoesNotLimitLaterRootPropagation) {
  SatOptions Opts;
  Opts.MaxPropagations = 1;

  SatSolver S(Opts);
  SatVar A = S.newVar();
  SatVar B = S.newVar();
  ASSERT_TRUE(S.addClause(SatLit::negative(A), SatLit::positive(B)));
  ASSERT_EQ(S.solve(), SatResult::Unknown);

  ASSERT_TRUE(S.addClause(SatLit::positive(A)));
  EXPECT_FALSE(S.addClause(SatLit::negative(B)));
  EXPECT_TRUE(S.isFalsified());
}

TEST(SatSolver, TheSameFormulaIsSearchedTheSameWay) {
  auto Run = [](SatStats &Stats, std::vector<SatValue> &Model) {
    Formula F;
    addPigeonhole(F, /*Pigeons=*/5, /*Holes=*/4);
    std::vector<SatVar> Vars;
    addChain(F, 20, Vars);

    // Unsatisfiable overall, so the run is a full search rather than a lucky
    // first guess.
    EXPECT_EQ(F.Solver.solve(), SatResult::Unsat);
    Stats = F.Solver.stats();

    Model.clear();
    for (uint32_t V = 0; V < F.Solver.numVars(); ++V)
      Model.push_back(F.Solver.modelValue(V));
  };

  SatStats FirstStats;
  SatStats SecondStats;
  std::vector<SatValue> FirstModel;
  std::vector<SatValue> SecondModel;
  Run(FirstStats, FirstModel);
  Run(SecondStats, SecondModel);

  EXPECT_EQ(FirstStats.Decisions, SecondStats.Decisions);
  EXPECT_EQ(FirstStats.Conflicts, SecondStats.Conflicts);
  EXPECT_EQ(FirstStats.Propagations, SecondStats.Propagations);
  EXPECT_EQ(FirstStats.LearnedClauses, SecondStats.LearnedClauses);
  EXPECT_EQ(FirstModel, SecondModel);
}

TEST(SatSolver, LiteralsPackAndUnpack) {
  SatLit P = SatLit::positive(7);
  SatLit N = SatLit::negative(7);

  EXPECT_EQ(P.var(), 7u);
  EXPECT_EQ(N.var(), 7u);
  EXPECT_FALSE(P.isNegated());
  EXPECT_TRUE(N.isNegated());
  EXPECT_EQ(~P, N);
  EXPECT_EQ(~N, P);
  EXPECT_EQ(SatLit::fromIndex(P.index()), P);
  EXPECT_EQ(P.withPolarity(true), P);
  EXPECT_EQ(P.withPolarity(false), N);
  EXPECT_LT(SatLit::positive(1), SatLit::positive(2));
  EXPECT_FALSE(SatLit().isValid());
}

} // namespace
