//===- OrderedQueryTests.cpp - Complete ordered query obligations
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/core/OrderedQuery.h"
#include "gtest/gtest.h"

#include <vector>

using namespace neverd::analysis::detail;
using namespace neverd::solver;
using namespace neverd::symbolic;

namespace {

struct OrderedProblem {
  SymContext Ctx;
  SolverOptions Options;
  SymRef X, Y, Z, Domain, First, Full;
  unsigned RetryCharges = 0;

  OrderedProblem(unsigned Width = 4, unsigned Bias = 0,
                 bool EqualProducts = false) {
    Options.BuildModel = false;
    Options.Blast.MaxGates = 262144;
    Options.Sat.MaxConflicts = 10000;
    Options.Sat.MaxPropagations = EqualProducts ? 10425 : (Bias ? 228 : 64);
    Options.Sat.MaxWatchVisits = 10000000;
    X = Ctx.mkVar("x", Width);
    Y = Ctx.mkVar("y", Width);
    Z = Ctx.mkVar("z", Width);
    if (EqualProducts) {
      Domain = Ctx.mkEq(X, Y);
      First = Ctx.mkNe(Ctx.mkAdd(Ctx.mkMul(X, Z), Ctx.mkConst(Width, Bias)),
                       Ctx.mkMul(Y, Z));
    } else {
      const auto Odd =
          Ctx.mkEq(Ctx.mkAnd(Y, Ctx.mkConst(Width, 1)), Ctx.mkConst(Width, 1));
      const auto Product = Ctx.mkAdd(Ctx.mkMul(X, Y), Z);
      Domain = Ctx.mkAnd(
          Odd, Ctx.mkEq(Product, Ctx.mkAdd(Z, Ctx.mkConst(Width, Bias))));
      First = Ctx.mkNot(Ctx.mkEq(X, Ctx.mkZero(Width)));
    }
    Full = Ctx.mkAnd(Domain, First);
  }

