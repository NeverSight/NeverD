//===- SymExprTests.cpp - Interning and canonicalisation ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Covers the two properties the rest of the optimiser is allowed to assume:
/// that structurally equal expressions are the same \c SymRef, and that every
/// way of writing the same value reaches the same node without any rewrite
/// pass running.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"

#include <random>

using namespace neverd::symbolic;

namespace {

constexpr uint32_t W32 = 32;

TEST(SymExpr, InterningMakesStructuralEqualityAPointerCompare) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  EXPECT_EQ(X, Ctx.mkVar("x", W32));
  EXPECT_NE(X, Y);
  EXPECT_EQ(Ctx.mkXor(X, Y), Ctx.mkXor(X, Y));
  EXPECT_EQ(Ctx.mkConst(W32, 7), Ctx.mkConst(W32, 7));
  // Same value, different width, different node.
  EXPECT_NE(Ctx.mkConst(W32, 7), Ctx.mkConst(8, 7));
}

TEST(SymExpr, CommutativeOperandsAreSorted) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef Z = Ctx.mkVar("z", W32);

  EXPECT_EQ(Ctx.mkAdd(X, Y), Ctx.mkAdd(Y, X));
  EXPECT_EQ(Ctx.mkAnd(X, Y), Ctx.mkAnd(Y, X));
  EXPECT_EQ(Ctx.mkOr(X, Y), Ctx.mkOr(Y, X));
  EXPECT_EQ(Ctx.mkXor(X, Y), Ctx.mkXor(Y, X));
  EXPECT_EQ(Ctx.mkMul(X, Y), Ctx.mkMul(Y, X));

  // Associativity is flattened, so the grouping an obfuscator chose is gone.
  EXPECT_EQ(Ctx.mkAdd(Ctx.mkAdd(X, Y), Z), Ctx.mkAdd(X, Ctx.mkAdd(Y, Z)));
  EXPECT_EQ(Ctx.mkXor(Ctx.mkXor(Z, X), Y), Ctx.mkXor(X, Ctx.mkXor(Y, Z)));
}

TEST(SymExpr, ConstantsFoldOnConstruction) {
  SymContext Ctx;
  EXPECT_EQ(Ctx.mkAdd(Ctx.mkConst(W32, 3), Ctx.mkConst(W32, 4)),
            Ctx.mkConst(W32, 7));
  EXPECT_EQ(Ctx.mkMul(Ctx.mkConst(W32, 3), Ctx.mkConst(W32, 4)),
            Ctx.mkConst(W32, 12));
  EXPECT_EQ(Ctx.mkNot(Ctx.mkConst(W32, 0)), Ctx.mkOnes(W32));
  // Wrapping is modulo the width, not an overflow.
  EXPECT_EQ(Ctx.mkAdd(Ctx.mkOnes(W32), Ctx.mkConst(W32, 1)), Ctx.mkZero(W32));
}

TEST(SymExpr, SumsCollectLikeTerms) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  EXPECT_EQ(Ctx.mkAdd(X, X), Ctx.mkMul(Ctx.mkConst(W32, 2), X));
  EXPECT_EQ(Ctx.mkAdd(X, Ctx.mkMul(Ctx.mkConst(W32, 2), X)),
            Ctx.mkMul(Ctx.mkConst(W32, 3), X));
  EXPECT_EQ(Ctx.mkSub(X, X), Ctx.mkZero(W32));
  EXPECT_EQ(Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(W32, 3), X),
                      Ctx.mkMul(Ctx.mkOnes(W32), X)),
            Ctx.mkMul(Ctx.mkConst(W32, 2), X));
  // A shared base with a compound body still collects.
  SymRef XY = Ctx.mkXor(X, Y);
  EXPECT_EQ(Ctx.mkAdd(XY, XY), Ctx.mkMul(Ctx.mkConst(W32, 2), XY));
}

