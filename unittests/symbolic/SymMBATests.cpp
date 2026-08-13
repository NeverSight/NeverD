//===- SymMBATests.cpp - Mixed boolean-arithmetic simplification ----------===//
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
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

#include <random>

using namespace neverd::symbolic;

namespace {

constexpr uint32_t W32 = 32;

/// Simplify \p Text and return what the solver made of it.
std::string simplified(llvm::StringRef Text, uint32_t Width = W32,
                       const MBAOptions &Opts = {}) {
  SymContext Ctx;
  SymParseResult Parsed = parseSymExpr(Ctx, Text, Width);
  EXPECT_TRUE(Parsed.ok()) << Text.str() << ": " << Parsed.Error;
  if (!Parsed.ok())
    return "<parse error>";
  return Ctx.toString(simplifyMBA(Ctx, Parsed.Root, Opts).Expr);
}

/// Assert that \p Text simplifies to something denoting the same node as
/// \p Want, comparing interned nodes rather than spelling.
void simplifiesTo(llvm::StringRef Text, llvm::StringRef Want,
                  uint32_t Width = W32) {
  SymContext Ctx;
  SymParseResult Parsed = parseSymExpr(Ctx, Text, Width);
  ASSERT_TRUE(Parsed.ok()) << Text.str() << ": " << Parsed.Error;
  SymParseResult Expected = parseSymExpr(Ctx, Want, Width);
  ASSERT_TRUE(Expected.ok()) << Want.str() << ": " << Expected.Error;

  MBAResult R = simplifyMBA(Ctx, Parsed.Root);
  EXPECT_EQ(R.Expr, Expected.Root)
      << Text.str() << "\n  became: " << Ctx.toString(R.Expr)
      << "\n  wanted: " << Ctx.toString(Expected.Root);
}

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

//===----------------------------------------------------------------------===//
// Randomised checking
//===----------------------------------------------------------------------===//

/// Do two expressions agree at a spread of random points?
bool sameValue(const SymContext &Ctx, SymRef A, SymRef B, uint32_t Width) {
  SymEvalPlan PlanA(Ctx, A);
  SymEvalPlan PlanB(Ctx, B);
  std::vector<llvm::APInt> Assignment(Ctx.numVars(), llvm::APInt(Width, 0));
  std::mt19937_64 Rng(0x1234ABCD);
  for (unsigned Sample = 0; Sample < 48; ++Sample) {
    for (llvm::APInt &V : Assignment)
      V = llvm::APInt(Width, Rng(), /*isSigned=*/false, /*implicitTrunc=*/true);
    if (PlanA.eval(Assignment) != PlanB.eval(Assignment))
      return false;
  }
  return true;
}

/// Grow a random linear MBA that is equal to \p Seed by construction, by
/// repeatedly rewriting a term with an identity that preserves its value.
/// Whatever comes out, the solver has to bring back.
class Obfuscator {
public:
  Obfuscator(SymContext &Ctx, uint32_t Width, uint64_t Seed)
      : Ctx(Ctx), Width(Width), Rng(Seed) {}

  SymRef expand(SymRef E, unsigned Rounds) {
    for (unsigned I = 0; I < Rounds; ++I)
      E = once(E);
    return E;
  }

private:
  SymRef once(SymRef E) {
    SymRef X = E;
    SymRef Y = atom();
    switch (Rng() % 6) {
    case 0: // x  ->  (x ^ y) + 2*(x & y) - y
      return Ctx.mkSub(
          Ctx.mkAdd(Ctx.mkXor(X, Y),
                    Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkAnd(X, Y))),
          Y);
    case 1: // x  ->  (x | y) + (x & y) - y
      return Ctx.mkSub(Ctx.mkAdd(Ctx.mkOr(X, Y), Ctx.mkAnd(X, Y)), Y);
    case 2: // x  ->  (x ^ y) + (x & y) - (~x & y)
      return Ctx.mkSub(Ctx.mkAdd(Ctx.mkXor(X, Y), Ctx.mkAnd(X, Y)),
                       Ctx.mkAnd(Ctx.mkNot(X), Y));
    case 3: // x  ->  -(~x) - 1
      return Ctx.mkSub(Ctx.mkNeg(Ctx.mkNot(X)), Ctx.mkOne(Width));
    case 4: // x  ->  (x | y) - (~x & y)
      return Ctx.mkSub(Ctx.mkOr(X, Y), Ctx.mkAnd(Ctx.mkNot(X), Y));
    default: // x  ->  x + (y ^ y)
      return Ctx.mkAdd(X, Ctx.mkXor(Y, Y));
    }
  }

