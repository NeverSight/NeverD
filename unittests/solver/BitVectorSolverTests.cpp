//===- BitVectorSolverTests.cpp - Proofs, models and refutations ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercises the interface the rest of NeverD will call: proving two
/// expressions equal everywhere, finding an input that satisfies a constraint,
/// refuting one that nothing satisfies, and asking a series of related
/// questions without rebuilding the formula.
///
/// The identities proved here are the ones semantic simplification actually
/// needs.  A mixed boolean-arithmetic rewrite is exactly the case where
/// checking at a million random points proves nothing, because the obfuscated
/// and the short form are built to agree almost everywhere; only a decision
/// procedure can tell "agrees everywhere" from "has not disagreed yet".
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/solver/SatTypes.h"
#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymSynth.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

using namespace neverd::solver;
using neverd::symbolic::SymContext;
using neverd::symbolic::SymRef;
using neverd::symbolic::SynthOptions;
using neverd::symbolic::SynthVerification;

namespace {

constexpr uint32_t W8 = 8;
constexpr uint32_t W16 = 16;
constexpr uint32_t W32 = 32;

static_assert(static_cast<uint8_t>(SatResult::Invalid) == 3);
static_assert(static_cast<uint8_t>(EquivResult::Invalid) == 3);
static_assert(static_cast<uint8_t>(ProofStatus::Invalid) == 4);

TEST(BitVectorSolver, ProvesMixedBooleanArithmeticIdentities) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  // The identity an obfuscator runs backwards to hide an exclusive or.
  EXPECT_TRUE(proveEqual(Ctx, Ctx.mkXor(X, Y),
                         Ctx.mkSub(Ctx.mkOr(X, Y), Ctx.mkAnd(X, Y))));

  // ...and the one it runs backwards to hide an addition.
  EXPECT_TRUE(
      proveEqual(Ctx, Ctx.mkAdd(X, Y),
                 Ctx.mkAdd(Ctx.mkXor(X, Y),
                           Ctx.mkMul(Ctx.mkConst(W32, 2), Ctx.mkAnd(X, Y)))));

  // Subtraction through the bitwise algebra.
  EXPECT_TRUE(proveEqual(
      Ctx, Ctx.mkSub(X, Y),
      Ctx.mkSub(Ctx.mkXor(X, Y),
                Ctx.mkMul(Ctx.mkConst(W32, 2), Ctx.mkAnd(Ctx.mkNot(X), Y)))));

  // A near miss must not be provable, and the counterexample has to be an
  // input that really tells the two apart.  `(x|y) + (x&y)` is `x + y`, which
  // agrees with the exclusive or on every input whose operands share no bit —
  // the shape of near miss that sampling is worst at catching.
  SymRef Left = Ctx.mkXor(X, Y);
  SymRef Right = Ctx.mkAdd(Ctx.mkOr(X, Y), Ctx.mkAnd(X, Y));
  BitVectorModel Counterexample;
  ASSERT_EQ(checkEqual(Ctx, Left, Right, &Counterexample),
            EquivResult::Different);

  std::vector<llvm::APInt> Values = Counterexample.asVarValues(Ctx);
  EXPECT_NE(Ctx.eval(Left, Values), Ctx.eval(Right, Values));
}

TEST(BitVectorSolver, ProvesDeMorgansLaws) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  EXPECT_TRUE(proveEqual(Ctx, Ctx.mkNot(Ctx.mkAnd(X, Y)),
                         Ctx.mkOr(Ctx.mkNot(X), Ctx.mkNot(Y))));
  EXPECT_TRUE(proveEqual(Ctx, Ctx.mkNot(Ctx.mkOr(X, Y)),
                         Ctx.mkAnd(Ctx.mkNot(X), Ctx.mkNot(Y))));
}

TEST(BitVectorSolver, ProvesThatDoublingIsAShift) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Doubled = Ctx.mkAdd(X, X);

  EXPECT_TRUE(proveEqual(Ctx, Doubled, Ctx.mkShl(X, Ctx.mkConst(W32, 1))));

  // Spelling the shift structurally puts it beyond what the expression
  // builders normalise, so this form is decided by the circuit rather than by
  // the two sides interning to one node.
  SymRef Shifted = Ctx.mkConcat(Ctx.mkExtract(X, 0, W32 - 1), Ctx.mkZero(1));
  EXPECT_NE(Doubled, Shifted);
  EXPECT_TRUE(proveEqual(Ctx, Doubled, Shifted));
}