TEST(SymExpr, ShiftsByConstantsBecomeProductsSoSumsCanCollectThem) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);

  EXPECT_EQ(Ctx.mkShl(X, Ctx.mkConst(W32, 1)),
            Ctx.mkMul(Ctx.mkConst(W32, 2), X));
  // This is the point of the normalisation: `x + (x << 1)` is `3*x`.
  EXPECT_EQ(Ctx.mkAdd(X, Ctx.mkShl(X, Ctx.mkConst(W32, 1))),
            Ctx.mkMul(Ctx.mkConst(W32, 3), X));
  // A shift past the width is zero, matching QF_BV rather than any one CPU.
  EXPECT_EQ(Ctx.mkShl(X, Ctx.mkConst(W32, 32)), Ctx.mkZero(W32));
  EXPECT_EQ(Ctx.mkLShr(X, Ctx.mkConst(W32, 99)), Ctx.mkZero(W32));
}

TEST(SymExpr, BitwiseIdentitiesHoldOnConstruction) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);

  EXPECT_EQ(Ctx.mkXor(X, X), Ctx.mkZero(W32));
  EXPECT_EQ(Ctx.mkAnd(X, X), X);
  EXPECT_EQ(Ctx.mkOr(X, X), X);
  EXPECT_EQ(Ctx.mkAnd(X, Ctx.mkZero(W32)), Ctx.mkZero(W32));
  EXPECT_EQ(Ctx.mkAnd(X, Ctx.mkOnes(W32)), X);
  EXPECT_EQ(Ctx.mkOr(X, Ctx.mkOnes(W32)), Ctx.mkOnes(W32));
  EXPECT_EQ(Ctx.mkOr(X, Ctx.mkZero(W32)), X);
  EXPECT_EQ(Ctx.mkNot(Ctx.mkNot(X)), X);
  EXPECT_EQ(Ctx.mkXor(X, Ctx.mkZero(W32)), X);
}

TEST(SymExpr, StructuralOperatorsCollapseWhereTheyCan) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);

  EXPECT_EQ(Ctx.mkExtract(X, 0, W32), X);
  EXPECT_EQ(Ctx.mkExtract(Ctx.mkExtract(X, 8, 16), 4, 8),
            Ctx.mkExtract(X, 12, 8));
  EXPECT_EQ(Ctx.mkZExt(Ctx.mkZExt(Ctx.mkExtract(X, 0, 8), 16), W32),
            Ctx.mkZExt(Ctx.mkExtract(X, 0, 8), W32));
  // Adjacent literals in a concatenation merge into one word.
  EXPECT_EQ(Ctx.mkConcat(Ctx.mkConst(8, 0xAB), Ctx.mkConst(8, 0xCD)),
            Ctx.mkConst(16, 0xABCD));
  EXPECT_EQ(
      Ctx.width(Ctx.mkConcat(Ctx.mkExtract(X, 0, 8), Ctx.mkExtract(X, 8, 8))),
      16u);
}

TEST(SymExpr, SelectFoldsToItsConditionWhenTheArmsAreTheTruthValues) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef C = Ctx.mkUlt(X, Ctx.mkConst(W32, 10));

  EXPECT_EQ(Ctx.mkIte(C, Ctx.mkTrue(), Ctx.mkFalse()), C);
  EXPECT_EQ(Ctx.mkIte(C, Ctx.mkFalse(), Ctx.mkTrue()), Ctx.mkNot(C));
  EXPECT_EQ(Ctx.mkIte(Ctx.mkTrue(), X, Ctx.mkZero(W32)), X);
  EXPECT_EQ(Ctx.mkIte(C, X, X), X);
}

TEST(SymExpr, DagSizeCountsSharedSubtermsOnce) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef Shared = Ctx.mkXor(X, Y);

  // Doubling a shared node adds the product and its coefficient, not a second
  // copy of the subtree.  A tree-shaped count would call this expression twice
  // the size it costs to work with.
  size_t Before = Ctx.dagSize(Shared);
  size_t After = Ctx.dagSize(Ctx.mkAdd(Shared, Shared));
  EXPECT_EQ(Before, 3u);
  EXPECT_EQ(After, Before + 2);
}

