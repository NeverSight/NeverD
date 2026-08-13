//===- SymMBATests.cpp - Linear mixed boolean-arithmetic identities -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercises the solver on the identities MBA obfuscators are built out of.
///
/// These are the interesting cases precisely because they are the ones a
/// peephole simplifier cannot touch: each is an equality between an arithmetic
/// expression and a bitwise one, so no rule stated in either algebra alone can
/// see it.
///
/// This file holds the linear theory. The polynomial identities are in
/// SymMBAPolynomialTests.cpp, what the solver reports and refuses is in
/// SymMBAOutcomeTests.cpp, and the randomised gates are in
/// SymMBAStressTests.cpp.
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
// The classic identities
//===----------------------------------------------------------------------===//

TEST(SymMBA, RecoversAdditionFromItsBitwiseRewriting) {
  // The first identity anyone reaches for, and the seed of most of the rest.
  simplifiesTo("(x ^ y) + 2 * (x & y)", "x + y");
  simplifiesTo("(x | y) + (x & y)", "x + y");
  simplifiesTo("(x | y) + y - (~x & y)", "x + y");
  simplifiesTo("2 * (x | y) - (x ^ y)", "x + y");
}

TEST(SymMBA, RecoversTheBitwiseOperatorsFromTheirArithmeticRewritings) {
  simplifiesTo("(x | y) - (x & y)", "x ^ y");
  simplifiesTo("x + y - 2 * (x & y)", "x ^ y");
  simplifiesTo("(x + y) - (x | y)", "x & y");
  simplifiesTo("(x + y) - (x ^ y) - (x & y)", "x & y");
  simplifiesTo("x + y - (x & y)", "x | y");
  simplifiesTo("(x ^ y) + (x & y)", "x | y");
}

TEST(SymMBA, RecoversSubtractionAndNegation) {
  simplifiesTo("(x ^ ~y) + 2 * (x | y) - 2 * y + 1", "x - y");
  simplifiesTo("~x + 1", "-x");
  simplifiesTo("-x - 1", "~x");
  // Complement over a sum: only reachable because `~z` is `-z - 1`, so the
  // node stays inside the algebra instead of becoming an opaque input.
  simplifiesTo("~(x - 1)", "-x");
  simplifiesTo("~(~x + y)", "x - y");
}

TEST(SymMBA, CollapsesAnExpressionThatIsSecretlyConstant) {
  // The shape an opaque predicate takes once it reaches the expression layer.
  simplifiesTo("(x | ~x) + 1", "0");
  simplifiesTo("(x & y) + (x | y) - x - y", "0");
  simplifiesTo("(x ^ y) - (x | y) + (x & y)", "0");
  simplifiesTo("x - x + 7", "7");
}

TEST(SymMBA, RecoversAnExpressionOverThreeVariables) {
  simplifiesTo("(x ^ y ^ z) + 2 * ((x & y) ^ (x & z) ^ (y & z))", "x + y + z");
  // y drops out entirely.  Measuring finds that the result cannot depend on
  // it, and synthesis then never mentions it — a rewrite no rule set written
  // over the syntax would reach.
  simplifiesTo("(x | y | z) - (x & y & z) - ((x ^ y) & (y ^ z))", "x ^ z");
}

TEST(SymMBA, HandlesACoefficientOtherThanOne) {
  simplifiesTo("3 * (x ^ y) + 6 * (x & y)", "3 * x + 3 * y");
  simplifiesTo("(x ^ y) * 2 + (x & y) * 4", "2 * x + 2 * y");
}

//===----------------------------------------------------------------------===//
// Regions too wide to measure whole
//===----------------------------------------------------------------------===//

TEST(SymMBA, RecoversASumWiderThanOneMeasurementCanReach) {
  // Nine carry-save additions side by side: eighteen inputs, well past what a
  // single 2^t sweep can afford, but no input interacts with more than one
  // other.  Measured in groups this costs 9 * 2^2 instead of 2^18, and every
  // addition comes back.
  MBAOptions Defaults;
  ASSERT_LT(Defaults.MaxAtoms, 18u) << "this has to be the wide case";

  simplifiesTo(
      "(a ^ b) + 2 * (a & b) + (c ^ d) + 2 * (c & d) + (e ^ f) + 2 * (e & f) + "
      "(g ^ h) + 2 * (g & h) + (i ^ j) + 2 * (i & j) + (k ^ l) + 2 * (k & l) + "
      "(m ^ n) + 2 * (m & n) + (o ^ p) + 2 * (o & p) + (q ^ r) + 2 * (q & r)",
      "a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p + q + r");
}