  SatResult check(SymRef Preferred) {
    return checkWithOrderedConjunct(Ctx, Full, Preferred, Options,
                                    [&] { ++RetryCharges; });
  }
};

TEST(OrderedQuery, IndependentExhaustiveArithmeticOracle) {
  for (unsigned Bias : {0u, 1u}) {
    unsigned Satisfying = 0;
    for (unsigned X = 0; X != 16; ++X)
      for (unsigned Y = 0; Y != 16; ++Y)
        for (unsigned Z = 0; Z != 16; ++Z)
          Satisfying +=
              (Y & 1) && (((X * Y + Z) & 15) == ((Z + Bias) & 15)) && X != 0;
    EXPECT_EQ(Satisfying, Bias ? 128u : 0u);
  }
  // Independent modular oracle for the public completed-retry example.
  unsigned EqualProductViolations = 0, BiasedProductWitnesses = 0;
  for (unsigned X = 0; X != 32; ++X)
    for (unsigned Y = 0; Y != 32; ++Y)
      for (unsigned Z = 0; Z != 32; ++Z) {
        EqualProductViolations += X == Y && ((X * Z) & 31) != ((Y * Z) & 31);
        BiasedProductWitnesses +=
            X == Y && ((X * Z + 1) & 31) != ((Y * Z) & 31);
      }
  EXPECT_EQ(EqualProductViolations, 0u);
  EXPECT_EQ(BiasedProductWitnesses, 1024u);
}

TEST(OrderedQuery, CompleteUnsatRetriesExactlyOnce) {
  OrderedProblem P(5, 0, true);
  // How far a search gets on a budget depends on the order a compiler numbers
  // the solver's variables in. Find one the whole query spends and its retry
  // does not, from the budget this test names on Clang.
  auto &Budget = P.Options.Sat.MaxPropagations;
  const auto Largest = Budget * 16;
  SatResult Answer = SatResult::Unknown;
  for (; Budget <= Largest && Answer == SatResult::Unknown; Budget += 64) {
    if (checkSat(P.Ctx, P.Full, nullptr, P.Options) != SatResult::Unknown)
      break;
    ASSERT_EQ(checkSat(P.Ctx, P.First, nullptr, P.Options), SatResult::Sat);
    P.RetryCharges = 0;
    Answer = P.check(P.First);
    EXPECT_EQ(P.RetryCharges, 1u);
  }
  EXPECT_EQ(Answer, SatResult::Unsat);
}

TEST(OrderedQuery, CompleteSatRetriesExactlyOnce) {
  OrderedProblem P(6, 1);
  ASSERT_EQ(checkSat(P.Ctx, P.Full, nullptr, P.Options), SatResult::Unknown);
  EXPECT_EQ(P.check(P.First), SatResult::Sat);
  EXPECT_EQ(P.RetryCharges, 1u);
}

TEST(OrderedQuery, UndecidedSatisfiableQuestionRemainsUnknown) {
  OrderedProblem P(6, 1);
  P.Options.Sat.MaxPropagations = 1;
  ASSERT_EQ(checkSat(P.Ctx, P.Full, nullptr, P.Options), SatResult::Unknown);
  EXPECT_EQ(P.check(P.First), SatResult::Unknown);
  EXPECT_EQ(P.RetryCharges, 1u);
  P.Options.Sat.MaxPropagations = 1000000;
  EXPECT_EQ(checkSat(P.Ctx, P.Full, nullptr, P.Options), SatResult::Sat);
}

TEST(OrderedQuery, OptOutKeepsTheOriginalRefusal) {
  OrderedProblem P;
  EXPECT_EQ(P.check({}), SatResult::Unknown);
  EXPECT_EQ(P.RetryCharges, 0u);
}

TEST(OrderedQuery, UnrelatedConditionCannotStrengthenTheQuestion) {
  OrderedProblem P(6, 1);
  EXPECT_EQ(P.check(P.Ctx.mkNot(P.First)), SatResult::Unknown);
  EXPECT_EQ(P.RetryCharges, 0u);
}

TEST(OrderedQuery, RetryMustFitTheCallersUnchangedQueryAllowance) {
  OrderedProblem P;
  struct NoQueryAllowance {};
  EXPECT_THROW(checkWithOrderedConjunct(P.Ctx, P.Full, P.First, P.Options,
                                        [&] {
                                          ++P.RetryCharges;
                                          throw NoQueryAllowance{};
                                        }),
               NoQueryAllowance);
  EXPECT_EQ(P.RetryCharges, 1u);
}

TEST(OrderedQuery, WidthAndGateRefusalsDoNotRetry) {
  OrderedProblem Width;
  Width.Options.Blast.MaxWidth = 3;
  EXPECT_EQ(Width.check(Width.First), SatResult::Unknown);
  EXPECT_EQ(Width.RetryCharges, 0u);
  OrderedProblem Gates;
  Gates.Options.Blast.MaxGates = 2;
  EXPECT_EQ(Gates.check(Gates.First), SatResult::Unknown);
  EXPECT_EQ(Gates.RetryCharges, 0u);
}

TEST(OrderedQuery, UnknownAfterTheRetryIsStillUnknown) {
  OrderedProblem P;
  P.Options.Sat.MaxPropagations = 1;
  EXPECT_EQ(P.check(P.First), SatResult::Unknown);
  EXPECT_EQ(P.RetryCharges, 1u);
}

TEST(OrderedQuery, CompletedFirstSearchDoesNotRetry) {
  for (unsigned Bias : {0u, 1u}) {
    OrderedProblem P(4, Bias);
    P.Options.Sat.MaxPropagations = 1000000;
    EXPECT_EQ(P.check(P.First), Bias ? SatResult::Sat : SatResult::Unsat);
    EXPECT_EQ(P.RetryCharges, 0u);
  }
}

TEST(OrderedQuery, MalformedOriginalQuestionDoesNotRetry) {
  OrderedProblem P;
  EXPECT_EQ(checkWithOrderedConjunct(P.Ctx, {}, P.First, P.Options,
                                     [&] { ++P.RetryCharges; }),
            SatResult::Invalid);
  EXPECT_EQ(P.RetryCharges, 0u);
}

TEST(OrderedQuery, WideOriginalConditionsKeepTheirNonzeroMeaning) {
  OrderedProblem P;
  EXPECT_EQ(checkWithOrderedConjunct(P.Ctx, P.X, P.First, P.Options,
                                     [&] { ++P.RetryCharges; }),
            SatResult::Sat);
  EXPECT_EQ(P.RetryCharges, 0u);
}

TEST(OrderedQuery, InvalidPriorityShapesKeepTheOriginalRefusal) {
  OrderedProblem Whole;
  EXPECT_EQ(Whole.check(Whole.Full), SatResult::Unknown);
  EXPECT_EQ(Whole.RetryCharges, 0u);
  OrderedProblem Constant;
  EXPECT_EQ(Constant.check(Constant.Ctx.mkFalse()), SatResult::Unknown);
  EXPECT_EQ(Constant.RetryCharges, 0u);
  OrderedProblem NonBoolean;
  EXPECT_EQ(NonBoolean.check(NonBoolean.X), SatResult::Unknown);
  EXPECT_EQ(NonBoolean.RetryCharges, 0u);
  OrderedProblem Nested;
  const auto Other = Nested.Ctx.mkAnd(
      Nested.First, Nested.Ctx.mkEq(Nested.Z, Nested.Ctx.mkZero(4)));
  EXPECT_EQ(Nested.check(Other), SatResult::Unknown);
  EXPECT_EQ(Nested.RetryCharges, 0u);
}

TEST(OrderedQuery, DirectFactorInspectionHasAnExactBound) {
  OrderedProblem P;
  std::vector<SymRef> Factors(P.Ctx.operands(P.Full).begin(),
                              P.Ctx.operands(P.Full).end());
  for (unsigned I = Factors.size(); I != 64; ++I)
    Factors.push_back(P.Ctx.mkFreshVar(1));
  const auto Exact = P.Ctx.mkAnd(Factors);
  ASSERT_EQ(P.Ctx.numOperands(Exact), 64u);
  EXPECT_TRUE(isOriginalQueryConjunct(P.Ctx, Exact, P.First));
  Factors.push_back(P.Ctx.mkFreshVar(1));
  const auto Large = P.Ctx.mkAnd(Factors);
  ASSERT_EQ(P.Ctx.numOperands(Large), 65u);
  EXPECT_FALSE(isOriginalQueryConjunct(P.Ctx, Large, P.First));
}

} // namespace
