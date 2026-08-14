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

#include "neverd/symbolic/SymBitwise.h"
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
// Identities over more inputs than a truth table used to hold
//===----------------------------------------------------------------------===//

/// An exclusive-or of seven inputs, dressed as `(P | Q) - (P & Q)`.
///
/// Seven is the interesting number.  A truth table was one machine word, so
/// six inputs was the most the grouped form could be written over — a limit on
/// the container rather than on the algebra, since nothing about deriving a
/// seven-input answer is harder than deriving a six-input one.  The
/// conjunction basis cannot stand in for it either: the parity's coefficients
/// over the subset lattice are all hundred-and-twenty-seven of them, far past
/// any term budget.  So the whole rewrite was abandoned.
constexpr llvm::StringLiteral kSevenInputParity =
    "(((a ^ b) ^ (c ^ d)) | ((e ^ f) ^ g)) - "
    "(((a ^ b) ^ (c ^ d)) & ((e ^ f) ^ g))";

TEST(SymMBA, RecoversAnIdentityOverMoreInputsThanAWordHeldATableFor) {
  MBAOptions AsItWas;
  AsItWas.MaxSynthesisAtoms = 6;
  SymContext Ctx;
  SymParseResult P = parseSymExpr(Ctx, kSevenInputParity, W32);
  ASSERT_TRUE(P.ok()) << P.Error;
  EXPECT_FALSE(simplifyMBA(Ctx, P.Root, AsItWas).Changed)
      << "the six-input ceiling is the whole point of this case";

  simplifiesTo(kSevenInputParity, "a ^ b ^ c ^ d ^ e ^ f ^ g");
}

TEST(SymMBA, HasNoArityAtWhichItStopsWritingTheAnswerDown) {
  // Ten inputs, to make the point that seven was not a new ceiling.  A parity
  // is also the shape that separates the two general constructions: as a union
  // of cubes it is 512 products of ten literals each, and as an exclusive-or
  // of conjunctions it is ten atoms.  Both are exact and the cheaper is kept,
  // so which family the obfuscator reached for does not decide how the answer
  // reads.
  simplifiesTo("((((a ^ b) ^ (c ^ d)) ^ ((e ^ f) ^ g)) | (h ^ (i ^ j))) - "
               "((((a ^ b) ^ (c ^ d)) ^ ((e ^ f) ^ g)) & (h ^ (i ^ j)))",
               "a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ i ^ j");
}