TEST(SymMBA, SizesEachMeasurementToItsOwnGroup) {
  // A three-input tangle among two-input ones, plus a constant belonging to no
  // group.  Each group has to be measured at its own width rather than the
  // whole being refused for the width of the widest.
  simplifiesTo("(x ^ y ^ z) + 2 * ((x & y) ^ (x & z) ^ (y & z)) + "
               "(a ^ b) + 2 * (a & b) + (c ^ d) + 2 * (c & d) + "
               "(e ^ f) + 2 * (e & f) + (g ^ h) + 2 * (g & h) + "
               "(i ^ j) + 2 * (i & j) + (k ^ l) + 2 * (k & l) + "
               "(m ^ n) + 2 * (m & n) + 7",
               "x + y + z + a + b + c + d + e + f + g + h + i + j + k + l + "
               "m + n + 7");
}

TEST(SymMBA, LeavesAWideExpressionWhoseInputsAreGenuinelyTangled) {
  // Every input reaches every other through the terms, so there are no
  // independent groups to measure and the width is a property of the
  // expression.  Refusing it is the honest answer; returning something
  // unverified would not be.
  const char *Tangled = "(a & b & c & d & e & f & g & h & i & j & k & l & m & "
                        "n & o & p & q & r) + (a | b | c | d | e | f | g | h | "
                        "i | j | k | l | m | n | o | p | q | r)";
  EXPECT_EQ(simplified(Tangled), simplified(Tangled))
      << "the solver must at least be deterministic about declining";
  SymContext Ctx;
  SymParseResult Parsed = parseSymExpr(Ctx, Tangled, W32);
  ASSERT_TRUE(Parsed.ok()) << Parsed.Error;
  EXPECT_FALSE(simplifyMBA(Ctx, Parsed.Root).Changed);
}

//===----------------------------------------------------------------------===//
// Regions guarded by constant masks
//===----------------------------------------------------------------------===//

TEST(SymMBA, RecoversArithmeticGuardedByAMask) {
  // A mask defeats a corner measurement -- it is the one thing that tells bit
  // positions apart -- so the obfuscation underneath one used to be out of
  // reach.  Measured a column at a time, it comes back.
  simplifiesTo("((x ^ y) + 2 * (x & y)) & 0xff", "(x + y) & 0xff");
  simplifiesTo("((x | y) - (x & y)) & 0xffff", "(x ^ y) & 0xffff");
}

TEST(SymMBA, MeasuresEachMaskColumnOnItsOwn) {
  // Two masks that overlap nowhere cut the word into three columns, two of them
  // carrying an obfuscation of their own.  Each is measured at its own width
  // and only the bits it owns survive into the answer.
  simplifiesTo("(((x ^ y) + 2 * (x & y)) & 0xff) | "
               "(((a | b) - (a & b)) & 0xff0000)",
               "((x + y) & 0xff) | ((a ^ b) & 0xff0000)");
}

TEST(SymMBA, DeclinesAMaskSplitThatACarryWouldCross) {
  // The column split is exact only while no carry crosses a boundary.  Here the
  // unmasked summand carries into the masked nibble, so a per-column answer
  // would be wrong however well each column measured.  Declining is the only
  // correct outcome, and it must follow from structure even when no samples
  // are requested.
  SymContext Ctx;
  SymParseResult Parsed =
      parseSymExpr(Ctx, "(((x ^ y) + 2 * (x & y)) & 15) + z", W32);
  ASSERT_TRUE(Parsed.ok()) << Parsed.Error;
  MBAOptions Opts;
  Opts.VerifySamples = 0;
  EXPECT_FALSE(simplifyMBA(Ctx, Parsed.Root, Opts).Changed);
}
} // namespace