  SymRef atom() {
    static constexpr const char *Names[] = {"a", "b", "c"};
    return Ctx.mkVar(Names[Rng() % 3], Width);
  }

  SymContext &Ctx;
  uint32_t Width;
  std::mt19937_64 Rng;
};

TEST(SymMBA, BringsBackWhateverTheIdentitiesWereUsedToHide) {
  for (uint32_t Width : {8u, 32u, 64u}) {
    for (unsigned Trial = 0; Trial < 40; ++Trial) {
      SymContext Ctx;
      SymRef A = Ctx.mkVar("a", Width);
      SymRef B = Ctx.mkVar("b", Width);
      SymRef Original = Ctx.mkAdd(A, B);

      Obfuscator Obf(Ctx, Width, 0xB16B00B5ull + Trial * 131 + Width);
      SymRef Hidden = Obf.expand(Original, 4);
      if (Hidden == Original)
        continue;
      // A mistyped identity would look exactly like a solver that returns the
      // wrong answer, so the obfuscation is checked before the solver is.
      ASSERT_TRUE(sameValue(Ctx, Hidden, Original, Width))
          << "the obfuscation itself changed the value: "
          << Ctx.toString(Hidden);

      // Four rounds of wrapping is four layers, and only the deep walk reaches
      // past the first: each identity buries the previous one inside a bitwise
      // operator, where a single measurement can only treat it as an input.
      MBAResult R = simplifyMBADeep(Ctx, Hidden);
      EXPECT_EQ(R.Expr, Original) << "width " << Width << ", trial " << Trial
                                  << "\n  hidden: " << Ctx.toString(Hidden)
                                  << "\n   found: " << Ctx.toString(R.Expr);
    }
  }
}

TEST(SymMBA, NeverChangesWhatAnExpressionComputes) {
  // The size guard means most random expressions come back untouched, so the
  // point here is not the rewriting rate but that no rewrite is ever wrong.
  constexpr uint32_t Width = 32;
  SymContext Ctx;
  llvm::SmallVector<SymRef, 3> Vars{
      Ctx.mkVar("a", Width), Ctx.mkVar("b", Width), Ctx.mkVar("c", Width)};
  std::mt19937_64 Rng(0xC0FFEE);

  auto build = [&](auto &&Self, unsigned Depth) -> SymRef {
    if (Depth == 0)
      return Rng() % 4 == 0 ? Ctx.mkConst(Width, Rng()) : Vars[Rng() % 3];
    SymRef L = Self(Self, Depth - 1);
    SymRef R = Self(Self, Depth - 1);
    switch (Rng() % 8) {
    case 0:
      return Ctx.mkAdd(L, R);
    case 1:
      return Ctx.mkSub(L, R);
    case 2:
      return Ctx.mkAnd(L, R);
    case 3:
      return Ctx.mkOr(L, R);
    case 4:
      return Ctx.mkXor(L, R);
    case 5:
      return Ctx.mkNot(L);
    case 6:
      // A product of two unknowns, so the degree-two expansion is exercised
      // alongside everything else.
      return Ctx.mkMul(L, R);
    default:
      return Ctx.mkMul(Ctx.mkConst(Width, Rng() % 8), L);
    }
  };

  std::vector<llvm::APInt> Assignment;
  for (unsigned Trial = 0; Trial < 300; ++Trial) {
    SymRef E = build(build, 4);
    MBAResult R = simplifyMBA(Ctx, E);
    if (!R.Changed)
      continue;
    // A rewrite may come back the same cost as what it replaced — a tie goes
    // to the measured form — but never a worse one.
    EXPECT_LE(R.SizeAfter, R.SizeBefore);

    Assignment.assign(Ctx.numVars(), llvm::APInt(Width, 0));
    SymEvalPlan Before(Ctx, E);
    SymEvalPlan After(Ctx, R.Expr);
    for (unsigned Sample = 0; Sample < 32; ++Sample) {
      for (llvm::APInt &V : Assignment)
        V = llvm::APInt(Width, Rng(), /*isSigned=*/false,
                        /*implicitTrunc=*/true);
      EXPECT_EQ(Before.eval(Assignment), After.eval(Assignment))
          << "trial " << Trial << "\n  before: " << Ctx.toString(E)
          << "\n   after: " << Ctx.toString(R.Expr);
    }
  }
}

