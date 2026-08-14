//===- SymSynthTests.cpp - Oracle-guided expression synthesis -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercises the search on the shapes it exists for: expressions the MBA
/// measurement declines, because a shift stands between it and what it wants
/// to read.
///
/// A logical shift right is an opaque input to that measurement, and two
/// spellings of one — `x >> 4` and `(x >> 2) >> 2` — are two unrelated inputs
/// to it, which is exactly why an obfuscator writes them that way.  Running
/// the expression instead of reading it makes the difference disappear: the
/// two produce the same values, so an answer written over one of them
/// reproduces the whole.
///
/// Every rewrite this file asserts is also checked the long way, against the
/// original at hundreds of points, by machinery that shares nothing with the
/// engine's own checking.  A test that trusted the engine's verdict on the
/// engine's own answer would be checking that the code runs, not that it is
/// right.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"
#include "neverd/symbolic/SymSynth.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

using namespace neverd::symbolic;

namespace {

constexpr uint32_t W32 = 32;

SymRef parsed(SymContext &Ctx, llvm::StringRef Text, uint32_t Width = W32) {
  SymParseResult Result = parseSymExpr(Ctx, Text, Width);
  EXPECT_TRUE(Result.ok()) << Text.str() << ": " << Result.Error;
  return Result.Root;
}

/// Compare two expressions at every corner and a spread of ordinary points.
///
/// Deliberately written out here rather than borrowed from the engine: this is
/// what decides whether an answer is right, so it must not be the same code
/// that decided the answer was right in the first place.
bool behavesTheSame(SymContext &Ctx, SymRef A, SymRef B,
                    unsigned Points = 400) {
  llvm::SmallVector<uint32_t, 8> Vars;
  Ctx.collectVars(A, Vars);
  llvm::SmallVector<uint32_t, 8> FromB;
  Ctx.collectVars(B, FromB);
  for (uint32_t Id : FromB)
    if (!llvm::is_contained(Vars, Id))
      Vars.push_back(Id);

  std::vector<llvm::APInt> Assignment;
  for (size_t I = 0; I < Ctx.numVars(); ++I)
    Assignment.emplace_back(Ctx.varInfo(uint32_t(I)).Width, 0);

  SymEvalPlan PlanA(Ctx, A);
  SymEvalPlan PlanB(Ctx, B);

  // The first rounds walk the corners, where a bitwise rewriting and an
  // arithmetic one are likeliest to agree by accident; the rest are ordinary
  // values, where an error in the algebra shows instead.
  const unsigned Corners = Vars.size() < 8 ? 1u << Vars.size() : 0u;
  uint64_t State = 0x5DEECE66Dull;
  for (unsigned Round = 0; Round < Points; ++Round) {
    for (size_t I = 0; I < Vars.size(); ++I) {
      const uint32_t Id = Vars[I];
      const uint32_t Width = Ctx.varInfo(Id).Width;
      if (Round < Corners) {
        Assignment[Id] = (Round >> I) & 1 ? llvm::APInt::getAllOnes(Width)
                                          : llvm::APInt(Width, 0);
        continue;
      }
      llvm::SmallVector<uint64_t, 4> Words((Width + 63) / 64);
      for (uint64_t &Word : Words) {
        State = State * 6364136223846793005ull + 1442695040888963407ull;
        Word = State ^ (State >> 29);
      }
      Assignment[Id] = llvm::APInt(Width, Words);
    }
    if (PlanA.eval(Assignment) != PlanB.eval(Assignment))
      return false;
  }
  return true;
}

/// Options that keep a test to a second or so.  Every test states them rather
/// than taking the defaults, so a change to the defaults cannot silently make
/// a test prove something else.
SynthOptions quick() {
  SynthOptions Opts;
  Opts.MaxCost = 5;
  Opts.MaxSamples = 48;
  Opts.VerifySamples = 256;
  Opts.MaxWork = size_t(1) << 18;
  Opts.UseStochasticFallback = false;
  return Opts;
}

//===----------------------------------------------------------------------===//
// What the measurement cannot reach
//===----------------------------------------------------------------------===//

TEST(SymSynth, RecoversAFormBehindTwoSpellingsOfOneShift) {
  // `(x >> 2) >> 2` and `x >> 4` are the same value written two ways.  The MBA
  // measurement sees two unrelated opaque inputs and reports their sum, which
  // is what it already had; running the expression sees one value twice.
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) + ((x >> 2) >> 2)");
  SymRef Want = parsed(Ctx, "2 * (x >> 4)");

