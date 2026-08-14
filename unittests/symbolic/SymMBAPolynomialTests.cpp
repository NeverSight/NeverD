//===- SymMBAPolynomialTests.cpp - Polynomial MBA identities --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The identities that multiply bitwise terms together. No amount of corner
/// measurement reaches these, so they are what the symbolic expansion exists
/// for.
///
//===----------------------------------------------------------------------===//

#include "SymMBATestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"

using namespace neverd::symbolic;

namespace {

using test::simplifiesTo;
using test::W32;

//===----------------------------------------------------------------------===//
// Products of bitwise terms
//===----------------------------------------------------------------------===//

TEST(SymMBA, RecoversAProductFromItsBitwiseRewriting) {
  // Multiplying out the disjoint splits of x and y gives this, and it is what
  // polynomial MBA obfuscation is built on.  No amount of corner measurement
  // reaches it — a product of bitwise terms is invisible to that — so this is
  // the symbolic expansion doing the work.
  simplifiesTo("(x & y) * (x | y) + (x & ~y) * (~x & y)", "x * y");
  simplifiesTo("3 * ((x & y) * (x | y) + (x & ~y) * (~x & y))", "3 * (x * y)");
}

TEST(SymMBA, KeepsWhateverLinearPartRidesAlongsideTheProduct) {
  simplifiesTo(
      "(x & y) * (x | y) + (x & ~y) * (~x & y) + (x ^ y) + 2 * (x & y)",
      "x * y + x + y");
}

TEST(SymMBA, NoticesWhenTheProductsCancelAndTheRestIsLinear) {
  // An obfuscator that multiplies a term out and adds the pieces back leaves a
  // quadratic-looking expression whose quadratic part is zero.
  simplifiesTo("(x | y) * (x & y) - (x & y) * (x | y) + (x ^ y)", "x ^ y");
  simplifiesTo("x * y - (x & y) * (x | y) - (x & ~y) * (~x & y)", "0");
}

TEST(SymMBA, LeavesAProductAloneWhenItIsAlreadyShortest) {
  SymContext Ctx;
  for (const char *Text : {"x * y", "x * y + z", "2 * (x * y)"}) {
    SymParseResult P = parseSymExpr(Ctx, Text, W32);
    ASSERT_TRUE(P.ok()) << Text;
    MBAResult R = simplifyMBA(Ctx, P.Root);
    EXPECT_EQ(R.Expr, P.Root) << Text << " became " << Ctx.toString(R.Expr);
  }
}

TEST(SymMBA, TreatsAProductInsideABitwiseOperatorAsOpaque) {
  // A product is not a bitwise function of the inputs, so one sitting under `^`
  // or `&` has to become an input whatever its degree.  What surrounds it is
  // still measured, and the product comes back untouched.
  SymContext Ctx;
  SymParseResult P =
      parseSymExpr(Ctx, "((x * y * z) ^ w) + 2 * ((x * y * z) & w)", W32);
  ASSERT_TRUE(P.ok());
  MBAResult R = simplifyMBA(Ctx, P.Root);
  EXPECT_TRUE(R.Changed);
  EXPECT_EQ(Ctx.toString(R.Expr), "x * y * z + w");
}

TEST(SymMBA, RecoversAProductOfThreeTerms) {
  // Obfuscating one factor of `x * y * z` leaves a sum of two degree-three
  // products.  Reading it back is the same argument the degree-two case rests
  // on, run at one higher arity: expand every factor over the minterms it
  // selects, then find the single product whose expansion is what is there.
  simplifiesTo("x * (y & z) * (y | z) + x * (y & ~z) * (~y & z)", "x * y * z");
  simplifiesTo("2 * x * (y & z) * (y | z) + 2 * x * (y & ~z) * (~y & z)",
               "2 * x * y * z");
}

TEST(SymMBA, RecoversAProductWithoutADegreeCutoff) {
  // Multiplying the two-factor identity by x three more times produces a
  // genuine degree-five region.  The representation and traversal must carry
  // it whole; only MaxWork may stop a combinatorial search.
  simplifiesTo("(x & y) * (x | y) * x * x * x + "
               "(x & ~y) * (~x & y) * x * x * x",
               "x * x * x * x * y");
}

TEST(SymMBA, RecoversAPolynomialAcrossFourInputs) {
  // Four inputs leave the globally-minimal truth-table synthesizer's range,
  // but the prime-implicant backend remains exact and the work budget keeps
  // factor search bounded.
  simplifiesTo("((x & y) & (z & w)) * ((x & y) | (z & w)) + "
               "((x & y) & ~(z & w)) * (~(x & y) & (z & w))",
               "(x & y) * (z & w)");
}

TEST(SymMBA, KeepsALinearPartAlongsideADegreeThreeProduct) {
  simplifiesTo("x * (y & z) * (y | z) + x * (y & ~z) * (~y & z) + "
               "(x ^ y) + 2 * (x & y)",
               "x * y * z + x + y");
}

TEST(SymMBA, RecoversTwoProductsOfDifferentDegreesFromOneSum) {
  // A product contributes monomials of exactly its own degree.  That was the
  // reason a target mixing degrees used to be refused, and read the other way
  // round it is the reason it need not be: the degrees cannot interact, so the
  // cubic monomials can only have come from a cubic product and the quadratic
  // ones from a quadratic product.  Matching each degree against its own part
  // of the target is a decomposition rather than a guess, and it is what
  // reaches the shape an obfuscator leaves when it expands a square and a cube
  // into the same sum.
  simplifiesTo("(x & y) * (x | y) + (x & ~y) * (~x & y) + "
               "x * (y & z) * (y | z) + x * (y & ~z) * (~y & z)",
               "x * y + x * y * z");
}

TEST(SymMBA, NoticesWhenDegreeThreeProductsCancel) {
  // The shape an obfuscator leaves when it multiplies a cube out and adds the
  // pieces back: it looks cubic, and the cubic part is zero.
  simplifiesTo("x * (y & z) * (y | z) + x * (y & ~z) * (~y & z) - x * y * z + "
               "(x | y) - (x & y)",
               "x ^ y");
}

TEST(SymMBA, PolynomialReadingHasNoSixteenInputIndexCeiling) {
  // The polynomial key used to store a minterm index in sixteen bits.  A
  // seventeen-input table has 131072 entries, so that representation could
  // not name every entry and the whole polynomial reading was skipped.  Two
  // one-minterm functions keep the regression affordable while making every
  // one of the seventeen inputs semantically relevant to the reading.
  constexpr unsigned NumAtoms = 17;
  SymContext Ctx;
  llvm::SmallVector<SymRef, NumAtoms> Positive;
  llvm::SmallVector<SymRef, NumAtoms> Negative;
  for (unsigned I = 0; I < NumAtoms; ++I) {
    llvm::SmallString<8> Name;
    Name += "x";
    Name += llvm::utostr(I);
    SymRef Var = Ctx.mkVar(Name, W32);
    Positive.push_back(Var);
    Negative.push_back(Ctx.mkNot(Var));
  }

  SymRef X = Ctx.mkAnd(Positive);
  SymRef Y = Ctx.mkAnd(Negative);
  SymRef Hidden = Ctx.mkAdd(
      Ctx.mkMul(Ctx.mkAnd(X, Y), Ctx.mkOr(X, Y)),
      Ctx.mkMul(Ctx.mkAnd(X, Ctx.mkNot(Y)), Ctx.mkAnd(Ctx.mkNot(X), Y)));
  SymRef Expected = Ctx.mkMul(X, Y);
  ASSERT_NE(Hidden, Expected);

  const MBAOptions Unlimited = MBAOptions::unlimited();
  EXPECT_EQ(Unlimited.MaxAtoms, MBAOptions::Unlimited);
  EXPECT_EQ(Unlimited.MaxSynthesisAtoms, MBAOptions::Unlimited);

  MBAResult Result = simplifyMBA(Ctx, Hidden, Unlimited);
  EXPECT_EQ(Result.Outcome, MBAOutcome::Rewritten);
  EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
  EXPECT_EQ(Result.Expr, Expected) << Ctx.toString(Result.Expr);
}
} // namespace