//===----------------------------------------------------------------------===//
// Mixed widths, which real code is full of
//===----------------------------------------------------------------------===//

TEST(SymMBA, SurvivesAnExpressionThatMixesWidths) {
  // Real lifted code puts bytes, words and vector lanes in one function, so the
  // deep walk measures a wide region whose subterms are narrower.  Two things
  // used to fault there: a mask collected from a narrow subterm was intersected
  // with a column bitmask at the region's width, and a narrow subterm was given
  // a placeholder at the region's width and then rebuilt into its narrower
  // parent.  Neither may crash, and whatever comes back must still compute what
  // it replaced.
  SymContext Ctx;
  SymRef A = Ctx.mkVar("a", 32);
  SymRef B = Ctx.mkVar("b", 64);
  SymRef C = Ctx.mkVar("c", 64);

  // A narrow masked term widened into a wide sum, sitting next to a wide
  // product -- the two shapes the two faults needed.
  SymRef Narrow = Ctx.mkAnd(A, Ctx.mkConst(32, 0xff));
  SymRef Root = Ctx.mkAdd(Ctx.mkZExt(Narrow, 64), Ctx.mkMul(B, C));

  MBAResult R = simplifyMBADeep(Ctx, Root);

  SymEvalPlan Before(Ctx, Root);
  SymEvalPlan After(Ctx, R.Expr);
  std::vector<llvm::APInt> Assignment;
  for (size_t I = 0; I < Ctx.numVars(); ++I)
    Assignment.emplace_back(Ctx.varInfo(uint32_t(I)).Width, 0);
  std::mt19937_64 Rng(0xA11CE);
  for (unsigned S = 0; S < 16; ++S) {
    for (size_t I = 0; I < Assignment.size(); ++I) {
      uint32_t W = Ctx.varInfo(uint32_t(I)).Width;
      llvm::SmallVector<uint64_t, 4> Words((W + 63) / 64);
      for (uint64_t &Word : Words)
        Word = Rng();
      Assignment[I] = llvm::APInt(W, Words);
    }
    EXPECT_EQ(Before.eval(Assignment), After.eval(Assignment));
  }
}

