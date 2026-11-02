//===- SymExprBooleanEqualityTests.cpp - Boolean equality identity --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify Boolean equality normalization before and after integer promotion,
/// retaining the distinction between predicates and wider integer values.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/APInt.h"

using namespace neverd::symbolic;

TEST(SymExprBooleanEquality, PromotedTestsRetainTheUnderlyingPredicate) {
  SymContext C;
  const auto P = C.mkVar("p", 1);
  for (unsigned Width : {1u, 8u, 32u, 64u, 128u, 256u})
    for (bool Signed : {false, true}) {
      const auto Value = Signed ? C.mkSExt(P, Width) : C.mkZExt(P, Width);
      const auto True = C.mkConst(Signed ? llvm::APInt::getAllOnes(Width)
                                         : llvm::APInt(Width, 1));
      const auto Zero = C.mkZero(Width);
      EXPECT_EQ(C.mkEq(Value, True), P);
      EXPECT_EQ(C.mkEq(True, Value), P);
      EXPECT_EQ(C.mkEq(Value, Zero), C.mkNot(P));
      EXPECT_EQ(C.mkEq(Zero, Value), C.mkNot(P));
      EXPECT_EQ(C.mkNe(Value, Zero), P);
      EXPECT_EQ(C.mkNe(Value, True), C.mkNot(P));
      for (unsigned Input : {0u, 1u}) {
        const llvm::APInt Bits(1, Input);
        EXPECT_EQ(C.eval(C.mkEq(Value, Zero), {Bits}).isOne(), Input == 0);
        EXPECT_EQ(C.eval(C.mkEq(Value, True), {Bits}).isOne(), Input == 1);
      }
    }
}

TEST(SymExprBooleanEquality, WiderIntegersRemainFullValueComparisons) {
  SymContext C;
  const auto X = C.mkVar("x", 8);
  for (unsigned Literal : {0u, 1u}) {
    const auto Equal = C.mkEq(X, C.mkConst(8, Literal));
    for (unsigned Input = 0; Input != 256; ++Input)
      EXPECT_EQ(C.eval(Equal, {llvm::APInt(8, Input)}).isOne(),
                Input == Literal);
  }
}

TEST(SymExprBooleanEquality, DoesNotTraverseThePredicate) {
  SymContext C;
  auto X = C.mkVar("x", 64);
  const auto Divisor = C.mkVar("divisor", 64);
  for (unsigned I = 0; I != 4096; ++I)
    X = C.mkUDiv(X, Divisor);
  const auto P = C.mkEq(X, C.mkConst(64, 17));
  const auto NotP = C.mkNot(P), Zero = C.mkFalse(), One = C.mkTrue();
  const auto Before = C.numNodes();
  EXPECT_EQ(C.mkEq(P, One), P);
  EXPECT_EQ(C.mkEq(P, Zero), NotP);
  EXPECT_EQ(C.mkNe(P, Zero), P);
  EXPECT_EQ(C.numNodes(), Before);
}
