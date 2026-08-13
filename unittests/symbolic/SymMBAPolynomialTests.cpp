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

TEST(SymMBA, NoticesWhenDegreeThreeProductsCancel) {
  // The shape an obfuscator leaves when it multiplies a cube out and adds the
  // pieces back: it looks cubic, and the cubic part is zero.
  simplifiesTo("x * (y & z) * (y | z) + x * (y & ~z) * (~y & z) - x * y * z + "
               "(x | y) - (x & y)",
               "x ^ y");
}
} // namespace