TEST(SymMBA, RecoversAMaskedTermWidenedIntoAWiderSum) {
  // The narrow masked carry-save addition still collapses on its own, even
  // when it reaches the solver only as a subterm of something wider.
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8);
  SymRef Y = Ctx.mkVar("y", 8);
  SymRef Carry = Ctx.mkMul(Ctx.mkAnd(X, Y), Ctx.mkConst(8, 2));
  SymRef Mba =
      Ctx.mkAnd(Ctx.mkAdd(Ctx.mkXor(X, Y), Carry), Ctx.mkConst(8, 0x7f));
  SymRef Root = Ctx.mkAdd(Ctx.mkZExt(Mba, 32), Ctx.mkVar("z", 32));

  MBAResult R = simplifyMBADeep(Ctx, Root);
  // The narrow `((x ^ y) + 2*(x & y)) & 0x7f` inside is `(x + y) & 0x7f`; the
  // whole thing gets shorter without the wide sum having to be understood.
  EXPECT_TRUE(R.Changed);

  SymEvalPlan Before(Ctx, Root);
  SymEvalPlan After(Ctx, R.Expr);
  std::vector<llvm::APInt> Assignment;
  for (size_t I = 0; I < Ctx.numVars(); ++I)
    Assignment.emplace_back(Ctx.varInfo(uint32_t(I)).Width, 0);
  std::mt19937_64 Rng(0xBEEF);
  for (unsigned S = 0; S < 16; ++S) {
    for (size_t I = 0; I < Assignment.size(); ++I) {
      uint32_t W = Ctx.varInfo(uint32_t(I)).Width;
      Assignment[I] = llvm::APInt(W, Rng(), /*isSigned=*/false,
                                  /*implicitTrunc=*/true);
    }
    EXPECT_EQ(Before.eval(Assignment), After.eval(Assignment));
  }
}

//===----------------------------------------------------------------------===//
// The gates a release has to clear
//===----------------------------------------------------------------------===//

/// A random expression over \p Vars, built from the operators obfuscation uses.
SymRef randomExpr(SymContext &Ctx, llvm::ArrayRef<SymRef> Vars, uint32_t Width,
                  unsigned Depth, std::mt19937_64 &Rng) {
  if (Depth == 0)
    return Rng() % 4 == 0 ? Ctx.mkConst(Width, Rng())
                          : Vars[Rng() % Vars.size()];
  SymRef L = randomExpr(Ctx, Vars, Width, Depth - 1, Rng);
  SymRef R = randomExpr(Ctx, Vars, Width, Depth - 1, Rng);
  switch (Rng() % 9) {
  case 0:
    return Ctx.mkAdd(L, R);
  case 1:
    return Ctx.mkSub(L, R);
  case 2:
    return Ctx.mkAnd(L, R);
  case 3:
    return Ctx.mkOr(L, R);
  case 4:
    return Ctx.mkXor(L, R);
  case 5:
    return Ctx.mkNot(L);
  case 6:
    return Ctx.mkMul(L, R);
  case 7:
    // A constant mask, so the column split is exercised alongside everything
    // else rather than only where a test names it.
    return Ctx.mkAnd(L, Ctx.mkConst(Width, Rng()));
  default:
    return Ctx.mkMul(Ctx.mkConst(Width, Rng() % 8), L);
  }
}

TEST(SymMBA, IsExhaustivelyEquivalentAtASmallWidth) {
  // The coefficient derivation is what authorizes a rewrite.  Enumerating every
  // input independently checks its implementation, and a narrow word makes
  // that affordable: three four-bit inputs is 4096 points, checked in full.  It
  // is worth doing at a narrow width because none of the algebra depends on the
  // width -- the same derivation runs at 4 bits and at 256 -- so a fault in it
  // has nowhere to hide here.
  //
  // This is the equivalence gate: it has to find nothing, every time.
  constexpr uint32_t Width = 4;
  SymContext Ctx;
  llvm::SmallVector<SymRef, 3> Vars{
      Ctx.mkVar("p", Width), Ctx.mkVar("q", Width), Ctx.mkVar("r", Width)};
  std::mt19937_64 Rng(0x5EED);

  unsigned Rewrites = 0;
  for (unsigned Trial = 0; Trial < 400; ++Trial) {
    SymRef E = randomExpr(Ctx, Vars, Width, 3, Rng);
    MBAResult R = simplifyMBADeep(Ctx, E);
    if (!R.Changed)
      continue;
    ++Rewrites;

    SymEvalPlan Before(Ctx, E);
    SymEvalPlan After(Ctx, R.Expr);
    std::vector<llvm::APInt> Assignment(Ctx.numVars(), llvm::APInt(Width, 0));
    const unsigned Points = 1u << Width;
    for (unsigned A = 0; A < Points; ++A)
      for (unsigned B = 0; B < Points; ++B)
        for (unsigned C = 0; C < Points; ++C) {
          Assignment[Ctx.varId(Vars[0])] = llvm::APInt(Width, A);
          Assignment[Ctx.varId(Vars[1])] = llvm::APInt(Width, B);
          Assignment[Ctx.varId(Vars[2])] = llvm::APInt(Width, C);
          ASSERT_EQ(Before.eval(Assignment), After.eval(Assignment))
              << "trial " << Trial << " at (" << A << "," << B << "," << C
              << ")\n  before: " << Ctx.toString(E)
              << "\n   after: " << Ctx.toString(R.Expr);
        }
  }
  // A run that rewrote nothing would pass the loop above without checking
  // anything, so the gate has to know work was done.
  EXPECT_GT(Rewrites, 50u) << "the corpus stopped exercising the solver";
}

