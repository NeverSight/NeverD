//===- SymMBAOutcomeTests.cpp - What the MBA solver reports ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The answers a caller acts on: which outcome a refusal carries, what stands
/// behind a rewrite, and that none of it depends on the word width.
///
//===----------------------------------------------------------------------===//

#include "SymMBATestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

using namespace neverd::symbolic;

namespace {

using test::simplified;
using test::simplifiesTo;
using test::W32;

//===----------------------------------------------------------------------===//
// What the solver reports about what it did
//===----------------------------------------------------------------------===//

TEST(SymMBA, TellsTheReasonsForLeavingAnExpressionAlone) {
  // "Unchanged" covers three different answers, and a caller deciding whether
  // to spend more has to be able to tell them apart.
  SymContext Ctx;
  auto outcomeOf = [&](llvm::StringRef Text, const MBAOptions &Opts = {}) {
    SymParseResult P = parseSymExpr(Ctx, Text, W32);
    EXPECT_TRUE(P.ok()) << Text.str() << ": " << P.Error;
    return simplifyMBA(Ctx, P.Root, Opts).Outcome;
  };

  // Nothing to measure: no input the algebra can drive.
  EXPECT_EQ(outcomeOf("42"), MBAOutcome::NotApplicable);
  // Measured, and nothing shorter exists.
  EXPECT_EQ(outcomeOf("x + y"), MBAOutcome::AlreadyShortest);
  // Refused for width, which is the one refusal a larger budget would undo.
  MBAOptions Tight;
  Tight.MaxAtoms = 2;
  EXPECT_EQ(outcomeOf("x + y + z", Tight), MBAOutcome::TooManyInputs);

  MBAOptions SmallSearch;
  SmallSearch.MaxWork = 1;
  EXPECT_EQ(outcomeOf("x * (y & z) * (y | z) + "
                      "x * (y & ~z) * (~y & z)",
                      SmallSearch),
            MBAOutcome::BudgetExhausted);
}

TEST(SymMBA, SaysWhatStandsBehindARewrite) {
  SymContext Ctx;
  MBAOptions ProofOnly;
  ProofOnly.VerifySamples = 0;
  auto resultOf = [&](llvm::StringRef Text) {
    SymParseResult P = parseSymExpr(Ctx, Text, W32);
    EXPECT_TRUE(P.ok()) << Text.str() << ": " << P.Error;
    return simplifyMBA(Ctx, P.Root, ProofOnly);
  };

  // No samples are run in this test.  Every accepted result therefore has to
  // clear its deterministic coefficient verifier on its own.
  MBAResult Measured = resultOf("(x ^ y) + 2 * (x & y)");
  EXPECT_EQ(Measured.Outcome, MBAOutcome::Rewritten);
  EXPECT_EQ(Measured.Evidence, MBAEvidence::Derivation);
  EXPECT_GT(Measured.Work, 0u);

  // A mask column split is accepted only when every path above each mask is
  // bitwise and therefore cannot carry information across columns.  Sampling
  // is only a defect net for that structural derivation.
  MBAResult Masked = resultOf("((x ^ y) + 2 * (x & y)) & 0xff");
  EXPECT_EQ(Masked.Outcome, MBAOutcome::Rewritten);
  EXPECT_EQ(Masked.Evidence, MBAEvidence::Derivation);

  MBAResult Polynomial = resultOf("(x & y) * (x | y) + (x & ~y) * (~x & y)");
  EXPECT_EQ(Polynomial.Outcome, MBAOutcome::Rewritten);
  EXPECT_EQ(Polynomial.Evidence, MBAEvidence::Derivation);
}

//===----------------------------------------------------------------------===//
// What the solver must refuse to do
//===----------------------------------------------------------------------===//

TEST(SymMBA, LeavesAnExpressionAloneWhenNoFormIsShorter) {
  SymContext Ctx;
  for (const char *Text : {"x + y", "x ^ y", "x & y | z", "x", "42"}) {
    SymParseResult P = parseSymExpr(Ctx, Text, W32);
    ASSERT_TRUE(P.ok());
    MBAResult R = simplifyMBA(Ctx, P.Root);
    EXPECT_FALSE(R.Changed) << Text << " became " << Ctx.toString(R.Expr);
    EXPECT_EQ(R.Expr, P.Root);
  }
}

TEST(SymMBA, StepsAroundWhatTheLinearTheoryDoesNotCover) {
  SymContext Ctx;
  // A quotient, a product of two unknowns, and a variable shift are all
  // outside the algebra; each becomes an input, and what surrounds it is still
  // simplified.
  SymParseResult P =
      parseSymExpr(Ctx, "((x / y) ^ z) + 2 * ((x / y) & z)", W32);
  ASSERT_TRUE(P.ok());
  MBAResult R = simplifyMBA(Ctx, P.Root);
  EXPECT_TRUE(R.Changed);
  EXPECT_EQ(Ctx.toString(R.Expr), "x / y + z");

  SymParseResult Q = parseSymExpr(Ctx, "((x * y) | z) - ((x * y) & z)", W32);
  ASSERT_TRUE(Q.ok());
  EXPECT_EQ(Ctx.toString(simplifyMBA(Ctx, Q.Root).Expr), "z ^ x * y");
}

TEST(SymMBA, TreatsAMaskInsideABitwiseOperatorAsOpaque) {
  // `x & 0xff` is not a bitwise function of x in the sense the measurement
  // needs — it tells bit positions apart — so it has to become an input rather
  // than be measured as one.  Getting this wrong would produce a confidently
  // wrong answer, so the check is that the surrounding algebra still works and
  // the mask survives untouched.
  SymContext Ctx;
  SymParseResult P =
      parseSymExpr(Ctx, "((x & 0xff) ^ y) + 2 * ((x & 0xff) & y)", W32);
  ASSERT_TRUE(P.ok());
  MBAResult R = simplifyMBA(Ctx, P.Root);
  EXPECT_TRUE(R.Changed);
  EXPECT_EQ(Ctx.toString(R.Expr), "(255 & x) + y");
}

TEST(SymMBA, DeclinesWhenThereAreMoreInputsThanItWillMeasure) {
  MBAOptions Tight;
  Tight.MaxAtoms = 2;
  SymContext Ctx;
  SymParseResult P = parseSymExpr(Ctx, "(x ^ y ^ z) + 2 * (x & y)", W32);
  ASSERT_TRUE(P.ok());
  MBAResult R = simplifyMBA(Ctx, P.Root, Tight);
  EXPECT_FALSE(R.Changed);
  EXPECT_EQ(R.NumAtoms, 0u);
}

//===----------------------------------------------------------------------===//
// Widths
//===----------------------------------------------------------------------===//

TEST(SymMBA, WorksAtEveryWidthIncludingOnesNoMachineHas) {
  for (uint32_t Width : {8u, 16u, 32u, 64u, 128u, 256u}) {
    EXPECT_EQ(simplified("(x ^ y) + 2 * (x & y)", Width), "x + y")
        << "at width " << Width;
    EXPECT_EQ(simplified("(x | y) - (x & y)", Width), "x ^ y")
        << "at width " << Width;
  }
}

TEST(SymMBA, AnEvmWordIsMeasuredLikeAnyOther) {
  // Nothing about the measurement is tied to a machine word, so a contract's
  // 256-bit arithmetic is as ordinary here as a byte.
  simplifiesTo("(x ^ y) + 2 * (x & y)", "x + y", 256);
  simplifiesTo("(x | y) + y - (~x & y)", "x + y", 256);
  simplifiesTo("~x + 1", "-x", 256);
}

TEST(SymMBA, ASingleBitWordIsNotASpecialCase) {
  // At one bit, addition is exclusive-or, doubling is zero and negation is the
  // identity, so most of these collapse before the solver is even reached.
  EXPECT_EQ(simplified("(x ^ y) + 2 * (x & y)", 1), "x ^ y");
  EXPECT_EQ(simplified("(x | y) - (x & y)", 1), "x ^ y");
  EXPECT_EQ(simplified("x & ~x", 1), "0");

  // `x + y` is exclusive-or here too, and at the same cost, so which one comes
  // back is a tie broken towards the measured form.
  EXPECT_EQ(simplified("x + y", 1), "x ^ y");
}
} // namespace
