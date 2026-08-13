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

#include "neverd/symbolic/SymMBA.h"

#include "neverd/symbolic/SymParse.h"

#include "gtest/gtest.h"

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
// Degree two
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
  simplifiesTo("(x & y) * (x | y) + (x & ~y) * (~x & y) + (x ^ y) + 2 * (x & y)",
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

TEST(SymMBA, StopsAtDegreeThree) {
  // The expansion only covers products of two, so a cube stays an input and
  // whatever surrounds it is still measured.
  SymContext Ctx;
  SymParseResult P =
      parseSymExpr(Ctx, "((x * y * z) ^ w) + 2 * ((x * y * z) & w)", W32);
  ASSERT_TRUE(P.ok());
  MBAResult R = simplifyMBA(Ctx, P.Root);
  EXPECT_TRUE(R.Changed);
  EXPECT_EQ(Ctx.toString(R.Expr), "x * y * z + w");
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
  SymParseResult P = parseSymExpr(Ctx, "((x / y) ^ z) + 2 * ((x / y) & z)", W32);
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
      return Ctx.mkSub(Ctx.mkAdd(Ctx.mkXor(X, Y),
                                 Ctx.mkMul(Ctx.mkConst(Width, 2),
                                           Ctx.mkAnd(X, Y))),
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
      EXPECT_EQ(R.Expr, Original)
          << "width " << Width << ", trial " << Trial
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
  llvm::SmallVector<SymRef, 3> Vars{Ctx.mkVar("a", Width),
                                    Ctx.mkVar("b", Width),
                                    Ctx.mkVar("c", Width)};
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

} // namespace
