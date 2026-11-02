//===- SymMBAFactorCostTests.cpp - Bounded factor candidate work ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include <array>

using namespace neverd::symbolic;

namespace {

TEST(SymMBAFactorCost, KeepsExactFactorizationWithoutQuotientRewrite) {
  for (unsigned W : {1u, 3u, 8u, 64u, 257u}) {
    SymContext C;
    SymRef U = C.mkVar("u", W), X = C.mkVar("x", W), Y = C.mkVar("y", W);
    SymRef A = C.mkVar("a", W), B = C.mkVar("b", W), F = C.mkNot(U);
    SymRef P = C.mkXor(X, Y), Q = C.mkOr(A, B);
    SymRef Input = C.mkAdd({F, C.mkMul(F, P), C.mkMul(F, Q)});
    SymRef Expected = C.mkMul(F, C.mkAdd({C.mkOne(W), P, Q}));
    MBAOptions Options;
    Options.VerifySamples = 0;
    detail::WorkBudget Budget(Options.MaxWork);
    detail::SolveReport Report;
    SymRef Result =
        detail::solveStructuralFactors(C, Input, Options, Budget, Report);
    if (W != 1)
      EXPECT_EQ(Result, Expected) << C.toString(Result);
    EXPECT_LE(C.readabilityCost(Result), C.readabilityCost(Expected));
    EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
    EXPECT_LT(C.readabilityCost(Result), C.readabilityCost(Input));
    if (W <= 3)
      for (unsigned I = 0; I != (1u << (5 * W)); ++I) {
        const unsigned Mask = (1u << W) - 1;
        std::array<uint64_t, 5> Values{
            I & Mask, (I >> W) & Mask, (I >> (2 * W)) & Mask,
            (I >> (3 * W)) & Mask, (I >> (4 * W)) & Mask};
        EXPECT_EQ(C.evalU64(Input, Values), C.evalU64(Result, Values));
      }
  }
}

TEST(SymMBAFactorCost, PreservesCompletedFactorizationOnOptionalRefusal) {
  SymContext C;
  SymRef U = C.mkVar("u", 8), X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  SymRef Z = C.mkVar("z", 8), F = C.mkUDiv(U, Z);
  SymRef P = C.mkOr(X, Y), Q = C.mkAnd(X, Y);
  SymRef Input = C.mkAdd({F, C.mkMul(F, P), C.mkMul(F, Q)});
  SymRef Factored = C.mkMul(F, C.mkAdd({C.mkOne(8), P, Q}));
  MBAOptions Options;
  Options.MaxWork = 20;
  Options.VerifySamples = 0;
  detail::WorkBudget Budget(Options.MaxWork);
  detail::SolveReport Report;
  SymRef Result =
      detail::solveStructuralFactors(C, Input, Options, Budget, Report);
  EXPECT_EQ(Result, Factored);
  EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
  EXPECT_TRUE(Report.BudgetExhausted);
  EXPECT_LE(Budget.used(), Options.MaxWork);
}

TEST(SymMBAFactorCost, RetestsSharedInputsAndOpaqueQuotients) {
  for (unsigned W : {1u, 3u, 8u, 64u, 257u}) {
    SymContext C;
    SymRef U = C.mkVar("u", W), X = C.mkVar("x", W), Y = C.mkVar("y", W);
    SymRef F = C.mkNot(U), Opaque = C.mkUDiv(X, Y);
    for (SymRef A : {X, Opaque}) {
      SymRef P = C.mkOr(A, Y), Q = C.mkAnd(A, Y);
      SymRef Input = C.mkAdd({F, C.mkMul(F, P), C.mkMul(F, Q)});
      SymRef Expected = C.mkMul(F, C.mkAdd({C.mkOne(W), A, Y}));
      MBAOptions Options;
      Options.VerifySamples = 0;
      detail::WorkBudget Budget(Options.MaxWork);
      detail::SolveReport Report;
      SymRef Result =
          detail::solveStructuralFactors(C, Input, Options, Budget, Report);
      EXPECT_LE(C.readabilityCost(Result), C.readabilityCost(Expected))
          << C.toString(Result);
      EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
      if (W <= 3)
        for (unsigned I = 0; I != (1u << (3 * W)); ++I) {
          const unsigned Mask = (1u << W) - 1;
          std::array<uint64_t, 3> Values{I & Mask, (I >> W) & Mask,
                                         (I >> (2 * W)) & Mask};
          EXPECT_EQ(C.evalU64(Input, Values), C.evalU64(Result, Values));
        }
    }
  }
}

TEST(SymMBAFactorCost, RefusesBeforeBuildingAndBoundsIndependentWork) {
  SymContext C;
  constexpr unsigned W = 64;
  SymRef U = C.mkVar("u", W), X = C.mkVar("x", W), Y = C.mkVar("y", W);
  SymRef A = C.mkVar("a", W), B = C.mkVar("b", W), F = C.mkNot(U);
  SymRef Input =
      C.mkAdd({F, C.mkMul(F, C.mkXor(X, Y)), C.mkMul(F, C.mkOr(A, B))});
  for (bool Storage : {false, true}) {
    MBAOptions Options;
    if (Storage)
      Options.MaxTableBytes = 0;
    else
      Options.MaxWork = 0;
    detail::WorkBudget Budget(Options.MaxWork);
    detail::SolveReport Report;
    const auto Before = C.numNodes();
    EXPECT_EQ(detail::solveStructuralFactors(C, Input, Options, Budget, Report),
              Input);
    EXPECT_EQ(C.numNodes(), Before);
  }
  MBAOptions Options;
  detail::WorkBudget Budget(Options.MaxWork);
  detail::SolveReport Report;
  SymRef Result =
      detail::solveStructuralFactors(C, Input, Options, Budget, Report);
  EXPECT_LT(C.readabilityCost(Result), C.readabilityCost(Input));
  // The two bounded partition passes charge 3 terms + 4 product edges each.
  // Quotient (3), product (2), and final sum (1) builders add six.
  // Independent quotient terms do not start another region search.
  EXPECT_EQ(Budget.used(), 20u);
  EXPECT_FALSE(Report.BudgetExhausted);
}

TEST(SymMBAFactorCost, RetainsCompleteComplementWithPlainQuotient) {
  for (unsigned W : {1u, 3u, 8u, 64u, 257u}) {
    SymContext C;
    SymRef U = C.mkVar("u", W), X = C.mkVar("x", W), Y = C.mkVar("y", W);
    SymRef Z = C.mkVar("z", W), F = C.mkNot(U);
    SymRef Input = C.mkAdd({C.mkMul(F, X), C.mkMul(F, Y), Z});
    SymRef Expected = C.mkAdd(C.mkMul(F, C.mkAdd(X, Y)), Z);
    MBAOptions Options;
    Options.VerifySamples = 0;
    for (bool Deep : {false, true}) {
      auto Result = Deep ? simplifyMBADeep(C, Input, Options)
                         : simplifyMBA(C, Input, Options);
      EXPECT_LE(Result.SizeAfter, C.readabilityCost(Expected))
          << C.toString(Result.Expr);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    }
  }
}

TEST(SymMBAFactorCost, RefusesLargeSumExposedByFactorRemoval) {
  SymContext C;
  constexpr unsigned W = 64;
  SymRef U = C.mkVar("u", W), X = C.mkVar("x", W), Y = C.mkVar("y", W);
  SymRef Z = C.mkVar("z", W), F = C.mkNot(U);
  llvm::SmallVector<SymRef, 72> Terms;
  for (unsigned I = 0; I != 70; ++I)
    Terms.push_back(C.mkVar("v" + std::to_string(I), W));
  SymRef Wide = C.mkAdd(Terms);
  for (unsigned RestCount : {1u, 62u}) {
    llvm::SmallVector<SymRef, 64> Outer{C.mkMul(F, Wide),
                                        C.mkMul(F, C.mkOr(X, Y))};
    for (unsigned I = 0; I != RestCount; ++I)
      Outer.push_back(C.mkVar("r" + std::to_string(I), W));
    SymRef Input = C.mkAdd(Outer);
    MBAOptions Options;
    Options.MaxWork = 256;
    Options.MaxTableBytes = 65536;
    detail::WorkBudget Budget(Options.MaxWork);
    detail::SolveReport Report;
    const auto Nodes = C.numNodes();
    EXPECT_EQ(detail::solveStructuralFactors(C, Input, Options, Budget, Report),
              Input);
    EXPECT_EQ(C.numNodes(), Nodes);
    // The two small products are also intersected before the exposed wide
    // sum is refused. That bounded scan must not walk the sum's 70 children.
    EXPECT_LE(Budget.used(), 2 * (RestCount + 6) + 8);
    EXPECT_TRUE(Report.BudgetExhausted);
  }
}

TEST(SymMBAFactorCost, BoundsSumExposedByUnitQuotient) {
  SymContext C;
  constexpr unsigned W = 64;
  SymRef X = C.mkVar("x", W), Y = C.mkVar("y", W);
  llvm::SmallVector<SymRef, 72> Terms;
  for (unsigned I = 0; I != 70; ++I)
    Terms.push_back(C.mkVar("v" + std::to_string(I), W));
  SymRef F = C.mkAdd(Terms);
  SymRef Input = C.mkAdd({C.mkMul(F, C.mkOr(X, Y)), C.mkMul(F, C.mkAnd(X, Y)),
                          C.mkMul(F, C.mkNot(X)), C.mkNeg(C.mkMul(F, Y)),
                          C.mkMul(C.mkConst(llvm::APInt(W, 2)), F)});
  MBAOptions Options;
  Options.VerifySamples = 0;
  detail::WorkBudget Budget(Options.MaxWork);
  detail::SolveReport Report;
  SymRef Result =
      detail::solveStructuralFactors(C, Input, Options, Budget, Report);
  EXPECT_NE(Result, F);
  EXPECT_LT(C.readabilityCost(Result), C.readabilityCost(Input));
  EXPECT_TRUE(Report.BudgetExhausted);
  EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
}

} // namespace
