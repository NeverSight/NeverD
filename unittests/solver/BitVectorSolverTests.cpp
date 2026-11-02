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

#include "PermanentConjuncts.h"
#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/solver/CnfEncoder.h"
#include "neverd/solver/SatTypes.h"
#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymSynth.h"
#include "neverd/symbolic/SymWideArithmetic.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
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

TEST(BitVectorSolver, SparseLateInputsKeepIncrementalModelsAndAssumptions) {
  SymContext Ctx;
  const auto Unused = Ctx.mkFreshVar(8, "unused");
  for (unsigned I = 0; I != 65536; ++I) {
    Ctx.mkConst(64, uint64_t(1) << 32 | I);
    Ctx.mkFreshVar(8, "unrelated");
  }
  const auto X = Ctx.mkFreshVar(8, "x"), Y = Ctx.mkFreshVar(8, "y");
  const auto Sum = Ctx.mkAdd(X, Ctx.mkMul(Y, Ctx.mkConst(8, 3)));
  BitVectorSolver Solver(Ctx);
  ASSERT_TRUE(Solver.assertEqual(Sum, Ctx.mkConst(8, 91)));
  const auto First = Ctx.mkEq(X, Ctx.mkConst(8, 4));
  ASSERT_EQ(Solver.check({First}), SatResult::Sat);
  ASSERT_TRUE(Solver.model().value(Ctx, X));
  ASSERT_TRUE(Solver.model().value(Ctx, Y));
  EXPECT_EQ(Solver.model().value(Ctx, X)->getZExtValue(), 4U);
  EXPECT_EQ(Solver.model().value(Ctx, Y)->getZExtValue(), 29U);
  EXPECT_FALSE(Solver.model().value(Ctx, Unused));
  EXPECT_EQ(Solver.blaster().encodedVars().size(), 2U);

  for (unsigned I = 0; I != 65536; ++I)
    Ctx.mkFreshVar(8, "later_unrelated");
  const auto Z = Ctx.mkFreshVar(8, "z");
  ASSERT_TRUE(Solver.assertEqual(Z, Ctx.mkXor(X, Y)));
  const auto Second = Ctx.mkEq(X, Ctx.mkConst(8, 7));
  ASSERT_EQ(Solver.check({Second}), SatResult::Sat);
  ASSERT_TRUE(Solver.model().value(Ctx, X));
  ASSERT_TRUE(Solver.model().value(Ctx, Y));
  ASSERT_TRUE(Solver.model().value(Ctx, Z));
  EXPECT_EQ(Solver.model().value(Ctx, X)->getZExtValue(), 7U);
  EXPECT_EQ(Solver.model().value(Ctx, Y)->getZExtValue(), 28U);
  EXPECT_EQ(Solver.model().value(Ctx, Z)->getZExtValue(), 27U);
  EXPECT_FALSE(Solver.model().value(Ctx, Unused));
  EXPECT_EQ(Solver.blaster().encodedVars().size(), 3U);
  EXPECT_EQ(Solver.check({First, Second}), SatResult::Unsat);
  ASSERT_EQ(Solver.check({First}), SatResult::Sat);
  ASSERT_TRUE(Solver.model().value(Ctx, Z));
  EXPECT_EQ(Solver.model().value(Ctx, Z)->getZExtValue(), 25U);
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

TEST(BitVectorSolver, PartialCounterUpdatesPreserveWideBounds) {
  for (uint32_t Width : {16u, 32u, 64u, 65u, 128u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto Counter = Ctx.mkVar("counter", Width);
    const auto Bound = Ctx.mkVar("bound", Width);
    const auto Low = Ctx.mkExtract(Counter, 0, 8);
    const auto Updated = Ctx.mkConcat(Ctx.mkExtract(Counter, 8, Width - 8),
                                      Ctx.mkAdd(Low, Ctx.mkOne(8)));
    const auto Before = Ctx.mkUle(Counter, Bound);
    const auto Exit = Ctx.mkEq(Low, Ctx.mkExtract(Bound, 0, 8));
    const auto Bad = Ctx.mkNot(Ctx.mkUle(Updated, Bound));
    const auto Query = Ctx.mkAnd(Ctx.mkAnd(Before, Ctx.mkNot(Exit)), Bad);
    SolverOptions Limits;
    Limits.Blast.MaxGates = 262144;
    Limits.Sat.MaxConflicts = 10000;
    Limits.Sat.MaxPropagations = 1000000;
    Limits.Sat.MaxWatchVisits = 10000000;
    EXPECT_EQ(checkSat(Ctx, Query, nullptr, Limits), SatResult::Unsat);

    // Without the exit guard an equal counter can advance past its bound.
    const auto Unguarded = Ctx.mkAnd(Before, Bad);
    BitVectorModel Model;
    ASSERT_EQ(checkSat(Ctx, Unguarded, &Model, Limits), SatResult::Sat);
    EXPECT_TRUE(Ctx.eval(Unguarded, Model.asVarValues(Ctx)).isOne());
    Limits.Blast.MaxGates = 1;
    EXPECT_EQ(checkSat(Ctx, Query, nullptr, Limits), SatResult::Unknown);
  }
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

TEST(BitVectorSolver, SynthesisVerifierRefutationsBelongToTheCurrentContext) {
  for (ProofBackend Backend : {ProofBackend::BuiltIn, ProofBackend::Z3}) {
    if (!proofBackendAvailable(Backend))
      continue;
    SCOPED_TRACE(static_cast<unsigned>(Backend));
    SymContext First;
    const SymRef X = First.mkVar("x", 8);
    const SymRef FirstCandidate = First.mkZero(8);
    SymContext Second;
    Second.mkVar("unrelated", 8);
    const SymRef Y = Second.mkVar("y", 16);
    const SymRef SecondCandidate = Second.mkConst(16, 7);

    SymSynthVerifier Verify({}, Backend);
    ASSERT_EQ(Verify(First, X, FirstCandidate), SynthVerification::Different);
    ASSERT_EQ(Verify(Second, Y, SecondCandidate), SynthVerification::Different);
    const auto &Report = Verify.report();
    EXPECT_EQ(Report.Stats.Queries, 2u);
    EXPECT_EQ(Report.RejectedCandidate, SecondCandidate);
    ASSERT_TRUE(Report.Counterexample.has_value());
    ASSERT_TRUE(Report.Counterexample->contains(Second.varId(Y)));
    EXPECT_EQ(Report.Counterexample->value(Second.varId(Y))->getBitWidth(),
              16u);
    const auto Values = Report.Counterexample->asVarValues(Second);
    EXPECT_NE(Second.eval(Y, Values), Second.eval(SecondCandidate, Values));

    // Further candidates in this context keep its first refutation.
    ASSERT_EQ(Verify(Second, Y, Second.mkOne(16)),
              SynthVerification::Different);
    EXPECT_EQ(Report.RejectedCandidate, SecondCandidate);
    EXPECT_EQ(Report.Stats.Queries, 3u);
    ASSERT_EQ(Verify(First, X, FirstCandidate), SynthVerification::Different);
    EXPECT_EQ(Report.RejectedCandidate, FirstCandidate);
    ASSERT_TRUE(Report.Counterexample.has_value());
    EXPECT_EQ(Report.Counterexample->value(First.varId(X))->getBitWidth(), 8u);
    EXPECT_EQ(Report.Stats.Queries, 4u);
  }
}

TEST(BitVectorSolver, RecoversProvedSplitWordArithmetic) {
  using namespace neverd::symbolic;
  for (uint32_t Width : {8u, 16u, 32u}) {
    SymContext Ctx;
    SymRef AL = Ctx.mkVar("al", Width);
    SymRef AH = Ctx.mkVar("ah", Width);
    SymRef BL = Ctx.mkVar("bl", Width);
    SymRef BH = Ctx.mkVar("bh", Width);
    SymRef Parity = Ctx.mkXor(AL, BL);
    SymRef Both = Ctx.mkAnd(AL, BL);
    SymRef Low = Ctx.mkAdd(Parity, Ctx.mkShl(Both, Ctx.mkOne(Width)));
    SymRef High =
        Ctx.mkAdd({Ctx.mkXor(AH, BH),
                   Ctx.mkOr(Ctx.mkShl(Ctx.mkAnd(AH, BH), Ctx.mkOne(Width)),
                            Ctx.mkLShr(Both, Ctx.mkConst(Width, Width - 1))),
                   Ctx.mkZExt(Ctx.mkUlt(Low, Parity), Width)});
    SymRef Original = Ctx.mkConcat(High, Low);
    SymRef Expected = Ctx.mkAdd(Ctx.mkConcat(AH, AL), Ctx.mkConcat(BH, BL));
    SymSynthVerifier Verifier;
    auto Verify = [&](SymContext &Context, SymRef A, SymRef B) {
      return Verifier(Context, A, B);
    };
    EXPECT_EQ(recoverSplitWordArithmetic(Ctx, Original, Verify), Expected);
    EXPECT_EQ(Verifier.report().Proof, ProofStatus::Equivalent);

    SymRef NotBL = Ctx.mkNot(BL);
    SymRef SubLow = Ctx.mkAdd(
        {Ctx.mkXor(AL, NotBL),
         Ctx.mkShl(Ctx.mkAnd(AL, NotBL), Ctx.mkOne(Width)), Ctx.mkOne(Width)});
    SymRef SubHigh =
        Ctx.mkSub(Ctx.mkSub(AH, BH), Ctx.mkZExt(Ctx.mkUlt(AL, BL), Width));
    SymRef SubOriginal = Ctx.mkConcat(SubHigh, SubLow);
    SymRef SubExpected = Ctx.mkSub(Ctx.mkConcat(AH, AL), Ctx.mkConcat(BH, BL));
    EXPECT_EQ(recoverSplitWordArithmetic(Ctx, SubOriginal, Verify),
              SubExpected);
    EXPECT_EQ(Verifier.report().Proof, ProofStatus::Equivalent);

    SymRef OffsetLow = Ctx.mkConst(Width, 0xf0);
    SymRef OffsetHigh = Ctx.mkConst(Width, 0x56);
    SymRef Offset = Ctx.mkConcat(OffsetHigh, OffsetLow);
    auto BiasResult = [&](SymRef Upper, SymRef Lower) {
      SymRef BiasedLow = Ctx.mkAdd(Lower, OffsetLow);
      SymRef Carry = Ctx.mkZExt(Ctx.mkUlt(BiasedLow, Lower), Width);
      return Ctx.mkConcat(Ctx.mkAdd({Upper, OffsetHigh, Carry}), BiasedLow);
    };
    SymRef AffineAdd = BiasResult(High, Low);
    SymRef AffineAddExpected = Ctx.mkAdd(Expected, Offset);
    EXPECT_EQ(recoverSplitWordArithmetic(Ctx, AffineAdd, Verify),
              AffineAddExpected);
    EXPECT_EQ(Verifier.report().Proof, ProofStatus::Equivalent);
    SymRef AffineSub = BiasResult(SubHigh, SubLow);
    SymRef AffineSubExpected = Ctx.mkAdd(SubExpected, Offset);
    EXPECT_EQ(recoverSplitWordArithmetic(Ctx, AffineSub, Verify),
              AffineSubExpected);
    EXPECT_EQ(Verifier.report().Proof, ProofStatus::Equivalent);

    SymRef Rare = Ctx.mkAnd(Ctx.mkEq(AL, Ctx.mkConst(Width, 0x6d)),
                            Ctx.mkEq(AH, Ctx.mkConst(Width, 0x29)));
    SymRef Wrong = Ctx.mkConcat(
        Ctx.mkIte(Rare, Ctx.mkAdd(High, Ctx.mkOne(Width)), High), Low);
    EXPECT_EQ(recoverSplitWordArithmetic(Ctx, Wrong, Verify), Wrong);
    EXPECT_EQ(Verifier.report().Proof, ProofStatus::Different);
    EXPECT_EQ(recoverSplitWordArithmetic(Ctx, Original,
                                         [](SymContext &, SymRef, SymRef) {
                                           return SynthVerification::Unknown;
                                         }),
              Original);
  }
}

TEST(BitVectorSolver, RecoversProvedThreeInputSplitWordArithmetic) {
  using namespace neverd::symbolic;
  for (uint32_t Width : {8u, 16u, 32u}) {
    SymContext Ctx;
    SymRef AL = Ctx.mkVar("al", Width);
    SymRef AH = Ctx.mkVar("ah", Width);
    SymRef BL = Ctx.mkVar("bl", Width);
    SymRef BH = Ctx.mkVar("bh", Width);
    SymRef CL = Ctx.mkVar("cl", Width);
    SymRef CH = Ctx.mkVar("ch", Width);
    auto Majority = [&](SymRef A, SymRef B, SymRef C) {
      return Ctx.mkOr(Ctx.mkAnd(A, B),
                      Ctx.mkOr(Ctx.mkAnd(A, C), Ctx.mkAnd(B, C)));
    };
    SymRef LowParity = Ctx.mkXor(Ctx.mkXor(AL, BL), CL);
    SymRef LowMajority = Majority(AL, BL, CL);
    SymRef Low = Ctx.mkAdd(LowParity, Ctx.mkShl(LowMajority, Ctx.mkOne(Width)));
    SymRef High =
        Ctx.mkAdd({Ctx.mkXor(Ctx.mkXor(AH, BH), CH),
                   Ctx.mkShl(Majority(AH, BH, CH), Ctx.mkOne(Width)),
                   Ctx.mkLShr(LowMajority, Ctx.mkConst(Width, Width - 1)),
                   Ctx.mkZExt(Ctx.mkUlt(Low, LowParity), Width)});
    SymRef Original = Ctx.mkConcat(High, Low);
    SymRef Expected = Ctx.mkAdd(
        {Ctx.mkConcat(AH, AL), Ctx.mkConcat(BH, BL), Ctx.mkConcat(CH, CL)});
    SymSynthVerifier Verifier;
    auto Verify = [&](SymContext &Context, SymRef A, SymRef B) {
      return Verifier(Context, A, B);
    };
    EXPECT_EQ(recoverSplitWordArithmetic(Ctx, Original, Verify), Expected);
    EXPECT_EQ(Verifier.report().Proof, ProofStatus::Equivalent);

    SymRef OffsetLow = Ctx.mkConst(Width, 0xf0);
    SymRef OffsetHigh = Ctx.mkConst(Width, 0x56);
    SymRef Offset = Ctx.mkConcat(OffsetHigh, OffsetLow);
    SymRef BiasedLow = Ctx.mkAdd(Low, OffsetLow);
    SymRef BiasedHigh = Ctx.mkAdd(
        {High, OffsetHigh, Ctx.mkZExt(Ctx.mkUlt(BiasedLow, Low), Width)});
    SymRef Affine = Ctx.mkConcat(BiasedHigh, BiasedLow);
    EXPECT_EQ(recoverSplitWordArithmetic(Ctx, Affine, Verify),
              Ctx.mkAdd(Expected, Offset));
    EXPECT_EQ(Verifier.report().Proof, ProofStatus::Equivalent);

    SymRef Rare = Ctx.mkAnd(Ctx.mkEq(AL, Ctx.mkConst(Width, 0x6d)),
                            Ctx.mkEq(BL, Ctx.mkConst(Width, 0x29)));
    SymRef Wrong = Ctx.mkConcat(
        Ctx.mkIte(Rare, Ctx.mkAdd(High, Ctx.mkOne(Width)), High), Low);
    EXPECT_EQ(recoverSplitWordArithmetic(Ctx, Wrong, Verify), Wrong);
    EXPECT_EQ(Verifier.report().Proof, ProofStatus::Different);
  }
}

TEST(BitVectorSolver, RecoversProvedFourInputSplitWordArithmetic) {
  using namespace neverd::symbolic;
  for (uint32_t Width : {8u, 16u, 32u}) {
    SymContext Ctx;
    SymRef AL = Ctx.mkVar("al", Width);
    SymRef AH = Ctx.mkVar("ah", Width);
    SymRef BL = Ctx.mkVar("bl", Width);
    SymRef BH = Ctx.mkVar("bh", Width);
    SymRef CL = Ctx.mkVar("cl", Width);
    SymRef CH = Ctx.mkVar("ch", Width);
    SymRef DL = Ctx.mkVar("dl", Width);
    SymRef DH = Ctx.mkVar("dh", Width);
    auto Majority = [&](SymRef A, SymRef B, SymRef C) {
      return Ctx.mkOr(Ctx.mkAnd(A, B),
                      Ctx.mkOr(Ctx.mkAnd(A, C), Ctx.mkAnd(B, C)));
    };
    SymRef LowParity = Ctx.mkXor(Ctx.mkXor(AL, BL), CL);
    SymRef LowMajority = Majority(AL, BL, CL);
    SymRef LowPair = Ctx.mkAnd(LowParity, DL);
    SymRef LowXor = Ctx.mkXor(LowParity, DL);
    SymRef LowFirst = Ctx.mkAdd(LowXor, Ctx.mkShl(LowPair, Ctx.mkOne(Width)));
    SymRef Low = Ctx.mkAdd(LowFirst, Ctx.mkShl(LowMajority, Ctx.mkOne(Width)));
    SymRef HighParity = Ctx.mkXor(Ctx.mkXor(AH, BH), CH);
    SymRef High =
        Ctx.mkAdd({Ctx.mkXor(HighParity, DH),
                   Ctx.mkShl(Ctx.mkAnd(HighParity, DH), Ctx.mkOne(Width)),
                   Ctx.mkShl(Majority(AH, BH, CH), Ctx.mkOne(Width)),
                   Ctx.mkLShr(LowPair, Ctx.mkConst(Width, Width - 1)),
                   Ctx.mkLShr(LowMajority, Ctx.mkConst(Width, Width - 1)),
                   Ctx.mkZExt(Ctx.mkUlt(LowFirst, LowXor), Width),
                   Ctx.mkZExt(Ctx.mkUlt(Low, LowFirst), Width)});
    SymRef Original = Ctx.mkConcat(High, Low);
    SymRef Expected = Ctx.mkAdd({Ctx.mkConcat(AH, AL), Ctx.mkConcat(BH, BL),
                                 Ctx.mkConcat(CH, CL), Ctx.mkConcat(DH, DL)});
    SymSynthVerifier Verifier;
    auto Verify = [&](SymContext &Context, SymRef A, SymRef B) {
      return Verifier(Context, A, B);
    };
    EXPECT_EQ(recoverSplitWordArithmetic(Ctx, Original, Verify), Expected);
    EXPECT_EQ(Verifier.report().Proof, ProofStatus::Equivalent);

    SymRef AddedLow = Ctx.mkAdd(AL, BL);
    SymRef FirstDifference = Ctx.mkSub(AddedLow, CL);
    SymRef MixedLow = Ctx.mkSub(FirstDifference, DL);
    SymRef MixedHigh = Ctx.mkSub(
        Ctx.mkSub(
            Ctx.mkAdd({AH, BH, Ctx.mkZExt(Ctx.mkUlt(AddedLow, AL), Width)}),
            CH),
        DH);
    MixedHigh = Ctx.mkSub(
        Ctx.mkSub(MixedHigh, Ctx.mkZExt(Ctx.mkUlt(AddedLow, CL), Width)),
        Ctx.mkZExt(Ctx.mkUlt(FirstDifference, DL), Width));
    SymRef Mixed = Ctx.mkConcat(MixedHigh, MixedLow);
    SymRef MixedExpected = Ctx.mkSub(
        Ctx.mkSub(Ctx.mkAdd(Ctx.mkConcat(AH, AL), Ctx.mkConcat(BH, BL)),
                  Ctx.mkConcat(CH, CL)),
        Ctx.mkConcat(DH, DL));
    EXPECT_EQ(recoverSplitWordArithmetic(Ctx, Mixed, Verify), MixedExpected);
    EXPECT_EQ(Verifier.report().Proof, ProofStatus::Equivalent);

    SymRef OffsetLow = Ctx.mkConst(Width, 0xf0);
    SymRef OffsetHigh = Ctx.mkConst(Width, 0x56);
    SymRef Offset = Ctx.mkConcat(OffsetHigh, OffsetLow);
    SymRef BiasedLow = Ctx.mkAdd(Low, OffsetLow);
    SymRef BiasedHigh = Ctx.mkAdd(
        {High, OffsetHigh, Ctx.mkZExt(Ctx.mkUlt(BiasedLow, Low), Width)});
    SymRef Affine = Ctx.mkConcat(BiasedHigh, BiasedLow);
    EXPECT_EQ(recoverSplitWordArithmetic(Ctx, Affine, Verify),
              Ctx.mkAdd(Expected, Offset));
    EXPECT_EQ(Verifier.report().Proof, ProofStatus::Equivalent);

    if (Width == 8) {
      SymRef Rare = Ctx.mkAnd(Ctx.mkEq(AL, Ctx.mkConst(Width, 0x6d)),
                              Ctx.mkEq(DL, Ctx.mkConst(Width, 0x29)));
      SymRef Wrong = Ctx.mkConcat(
          Ctx.mkIte(Rare, Ctx.mkAdd(High, Ctx.mkOne(Width)), High), Low);
      EXPECT_EQ(recoverSplitWordArithmetic(Ctx, Wrong, Verify), Wrong);
      EXPECT_EQ(Verifier.report().Proof, ProofStatus::Different);
    }
  }
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

namespace {
void sameStats(const BitVectorSolver &A, const BitVectorSolver &B) {
#define CHECK(Name) EXPECT_EQ(A.stats().Name, B.stats().Name) << #Name
  CHECK(Decisions);
  CHECK(Conflicts);
  CHECK(Propagations);
  CHECK(WatchVisits);
  CHECK(Restarts);
  CHECK(LearnedClauses);
  CHECK(DeletedClauses);
  CHECK(MinimizedLiterals);
#undef CHECK
}
void sameState(BitVectorSolver &A, BitVectorSolver &B) {
  EXPECT_EQ(A.sat().numVars(), B.sat().numVars());
  EXPECT_EQ(A.sat().numClauses(), B.sat().numClauses());
  EXPECT_EQ(A.sat().numLearnedClauses(), B.sat().numLearnedClauses());
  EXPECT_EQ(A.blaster().encoder().numGates(), B.blaster().encoder().numGates());
  EXPECT_EQ(A.encodeError(), B.encodeError());
  sameStats(A, B);
  EXPECT_EQ(A.model().vars(), B.model().vars());
  for (uint32_t V : A.model().vars())
    EXPECT_EQ(A.model().value(V), B.model().value(V));
  for (SatVar V = 0; V < A.sat().numVars(); ++V)
    EXPECT_EQ(A.sat().modelValue(V), B.sat().modelValue(V));
}
TEST(BitVectorEncodingClone, FullFormulaModelAndIncrementalWorkMatchFresh) {
  for (unsigned W : {1U, 4U, 8U, 17U, 64U}) {
    SymContext C;
    auto X = C.mkVar("x", W), Y = C.mkVar("y", W);
    auto P = C.mkEq(C.mkAdd(X, Y), C.mkConst(W, 7));
    BitVectorSolver Base(C);
    ASSERT_TRUE(Base.assertTrue(P));
    auto OriginalGates = Base.blaster().encoder().numGates();
    for (unsigned I = 0; I < 32; ++I) {
      auto Copy = Base.cloneEncoding();
      ASSERT_TRUE(Copy);
      BitVectorSolver Fresh(C);
      ASSERT_TRUE(Fresh.assertTrue(P));
      sameState(*Copy, Fresh);
      auto Input = C.mkEq(X, C.mkConst(W, I));
      ASSERT_TRUE(Copy->assertTrue(Input));
      ASSERT_TRUE(Fresh.assertTrue(Input));
      ASSERT_EQ(Copy->check(), SatResult::Sat);
      ASSERT_EQ(Fresh.check(), SatResult::Sat);
      sameState(*Copy, Fresh);
      auto Values = Copy->model().asVarValues(C);
      EXPECT_TRUE(C.eval(P, Values).isOne());
      EXPECT_TRUE(C.eval(Input, Values).isOne());
      auto Wrong = C.mkEq(Y, C.mkConst(W, 8 - I));
      ASSERT_EQ(Copy->check({Wrong}), SatResult::Unsat);
      ASSERT_EQ(Fresh.check({Wrong}), SatResult::Unsat);
      sameState(*Copy, Fresh);
      EXPECT_EQ(Copy->failedAssumptions(), Fresh.failedAssumptions());
      ASSERT_EQ(Copy->check(), SatResult::Sat);
      ASSERT_EQ(Fresh.check(), SatResult::Sat);
      sameState(*Copy, Fresh);
      EXPECT_FALSE(Copy->cloneEncoding());
    }
    EXPECT_EQ(Base.blaster().encoder().numGates(), OriginalGates);
    EXPECT_EQ(Base.stats().Decisions, 0U);
    EXPECT_TRUE(Base.model().empty());
    EXPECT_TRUE(Base.cloneEncoding());
  }
}
void pigeonhole(BitVectorSolver &S, unsigned N, unsigned H) {
  std::vector<std::vector<SatLit>> In(N, std::vector<SatLit>(H));
  for (auto &Row : In)
    for (auto &L : Row)
      L = SatLit::positive(S.sat().newVar());
  for (auto &Row : In)
    S.sat().addClause(Row);
  for (unsigned I = 0; I < N; ++I)
    for (unsigned J = I + 1; J < N; ++J)
      for (unsigned K = 0; K < H; ++K)
        S.sat().addClause(~In[I][K], ~In[J][K]);
}
TEST(BitVectorEncodingClone,
     ReboundActivityQueueOutlivesSourceAndMatchesBudgets) {
  for (unsigned Budget : {0U, 1U, 12U, 100U, 10000U})
    for (unsigned Kind = 0; Kind != 3; ++Kind) {
      SymContext C;
      SolverOptions O;
      O.Sat.RestartInterval = 2;
      O.Sat.LearnedFraction = 0.01;
      if (Kind == 0)
        O.Sat.MaxConflicts = Budget;
      if (Kind == 1)
        O.Sat.MaxPropagations = Budget;
      if (Kind == 2)
        O.Sat.MaxWatchVisits = Budget;
      auto Source = std::make_unique<BitVectorSolver>(C, O);
      pigeonhole(*Source, 5, 4);
      auto Copy = Source->cloneEncoding();
      ASSERT_TRUE(Copy);
      Source.reset();
      BitVectorSolver Fresh(C, O);
      pigeonhole(Fresh, 5, 4);
      sameState(*Copy, Fresh);
      auto A = Copy->check(), B = Fresh.check();
      EXPECT_EQ(A, B);
      sameState(*Copy, Fresh);
      if (!Budget) {
        EXPECT_EQ(A, SatResult::Unsat);
        EXPECT_GT(Copy->stats().Conflicts, 0U);
      }
      if (Budget == 1)
        EXPECT_EQ(A, SatResult::Unknown);
      EXPECT_FALSE(Copy->cloneEncoding());
    }
}
TEST(BitVectorEncodingClone, RootPropagationAndSiblingChangesAreIndependent) {
  SymContext C;
  auto X = C.mkVar("x", 32), Y = C.mkVar("y", 32);
  BitVectorSolver Base(C);
  ASSERT_TRUE(Base.assertEqual(X, C.mkConst(32, 9)));
  ASSERT_TRUE(Base.assertEqual(Y, C.mkAdd(X, C.mkConst(32, 3))));
  auto A = Base.cloneEncoding(), B = Base.cloneEncoding();
  ASSERT_TRUE(A);
  ASSERT_TRUE(B);
  ASSERT_TRUE(Base.assertFalse(C.mkTrue()));
  ASSERT_TRUE(A->assertEqual(Y, C.mkConst(32, 11)));
  EXPECT_EQ(A->check(), SatResult::Unsat);
  EXPECT_EQ(Base.check(), SatResult::Unsat);
  EXPECT_EQ(B->check(), SatResult::Sat);
  ASSERT_TRUE(B->model().value(C, Y));
  EXPECT_EQ(*B->model().value(C, Y), llvm::APInt(32, 12));
}
TEST(BitVectorEncodingClone, OriginalGateBudgetIncludesClonedGates) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8), Z = C.mkVar("z", 8);
  auto P = C.mkEq(C.mkAdd(X, Y), C.mkConst(8, 10));
  auto Q = C.mkEq(C.mkAdd(Y, Z), C.mkConst(8, 37));
  BitVectorSolver Measure(C);
  ASSERT_TRUE(Measure.assertTrue(P));
  auto DomainGates = Measure.blaster().encoder().numGates();
  ASSERT_TRUE(Measure.assertTrue(Q));
  auto AllGates = Measure.blaster().encoder().numGates();
  ASSERT_GT(AllGates, DomainGates);
  for (size_t Limit :
       {size_t(0), size_t(1), DomainGates, AllGates - 1, AllGates}) {
    SolverOptions O;
    O.Blast.MaxGates = Limit;
    BitVectorSolver Base(C, O), Fresh(C, O);
    bool A = Base.assertTrue(P), B = Fresh.assertTrue(P);
    ASSERT_EQ(A, B);
    if (!A) {
      EXPECT_FALSE(Base.cloneEncoding());
      EXPECT_EQ(Base.check(), Fresh.check());
      continue;
    }
    auto Copy = Base.cloneEncoding();
    ASSERT_TRUE(Copy);
    EXPECT_EQ(Copy->assertTrue(Q), Fresh.assertTrue(Q));
    EXPECT_EQ(Copy->check(), Fresh.check());
    sameState(*Copy, Fresh);
    if (Limit == DomainGates || Limit == AllGates - 1)
      EXPECT_EQ(Copy->encodeError(), BlastError::TooManyGates);
    if (Limit == AllGates)
      EXPECT_TRUE(Copy->ok());
  }
}
TEST(BitVectorEncodingClone, RefusesMalformedLimitedAndDirectlySearchedState) {
  SymContext C;
  auto X = C.mkVar("x", 32);
  BitVectorSolver Bad(C);
  EXPECT_FALSE(Bad.assertTrue({}));
  EXPECT_FALSE(Bad.cloneEncoding());
  SolverOptions O;
  O.Blast.MaxWidth = 8;
  BitVectorSolver Wide(C, O);
  BitLits Bits;
  EXPECT_FALSE(Wide.blaster().blast(X, Bits));
  EXPECT_FALSE(Wide.cloneEncoding());
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    BitVectorSolver S(C);
    if (Kind == 0)
      EXPECT_EQ(S.check(), SatResult::Sat);
    if (Kind == 1)
      EXPECT_EQ(S.sat().solve(), SatResult::Sat);
    if (Kind == 2) {
      SatLit Invalid;
      EXPECT_EQ(S.sat().solve({Invalid}), SatResult::Invalid);
    }
    EXPECT_FALSE(S.cloneEncoding());
  }
}
TEST(BitVectorEncodingClone,
     ContradictoryUnsearchedFormulaRemainsContradictory) {
  SymContext C;
  BitVectorSolver Base(C);
  ASSERT_TRUE(Base.assertFalse(C.mkTrue()));
  auto Copy = Base.cloneEncoding();
  ASSERT_TRUE(Copy);
  EXPECT_EQ(Copy->check(), SatResult::Unsat);
  EXPECT_EQ(Base.check(), SatResult::Unsat);
}
TEST(BitVectorEncodingClone,
     RootQueuePreservesDecisionsAcrossGrowthAndBudgets) {
  for (unsigned Count : {0U, 1U, 31U, 257U})
    for (bool Positive : {false, true}) {
      SCOPED_TRACE(Count);
      SCOPED_TRACE(Positive);
      SymContext C;
      SolverOptions O;
      O.Sat.DefaultPhase = Positive;
      O.Sat.RestartInterval = 2;
      O.Sat.LearnedFraction = 0.01;
      std::vector<SatLit> Roots;
      std::vector<SatVar> Free;
      const auto Fill = [&](BitVectorSolver &S, bool Record) {
        std::vector<SatLit> Units;
        for (unsigned I = 0; I < Count; ++I) {
          const auto Root = SatLit::mk(S.sat().newVar(), I % 2 == 0);
          const auto Unassigned = S.sat().newVar();
          const auto Derived = SatLit::positive(S.sat().newVar(false));
          ASSERT_TRUE(S.sat().addClause(~Root, Derived));
          Units.push_back(Root);
          if (Record) {
            Roots.push_back(Root);
            Roots.push_back(Derived);
            Free.push_back(Unassigned);
          }
        }
        // Populate the heap before unit propagation leaves holes among its
        // undecided variables. Include roots outside the decision queue.
        for (SatLit Unit : Units)
          ASSERT_TRUE(S.sat().addClause(Unit));
        pigeonhole(S, 4, 4);
      };
      auto Source = std::make_unique<BitVectorSolver>(C, O);
      Fill(*Source, true);
      auto Template = Source->cloneEncoding();
      ASSERT_TRUE(Template);
      auto Copy = Template->cloneEncoding();
      ASSERT_TRUE(Copy);
      Source.reset();
      Template.reset();
      BitVectorSolver Fresh(C, O);
      Fill(Fresh, false);
      sameState(*Copy, Fresh);

      // Insertion after compaction must maintain both positions and priority.
      for (unsigned I = 0; I < 65; ++I) {
        SatVar V = Copy->sat().newVar();
        ASSERT_EQ(V, Fresh.sat().newVar());
        Free.push_back(V);
      }
      SatOptions Limited = O.Sat;
      Limited.MaxPropagations = 1;
      Copy->sat().setOptions(Limited);
      Fresh.sat().setOptions(Limited);
      EXPECT_EQ(Copy->check(), SatResult::Unknown);
      EXPECT_EQ(Fresh.check(), SatResult::Unknown);
      sameState(*Copy, Fresh);
      Copy->sat().setOptions(O.Sat);
      Fresh.sat().setOptions(O.Sat);
      ASSERT_EQ(Copy->check(), SatResult::Sat);
      ASSERT_EQ(Fresh.check(), SatResult::Sat);
      sameState(*Copy, Fresh);
      for (SatLit Root : Roots)
        EXPECT_EQ(Copy->sat().modelValue(Root), SatValue::True);
      for (SatVar V : Free)
        EXPECT_EQ(Copy->sat().modelValue(V),
                  Positive ? SatValue::True : SatValue::False);

      // The surviving positions must still support conflict activity changes,
      // backtracking, restarts and incremental clauses after a complete solve.
      pigeonhole(*Copy, 5, 4);
      pigeonhole(Fresh, 5, 4);
      Limited = O.Sat;
      Limited.MaxConflicts = 1;
      Copy->sat().setOptions(Limited);
      Fresh.sat().setOptions(Limited);
      EXPECT_EQ(Copy->check(), SatResult::Unknown);
      EXPECT_EQ(Fresh.check(), SatResult::Unknown);
      sameState(*Copy, Fresh);
      Copy->sat().setOptions(O.Sat);
      Fresh.sat().setOptions(O.Sat);
      EXPECT_EQ(Copy->check(), SatResult::Unsat);
      EXPECT_EQ(Fresh.check(), SatResult::Unsat);
      sameState(*Copy, Fresh);
      EXPECT_GT(Copy->stats().Restarts, 0U);
      EXPECT_FALSE(Copy->cloneEncoding());
    }
}

TEST(BitVectorEncodingClone, WatchMigrationAndGrowthOutliveTheSource) {
  for (unsigned Count : {1U, 3U, 4U, 5U, 17U, 65U}) {
    SymContext C;
    std::vector<std::vector<SatLit>> Clauses;
    std::vector<std::pair<SatLit, SatLit>> Inputs;
    const auto Fill = [&](BitVectorSolver &S, bool Record) {
      const auto Guard = SatLit::positive(S.sat().newVar());
      for (unsigned I = 0; I < Count; ++I) {
        const auto A = SatLit::positive(S.sat().newVar());
        const auto B = SatLit::positive(S.sat().newVar());
        const std::vector<SatLit> Positive{~Guard, A, B};
        const std::vector<SatLit> Negative{~Guard, ~A, ~B};
        S.sat().addClause(Positive);
        S.sat().addClause(Negative);
        if (Record) {
          Inputs.emplace_back(A, B);
          Clauses.push_back(Positive);
          Clauses.push_back(Negative);
        }
      }
      return Guard;
    };
    auto Source = std::make_unique<BitVectorSolver>(C);
    const auto Guard = Fill(*Source, true);
    auto Copy = Source->cloneEncoding(), Sibling = Source->cloneEncoding();
    ASSERT_TRUE(Copy);
    ASSERT_TRUE(Sibling);
    Source.reset();
    BitVectorSolver Fresh(C);
    ASSERT_EQ(Fill(Fresh, false), Guard);

    // Grow both the table of literal lists and one existing high-fanout list
    // after copying. The sibling retains the original independent formula.
    for (unsigned I = 0; I < 257; ++I) {
      const auto Extra = SatLit::positive(Copy->sat().newVar());
      ASSERT_EQ(SatLit::positive(Fresh.sat().newVar()), Extra);
      Copy->sat().addClause(~Guard, Extra);
      Fresh.sat().addClause(~Guard, Extra);
      Clauses.push_back({~Guard, Extra});
    }
    Sibling->sat().addClause(~Guard);
    EXPECT_EQ(Sibling->sat().solve({Guard}), SatResult::Unsat);
    EXPECT_EQ(Sibling->sat().solve({~Guard}), SatResult::Sat);

    for (unsigned Pattern = 0; Pattern < 4; ++Pattern) {
      std::vector<SatLit> Assumptions{Guard};
      for (unsigned I = 0; I < Count; ++I)
        Assumptions.push_back(
            Inputs[I].first.withPolarity((I + Pattern) % 3 == 0));
      SatOptions Limited;
      Limited.MaxWatchVisits = 1;
      Copy->sat().setOptions(Limited);
      Fresh.sat().setOptions(Limited);
      EXPECT_EQ(Copy->sat().solve(Assumptions), SatResult::Unknown);
      EXPECT_EQ(Fresh.sat().solve(Assumptions), SatResult::Unknown);
      sameState(*Copy, Fresh);
      Copy->sat().setOptions(SatOptions{});
      Fresh.sat().setOptions(SatOptions{});
      ASSERT_EQ(Copy->sat().solve(Assumptions), SatResult::Sat);
      ASSERT_EQ(Fresh.sat().solve(Assumptions), SatResult::Sat);
      sameState(*Copy, Fresh);
      // Check the original clauses and the independently derived B = !A
      // relation, including after interrupted watch migration and replay.
      for (const auto &Clause : Clauses) {
        bool Satisfied = false;
        for (SatLit L : Clause)
          Satisfied |= Copy->sat().modelValue(L) == SatValue::True;
        EXPECT_TRUE(Satisfied);
      }
      for (unsigned I = 0; I < Count; ++I)
        EXPECT_EQ(Copy->sat().modelValue(Inputs[I].second),
                  (I + Pattern) % 3 == 0 ? SatValue::False : SatValue::True);
    }
  }
}
} // namespace

TEST(RootFacts, PermanentImplicationsAreDistinctFromModels) {
  SatSolver S;
  const auto A = SatLit::positive(S.newVar());
  const auto B = SatLit::positive(S.newVar());
  ASSERT_TRUE(S.addClause(~A, B));
  EXPECT_EQ(S.rootValue(A), SatValue::Unknown);
  EXPECT_EQ(S.rootValue(B), SatValue::Unknown);
  ASSERT_TRUE(S.addClause(A));
  EXPECT_EQ(S.rootValue(A), SatValue::True);
  EXPECT_EQ(S.rootValue(~B), SatValue::False);
  EXPECT_EQ(S.modelValue(A), SatValue::Unknown);
  EXPECT_EQ(S.rootValue(SatLit{}), SatValue::Unknown);
  EXPECT_EQ(S.rootValue(SatLit::positive(S.numVars())), SatValue::Unknown);
  EXPECT_EQ(S.solve({~A}), SatResult::Unsat);
  EXPECT_EQ(S.rootValue(B), SatValue::True);
}

TEST(RootFacts, AssumptionsNeverBecomePermanentConstants) {
  SatSolver S;
  CnfEncoder E(S);
  auto A = E.freshLit(), B = E.freshLit();
  ASSERT_TRUE(S.addClause(A, B));
  for (auto Choice : {A, ~A, B, ~B}) {
    ASSERT_EQ(S.solve({Choice}), SatResult::Sat);
    EXPECT_EQ(S.modelValue(Choice), SatValue::True);
    EXPECT_EQ(S.rootValue(A), SatValue::Unknown);
    EXPECT_EQ(S.rootValue(B), SatValue::Unknown);
    EXPECT_FALSE(E.isConstant(A));
    EXPECT_FALSE(E.isConstant(B));
  }
}

TEST(RootFacts, PermanentConstantsFoldAllGateFamilies) {
  SatSolver S;
  CnfEncoder E(S);
  auto A = E.freshLit(), B = E.freshLit(), C = E.freshLit();
  ASSERT_TRUE(E.assertTrue(A));
  EXPECT_TRUE(E.isTrueLit(A));
  EXPECT_TRUE(E.isFalseLit(~A));
  EXPECT_EQ(E.mkAnd(A, B), B);
  EXPECT_EQ(E.mkOr(A, B), E.trueLit());
  EXPECT_EQ(E.mkXor(A, B), ~B);
  EXPECT_EQ(E.mkIte(A, B, C), B);
  EXPECT_EQ(E.mkMajority(A, B, C), E.mkOr(B, C));
  SatLit Sum, Carry;
  E.mkFullAdder(A, B, ~B, Sum, Carry);
  EXPECT_TRUE(E.isFalseLit(Sum));
  EXPECT_EQ(Carry, E.trueLit());
}

TEST(RootFacts, PartialGatePolarityDoesNotInventInputFacts) {
  for (auto Polarity : {GatePolarity::Positive, GatePolarity::Negative}) {
    SatSolver S;
    CnfEncoder E(S);
    auto A = E.freshLit(), B = E.freshLit();
    const auto G = E.mkAnd(A, B, Polarity);
    ASSERT_TRUE(E.assertTrue(G));
    if (Polarity == GatePolarity::Positive) {
      EXPECT_TRUE(E.isTrueLit(A));
      EXPECT_TRUE(E.isTrueLit(B));
    } else {
      EXPECT_FALSE(E.isConstant(A));
      EXPECT_FALSE(E.isConstant(B));
      EXPECT_EQ(S.solve({~A, ~B}), SatResult::Sat);
    }
  }
}

TEST(RootFacts, ClonesOwnIndependentPermanentFacts) {
  SymContext C;
  const auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  BitVectorSolver S(C);
  ASSERT_TRUE(S.assertEqual(X, C.mkConst(8, 42)));
  auto Copy = S.cloneEncoding();
  ASSERT_NE(Copy, nullptr);
  ASSERT_TRUE(S.assertEqual(Y, C.mkZero(8)));
  auto One = C.mkEq(Y, C.mkOne(8));
  EXPECT_EQ(S.check({One}), SatResult::Unsat);
  ASSERT_EQ(Copy->check({One}), SatResult::Sat);
  ASSERT_TRUE(Copy->model().contains(C.varId(X)));
  ASSERT_TRUE(Copy->model().contains(C.varId(Y)));
  EXPECT_EQ(Copy->model().value(C, X)->getZExtValue(), 42U);
  EXPECT_EQ(Copy->model().value(C, Y)->getZExtValue(), 1U);
}

TEST(RootFacts, SavedModelDoesNotConstrainLaterEncoding) {
  SymContext C;
  const auto X = C.mkVar("x", 1), Y = C.mkVar("y", 1);
  BitVectorSolver S(C);
  ASSERT_EQ(S.check({X}), SatResult::Sat);
  ASSERT_TRUE(S.assertTrue(C.mkXor(X, Y)));
  ASSERT_EQ(S.check({C.mkNot(X)}), SatResult::Sat);
  EXPECT_TRUE(S.model().value(C, X)->isZero());
  EXPECT_TRUE(S.model().value(C, Y)->isOne());
}

TEST(EntailedConditions, BooleanWrappersRetainEveryAssignment) {
  for (unsigned Kind = 0; Kind != 8; ++Kind)
    for (bool Positive : {false, true})
      for (bool NumberFirst : {false, true}) {
        SymContext C;
        const auto X = C.mkVar("x", 1), Y = C.mkVar("y", 1);
        const auto Z = C.mkVar("z", 1);
        std::array<SymRef, 8> Forms{C.mkAnd(X, Y),
                                    C.mkOr(X, Y),
                                    C.mkNot(C.mkAnd(X, Y)),
                                    C.mkNot(C.mkOr(X, Y)),
                                    C.mkOr(X, C.mkAnd(Y, Z)),
                                    C.mkAnd(C.mkNot(X), C.mkOr(Y, Z)),
                                    C.mkXor(X, Y),
                                    C.mkIte(X, Y, Z)};
        const auto Extended = C.mkZExt(Forms[Kind], 8);
        const auto Number = C.mkConst(8, Positive);
        const auto Pred =
            NumberFirst ? C.mkEq(Number, Extended) : C.mkEq(Extended, Number);
        BitVectorSolver S(C);
        ASSERT_TRUE(S.assertTrue(Pred));
        for (unsigned Bits = 0; Bits != 8; ++Bits) {
          std::vector<llvm::APInt> Values;
          llvm::SmallVector<SymRef, 3> Assumptions;
          unsigned I = 0;
          for (auto V : {X, Y, Z}) {
            const bool Set = Bits & (1U << I++);
            Values.emplace_back(1, Set);
            Assumptions.push_back(Set ? V : C.mkNot(V));
          }
          const auto Expected = C.eval(Pred, Values).isOne();
          ASSERT_EQ(S.check(Assumptions),
                    Expected ? SatResult::Sat : SatResult::Unsat);
          if (Expected) {
            for (auto V : {X, Y, Z})
              ASSERT_TRUE(S.model().contains(C.varId(V)));
            EXPECT_TRUE(C.eval(Pred, S.model().asVarValues(C)).isOne());
          }
        }
      }
}

TEST(EntailedConditions, WiderBitwiseAndRetainsNonzeroSemantics) {
  SymContext C;
  const auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  BitVectorSolver S(C);
  ASSERT_TRUE(S.assertTrue(C.mkAnd(X, Y)));
  auto XOne = C.mkEq(X, C.mkOne(8));
  EXPECT_EQ(S.check({XOne, C.mkEq(Y, C.mkConst(8, 2))}), SatResult::Unsat);
  EXPECT_EQ(S.check({XOne, C.mkEq(Y, C.mkConst(8, 3))}), SatResult::Sat);
}

TEST(EntailedConditions, ExtensionKindAndOutOfRangeConstantsStayExact) {
  for (bool Signed : {false, true})
    for (unsigned Number : {0U, 1U, 2U, 255U}) {
      SymContext C;
      auto X = C.mkVar("x", 1);
      auto Extended = Signed ? C.mkSExt(X, 8) : C.mkZExt(X, 8);
      auto Pred = C.mkEq(Extended, C.mkConst(8, Number));
      BitVectorSolver S(C);
      ASSERT_TRUE(S.assertTrue(Pred));
      for (bool Set : {false, true}) {
        const auto Expected = (Set ? (Signed ? 255U : 1U) : 0U) == Number;
        EXPECT_EQ(S.check({Set ? X : C.mkNot(X)}),
                  Expected ? SatResult::Sat : SatResult::Unsat);
      }
    }
}

TEST(EntailedConditions, EncodingAndQueryLimitsStillRefuse) {
  SymContext C;
  auto X = C.mkVar("x", 64), Y = C.mkVar("y", 64);
  SolverOptions O;
  O.Blast.MaxGates = 1;
  BitVectorSolver S(C, O);
  EXPECT_FALSE(S.assertTrue(C.mkEq(C.mkAdd(X, Y), C.mkConst(64, 17))));
  EXPECT_EQ(S.check(), SatResult::Unknown);
  EXPECT_EQ(S.cloneEncoding(), nullptr);
}

// Keep these predicates above the builder's bounded constant scan so the
// solver preparation, rather than construction, still observes the extension.
constexpr unsigned UnfoldedEqualityWords =
    ::neverd::limits::symbolic::kSymExprExtensionCompareMaxWords + 1;
constexpr unsigned UnfoldedEqualityWidth = 64 * UnfoldedEqualityWords;
// Two visits, three edges and three scans of each constant word.
constexpr unsigned UnfoldedEqualityWork = 5 + 3 * UnfoldedEqualityWords;

TEST(PermanentConjunctPreparation, ExactAndShortWordInspectionBudgets) {
  SymContext C;
  const auto X = C.mkVar("x", 1);
  const auto Pred = C.mkEq(C.mkZExt(X, UnfoldedEqualityWidth),
                           C.mkOne(UnfoldedEqualityWidth));
  const auto Before = C.numNodes();
  ASSERT_EQ(C.op(Pred), neverd::symbolic::SymOp::Eq);
  auto Exact = detail::collectPermanentConjuncts(C, Pred, UnfoldedEqualityWork);
  ASSERT_TRUE(Exact);
  ASSERT_EQ(Exact->size(), 1U);
  EXPECT_EQ(Exact->front(), (detail::PermanentConjunct{X, true}));
  EXPECT_FALSE(
      detail::collectPermanentConjuncts(C, Pred, UnfoldedEqualityWork - 1));
  EXPECT_FALSE(detail::collectPermanentConjuncts(C, Pred, 0));
  EXPECT_EQ(C.numNodes(), Before);
}

TEST(PermanentConjunctPreparation, ExhaustionDiscardsAlreadyCollectedFacts) {
  SymContext C;
  const auto A = C.mkVar("a", 1), X = C.mkVar("x", 1);
  const auto Wrapped = C.mkEq(C.mkZExt(X, UnfoldedEqualityWidth),
                              C.mkOne(UnfoldedEqualityWidth));
  const auto Pred = C.mkAnd(A, Wrapped);
  // The conjunction adds a node, two edges and the other leaf's visit.
  auto Exact =
      detail::collectPermanentConjuncts(C, Pred, UnfoldedEqualityWork + 4);
  ASSERT_TRUE(Exact);
  ASSERT_EQ(Exact->size(), 2U);
  EXPECT_EQ((*Exact)[0], (detail::PermanentConjunct{A, true}));
  EXPECT_EQ((*Exact)[1], (detail::PermanentConjunct{X, true}));
  EXPECT_FALSE(
      detail::collectPermanentConjuncts(C, Pred, UnfoldedEqualityWork + 3));
}

TEST(PermanentConjunctPreparation, RevisitedLeavesStillConsumeWork) {
  SymContext C;
  const auto X = C.mkVar("x", 1);
  const auto One = C.mkEq(C.mkZExt(X, UnfoldedEqualityWidth),
                          C.mkOne(UnfoldedEqualityWidth));
  const auto NotZero = C.mkEq(C.mkZExt(C.mkNot(X), UnfoldedEqualityWidth),
                              C.mkZero(UnfoldedEqualityWidth));
  const auto Pred = C.mkAnd(One, NotZero);
  // Both paths revisit x; the second path also traverses its complement.
  auto Exact =
      detail::collectPermanentConjuncts(C, Pred, 2 * UnfoldedEqualityWork + 5);
  ASSERT_TRUE(Exact);
  ASSERT_EQ(Exact->size(), 1U);
  EXPECT_EQ(Exact->front(), (detail::PermanentConjunct{X, true}));
  EXPECT_FALSE(
      detail::collectPermanentConjuncts(C, Pred, 2 * UnfoldedEqualityWork + 4));
}

TEST(PermanentConjunctPreparation, WideCopiesRemainBoundedWithLargerRequests) {
  // Five units cover the node visits and edges; each constant word costs three.
  constexpr unsigned Words = (detail::MaxPermanentConjunctWork - 5) / 3;
  for (unsigned Width : {64 * Words, 64 * (Words + 1)}) {
    SymContext C;
    const auto X = C.mkVar("x", 1);
    const auto Pred = C.mkEq(C.mkZExt(X, Width), C.mkOne(Width));
    const auto Before = C.numNodes();
    auto Facts = detail::collectPermanentConjuncts(C, Pred, 1000000);
    EXPECT_EQ(Facts.has_value(), Width == 64 * Words);
    if (Facts) {
      ASSERT_EQ(Facts->size(), 1U);
      EXPECT_EQ(Facts->front(), (detail::PermanentConjunct{X, true}));
    }
    EXPECT_EQ(C.numNodes(), Before);
  }
}

TEST(PermanentConjunctPreparation, LargeConjunctionRetainsAllInputs) {
  SymContext C;
  llvm::SmallVector<SymRef, 600> Inputs;
  for (unsigned I = 0; I != 600; ++I)
    Inputs.push_back(C.mkVar("x" + std::to_string(I), 1));
  const auto Pred = C.mkAnd(Inputs);
  const auto Before = C.numNodes();
  EXPECT_FALSE(detail::collectPermanentConjuncts(C, Pred, 512));
  const auto Facts = detail::collectPermanentConjuncts(C, Pred);
  ASSERT_TRUE(Facts);
  EXPECT_EQ(Facts->size(), Inputs.size());
  EXPECT_EQ(C.numNodes(), Before);
  BitVectorSolver S(C);
  ASSERT_TRUE(S.assertTrue(Pred));
  ASSERT_EQ(S.check(), SatResult::Sat);
  for (auto X : Inputs) {
    const auto Value = S.model().value(C, X);
    ASSERT_TRUE(Value);
    EXPECT_TRUE(Value->isOne());
  }
  EXPECT_EQ(S.check({C.mkNot(Inputs.front())}), SatResult::Unsat);
}

TEST(PermanentConjunctPreparation, FanoutRefusalRetainsEncodingFailure) {
  SymContext C;
  llvm::SmallVector<SymRef, 32> Inputs;
  // Each child needs one edge and one visit, in addition to the root visit.
  for (unsigned I = 0; I != detail::MaxPermanentConjunctWork / 2; ++I)
    Inputs.push_back(C.mkVar("x" + std::to_string(I), 1));
  const auto Pred = C.mkAnd(Inputs);
  const auto Before = C.numNodes();
  EXPECT_FALSE(detail::collectPermanentConjuncts(C, Pred, 1000000));
  EXPECT_EQ(C.numNodes(), Before);
  SolverOptions O;
  O.Blast.MaxGates = 1;
  BitVectorSolver S(C, O);
  EXPECT_FALSE(S.assertTrue(Pred));
  EXPECT_EQ(S.encodeError(), BlastError::TooManyGates);
  EXPECT_EQ(S.check(), SatResult::Unknown);
}

TEST(PermanentConjunctPreparation,
     CollectedBitDoesNotBypassOriginalWidthLimit) {
  SymContext C;
  const auto X = C.mkVar("x", 1);
  const auto Pred = C.mkEq(C.mkZExt(X, UnfoldedEqualityWidth),
                           C.mkOne(UnfoldedEqualityWidth));
  ASSERT_EQ(C.op(Pred), neverd::symbolic::SymOp::Eq);
  const auto Facts = detail::collectPermanentConjuncts(C, Pred);
  ASSERT_TRUE(Facts);
  ASSERT_EQ(Facts->size(), 1U);
  EXPECT_EQ(Facts->front(), (detail::PermanentConjunct{X, true}));
  BitVectorSolver S(C);
  EXPECT_FALSE(S.assertTrue(Pred));
  EXPECT_EQ(S.encodeError(), BlastError::WidthTooLarge);
  EXPECT_EQ(S.check(), SatResult::Unknown);
}

TEST(PermanentConjunctPreparation, FoldedEqualitiesUseCanonicalCostAndWidth) {
  SymContext C;
  const auto X = C.mkVar("x", 1);
  const auto Pred = C.mkEq(C.mkZExt(X, 1024), C.mkOne(1024));
  ASSERT_EQ(Pred, X);
  const auto Facts = detail::collectPermanentConjuncts(C, Pred, 1);
  ASSERT_TRUE(Facts);
  ASSERT_EQ(Facts->size(), 1U);
  EXPECT_EQ(Facts->front(), (detail::PermanentConjunct{X, true}));
  EXPECT_FALSE(detail::collectPermanentConjuncts(C, Pred, 0));
  BitVectorSolver S(C);
  ASSERT_TRUE(S.assertTrue(Pred));
  EXPECT_EQ(S.check(), SatResult::Sat);
  EXPECT_EQ(S.check({C.mkNot(X)}), SatResult::Unsat);
}

TEST(PermanentConjunctPreparation, AlternativesRemainOpaque) {
  SymContext C;
  const auto X = C.mkVar("x", 1), Y = C.mkVar("y", 1);
  const auto Either = C.mkOr(X, Y), Both = C.mkAnd(X, Y);
  auto Positive = detail::collectPermanentConjuncts(C, Either);
  auto Negative = detail::collectPermanentConjuncts(C, C.mkNot(Both));
  ASSERT_TRUE(Positive);
  ASSERT_TRUE(Negative);
  ASSERT_EQ(Positive->size(), 1U);
  ASSERT_EQ(Negative->size(), 1U);
  EXPECT_EQ(Positive->front(), (detail::PermanentConjunct{Either, true}));
  EXPECT_EQ(Negative->front(), (detail::PermanentConjunct{Both, false}));
}

TEST(PermanentConjunctPreparation, RefusalRetainsMalformedInputFailure) {
  SymContext C;
  EXPECT_FALSE(detail::collectPermanentConjuncts(C, SymRef{}));
  BitVectorSolver S(C);
  EXPECT_FALSE(S.assertTrue(SymRef{}));
  EXPECT_EQ(S.encodeError(), BlastError::Malformed);
  EXPECT_EQ(S.check(), SatResult::Invalid);
}

TEST(RootFacts, FalsifiedSolverDoesNotExposeAdditionalConstants) {
  SatSolver S;
  CnfEncoder E(S);
  const auto A = E.freshLit();
  ASSERT_TRUE(E.assertTrue(A));
  EXPECT_TRUE(E.isTrueLit(A));
  EXPECT_FALSE(E.assertTrue(~A));
  EXPECT_EQ(S.rootValue(A), SatValue::Unknown);
  EXPECT_FALSE(E.isConstant(A));
}