TEST(SymMBA, HoldsATableWiderThanTheWordThatUsedToBeOne) {
  // The container on its own, below the solver that drives it.  Every arity
  // ceiling in the solver used to trace back to this one object being a
  // machine word, so the claim that the ceilings are budgets now rests on a
  // table of ten inputs -- sixteen times a word -- being an ordinary thing to
  // build, read back, and hand to synthesis.
  SymContext Ctx;
  llvm::SmallVector<SymRef, 10> Atoms;
  for (llvm::StringRef Name :
       {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j"})
    Atoms.push_back(Ctx.mkVar(Name, W32));

  TruthTable Parity = TruthTable::zero(10);
  ASSERT_EQ(Parity.numVars(), 10u);
  ASSERT_EQ(Parity.entries(), size_t(1024));
  for (size_t K = 0; K < Parity.entries(); ++K) {
    bool Odd = false;
    for (size_t Rest = K; Rest != 0; Rest &= Rest - 1)
      Odd = !Odd;
    Parity.setValue(K, Odd);
  }
  EXPECT_EQ(Parity.count(), size_t(512));
  // Past the sixty-fourth entry is where a word-backed table stopped saying
  // anything, so reading one back is the whole point of checking here.
  EXPECT_FALSE(Parity.at(1023));
  EXPECT_TRUE(Parity.at(1022));

  // A parity is the shape that separates the two constructions above the
  // exhaustive ceiling: 512 products of ten literals as a union of cubes, ten
  // atoms as an exclusive-or.  Both are exact, so this checks the ranking
  // rather than the correctness -- and it checks that nothing on the path
  // reached for the word the table no longer is.
  std::optional<SymRef> Written = synthesizeBitwise(Ctx, Parity, Atoms);
  ASSERT_TRUE(Written.has_value());
  SymParseResult Want =
      parseSymExpr(Ctx, "a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ i ^ j", W32);
  ASSERT_TRUE(Want.ok()) << Want.Error;
  EXPECT_EQ(*Written, Want.Root) << Ctx.toString(*Written);
}

TEST(SymMBA, RefusesOptimalBitwiseSynthesisWithoutAWorkBudget) {
  SymContext Ctx;
  llvm::SmallVector<SymRef, 3> Atoms{Ctx.mkVar("x", W32), Ctx.mkVar("y", W32),
                                     Ctx.mkVar("z", W32)};

  TruthTable Parity = atomTruthTable(0, 3);
  Parity ^= atomTruthTable(1, 3);
  Parity ^= atomTruthTable(2, 3);

  BitwiseSynthesisLimits Limits;
  Limits.MaxOptimalAtoms = 3;
  Limits.MaxWork = 0;
  const size_t NodesBefore = Ctx.numNodes();

  EXPECT_FALSE(synthesizeBitwise(Ctx, Parity, Atoms, Limits).has_value());
  EXPECT_EQ(Ctx.numNodes(), NodesBefore)
      << "a refused synthesis must not build a candidate";
}

TEST(SymMBA, RefusesOptimalBitwiseSynthesisAboveTheCostBudget) {
  SymContext Ctx;
  llvm::SmallVector<SymRef, 3> Atoms{Ctx.mkVar("x", W32), Ctx.mkVar("y", W32),
                                     Ctx.mkVar("z", W32)};

  TruthTable Parity = atomTruthTable(0, 3);
  Parity ^= atomTruthTable(1, 3);
  Parity ^= atomTruthTable(2, 3);

  BitwiseSynthesisLimits Limits;
  Limits.MaxOptimalAtoms = 3;
  Limits.MaxCost = 0;
  const size_t NodesBefore = Ctx.numNodes();

  EXPECT_FALSE(synthesizeBitwise(Ctx, Parity, Atoms, Limits).has_value());
  EXPECT_EQ(Ctx.numNodes(), NodesBefore)
      << "costing a refused synthesis must precede candidate construction";
}

TEST(SymMBA, RetainsOptimalBitwiseSynthesisWithUnlimitedBudgets) {
  SymContext Ctx;
  llvm::SmallVector<SymRef, 3> Atoms{Ctx.mkVar("x", W32), Ctx.mkVar("y", W32),
                                     Ctx.mkVar("z", W32)};

  TruthTable Parity = atomTruthTable(0, 3);
  Parity ^= atomTruthTable(1, 3);
  Parity ^= atomTruthTable(2, 3);

  BitwiseSynthesisLimits Limits;
  Limits.MaxOptimalAtoms = 3;
  Limits.MaxWork = std::numeric_limits<size_t>::max();
  Limits.MaxCost = std::numeric_limits<size_t>::max();

  std::optional<SymRef> Written = synthesizeBitwise(Ctx, Parity, Atoms, Limits);
  ASSERT_TRUE(Written.has_value());
  EXPECT_EQ(*Written, Ctx.mkXor(Atoms)) << Ctx.toString(*Written);
}

TEST(SymMBA, StillPrefersAUnionOfCubesWhereThatIsShorter) {
  // The other family, at the same arity.  A seven-way disjunction is seven
  // one-literal products as a union of cubes and a hundred and twenty-seven
  // conjunctions as an exclusive-or, so the cover has to win here as clearly
  // as the exclusive-or won above — otherwise reaching past six inputs would
  // have been bought by making the answers worse.
  simplifiesTo("(a | b | c) + (d | e | f | g) - "
               "((a | b | c) & (d | e | f | g))",
               "a | b | c | d | e | f | g");
}

//===----------------------------------------------------------------------===//
// Regions too wide to measure whole
//===----------------------------------------------------------------------===//

TEST(SymMBA, RecoversASumWiderThanOneMeasurementCanReach) {
  // Eleven carry-save additions side by side: twenty-two inputs, well past
  // what a single 2^t sweep can afford, but no input interacts with more than
  // one other.  Measured in groups this costs 11 * 2^2 instead of 2^22, and
  // every addition comes back.
  //
  // The count is checked against the option rather than assumed, because the
  // ceiling is a dial now: a case meant to be past it stops being one the
  // moment somebody raises it, and it would go on passing without testing
  // anything.
  MBAOptions Defaults;
  ASSERT_LT(Defaults.MaxAtoms, 22u) << "this has to be the wide case";

  simplifiesTo(
      "(a ^ b) + 2 * (a & b) + (c ^ d) + 2 * (c & d) + (e ^ f) + 2 * (e & f) + "
      "(g ^ h) + 2 * (g & h) + (i ^ j) + 2 * (i & j) + (k ^ l) + 2 * (k & l) + "
      "(m ^ n) + 2 * (m & n) + (o ^ p) + 2 * (o & p) + (q ^ r) + 2 * (q & r) + "
      "(s ^ t) + 2 * (s & t) + (u ^ v) + 2 * (u & v)",
      "a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p + q + r + "
      "s + t + u + v");
}

TEST(SymMBA, SizesEachMeasurementToItsOwnGroup) {
  // A three-input tangle among two-input ones, plus a constant belonging to no
  // group.  Each group has to be measured at its own width rather than the
  // whole being refused for the width of the widest.
  MBAOptions Defaults;
  ASSERT_LT(Defaults.MaxAtoms, 21u) << "this has to be the wide case";

  simplifiesTo("(x ^ y ^ z) + 2 * ((x & y) ^ (x & z) ^ (y & z)) + "
               "(a ^ b) + 2 * (a & b) + (c ^ d) + 2 * (c & d) + "
               "(e ^ f) + 2 * (e & f) + (g ^ h) + 2 * (g & h) + "
               "(i ^ j) + 2 * (i & j) + (k ^ l) + 2 * (k & l) + "
               "(m ^ n) + 2 * (m & n) + (o ^ p) + 2 * (o & p) + "
               "(q ^ r) + 2 * (q & r) + 7",
               "x + y + z + a + b + c + d + e + f + g + h + i + j + k + l + "
               "m + n + o + p + q + r + 7");
}

TEST(SymMBA, LeavesAWideExpressionWhoseInputsAreGenuinelyTangled) {
  // Every input reaches every other through the terms, so there are no
  // independent groups to measure and the width is a property of the
  // expression.  Refusing it is the honest answer; returning something
  // unverified would not be.
  //
  // Twenty-two inputs, and checked against the dial rather than against the
  // number that used to be in it.  A case meant to sit past the ceiling stops
  // being one the moment the ceiling moves, and would go on passing while
  // testing the opposite of what it says.
  MBAOptions Defaults;
  ASSERT_LT(Defaults.MaxAtoms, 22u) << "this has to be the unreachable case";

  const char *Tangled =
      "(a & b & c & d & e & f & g & h & i & j & k & l & m & n & o & p & q & r "
      "& s & t & u & v) + (a | b | c | d | e | f | g | h | i | j | k | l | m | "
      "n | o | p | q | r | s | t | u | v)";
  EXPECT_EQ(simplified(Tangled), simplified(Tangled))
      << "the solver must at least be deterministic about declining";
  SymContext Ctx;
  SymParseResult Parsed = parseSymExpr(Ctx, Tangled, W32);
  ASSERT_TRUE(Parsed.ok()) << Parsed.Error;
  MBAResult R = simplifyMBA(Ctx, Parsed.Root);
  EXPECT_FALSE(R.Changed);
  // Which refusal it is matters as much as that it refused: this is the one a
  // caller can do something about, and saying so is the whole reason the
  // ceilings became dials.
  EXPECT_EQ(R.Outcome, MBAOutcome::TooManyInputs);
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