  SynthResult R = discoverSynthesisCandidate(Ctx, E, quick());
  EXPECT_EQ(R.Outcome, SynthOutcome::Synthesized)
      << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Evidence, SynthEvidence::Samples);
  EXPECT_EQ(R.ProofQueries, 0u);
  EXPECT_TRUE(R.Changed);
  EXPECT_EQ(R.Expr, Want) << Ctx.toString(R.Expr);
  EXPECT_LT(R.SizeAfter, R.SizeBefore);
  EXPECT_TRUE(behavesTheSame(Ctx, E, R.Expr));

  // The claim this engine exists for: it reaches something the derivation-based
  // solver does not.  Stated as an inequality so that a stronger MBA solver
  // makes this pass more comfortably rather than failing it.
  EXPECT_LE(R.SizeAfter, simplifyMBADeep(Ctx, E).SizeAfter);
}

TEST(SymSynth, CollapsesAnExpressionThatIsSecretlyConstant) {
  // The same two spellings, subtracted.  Nothing in the shape of the
  // expression says so; only running it does.
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) - ((x >> 2) >> 2)");

  SynthResult R = discoverSynthesisCandidate(Ctx, E, quick());
  ASSERT_EQ(R.Outcome, SynthOutcome::Synthesized)
      << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Expr, Ctx.mkZero(W32)) << Ctx.toString(R.Expr);
  EXPECT_EQ(R.CandidateCost, 1u);
  EXPECT_TRUE(behavesTheSame(Ctx, E, R.Expr));
  EXPECT_LE(R.SizeAfter, simplifyMBADeep(Ctx, E).SizeAfter);
}

TEST(SymSynth, ZeroMaxCostDisablesEnumeration) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) - ((x >> 2) >> 2)");

  SynthOptions Opts = quick();
  Opts.MaxCost = 0;
  SynthResult R = discoverSynthesisCandidate(Ctx, E, Opts);

  EXPECT_EQ(R.Expr, E);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Outcome, SynthOutcome::AlreadyShortest);
  EXPECT_EQ(R.CandidateCost, 0u);
  EXPECT_EQ(R.Work, 0u);
}

TEST(SymSynth, MaximumMaxCostIsBoundedByWork) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) + ((x >> 2) >> 2)");

  SynthOptions Opts = quick();
  Opts.MaxCost = std::numeric_limits<size_t>::max();
  Opts.MaxWork = 1;
  Opts.UseStochasticFallback = false;
  SynthResult R = discoverSynthesisCandidate(Ctx, E, Opts);

  EXPECT_EQ(R.Expr, E);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Outcome, SynthOutcome::BudgetExhausted);
  EXPECT_EQ(R.Work, Opts.MaxWork);
}

TEST(SymSynth, RecoversANonlinearMixThroughARedundantFactor) {
  // A product of two inputs, multiplied by something that is secretly one.
  // The product puts the whole expression outside the linear theory, and the
  // redundant factor is invisible to the polynomial one, so the measurement
  // declines it outright.
  SymContext Ctx;
  SymRef E = parsed(Ctx, "x * y * ((x >> 4) - ((x >> 2) >> 2) + 1)");
  SymRef Want = parsed(Ctx, "x * y");

  SynthResult R = discoverSynthesisCandidate(Ctx, E, quick());
  ASSERT_EQ(R.Outcome, SynthOutcome::Synthesized)
      << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Expr, Want) << Ctx.toString(R.Expr);
  EXPECT_EQ(R.NumLeaves, 2u);
  EXPECT_TRUE(behavesTheSame(Ctx, E, R.Expr));
  EXPECT_LE(R.SizeAfter, simplifyMBADeep(Ctx, E).SizeAfter);
}

