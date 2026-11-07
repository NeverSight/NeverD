#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace neverd::symbolic;

TEST(SymMBASparseContext, PlanReadsOnlyReferencedHighVariableIds) {
  for (uint32_t Width : {8u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    for (unsigned I = 0; I < 1024; ++I)
      Ctx.mkVar("unused_before_" + std::to_string(I), 128);
    SymRef X = Ctx.mkVar("x", Width);
    Ctx.mkVar("unused_between", 257);
    SymRef Y = Ctx.mkVar("y", Width);
    SymEvalPlan Plan(Ctx, Ctx.mkAdd(X, Y));
    llvm::APInt A(Width + 8, 0x135), B(8, 0xa7);
    std::vector<uint32_t> Seen;
    llvm::APInt Actual = Plan.evalWith([&](uint32_t Id) -> const llvm::APInt * {
      Seen.push_back(Id);
      if (Id == Ctx.varId(X))
        return &A;
      if (Id == Ctx.varId(Y))
        return &B;
      ADD_FAILURE() << "unexpected variable lookup " << Id;
      return nullptr;
    });
    EXPECT_EQ(Actual, A.zextOrTrunc(Width) + B.zextOrTrunc(Width));
    std::sort(Seen.begin(), Seen.end());
    EXPECT_EQ(Seen, (std::vector<uint32_t>{Ctx.varId(X), Ctx.varId(Y)}));
    EXPECT_EQ(
        Plan.evalWith([](uint32_t) -> const llvm::APInt * { return nullptr; }),
        llvm::APInt(Width, 0));
  }
}

TEST(SymMBASparseContext, WordPlanTruncatesSparseValues) {
  for (uint32_t Width : {1u, 8u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    for (unsigned I = 0; I < 1024; ++I)
      Ctx.mkVar("unused_" + std::to_string(I), 128);
    SymRef Y = Ctx.mkVar("y", Width);
    SymEvalPlan Plan(Ctx, Ctx.mkAdd(X, Y));
    ASSERT_TRUE(Plan.fitsU64());
    constexpr uint64_t A = 0xfedcba9876543210ULL;
    constexpr uint64_t B = 0x8765432112345678ULL;
    std::vector<uint32_t> Seen;
    uint64_t Actual = Plan.evalU64With([&](uint32_t Id) {
      Seen.push_back(Id);
      if (Id == Ctx.varId(X))
        return A;
      if (Id == Ctx.varId(Y))
        return B;
      ADD_FAILURE() << "unexpected variable lookup " << Id;
      return uint64_t(0);
    });
    EXPECT_EQ(Actual,
              (llvm::APInt(Width, A) + llvm::APInt(Width, B)).getZExtValue());
    std::sort(Seen.begin(), Seen.end());
    EXPECT_EQ(Seen, (std::vector<uint32_t>{Ctx.varId(X), Ctx.varId(Y)}));
    EXPECT_EQ(Plan.evalU64With([](uint32_t) { return uint64_t(0); }), 0u);
  }
}

TEST(SymMBASparseContext, CornerWeightsIgnoreUnrelatedContextVariables) {
  for (uint32_t Width : {8u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    llvm::SmallVector<SymRef, 5> Vars;
    llvm::SmallVector<uint32_t, 5> Atoms;
    for (unsigned I = 0; I < 5; ++I) {
      Ctx.mkVar("unused_before_" + std::to_string(I), 128);
      SymRef Var = Ctx.mkVar("v" + std::to_string(I), Width);
      Vars.push_back(Var);
      Atoms.push_back(Ctx.varId(Var));
    }
    SymRef Expr = Ctx.mkAdd({Ctx.mkConst(Width, 37),
                             Ctx.mkMul(Ctx.mkConst(Width, 3), Vars.front()),
                             Ctx.mkNot(Ctx.mkXor(Vars)), Ctx.mkAnd(Vars),
                             Ctx.mkMul(Vars.front(), Vars.back())});
    auto Before = detail::measure(Ctx, Expr, Atoms);
    for (unsigned I = 0; I < 4096; ++I)
      Ctx.mkVar("unused_after_" + std::to_string(I), I % 2 ? 64 : 128);
    auto After = detail::measure(Ctx, Expr, Atoms);
    ASSERT_EQ(Before, After);
    ASSERT_EQ(After.size(), 32u);

    const llvm::APInt Zero(Width, 0);
    const llvm::APInt Ones = llvm::APInt::getAllOnes(Width);
    std::vector<llvm::APInt> Assignment(Ctx.numVars(), Zero);
    for (size_t Pattern = 0; Pattern < After.size(); ++Pattern) {
      for (unsigned J = 0; J < Atoms.size(); ++J)
        Assignment[Atoms[J]] = Pattern & (size_t(1) << J) ? Ones : Zero;
      EXPECT_EQ(After[Pattern], -Ctx.eval(Expr, Assignment));
    }
  }
}

TEST(SymMBASparseContext, PaddingPreservesProvedRewriteAndWork) {
  for (uint32_t Width : {64u, 128u}) {
    SCOPED_TRACE(Width);
    SymContext Small, Padded;
    SymRef SmallX = Small.mkVar("x", Width);
    SymRef SmallY = Small.mkVar("y", Width);
    SymRef PaddedX = Padded.mkVar("x", Width);
    SymRef PaddedY = Padded.mkVar("y", Width);
    SymRef SmallExpr =
        Small.mkAdd(Small.mkOr(SmallX, SmallY), Small.mkAnd(SmallX, SmallY));
    SymRef PaddedExpr = Padded.mkAdd(Padded.mkOr(PaddedX, PaddedY),
                                     Padded.mkAnd(PaddedX, PaddedY));
    for (unsigned I = 0; I < 4096; ++I)
      Padded.mkVar("unused_" + std::to_string(I), I % 2 ? 64 : 128);

    MBAOptions Options;
    Options.MaxWork = 10000;
    Options.MaxAtoms = Options.MaxSynthesisAtoms = 5;
    Options.MaxOptimalSynthesisAtoms = 2;
    MBAResult A = simplifyMBADeep(Small, SmallExpr, Options);
    MBAResult B = simplifyMBADeep(Padded, PaddedExpr, Options);
    ASSERT_TRUE(A.Changed);
    ASSERT_TRUE(B.Changed);
    EXPECT_EQ(A.Evidence, MBAEvidence::Derivation);
    EXPECT_EQ(B.Evidence, MBAEvidence::Derivation);
    EXPECT_EQ(Small.toString(A.Expr), Padded.toString(B.Expr));
    EXPECT_EQ(A.Outcome, B.Outcome);
    EXPECT_EQ(A.NumAtoms, B.NumAtoms);
    EXPECT_EQ(A.Work, B.Work);
  }
}

TEST(SymMBASparseContext, UnusedWideVariablesDoNotShiftRelevantSamples) {
  SymContext Ctx;
  Ctx.mkVar("unused_before", 257);
  SymRef X = Ctx.mkVar("x", 64);
  Ctx.mkVar("unused_after", 128);

  std::mt19937_64 Rng(0x9E3779B97F4A7C15ull);
  const uint64_t FirstRandom = Rng();
  ASSERT_NE(FirstRandom, 0u);
  ASSERT_NE(FirstRandom, 1u);
  ASSERT_NE(FirstRandom, ~uint64_t(0));

  SymRef Hit = Ctx.mkEq(X, Ctx.mkConst(64, FirstRandom));
  SymRef False = Ctx.mkFalse();
  EXPECT_TRUE(detail::agreeOnSamples(Ctx, Hit, False, 3));
  EXPECT_FALSE(detail::agreeOnSamples(Ctx, Hit, False, 4));
}

TEST(SymMBASparseContext, PlaceholderDeclarationsReuseTheCurrentContextOnly) {
  SymContext C;
  detail::WorkBudget Budget(1000000);
  Budget.MemoOwner = &C;
  Budget.MemoCapacity = 8;
  for (unsigned I = 0; I < 12; ++I) {
    // These are legal caller inputs, even though their names also occur in
    // the numbered placeholder pool. A reused declaration must stay reserved.
    const auto X = C.mkVar("mba$64.0", 64);
    const auto Y = C.mkVar("mba$64.1", 64);
    const auto Z = C.mkFreshVar(64, "other_");
    const auto Opaque = C.mkURem(X, C.mkConst(64, 17 + I));
    const auto Root = C.mkAdd(
        {C.mkXor(Opaque, Z), C.mkMul(C.mkConst(64, 2), C.mkAnd(Opaque, Z)), Y});
    const auto Abstract = detail::abstractToMBA(C, Root, false, &Budget);
    ASSERT_TRUE(Abstract);
    EXPECT_EQ(C.substitute(Abstract->Body, Abstract->Hidden), Root);
    EXPECT_FALSE(Abstract->Hidden.contains(X.index()));
    EXPECT_FALSE(Abstract->Hidden.contains(Y.index()));
    EXPECT_LE(Budget.PlaceholderInputs.size(), Budget.MemoCapacity);
  }
  EXPECT_GT(Budget.PlaceholderHits, 0u);
  EXPECT_LT(Budget.PlaceholderHits, Budget.PlaceholderLookups);
  SymContext Other;
  const auto A = Other.mkVar("a", 64), B = Other.mkVar("b", 64);
  const auto Root = Other.mkAdd(Other.mkURem(A, B), A);
  const auto Lookups = Budget.PlaceholderLookups;
  const auto Abstract = detail::abstractToMBA(Other, Root, false, &Budget);
  ASSERT_TRUE(Abstract);
  EXPECT_EQ(Other.substitute(Abstract->Body, Abstract->Hidden), Root);
  EXPECT_EQ(Budget.PlaceholderLookups, Lookups);
}