TEST(BitVectorSolver, ProvesIdentitiesInvolvingDivision) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W8);

  EXPECT_TRUE(proveEqual(Ctx, Ctx.mkUDiv(X, Ctx.mkOne(W8)), X));
  EXPECT_TRUE(proveEqual(Ctx, Ctx.mkURem(X, Ctx.mkOne(W8)), Ctx.mkZero(W8)));

  // Dividing by a power of two is a logical shift, which is the rewrite a
  // decompiler makes on sight and should therefore be able to justify.
  EXPECT_TRUE(proveEqual(Ctx, Ctx.mkUDiv(X, Ctx.mkConst(W8, 4)),
                         Ctx.mkLShr(X, Ctx.mkConst(W8, 2))));

  // The signed version of that rewrite is wrong — division truncates towards
  // zero and an arithmetic shift rounds towards minus infinity — and being
  // told so with a counterexample is exactly what the procedure is for.
  EXPECT_EQ(checkEqual(Ctx, Ctx.mkSDiv(X, Ctx.mkConst(W8, 4)),
                       Ctx.mkAShr(X, Ctx.mkConst(W8, 2))),
            EquivResult::Different);
}

TEST(BitVectorSolver, FindsAModelForASatisfiableConstraint) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W8);

  // Three is invertible modulo two hundred and fifty six, so seven is the only
  // answer and the model can be checked exactly rather than only validated.
  SymRef Constraint =
      Ctx.mkEq(Ctx.mkMul(Ctx.mkConst(W8, 3), X), Ctx.mkConst(W8, 21));

  BitVectorModel Model;
  ASSERT_EQ(checkSat(Ctx, Constraint, &Model), SatResult::Sat);

  std::optional<llvm::APInt> Value = Model.value(Ctx, X);
  ASSERT_TRUE(Value.has_value());
  EXPECT_EQ(Value->getZExtValue(), 7u);
  EXPECT_TRUE(Ctx.eval(Constraint, Model.asVarValues(Ctx)).isOne());
}

TEST(BitVectorSolver, AModelSatisfiesThePathConditionItCameFrom) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W16);
  SymRef Y = Ctx.mkVar("y", W16);

  const SymRef Conjuncts[] = {
      Ctx.mkUlt(X, Ctx.mkConst(W16, 1000)),
      Ctx.mkUgt(Y, Ctx.mkConst(W16, 2000)),
      Ctx.mkEq(Ctx.mkAdd(X, Y), Ctx.mkConst(W16, 2500)),
  };
  SymRef Path = Ctx.mkAnd(Conjuncts);

  BitVectorModel Model;
  ASSERT_EQ(checkSat(Ctx, Path, &Model), SatResult::Sat);
  EXPECT_TRUE(Ctx.eval(Path, Model.asVarValues(Ctx)).isOne());
}

TEST(BitVectorSolver, RefutesConstraintsNothingSatisfies) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W8);
  SymRef Y = Ctx.mkVar("y", W8);

  // An even value is never one, whatever the width.
  EXPECT_EQ(checkSat(Ctx, Ctx.mkEq(Ctx.mkMul(Ctx.mkConst(W8, 2), X),
                                   Ctx.mkConst(W8, 1))),
            SatResult::Unsat);

  // Two values cannot each be below the other.
  EXPECT_EQ(checkSat(Ctx, Ctx.mkAnd(Ctx.mkUlt(X, Y), Ctx.mkUlt(Y, X))),
            SatResult::Unsat);

  // A remainder is always below its divisor when the divisor is not zero.
  const SymRef Impossible[] = {
      Ctx.mkUgt(Y, Ctx.mkZero(W8)),
      Ctx.mkUge(Ctx.mkURem(X, Y), Y),
  };
  EXPECT_EQ(checkSat(Ctx, Ctx.mkAnd(Impossible)), SatResult::Unsat);
}