TEST(SymSynth, PutsBackTheSubtermItCouldNotSeeInsideOf) {
  // A division is outside the grammar, so it becomes one input; the answer is
  // synthesized over that input and the division is substituted back whole.
  // Getting this wrong would produce an expression that is short and about
  // something else.
  SymContext Ctx;
  SymRef E = parsed(Ctx, "((x / y) >> 4) + ((((x / y) >> 2)) >> 2)");
  SymRef Want = parsed(Ctx, "2 * ((x / y) >> 4)");

  SynthResult R = discoverSynthesisCandidate(Ctx, E, quick());
  ASSERT_EQ(R.Outcome, SynthOutcome::Synthesized)
      << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Expr, Want) << Ctx.toString(R.Expr);
  EXPECT_EQ(R.NumLeaves, 1u) << "the division should be one opaque input";
  EXPECT_TRUE(behavesTheSame(Ctx, E, R.Expr));
}

TEST(SymSynth, WorksAtAWidthWiderThanAMachineWord) {
  // Nothing in the search reads a word: values are APInt throughout, and a
  // 128-bit expression is measured exactly like a 32-bit one.
  constexpr uint32_t W128 = 128;
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) + ((x >> 2) >> 2)", W128);
  SymRef Want = parsed(Ctx, "2 * (x >> 4)", W128);

  SynthResult R = discoverSynthesisCandidate(Ctx, E, quick());
  ASSERT_EQ(R.Outcome, SynthOutcome::Synthesized)
      << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Expr, Want) << Ctx.toString(R.Expr);
  EXPECT_TRUE(behavesTheSame(Ctx, E, R.Expr));
}

//===----------------------------------------------------------------------===//
// What it declines
//===----------------------------------------------------------------------===//

TEST(SymSynth, LeavesAnExpressionThatIsAlreadyAsShortAsItGets) {
  // The search finds these — they are three nodes and it looks at everything
  // of that size — and returns the input anyway, because an answer that is not
  // shorter than the question is not an answer.
  for (llvm::StringRef Text : {"x ^ y", "x + y", "x & y", "x * y"}) {
    SymContext Ctx;
    SymRef E = parsed(Ctx, Text);
    SynthResult R = discoverSynthesisCandidate(Ctx, E, quick());
    EXPECT_EQ(R.Expr, E) << Text.str() << " became " << Ctx.toString(R.Expr);
    EXPECT_FALSE(R.Changed);
    EXPECT_EQ(R.Outcome, SynthOutcome::AlreadyShortest)
        << Text.str() << ": " << synthOutcomeName(R.Outcome);
  }
}

TEST(SymSynth, DeclinesAnExpressionThatIsOneOpaqueOperation) {
  // Everything about the expression is behind an operator the grammar cannot
  // say, so the reduction leaves one input and nothing to search over.  Saying
  // so immediately is the point: a budget spent here would buy nothing.
  SymContext Ctx;
  SymRef E = parsed(Ctx, "x / y");

  SynthResult R = discoverSynthesisCandidate(Ctx, E, quick());
  EXPECT_EQ(R.Expr, E);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Outcome, SynthOutcome::NotApplicable)
      << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Work, 0u);
}

TEST(SymSynth, DeclinesAnExpressionWithMoreInputsThanItWillSearchOver) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(a >> 1) + (b >> 1) + (c >> 1) + (d >> 1)");

  SynthOptions Opts = quick();
  Opts.MaxLeaves = 3;
  SynthResult R = discoverSynthesisCandidate(Ctx, E, Opts);
  EXPECT_EQ(R.Expr, E);
  EXPECT_EQ(R.Outcome, SynthOutcome::TooManyInputs)
      << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.NumLeaves, 4u);
}

//===----------------------------------------------------------------------===//
// The verification hook
//===----------------------------------------------------------------------===//

TEST(SymSynth, VerificationStatusValuesAreStable) {
  static_assert(
      std::is_same_v<std::underlying_type_t<SynthVerification>, uint8_t>);
  EXPECT_EQ(static_cast<uint8_t>(SynthVerification::Equivalent), 1u);
  EXPECT_EQ(static_cast<uint8_t>(SynthVerification::Different), 2u);
  EXPECT_EQ(static_cast<uint8_t>(SynthVerification::Unknown), 3u);
}

TEST(SymSynth, NoVerifierCompatibilityIsARealOverload) {
  using NoVerifierEntry =
      SymRef (*)(SymContext &, SymRef, const SynthOptions &);
  using VerifiedEntry =
      SymRef (*)(SymContext &, SymRef, const SynthOptions &, SynthVerifyFn);
  NoVerifierEntry Entry = &synthesizeEquivalent;
  VerifiedEntry Verified = &synthesizeEquivalent;
  EXPECT_NE(Verified, nullptr);

  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) + ((x >> 2) >> 2)");
  EXPECT_EQ(Entry(Ctx, E, quick()), E);
}

