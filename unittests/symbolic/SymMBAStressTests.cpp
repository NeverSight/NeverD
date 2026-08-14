//===- SymMBAStressTests.cpp - Randomised MBA equivalence gates -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Corpora rather than named identities: obfuscate by construction and demand
/// the original back, and check that no rewrite ever changes what an
/// expression computes.
///
//===----------------------------------------------------------------------===//

#include "SymMBATestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

#include <random>
#include <utility>

using namespace neverd::symbolic;

namespace {

using test::simplifiesTo;
using test::W32;

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
  constexpr unsigned kTrialsPerWidth = 336;
  unsigned Cases = 0;
  for (uint32_t Width : {8u, 32u, 64u}) {
    for (unsigned Trial = 0; Trial < kTrialsPerWidth; ++Trial) {
      SymContext Ctx;
      SymRef A = Ctx.mkVar("a", Width);
      SymRef B = Ctx.mkVar("b", Width);
      SymRef Original = Ctx.mkAdd(A, B);

      Obfuscator Obf(Ctx, Width, 0xB16B00B5ull + Trial * 131 + Width);
      SymRef Hidden = Obf.expand(Original, 4);
      if (Hidden == Original)
        continue;
      ++Cases;
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
  EXPECT_GE(Cases, 1000u)
      << "the deterministic recovery corpus fell below its acceptance floor";
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

//===----------------------------------------------------------------------===//
// What "as far as the resources allow" has to mean
//===----------------------------------------------------------------------===//

/// Twenty-four inputs with no independent part to split off: every term names
/// every input, so the width belongs to the expression rather than to how it
/// was written.  There is no answer here at any budget, which is exactly what
/// makes it the right shape for asking what the solver does when told to try
/// as hard as it can.
llvm::StringRef tangledOverTwentyFourInputs() {
  return "(a & b & c & d & e & f & g & h & i & j & k & l & m & n & o & p & q "
         "& r & s & t & u & v & w & aa) + (a | b | c | d | e | f | g | h | i "
         "| j | k | l | m | n | o | p | q | r | s | t | u | v | w | aa)";
}

/// Simplify \p Text under \p Opts and report both what came back and whether
/// it still computes what it replaced.
std::pair<MBAOutcome, bool> outcomeAndEquivalence(llvm::StringRef Text,
                                                  const MBAOptions &Opts,
                                                  bool Deep = false) {
  SymContext Ctx;
  SymParseResult P = parseSymExpr(Ctx, Text, W32);
  EXPECT_TRUE(P.ok()) << P.Error;
  if (!P.ok())
    return {MBAOutcome::NotApplicable, false};
  MBAResult R = Deep ? simplifyMBADeep(Ctx, P.Root, Opts)
                     : simplifyMBA(Ctx, P.Root, Opts);
  return {R.Outcome, sameValue(Ctx, P.Root, R.Expr, W32)};
}

TEST(SymMBA, TreatsAnUnlimitedArityAsAResourceQuestionNotAShiftCount) {
  // "As many inputs as it takes" has to resolve to a number before anything
  // downstream can use it, and the two ways of getting that wrong are a shift
  // by a count the type cannot hold and a table larger than there is memory
  // for.  Neither is a longer wait; both are a crash.  So the sentinel is
  // resolved against what the caller said it would hold, and a request that
  // does not fit comes back with an answer -- the expression, unmeasured --
  // rather than with an allocation.
  MBAOptions Unbounded;
  Unbounded.MaxAtoms = MBAOptions::Unlimited;
  Unbounded.MaxSynthesisAtoms = MBAOptions::Unlimited;
  // Enough for a twelve-input sweep and not a twenty-four-input one, so the
  // resolution is what decides rather than the arity dial.
  Unbounded.MaxTableBytes = size_t(1) << 16;

  auto [Outcome, Equivalent] =
      outcomeAndEquivalence(tangledOverTwentyFourInputs(), Unbounded);
  EXPECT_EQ(Outcome, MBAOutcome::TooManyInputs);
  EXPECT_TRUE(Equivalent);

  // The same request with no work to spend is a different refusal for a
  // different reason, and must be just as survivable.
  MBAOptions Broke;
  Broke.MaxAtoms = MBAOptions::Unlimited;
  Broke.MaxWork = 1;
  auto [BrokeOutcome, BrokeEquivalent] =
      outcomeAndEquivalence(tangledOverTwentyFourInputs(), Broke);
  EXPECT_EQ(BrokeOutcome, MBAOutcome::BudgetExhausted);
  EXPECT_TRUE(BrokeEquivalent);
}

TEST(SymMBA, SurvivesTwentyFourTangledInputsOnTheDefaultPath) {
  // The same expression with nothing configured.  A wide reading that cannot
  // be afforded is refused, not attempted and abandoned partway, so this has
  // to be quick as well as correct.
  auto [Outcome, Equivalent] =
      outcomeAndEquivalence(tangledOverTwentyFourInputs(), MBAOptions{});
  EXPECT_EQ(Outcome, MBAOutcome::TooManyInputs);
  EXPECT_TRUE(Equivalent);
}

TEST(SymMBA, ExhaustiveModeReachesWhatTheDefaultReaches) {
  // Removing every budget must not change an answer, only how long the solver
  // is willing to look for one.  A mode that returned something different from
  // the default would mean one of the two was not deriving its answer.
  const MBAOptions Unlimited = MBAOptions::unlimited();
  EXPECT_EQ(Unlimited.MaxWork, MBAOptions::UnlimitedWork);
  EXPECT_EQ(Unlimited.MaxAtoms, MBAOptions::Unlimited);

  for (const char *Text : {"(x ^ y) + 2 * (x & y)", "(x | y) - (x & y)",
                           "(x & y) * (x | y) + (x & ~y) * (~x & y)",
                           "((x ^ y) + 2 * (x & y)) & 0xff"}) {
    SymContext Ctx;
    SymParseResult P = parseSymExpr(Ctx, Text, W32);
    ASSERT_TRUE(P.ok()) << P.Error;
    EXPECT_EQ(simplifyMBADeep(Ctx, P.Root, Unlimited).Expr,
              simplifyMBADeep(Ctx, P.Root).Expr)
        << Text;
  }
}

TEST(SymMBA, LetsTheCallerTurnTheSampleNetAndTheSizeGuardOff) {
  // Both are the caller's to set and the layered walk is where that used to be
  // easy to lose, because it applies its options once per layer and returns
  // the composition of all of them.
  MBAOptions ProofOnly;
  ProofOnly.VerifySamples = 0;
  SymContext Ctx;
  SymParseResult P = parseSymExpr(
      Ctx, "(((x ^ y) + 2 * (x & y)) ^ z) + 2 * (((x ^ y) + 2 * (x & y)) & z)",
      W32);
  ASSERT_TRUE(P.ok()) << P.Error;
  MBAResult Proved = simplifyMBADeep(Ctx, P.Root, ProofOnly);
  EXPECT_TRUE(Proved.Changed);
  EXPECT_EQ(Proved.Evidence, MBAEvidence::Derivation);
  EXPECT_LE(Proved.SizeAfter, Proved.SizeBefore);

  // Growth mode is for measuring the solver, so it must not be able to make a
  // wrong answer -- only a longer one.
  MBAOptions Grow;
  Grow.AllowGrowth = true;
  MBAResult Grown = simplifyMBADeep(Ctx, P.Root, Grow);
  EXPECT_TRUE(sameValue(Ctx, P.Root, Grown.Expr, W32));
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