TEST(BitVectorSolver, AssumptionsAskOneQuestionAtATime) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W8);

  BitVectorSolver Solver(Ctx);
  ASSERT_TRUE(Solver.assertTrue(Ctx.mkUlt(X, Ctx.mkConst(W8, 10))));
  ASSERT_EQ(Solver.check(), SatResult::Sat);

  SymRef TooLarge = Ctx.mkUgt(X, Ctx.mkConst(W8, 20));
  const SymRef Assumptions[] = {TooLarge};
  EXPECT_EQ(Solver.check(Assumptions), SatResult::Unsat);
  ASSERT_EQ(Solver.failedAssumptions().size(), 1u);
  EXPECT_EQ(Solver.failedAssumptions()[0], TooLarge);

  // The assumption is gone again, and what was asserted is not.
  EXPECT_EQ(Solver.check(), SatResult::Sat);
  std::optional<llvm::APInt> Value = Solver.model().value(Ctx, X);
  ASSERT_TRUE(Value.has_value());
  EXPECT_LT(Value->getZExtValue(), 10u);

  // Narrowing further still works, and everything learned so far is kept.
  const SymRef Narrower[] = {Ctx.mkUgt(X, Ctx.mkConst(W8, 7))};
  EXPECT_EQ(Solver.check(Narrower), SatResult::Sat);
  Value = Solver.model().value(Ctx, X);
  ASSERT_TRUE(Value.has_value());
  EXPECT_GT(Value->getZExtValue(), 7u);
  EXPECT_LT(Value->getZExtValue(), 10u);
}

TEST(BitVectorSolver, AssertionsAccumulateUntilTheyCannotHold) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W8);

  BitVectorSolver Solver(Ctx);
  ASSERT_TRUE(Solver.assertTrue(Ctx.mkUgt(X, Ctx.mkConst(W8, 100))));
  EXPECT_EQ(Solver.check(), SatResult::Sat);

  ASSERT_TRUE(Solver.assertTrue(Ctx.mkUlt(X, Ctx.mkConst(W8, 200))));
  EXPECT_EQ(Solver.check(), SatResult::Sat);

  ASSERT_TRUE(
      Solver.assertDistinct(Ctx.mkAnd(X, Ctx.mkConst(W8, 1)), Ctx.mkZero(W8)));
  EXPECT_EQ(Solver.check(), SatResult::Sat);
  std::optional<llvm::APInt> Value = Solver.model().value(Ctx, X);
  ASSERT_TRUE(Value.has_value());
  EXPECT_GT(Value->getZExtValue(), 100u);
  EXPECT_LT(Value->getZExtValue(), 200u);
  EXPECT_EQ(Value->getZExtValue() & 1u, 1u);

  ASSERT_TRUE(Solver.assertTrue(Ctx.mkUlt(X, Ctx.mkConst(W8, 50))));
  EXPECT_EQ(Solver.check(), SatResult::Unsat);
}

TEST(BitVectorSolver, InterningAlreadyProvesSomeEqualities) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  // Nothing is encoded for these: the canonicalising builders reduced both
  // spellings to one node, and that is a proof on its own.
  EXPECT_EQ(checkEqual(Ctx, Ctx.mkAdd(X, Y), Ctx.mkAdd(Y, X)),
            EquivResult::Equal);
  EXPECT_TRUE(proveEqual(Ctx, Ctx.mkXor(X, Ctx.mkXor(X, Y)), Y));
}

TEST(BitVectorSolver, MalformedPredicatesAreInvalidQueries) {
  SymContext Ctx;
  EXPECT_EQ(checkSat(Ctx, SymRef()), SatResult::Invalid);
  EXPECT_EQ(checkEqual(Ctx, SymRef(), SymRef()), EquivResult::Invalid);
  EXPECT_STREQ(equivResultName(EquivResult::Invalid), "invalid");

  SymRef OutOfRange(static_cast<uint32_t>(Ctx.numNodes()));
  EXPECT_EQ(checkSat(Ctx, OutOfRange), SatResult::Invalid);
}

TEST(BitVectorSolver, WidthMismatchesAreInvalidRatherThanInconclusive) {
  SymContext Ctx;
  SymRef Byte = Ctx.mkVar("byte", W8);
  SymRef Word = Ctx.mkVar("word", W16);

  EXPECT_EQ(checkEqual(Ctx, Byte, Word), EquivResult::Invalid);
  EXPECT_FALSE(proveEqual(Ctx, Byte, Word));
}

TEST(BitVectorSolver, MalformedAssertionsPoisonAnIncrementalQuery) {
  SymContext Ctx;
  SymRef Byte = Ctx.mkVar("byte", W8);
  SymRef Word = Ctx.mkVar("word", W16);
  BitVectorSolver Solver(Ctx);

  EXPECT_FALSE(Solver.assertEqual(Byte, Word));
  EXPECT_FALSE(Solver.ok());
  EXPECT_EQ(Solver.encodeError(), BlastError::Malformed);
  EXPECT_EQ(Solver.check(), SatResult::Invalid);

  BitVectorSolver AssumptionSolver(Ctx);
  const SymRef InvalidAssumptions[] = {SymRef()};
  EXPECT_EQ(AssumptionSolver.check(InvalidAssumptions), SatResult::Invalid);
  EXPECT_EQ(AssumptionSolver.encodeError(), BlastError::Malformed);
}