TEST(SymSynth, ProductionEntryPointFailsClosedForAnEmptyLegacyVerifier) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) + ((x >> 2) >> 2)");
  SynthVerifyFn Missing;

  SynthResult R = synthesize(Ctx, E, quick(), Missing);
  EXPECT_EQ(R.Expr, E);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Outcome, SynthOutcome::ProofIncomplete);
  EXPECT_EQ(R.Verification, SynthVerification::Unknown);
  EXPECT_EQ(R.Work, 0u);
  EXPECT_EQ(R.ProofQueries, 0u);
  EXPECT_EQ(R.Evidence, SynthEvidence::None);
}

TEST(SymSynth, ProductionRejectsASampleOnlyCandidate) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) + ((x >> 2) >> 2)");

  EXPECT_EQ(synthesizeEquivalent(Ctx, E, quick()), E)
      << "a sample-only candidate must not enter a production rewrite";
}

unsigned HookCalls = 0;

SynthVerification provesEquivalent(SymContext &, SymRef, SymRef) {
  ++HookCalls;
  return SynthVerification::Equivalent;
}

SynthVerification findsCounterexample(SymContext &, SymRef, SymRef) {
  ++HookCalls;
  return SynthVerification::Different;
}

SynthVerification refutesOneCandidate(SymContext &, SymRef, SymRef) {
  ++HookCalls;
  return HookCalls == 1 ? SynthVerification::Different
                        : SynthVerification::Equivalent;
}

SynthVerification cannotDecide(SymContext &, SymRef, SymRef) {
  ++HookCalls;
  return SynthVerification::Unknown;
}

SynthVerification cannotDecideOnce(SymContext &, SymRef, SymRef) {
  ++HookCalls;
  return HookCalls == 1 ? SynthVerification::Unknown
                        : SynthVerification::Equivalent;
}

TEST(SymSynth, DoesNotVerifyCandidatesAfterBudgetIsExhausted) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) - ((x >> 2) >> 2)");

  SynthOptions Opts = quick();
  Opts.MaxWork = 0;
  HookCalls = 0;
  SynthResult R = synthesize(Ctx, E, Opts, provesEquivalent);

  EXPECT_EQ(HookCalls, 0u);
  EXPECT_EQ(R.ProofQueries, 0u);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Outcome, SynthOutcome::BudgetExhausted)
      << synthOutcomeName(R.Outcome);
}

TEST(SymSynth, RecordsThatASuppliedProcedureBackedTheRewrite) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) + ((x >> 2) >> 2)");
  SymRef Want = parsed(Ctx, "2 * (x >> 4)");

  HookCalls = 0;
  SynthResult R = synthesize(Ctx, E, quick(), provesEquivalent);
  EXPECT_EQ(R.Expr, Want) << Ctx.toString(R.Expr);
  EXPECT_EQ(R.Evidence, SynthEvidence::Verifier)
      << synthEvidenceName(R.Evidence);
  EXPECT_EQ(R.Verification, SynthVerification::Equivalent)
      << synthVerificationName(R.Verification);
  EXPECT_EQ(R.ProofQueries, 1u);
  EXPECT_EQ(HookCalls, 1u) << "the hook decides candidates, not every term";
}

TEST(SymSynth, IdentityCandidateNeverReportsAChange) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "x + y");
  SynthOptions Opts = quick();
  Opts.AllowGrowth = true;

  HookCalls = 0;
  SynthResult R = synthesize(Ctx, E, Opts, provesEquivalent);
  EXPECT_EQ(R.Expr, E);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Outcome, SynthOutcome::AlreadyShortest);
  EXPECT_EQ(R.Evidence, SynthEvidence::None);
  EXPECT_EQ(R.Verification, SynthVerification::Equivalent);
  EXPECT_EQ(R.ProofQueries, 1u);
  EXPECT_EQ(HookCalls, 1u);
}

TEST(SymSynth, ProductionRewriteAcceptsOnlyAProvedEquivalent) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) + ((x >> 2) >> 2)");
  SymRef Want = parsed(Ctx, "2 * (x >> 4)");

  EXPECT_EQ(synthesizeEquivalent(Ctx, E, quick(), provesEquivalent), Want);
  EXPECT_EQ(synthesizeEquivalent(Ctx, E, quick(), findsCounterexample), E);
  EXPECT_EQ(synthesizeEquivalent(Ctx, E, quick(), cannotDecide), E);
}