TEST(SymExpr, ReadabilityCostCountsEveryPrintedUse) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef Z = Ctx.mkVar("z", W32);
  SymRef W = Ctx.mkVar("w", W32);
  SymRef Shared = Ctx.mkXor(X, Y);

  // Populate the cache before appending the expressions below: a later query
  // must extend it rather than recompute or return a stale prefix.
  EXPECT_EQ(Ctx.readabilityCost(Shared), 3u);

  SymRef Twice = Ctx.mkOr(Ctx.mkAnd(Shared, Z), Ctx.mkAnd(Shared, W));
  EXPECT_EQ(Ctx.dagSize(Twice), 8u);
  EXPECT_EQ(Ctx.readabilityCost(Twice), 11u);

  // The all-ones factor is how the graph spells a sign, so it is not a printed
  // quantity in the cost model.
  EXPECT_EQ(Ctx.readabilityCost(Ctx.mkNeg(Shared)), 4u);
}

TEST(SymExpr, CollectVarsReportsEveryReachableVariableOnce) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef E = Ctx.mkAdd(Ctx.mkXor(X, Y), Ctx.mkMul(X, Ctx.mkConst(W32, 3)));

  llvm::SmallVector<uint32_t, 4> Vars;
  Ctx.collectVars(E, Vars);
  ASSERT_EQ(Vars.size(), 2u);
  EXPECT_EQ(Ctx.varInfo(Vars[0]).Name, "x");
  EXPECT_EQ(Ctx.varInfo(Vars[1]).Name, "y");
}

TEST(SymExpr, FindVarSeesDeclarationsAndNothingElse) {
  SymContext Ctx;
  Ctx.mkVar("x", W32);
  ASSERT_TRUE(Ctx.findVar("x").has_value());
  EXPECT_EQ(Ctx.varInfo(*Ctx.findVar("x")).Width, W32);
  EXPECT_FALSE(Ctx.findVar("nope").has_value());
}

TEST(SymExpr, SubstitutionRebuildsThroughTheCanonicalisingBuilders) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  // Replacing y by x turns `x + y` into `x + x`, which the builders collect
  // into `2*x` rather than leaving as a sum of equal terms.
  SymRef Sum = Ctx.mkAdd(X, Y);
  EXPECT_EQ(Ctx.substituteVar(Sum, Ctx.varId(Y), X),
            Ctx.mkMul(Ctx.mkConst(W32, 2), X));
  EXPECT_EQ(Ctx.substituteVar(Sum, Ctx.varId(Y), Ctx.mkZero(W32)), X);
}

//===----------------------------------------------------------------------===//
// Evaluation
//===----------------------------------------------------------------------===//

TEST(SymExpr, EvaluationAgreesAcrossAllThreeEntryPoints) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  // (x ^ y) + 2*(x & y), which is x + y written the long way round.
  SymRef E = Ctx.mkAdd(Ctx.mkXor(X, Y),
                       Ctx.mkMul(Ctx.mkConst(W32, 2), Ctx.mkAnd(X, Y)));
  ASSERT_TRUE(Ctx.fitsU64(E));

  SymEvalPlan Plan(Ctx, E);
  ASSERT_TRUE(Plan.fitsU64());

  std::mt19937_64 Rng(20260812);
  for (unsigned I = 0; I < 512; ++I) {
    uint64_t A = Rng() & 0xFFFFFFFFu;
    uint64_t B = Rng() & 0xFFFFFFFFu;
    uint64_t Want = (A + B) & 0xFFFFFFFFu;

    std::vector<uint64_t> VarsU64(Ctx.numVars(), 0);
    VarsU64[Ctx.varId(X)] = A;
    VarsU64[Ctx.varId(Y)] = B;

    std::vector<llvm::APInt> VarsAP(Ctx.numVars(), llvm::APInt(W32, 0));
    VarsAP[Ctx.varId(X)] = llvm::APInt(W32, A);
    VarsAP[Ctx.varId(Y)] = llvm::APInt(W32, B);

    EXPECT_EQ(Ctx.evalU64(E, VarsU64), Want);
    EXPECT_EQ(Ctx.eval(E, VarsAP).getZExtValue(), Want);
    EXPECT_EQ(Plan.evalU64(VarsU64), Want);
    EXPECT_EQ(Plan.eval(VarsAP).getZExtValue(), Want);
  }
}