TEST(BitVectorSolver, EncodingLimitsRemainRetryableUnknownResults) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  SolverOptions Limited;
  Limited.Blast.MaxWidth = W8;

  EXPECT_EQ(checkSat(Ctx, Ctx.mkUlt(X, Y), nullptr, Limited),
            SatResult::Unknown);
  EXPECT_EQ(checkEqual(Ctx, X, Y, nullptr, Limited), EquivResult::Unknown);

  BitVectorSolver Solver(Ctx, Limited);
  EXPECT_FALSE(Solver.assertDistinct(X, Y));
  EXPECT_EQ(Solver.encodeError(), BlastError::WidthTooLarge);
  EXPECT_EQ(Solver.check(), SatResult::Unknown);
}

TEST(BitVectorSolver, ZeroBlastLimitsRemoveCallerPolicyCeilings) {
  // 257 is one bit beyond the bounded production default.  The expression is
  // intentionally cheap to encode: this tests removal of a caller policy, not
  // an attempt to claim that arbitrary-width circuits need no physical
  // memory.
  constexpr uint32_t Width = 257;
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", Width);
  SymRef IsZero = Ctx.mkEq(X, Ctx.mkZero(Width));

  EXPECT_EQ(checkSat(Ctx, IsZero), SatResult::Unknown);

  const SolverOptions Unlimited = SolverOptions::unlimited();
  EXPECT_EQ(Unlimited.Blast.MaxWidth, 0u);
  EXPECT_EQ(Unlimited.Blast.MaxGates, 0u);
  EXPECT_EQ(Unlimited.Sat.MaxConflicts, 0u);
  EXPECT_EQ(Unlimited.Sat.MaxPropagations, 0u);
  EXPECT_EQ(Unlimited.Sat.MaxWatchVisits, 0u);
  EXPECT_EQ(checkSat(Ctx, IsZero, nullptr, Unlimited), SatResult::Sat);

  SolverOptions GateLimited = Unlimited;
  GateLimited.Blast.MaxGates = 1;
  BitVectorSolver Budgeted(Ctx, GateLimited);
  SymRef CarryHeavy =
      Ctx.mkEq(Ctx.mkAdd(X, Ctx.mkOne(Width)), Ctx.mkZero(Width));
  EXPECT_FALSE(Budgeted.assertTrue(CarryHeavy));
  EXPECT_EQ(Budgeted.encodeError(), BlastError::TooManyGates);
  EXPECT_EQ(Budgeted.check(), SatResult::Unknown);

  // Removing a resource policy must not turn a malformed request into a
  // retryable resource answer.
  EXPECT_EQ(checkSat(Ctx, SymRef(), nullptr, Unlimited), SatResult::Invalid);
}

TEST(BitVectorSolver, NoAnswerIsNotAProof) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  SymRef Left = Ctx.mkNot(Ctx.mkAnd(X, Y));
  SymRef Right = Ctx.mkOr(Ctx.mkNot(X), Ctx.mkNot(Y));
  ASSERT_TRUE(proveEqual(Ctx, Left, Right));

  // The same identity, asked with a limit that cannot encode it.  A caller
  // about to rewrite must be told no, not told the identity failed.
  SolverOptions Cramped;
  Cramped.Blast.MaxWidth = 4;
  EXPECT_EQ(checkEqual(Ctx, Left, Right, nullptr, Cramped),
            EquivResult::Unknown);
  EXPECT_FALSE(proveEqual(Ctx, Left, Right, Cramped));

  // A budget that stops the search reads the same way.
  SolverOptions Impatient;
  Impatient.Sat.MaxPropagations = 1;
  EXPECT_EQ(checkSat(Ctx, Ctx.mkUlt(X, Y), nullptr, Impatient),
            SatResult::Unknown);
}