TEST(SymSynth, ReportsWhenTheProcedureFindsACounterexample) {
  // A concrete difference is actionable: unlike a proof that merely ran out
  // of resources, it tells the caller that retrying the same candidate cannot
  // turn it into a rewrite.
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) + ((x >> 2) >> 2)");

  HookCalls = 0;
  SynthResult R = synthesize(Ctx, E, quick(), findsCounterexample);
  EXPECT_EQ(R.Expr, E) << Ctx.toString(R.Expr);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Outcome, SynthOutcome::Counterexample)
      << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Verification, SynthVerification::Different)
      << synthVerificationName(R.Verification);
  EXPECT_GT(HookCalls, 0u);
  EXPECT_EQ(R.ProofQueries, HookCalls);
}

TEST(SymSynth, KeepsSearchingAfterOneCandidateHasACounterexample) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) + ((x >> 2) >> 2)");

  HookCalls = 0;
  SynthResult R = synthesize(Ctx, E, quick(), refutesOneCandidate);
  EXPECT_TRUE(R.Changed);
  EXPECT_EQ(R.Outcome, SynthOutcome::Synthesized);
  EXPECT_EQ(R.Verification, SynthVerification::Equivalent);
  EXPECT_EQ(HookCalls, 2u);
  EXPECT_EQ(R.ProofQueries, 2u);
}

TEST(SymSynth, ReportsWhenProofIsIncomplete) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) + ((x >> 2) >> 2)");

  HookCalls = 0;
  SynthResult R = synthesize(Ctx, E, quick(), cannotDecide);
  EXPECT_EQ(R.Expr, E) << Ctx.toString(R.Expr);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Outcome, SynthOutcome::ProofIncomplete)
      << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Verification, SynthVerification::Unknown)
      << synthVerificationName(R.Verification);
  EXPECT_GT(HookCalls, 0u);
  EXPECT_EQ(R.ProofQueries, 1u);
}

TEST(SymSynth, StopsSearchingWhenProofIsIncomplete) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) + ((x >> 2) >> 2)");

  SynthOptions Opts = quick();
  Opts.UseStochasticFallback = true;
  HookCalls = 0;
  SynthResult R = synthesize(Ctx, E, Opts, cannotDecideOnce);

  EXPECT_EQ(HookCalls, 1u);
  EXPECT_EQ(R.Outcome, SynthOutcome::ProofIncomplete)
      << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Verification, SynthVerification::Unknown);
  EXPECT_EQ(R.ProofQueries, 1u);
}

TEST(SymSynth, ProofIncompleteOutranksTheBudgetSpentCheckingIt) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) - ((x >> 2) >> 2)");

  SynthOptions Opts = quick();
  Opts.MaxWork = Opts.VerifySamples;
  HookCalls = 0;
  SynthResult R = synthesize(Ctx, E, Opts, cannotDecide);

  ASSERT_EQ(HookCalls, 1u) << "the budget must reach exactly one proof call";
  EXPECT_EQ(R.Work, Opts.MaxWork);
  EXPECT_EQ(R.Outcome, SynthOutcome::ProofIncomplete)
      << synthOutcomeName(R.Outcome);
  EXPECT_EQ(R.Verification, SynthVerification::Unknown);
  EXPECT_EQ(R.ProofQueries, 1u);
}

//===----------------------------------------------------------------------===//
// The sampler
//===----------------------------------------------------------------------===//

TEST(SymSynth, ZeroStochasticSlotsDisablesFallback) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x ^ y) + 2 * (x & y)");

  SynthOptions Opts = quick();
  Opts.MaxCost = 1;
  Opts.UseStochasticFallback = true;
  Opts.StochasticSlots = 0;
  SynthResult R = discoverSynthesisCandidate(Ctx, E, Opts);

  EXPECT_EQ(R.Expr, E);
  EXPECT_FALSE(R.Changed);
  EXPECT_FALSE(R.FromSampler);
  EXPECT_EQ(R.Outcome, SynthOutcome::AlreadyShortest);
  EXPECT_EQ(R.Work, 0u);
}