TEST(SymMBA, ReachesThroughDeeplyNestedObfuscation) {
  // Depth is not a budget.  An obfuscator can always wrap one more layer, so a
  // walk that gives up at some nesting gives up on exactly the input it exists
  // for.  Each layer here is `-(~e) - 1`, which is `e` and which the builders
  // cannot fold, because complement is primitive to them.
  constexpr unsigned kLayers = 256;
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef E = X;
  for (unsigned I = 0; I < kLayers; ++I)
    E = Ctx.mkSub(Ctx.mkNeg(Ctx.mkNot(E)), Ctx.mkOne(W32));
  ASSERT_NE(E, X) << "the builders folded the layers away before the solver saw"
                     " them";

  MBAResult R = simplifyMBADeep(Ctx, E);
  EXPECT_TRUE(R.Changed);
  EXPECT_EQ(R.Expr, X) << "left " << Ctx.toString(R.Expr);
}

TEST(SymMBA, ExpandsPolynomialTermsWithoutANestingLimit) {
  // Term collection used to recurse through at most 32 alternating sums and
  // scaled products.  That was a shape limit rather than a resource budget:
  // adding one harmless layer could make a polynomial identity unreachable.
  // The explicit worklist must reach the product at the bottom regardless of
  // nesting.
  constexpr unsigned kLayers = 64;
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef Product = Ctx.mkAdd(
      Ctx.mkMul(Ctx.mkAnd(X, Y), Ctx.mkOr(X, Y)),
      Ctx.mkMul(Ctx.mkAnd(X, Ctx.mkNot(Y)), Ctx.mkAnd(Ctx.mkNot(X), Y)));
  SymRef E = Product;
  for (unsigned I = 0; I < kLayers; ++I)
    E = Ctx.mkMul(Ctx.mkConst(W32, 3), Ctx.mkAdd(E, Ctx.mkConst(W32, I + 1)));

  MBAResult R = simplifyMBA(Ctx, E);
  ASSERT_TRUE(R.Changed) << "left " << Ctx.toString(R.Expr);
  EXPECT_NE(Ctx.toString(R.Expr).find("x * y"), std::string::npos);

  SymEvalPlan Before(Ctx, E);
  SymEvalPlan After(Ctx, R.Expr);
  std::vector<llvm::APInt> Assignment(Ctx.numVars(), llvm::APInt(W32, 0));
  std::mt19937_64 Rng(0xD33F);
  for (unsigned Sample = 0; Sample < 16; ++Sample) {
    for (llvm::APInt &V : Assignment)
      V = llvm::APInt(W32, Rng(), /*isSigned=*/false,
                      /*implicitTrunc=*/true);
    EXPECT_EQ(Before.eval(Assignment), After.eval(Assignment));
  }
}