TEST(BitVectorSolver, SuppliesTypedProofsToProductionSynthesis) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Input = Ctx.mkAdd(
      Ctx.mkLShr(X, Ctx.mkConst(W32, 4)),
      Ctx.mkLShr(Ctx.mkLShr(X, Ctx.mkConst(W32, 2)), Ctx.mkConst(W32, 2)));

  SynthOptions Opts;
  Opts.MaxCost = 5;
  Opts.MaxSamples = 48;
  Opts.VerifySamples = 256;
  Opts.MaxWork = size_t(1) << 18;
  Opts.UseStochasticFallback = false;

  SolverOptions SolverOpts;
  BitVectorModel Counterexample;
  auto Verify = [&](SymContext &VerifyCtx, SymRef A, SymRef B) {
    switch (checkEqual(VerifyCtx, A, B, &Counterexample, SolverOpts)) {
    case EquivResult::Equal:
      return SynthVerification::Equivalent;
    case EquivResult::Different:
      return SynthVerification::Different;
    case EquivResult::Unknown:
      return SynthVerification::Unknown;
    case EquivResult::Invalid:
      return SynthVerification::Unknown;
    }
    llvm_unreachable("unhandled equivalence result");
  };

  SymRef Got = neverd::symbolic::synthesizeEquivalent(Ctx, Input, Opts, Verify);
  EXPECT_NE(Got, Input);
  EXPECT_EQ(checkEqual(Ctx, Input, Got), EquivResult::Equal);

  SymRef Different = Ctx.mkAdd(X, Ctx.mkOne(W32));
  ASSERT_EQ(Verify(Ctx, X, Different), SynthVerification::Different);
  std::vector<llvm::APInt> Values = Counterexample.asVarValues(Ctx);
  EXPECT_NE(Ctx.eval(X, Values), Ctx.eval(Different, Values));
}

TEST(BitVectorSolver, SynthesisVerifierMapsProofsAndOwnsTypedModels) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  SymRef Sum = Ctx.mkAdd(X, Y);
  SymRef CarrySum = Ctx.mkAdd(Ctx.mkXor(X, Y),
                              Ctx.mkMul(Ctx.mkConst(W32, 2), Ctx.mkAnd(X, Y)));
  SymRef NearMiss = Ctx.mkXor(X, Y);

  SymSynthVerifier Verify;
  EXPECT_EQ(Verify(Ctx, Sum, CarrySum), SynthVerification::Equivalent);
  EXPECT_EQ(Verify.report().Proof, ProofStatus::Equivalent);
  EXPECT_FALSE(Verify.report().Counterexample.has_value());
  ProofStats EquivalentWork = Verify.report().Stats;

  ASSERT_EQ(Verify(Ctx, Sum, NearMiss), SynthVerification::Different);
  const SymSynthProofReport &Report = Verify.report();
  EXPECT_EQ(Report.Proof, ProofStatus::Different);
  EXPECT_EQ(Report.Stats.Queries, 2u);
  EXPECT_EQ(Report.RejectedCandidate, NearMiss);
  ASSERT_TRUE(Report.Counterexample.has_value());

  const BitVectorModel &Model = *Report.Counterexample;
  ASSERT_FALSE(Model.empty());
  EXPECT_TRUE(std::is_sorted(Model.vars().begin(), Model.vars().end()));
  for (uint32_t Id : Model.vars()) {
    std::optional<llvm::APInt> Value = Model.value(Id);
    ASSERT_TRUE(Value.has_value());
    EXPECT_EQ(Value->getBitWidth(), Ctx.varInfo(Id).Width);
  }

  std::vector<llvm::APInt> Values = Model.asVarValues(Ctx);
  EXPECT_NE(Ctx.eval(Sum, Values), Ctx.eval(NearMiss, Values));

  SymSynthVerifier DifferentOnly;
  ASSERT_EQ(DifferentOnly(Ctx, Sum, NearMiss), SynthVerification::Different);
  const ProofStats &DifferentWork = DifferentOnly.report().Stats;
  EXPECT_EQ(Report.Stats.Conflicts,
            EquivalentWork.Conflicts + DifferentWork.Conflicts);
  EXPECT_EQ(Report.Stats.Propagations,
            EquivalentWork.Propagations + DifferentWork.Propagations);
  EXPECT_EQ(Report.Stats.WatchVisits,
            EquivalentWork.WatchVisits + DifferentWork.WatchVisits);
}