TEST(SymSynth, EachRandomRestartConsumesWork) {
  auto workFor = [](unsigned Restarts) {
    SymContext Ctx;
    SymRef E = parsed(Ctx, "x + x + y");
    SynthOptions Opts = quick();
    Opts.MaxCost = 1;
    Opts.MaxConstants = 0;
    Opts.MaxWork = 100;
    Opts.UseStochasticFallback = true;
    Opts.StochasticSlots = 1;
    Opts.StochasticRestarts = Restarts;
    Opts.StochasticIterations = 0;
    return discoverSynthesisCandidate(Ctx, E, Opts).Work;
  };

  const size_t OneRestart = workFor(1);
  const size_t FourRestarts = workFor(4);
  EXPECT_EQ(FourRestarts, OneRestart + 3);
  EXPECT_LE(FourRestarts, 100u);
}

TEST(SymSynth, SamplerStorageIsBoundedByRemainingWork) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x ^ y) + 2 * (x & y)");
  SynthOptions Opts = quick();
  Opts.MaxCost = 1;
  Opts.MaxWork = 1;
  Opts.UseStochasticFallback = true;
  Opts.StochasticSlots = std::numeric_limits<unsigned>::max();
  Opts.StochasticRestarts = 1;
  Opts.StochasticIterations = 0;

  SynthResult R = discoverSynthesisCandidate(Ctx, E, Opts);
  EXPECT_EQ(R.Expr, E);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.Outcome, SynthOutcome::BudgetExhausted);
  EXPECT_EQ(R.Work, Opts.MaxWork);
}

TEST(SymSynth, FindsWithTheSamplerWhatTheEnumerationIsNotAllowedToReach) {
  // Held to terminals only, the enumeration cannot build anything at all; the
  // sampler is the only thing that can answer.  Both halves are asserted,
  // because "the fallback found it" means nothing unless the enumeration
  // really could not.
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x ^ y) + 2 * (x & y)");
  SymRef Want = parsed(Ctx, "x + y");

  SynthOptions Opts = quick();
  Opts.MaxCost = 1;

  SynthResult Without = discoverSynthesisCandidate(Ctx, E, Opts);
  EXPECT_EQ(Without.Expr, E) << Ctx.toString(Without.Expr);

  Opts.UseStochasticFallback = true;
  SynthResult With = discoverSynthesisCandidate(Ctx, E, Opts);
  ASSERT_EQ(With.Outcome, SynthOutcome::Synthesized)
      << synthOutcomeName(With.Outcome);
  EXPECT_TRUE(With.FromSampler);
  EXPECT_EQ(With.Expr, Want) << Ctx.toString(With.Expr);
  EXPECT_TRUE(behavesTheSame(Ctx, E, With.Expr));
}

//===----------------------------------------------------------------------===//
// Reproducibility
//===----------------------------------------------------------------------===//

TEST(SymSynth, GivesTheSameAnswerTwiceForOneSeed) {
  // Two runs in two contexts, so nothing carries over between them but the
  // seed.  Answers are compared as text because two contexts have no reason to
  // number their nodes alike.
  auto run = [](uint64_t Seed) {
    SymContext Ctx;
    SymRef E = parsed(Ctx, "(x >> 4) + ((x >> 2) >> 2)");
    SynthOptions Opts = quick();
    Opts.UseStochasticFallback = true;
    Opts.Seed = Seed;
    SymRef Got = discoverSynthesisCandidate(Ctx, E, Opts).Expr;
    EXPECT_NE(Got, E) << "nothing was rewritten, so nothing is being compared";
    return Ctx.toString(Got);
  };

  EXPECT_EQ(run(1), run(1));
  EXPECT_EQ(run(0x9E3779B97F4A7C15ull), run(0x9E3779B97F4A7C15ull));
}

TEST(SymSynth, ReportsWhatItSpent) {
  SymContext Ctx;
  SymRef E = parsed(Ctx, "(x >> 4) + ((x >> 2) >> 2)");

  SynthResult R = discoverSynthesisCandidate(Ctx, E, quick());
  EXPECT_GT(R.Work, 0u);
  EXPECT_LE(R.Work, quick().MaxWork);
  EXPECT_EQ(R.NumLeaves, 1u);
  EXPECT_EQ(R.CandidateCost, 5u) << "2 * (x >> 4) is five nodes";
}

} // namespace
