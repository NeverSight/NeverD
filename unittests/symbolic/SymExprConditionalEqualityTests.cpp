//===- SymExprConditionalEqualityTests.cpp - Conditional tag predicates ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check conditional tag comparisons against independent value oracles and
/// retain arbitrary data inputs while recovering their control predicates.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/APInt.h"

using namespace neverd::symbolic;

TEST(SymExprConditionalEquality, PromotedTagsRecoverTheOriginalByteGuard) {
  SymContext C;
  const auto Byte = C.mkVar("byte", 8);
  const auto Zero = C.mkEq(Byte, C.mkZero(8));
  const auto Tag = C.mkIte(Zero, C.mkConst(8, 0x91), C.mkConst(8, 0x27));
  for (unsigned Width : {8U, 32U, 64U, 128U, 256U}) {
    for (bool Signed : {false, true}) {
      const auto Promoted =
          Signed ? C.mkSExt(Tag, Width) : C.mkZExt(Tag, Width);
      const llvm::APInt TagValue(8, 0x91);
      const auto Literal = C.mkConst(Signed ? TagValue.sextOrTrunc(Width)
                                            : TagValue.zextOrTrunc(Width));
      const auto Guard = C.mkEq(Promoted, Literal);
      EXPECT_EQ(Guard, Zero);
      EXPECT_EQ(C.mkEq(Literal, Promoted), Zero);
      // The original data remains arbitrary, including all 255 nonzero bytes.
      const auto Selected = C.mkIte(Guard, C.mkConst(8, 19), Byte);
      for (unsigned Value = 0; Value != 256; ++Value)
        EXPECT_EQ(C.eval(Selected, {llvm::APInt(8, Value)}).getZExtValue(),
                  Value ? Value : 19);
    }
  }
}

TEST(SymExprConditionalEquality, ExhaustiveSmallTagsMatchHostComparisons) {
  for (unsigned Width = 1; Width <= 4; ++Width) {
    SymContext C;
    const auto P = C.mkVar("p", 1);
    const unsigned Count = 1U << Width;
    for (unsigned Yes = 0; Yes != Count; ++Yes)
      for (unsigned No = 0; No != Count; ++No) {
        const auto Tag =
            C.mkIte(P, C.mkConst(Width, Yes), C.mkConst(Width, No));
        for (unsigned Compared = 0; Compared != Count; ++Compared) {
          const auto Literal = C.mkConst(Width, Compared);
          const auto Equal = C.mkEq(Tag, Literal);
          EXPECT_EQ(Equal, C.mkEq(Literal, Tag));
          for (unsigned Condition : {0U, 1U}) {
            const unsigned Actual = Condition ? Yes : No;
            const llvm::APInt Input(1, Condition);
            EXPECT_EQ(C.eval(Equal, {Input}).isOne(), Actual == Compared);
            EXPECT_EQ(C.eval(C.mkNe(Tag, Literal), {Input}).isOne(),
                      Actual != Compared);
          }
        }
      }
  }
}

TEST(SymExprConditionalEquality, WideTagsDistinguishAllHighBits) {
  for (unsigned Width : {65U, 128U, 257U, 4097U}) {
    SymContext C;
    const auto P = C.mkVar("p", 1);
    const auto Low = C.mkConst(Width, 7);
    const auto High =
        C.mkConst(llvm::APInt::getSignMask(Width) | llvm::APInt(Width, 7));
    const auto Neither = C.mkConst(llvm::APInt::getOneBitSet(Width, Width - 2) |
                                   llvm::APInt(Width, 7));
    const auto Tagged = C.mkIte(P, High, Low);
    EXPECT_EQ(C.mkEq(Tagged, High), P);
    EXPECT_EQ(C.mkEq(Tagged, Low), C.mkNot(P));
    EXPECT_EQ(C.mkEq(Tagged, Neither), C.mkFalse());
  }
}

TEST(SymExprConditionalEquality, VariableArmsRetainTheirDataDependencies) {
  SymContext C;
  const auto P = C.mkVar("p", 1), X = C.mkVar("x", 4);
  const auto Y = C.mkVar("y", 4), K = C.mkConst(4, 7);
  const auto Left = C.mkEq(C.mkIte(P, X, K), K);
  const auto Right = C.mkEq(C.mkIte(P, K, X), K);
  const auto Both = C.mkEq(C.mkIte(P, X, Y), K);
  const auto Variable = C.mkEq(C.mkIte(P, C.mkConst(4, 3), K), X);
  for (unsigned Condition : {0U, 1U})
    for (unsigned A = 0; A != 16; ++A)
      for (unsigned B = 0; B != 16; ++B) {
        const llvm::APInt Inputs[] = {llvm::APInt(1, Condition),
                                      llvm::APInt(4, A), llvm::APInt(4, B)};
        EXPECT_EQ(C.eval(Left, Inputs).isOne(), !Condition || A == 7);
        EXPECT_EQ(C.eval(Right, Inputs).isOne(), Condition || A == 7);
        EXPECT_EQ(C.eval(Both, Inputs).isOne(), (Condition ? A : B) == 7);
        EXPECT_EQ(C.eval(Variable, Inputs).isOne(), (Condition ? 3U : 7U) == A);
      }
}

TEST(SymExprConditionalEquality, DoesNotTraverseOrRewriteTheCondition) {
  SymContext C;
  auto X = C.mkVar("x", 64);
  const auto Divisor = C.mkVar("divisor", 64);
  for (unsigned I = 0; I != 4096; ++I)
    X = C.mkUDiv(X, Divisor);
  const auto Condition = C.mkEq(X, C.mkConst(64, 17));
  const auto Opposite = C.mkNot(Condition), False = C.mkFalse();
  const auto Yes = C.mkConst(64, 3), No = C.mkConst(64, 9);
  const auto Neither = C.mkConst(64, 5);
  const auto Tagged = C.mkIte(Condition, Yes, No);
  const auto Before = C.numNodes();
  EXPECT_EQ(C.mkEq(Tagged, Yes), Condition);
  EXPECT_EQ(C.mkEq(Tagged, No), Opposite);
  EXPECT_EQ(C.mkEq(Tagged, Neither), False);
  EXPECT_EQ(C.numNodes(), Before);
  EXPECT_EQ(C.operand(Tagged, 0), Condition);
}