TEST(BitVectorSolver, SynthesisVerifierKeepsOnlyARelevantRefutation) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef Narrow = Ctx.mkVar("narrow", W16);
  SymRef Sum = Ctx.mkAdd(X, Y);
  SymRef FirstCandidate = Ctx.mkXor(X, Y);
  SymRef SecondCandidate = Ctx.mkAnd(X, Y);

  SymSynthVerifier Verify;
  ASSERT_EQ(Verify(Ctx, Sum, FirstCandidate), SynthVerification::Different);
  ASSERT_TRUE(Verify.report().Counterexample.has_value());
  std::vector<llvm::APInt> FirstValues =
      Verify.report().Counterexample->asVarValues(Ctx);

  ASSERT_EQ(Verify(Ctx, Sum, SecondCandidate), SynthVerification::Different);
  EXPECT_EQ(Verify.report().RejectedCandidate, FirstCandidate);
  ASSERT_TRUE(Verify.report().Counterexample.has_value());
  std::vector<llvm::APInt> RetainedValues =
      Verify.report().Counterexample->asVarValues(Ctx);
  ASSERT_EQ(RetainedValues.size(), FirstValues.size());
  for (size_t I = 0; I < FirstValues.size(); ++I)
    EXPECT_EQ(RetainedValues[I], FirstValues[I]);
  EXPECT_NE(Ctx.eval(Sum, FirstValues), Ctx.eval(FirstCandidate, FirstValues));

  EXPECT_EQ(Verify(Ctx, Sum, Sum), SynthVerification::Equivalent);
  EXPECT_EQ(Verify.report().Proof, ProofStatus::Equivalent);
  EXPECT_FALSE(Verify.report().RejectedCandidate.isValid());
  EXPECT_FALSE(Verify.report().Counterexample.has_value());

  ASSERT_EQ(Verify(Ctx, Sum, FirstCandidate), SynthVerification::Different);
  ASSERT_TRUE(Verify.report().Counterexample.has_value());
  EXPECT_EQ(Verify(Ctx, Sum, Narrow), SynthVerification::Unknown);
  EXPECT_EQ(Verify.report().Proof, ProofStatus::Invalid);
  EXPECT_FALSE(Verify.report().RejectedCandidate.isValid());
  EXPECT_FALSE(Verify.report().Counterexample.has_value());

  EXPECT_EQ(Verify(Ctx, Sum, SymRef()), SynthVerification::Unknown);
  EXPECT_EQ(Verify.report().Proof, ProofStatus::Invalid);
}

TEST(BitVectorSolver, SynthesisVerifierReportsDeterministicBudgetUnknown) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef Sum = Ctx.mkAdd(X, Y);
  SymRef CarrySum = Ctx.mkAdd(Ctx.mkXor(X, Y),
                              Ctx.mkMul(Ctx.mkConst(W32, 2), Ctx.mkAnd(X, Y)));

  SymSynthVerifier Unbounded;
  ASSERT_EQ(Unbounded(Ctx, CarrySum, Sum), SynthVerification::Equivalent);
  ASSERT_GT(Unbounded.report().Stats.WatchVisits, 1u);

  SolverOptions LimitedOptions;
  LimitedOptions.Sat.MaxWatchVisits = 1;
  SymSynthVerifier Limited(LimitedOptions);
  EXPECT_EQ(Limited(Ctx, CarrySum, Sum), SynthVerification::Unknown);
  EXPECT_EQ(Limited.report().Proof, ProofStatus::Unknown);
  EXPECT_EQ(Limited.report().Stats.Queries, 1u);
  EXPECT_EQ(Limited.report().Stats.WatchVisits, 1u);
  EXPECT_FALSE(Limited.report().RejectedCandidate.isValid());
  EXPECT_FALSE(Limited.report().Counterexample.has_value());
}

TEST(BitVectorSolver, ModelsReportOnlyWhatTheFormulaMentioned) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W8);
  SymRef Unused = Ctx.mkVar("unused", W8);

  BitVectorModel Model;
  ASSERT_EQ(checkSat(Ctx, Ctx.mkUgt(X, Ctx.mkConst(W8, 3)), &Model),
            SatResult::Sat);

  EXPECT_TRUE(Model.contains(Ctx.varId(X)));
  EXPECT_FALSE(Model.contains(Ctx.varId(Unused)));
  EXPECT_FALSE(Model.value(Ctx.varId(Unused)).has_value());

  // Evaluating still needs a value for every variable, so the dense view fills
  // the unconstrained ones in rather than leaving a hole.
  std::vector<llvm::APInt> Values = Model.asVarValues(Ctx);
  ASSERT_EQ(Values.size(), Ctx.numVars());
  EXPECT_EQ(Values[Ctx.varId(Unused)].getBitWidth(), W8);
}

} // namespace