TEST(SymMBA, DoesNotSpendTheRegionBudgetAtOpaqueBoundaries) {
  // An unsupported operator is a boundary, not a region.  The deep walk still
  // has to simplify what is nested under it, but analysing every enclosing
  // boundary would repeatedly traverse an ever-larger prefix and turn depth
  // into quadratic work.
  constexpr unsigned kLayers = 8192;
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef Divisor = Ctx.mkConst(W32, 3);
  SymRef E = Ctx.mkAdd(Ctx.mkXor(X, Y),
                       Ctx.mkMul(Ctx.mkConst(W32, 2), Ctx.mkAnd(X, Y)));
  for (unsigned I = 0; I < kLayers; ++I)
    E = Ctx.mkUDiv(E, Divisor);

  MBAResult R = simplifyMBADeep(Ctx, E);
  ASSERT_TRUE(R.Changed);
  EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
  EXPECT_LT(R.Work, kLayers);

  SymRef Core = R.Expr;
  for (unsigned I = 0; I < kLayers; ++I) {
    ASSERT_EQ(Ctx.op(Core), SymOp::UDiv);
    EXPECT_EQ(Ctx.operand(Core, 1), Divisor);
    Core = Ctx.operand(Core, 0);
  }
  EXPECT_EQ(Core, Ctx.mkAdd(X, Y));
}

TEST(SymMBA, TerminatesOnNestingFarPastAnyBudget) {
  // Eight thousand layers is past what the default work budget covers, so the
  // answer here is not that everything collapses but that the walk comes back
  // at all -- iteratively, without running the stack out -- and that whatever
  // it returns still computes what it replaced.
  constexpr unsigned kLayers = 8192;
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef E = X;
  for (unsigned I = 0; I < kLayers; ++I)
    E = Ctx.mkSub(Ctx.mkNeg(Ctx.mkNot(E)), Ctx.mkOne(W32));

  MBAResult R = simplifyMBADeep(Ctx, E);
  SymEvalPlan Before(Ctx, E);
  SymEvalPlan After(Ctx, R.Expr);
  std::vector<llvm::APInt> Assignment(Ctx.numVars(), llvm::APInt(W32, 0));
  std::mt19937_64 Rng(0xD00D);
  for (unsigned Sample = 0; Sample < 8; ++Sample) {
    for (llvm::APInt &V : Assignment)
      V = llvm::APInt(W32, Rng(), /*isSigned=*/false, /*implicitTrunc=*/true);
    EXPECT_EQ(Before.eval(Assignment), After.eval(Assignment));
  }
}

TEST(SymMBA, HandlesAGraphWhoseTreeIsExponentiallyLarger) {
  // Every layer names the one below it twice, so twenty layers is a graph of a
  // few dozen nodes denoting a tree of a million.  Anything that walks the tree
  // rather than the graph -- including the cost model, which reports a tree
  // size -- has to stay finite here.
  SymContext Ctx;
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef Z = Ctx.mkVar("z", W32);
  SymRef E = Ctx.mkVar("w", W32);
  for (unsigned I = 0; I < 20; ++I)
    E = Ctx.mkXor(Ctx.mkAnd(E, Y), Ctx.mkOr(E, Z));

  MBAResult R = simplifyMBADeep(Ctx, E);
  SymEvalPlan Before(Ctx, E);
  SymEvalPlan After(Ctx, R.Expr);
  std::vector<llvm::APInt> Assignment(Ctx.numVars(), llvm::APInt(W32, 0));
  std::mt19937_64 Rng(0xFEED);
  for (unsigned Sample = 0; Sample < 16; ++Sample) {
    for (llvm::APInt &V : Assignment)
      V = llvm::APInt(W32, Rng(), /*isSigned=*/false, /*implicitTrunc=*/true);
    EXPECT_EQ(Before.eval(Assignment), After.eval(Assignment));
  }
}

TEST(SymMBA, MeasuresAWideWordTheSameWay) {
  // Nothing in the derivation mentions a width, and an EVM word is where that
  // stops being a claim and starts being load-bearing.
  simplifiesTo("(x ^ y) + 2 * (x & y)", "x + y", /*Width=*/256);
  simplifiesTo("(x & y) * (x | y) + (x & ~y) * (~x & y)", "x * y",
               /*Width=*/256);
  simplifiesTo("((x | y) - (x & y)) & 0xffffffffffffffff",
               "(x ^ y) & 0xffffffffffffffff", /*Width=*/256);
}

} // namespace