TEST(SymExpr, EvaluationPlanReportsTheVariablesItReads) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  Ctx.mkVar("unused", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  SymEvalPlan Plan(Ctx, Ctx.mkAnd(X, Y));
  ASSERT_EQ(Plan.vars().size(), 2u);
  EXPECT_EQ(Plan.vars()[0], Ctx.varId(X));
  EXPECT_EQ(Plan.vars()[1], Ctx.varId(Y));
  EXPECT_GT(Plan.numSteps(), 0u);
}

TEST(SymExpr, U64ArithmeticShiftByZeroKeepsTheWholeWord) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 64);
  SymRef Amount = Ctx.mkVar("amount", 64);
  SymEvalPlan Plan(Ctx, Ctx.mkAShr(X, Amount));
  ASSERT_TRUE(Plan.fitsU64());

  std::vector<uint64_t> Vars(Ctx.numVars(), 0);
  Vars[Ctx.varId(X)] = 0x8000000000000001ull;
  Vars[Ctx.varId(Amount)] = 0;
  EXPECT_EQ(Plan.evalU64(Vars), 0x8000000000000001ull);
}

TEST(SymExpr, DivisionAndRemainderFollowTheBitvectorTheory) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  std::vector<llvm::APInt> V{llvm::APInt(W32, 7)};

  // Totalised the way QF_BV totalises them, so a solver bridge needs no
  // correction terms: udiv by zero is all-ones, and the remainder is x.
  EXPECT_EQ(Ctx.eval(Ctx.mkUDiv(X, Ctx.mkZero(W32)), V),
            llvm::APInt::getAllOnes(W32));
  EXPECT_EQ(Ctx.eval(Ctx.mkURem(X, Ctx.mkZero(W32)), V), llvm::APInt(W32, 7));
  EXPECT_EQ(Ctx.eval(Ctx.mkUDiv(X, Ctx.mkConst(W32, 2)), V),
            llvm::APInt(W32, 3));
}

TEST(SymExpr, AWordWiderThanSixtyFourBitsIsOrdinary) {
  SymContext Ctx;
  constexpr uint32_t W256 = 256;
  SymRef X = Ctx.mkVar("x", W256);

  // Everything the canonicaliser does at 32 bits it does at 256, which is what
  // makes an EVM word a first-class citizen rather than a special case.
  EXPECT_EQ(Ctx.mkAdd(X, X), Ctx.mkMul(Ctx.mkConst(W256, 2), X));
  EXPECT_EQ(Ctx.mkXor(X, Ctx.mkOnes(W256)), Ctx.mkNot(X));
  EXPECT_EQ(Ctx.mkSub(X, X), Ctx.mkZero(W256));

  // `~x + 1` and `-x` are the same value but deliberately not the same node:
  // Not stays primitive because the MBA solver needs it as a generator of the
  // bitwise algebra.  Bridging the two is the simplifier's job.
  SymRef E = Ctx.mkAdd(Ctx.mkNot(X), Ctx.mkConst(W256, 1));
  EXPECT_NE(E, Ctx.mkNeg(X));
  EXPECT_FALSE(Ctx.fitsU64(E));

  llvm::APInt Big = llvm::APInt(W256, 1).shl(200) + 12345;
  std::vector<llvm::APInt> V(Ctx.numVars(), llvm::APInt(W256, 0));
  V[Ctx.varId(X)] = Big;
  EXPECT_EQ(Ctx.eval(E, V), -Big);
  EXPECT_EQ(SymEvalPlan(Ctx, E).eval(V), -Big);
}

} // namespace
