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

// A bounded search must still find these questions hard after symbolic
// construction, which cancels a summand both sides of an equality share.
// Each subexpression is built in its own statement: argument evaluation order
// differs between compilers and node order changes the search.
enum class Shape {
  // y odd, x * y + z == w + bias with w == z bounded from both sides.
  Product,
  // y odd, x * y + z == 3 * z + bias; the low bits disagree for an odd x
  // exactly when bias is even.
  Parity,
  // x == y with x * z + bias != y * z.
  EqualProducts,
};

struct OrderedProblem {
  SymContext Ctx;
  SolverOptions Options;
  SymRef X, Y, Z, Domain, First, Full;
  unsigned RetryCharges = 0;

  OrderedProblem(unsigned Width = 4, unsigned Bias = 0,
                 Shape Kind = Shape::Product) {
    Options.BuildModel = false;
    Options.Blast.MaxGates = 262144;
    Options.Sat.MaxConflicts = 10000;
    Options.Sat.MaxPropagations = Kind == Shape::EqualProducts ? 10425
                                  : Kind == Shape::Parity      ? 246
                                                               : 64;
    Options.Sat.MaxWatchVisits = 10000000;
    X = Ctx.mkVar("x", Width);
    Y = Ctx.mkVar("y", Width);
    Z = Ctx.mkVar("z", Width);
    const auto One = Ctx.mkConst(Width, 1);
    if (Kind == Shape::EqualProducts) {
      Domain = Ctx.mkEq(X, Y);
      const auto Left = Ctx.mkMul(X, Z);
      const auto Biased = Ctx.mkAdd(Left, Ctx.mkConst(Width, Bias));
      const auto Right = Ctx.mkMul(Y, Z);
      First = Ctx.mkNe(Biased, Right);
    } else if (Kind == Shape::Parity) {
      const auto LowY = Ctx.mkAnd(Y, One);
      const auto Odd = Ctx.mkEq(LowY, One);
      const auto Product = Ctx.mkMul(X, Y);
      const auto Left = Ctx.mkAdd(Product, Z);
      const auto Triple = Ctx.mkMul(Z, Ctx.mkConst(Width, 3));
      const auto Right = Ctx.mkAdd(Triple, Ctx.mkConst(Width, Bias));
      const auto Equal = Ctx.mkEq(Left, Right);
      Domain = Ctx.mkAnd(Odd, Equal);
      const auto LowX = Ctx.mkAnd(X, One);
      First = Ctx.mkEq(LowX, One);
    } else {
      const auto W = Ctx.mkVar("w", Width);
      const auto LowY = Ctx.mkAnd(Y, One);
      const auto Odd = Ctx.mkEq(LowY, One);
      const auto Below = Ctx.mkUle(W, Z);
      const auto Above = Ctx.mkUle(Z, W);
      const auto Product = Ctx.mkMul(X, Y);
      const auto Left = Ctx.mkAdd(Product, Z);
      const auto Right = Ctx.mkAdd(W, Ctx.mkConst(Width, Bias));
      const auto Equal = Ctx.mkEq(Left, Right);
      Domain = Ctx.mkAnd({Odd, Below, Above, Equal});
      const auto Zero = Ctx.mkZero(Width);
      const auto IsZero = Ctx.mkEq(X, Zero);
      First = Ctx.mkNot(IsZero);
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
  // The parity question at the width its retry tests use: odd x and y leave
  // x * y odd, so only an odd bias has witnesses.
  for (unsigned Bias : {0u, 3u}) {
    unsigned Witnesses = 0;
    for (unsigned X = 1; X < 256; X += 2)
      for (unsigned Y = 1; Y < 256; Y += 2)
        for (unsigned Z = 0; Z != 256; ++Z)
          Witnesses += ((X * Y + Z) & 255) == ((3 * Z + Bias) & 255);
    EXPECT_EQ(Witnesses, Bias ? 32768u : 0u);
  }
}

TEST(OrderedQuery, CompleteUnsatRetriesExactlyOnce) {
  OrderedProblem P(5, 0, Shape::EqualProducts);
  ASSERT_EQ(checkSat(P.Ctx, P.Full, nullptr, P.Options), SatResult::Unknown);
  ASSERT_EQ(checkSat(P.Ctx, P.First, nullptr, P.Options), SatResult::Sat);
  EXPECT_EQ(P.check(P.First), SatResult::Unsat);
  EXPECT_EQ(P.RetryCharges, 1u);
}

TEST(OrderedQuery, CompleteSatRetriesExactlyOnce) {
  OrderedProblem P(8, 3, Shape::Parity);
  ASSERT_EQ(checkSat(P.Ctx, P.Full, nullptr, P.Options), SatResult::Unknown);
  EXPECT_EQ(P.check(P.First), SatResult::Sat);
  EXPECT_EQ(P.RetryCharges, 1u);
}

TEST(OrderedQuery, UndecidedSatisfiableQuestionRemainsUnknown) {
  OrderedProblem P(8, 3, Shape::Parity);
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
  OrderedProblem P(8, 3, Shape::Parity);
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
