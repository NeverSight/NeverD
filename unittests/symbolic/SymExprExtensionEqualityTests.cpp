//===- SymExprExtensionEqualityTests.cpp - Promoted predicate identity
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent value oracles and bounded canonicalization for equality after
/// integer extension, including signed, mixed and out-of-range operands.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/APInt.h"

using namespace neverd::symbolic;

TEST(SymExprExtensionEquality, PromotedByteGuardsReuseTheirOriginalPredicate) {
  for (unsigned Wide : {16U, 32U, 64U, 128U, 256U}) {
    SymContext C;
    const auto Byte = C.mkVar("byte", 8);
    const auto Zero = C.mkEq(Byte, C.mkZero(8));
    for (auto Promoted : {C.mkZExt(Byte, Wide), C.mkSExt(Byte, Wide)}) {
      EXPECT_EQ(C.mkEq(Promoted, C.mkZero(Wide)), Zero);
      EXPECT_EQ(C.mkEq(C.mkZero(Wide), Promoted), Zero);
      EXPECT_EQ(C.mkNe(Promoted, C.mkZero(Wide)), C.mkNot(Zero));
      const auto Selected = C.mkIte(C.mkNe(Promoted, C.mkZero(Wide)),
                                    C.mkZExt(Byte, Wide), C.mkConst(Wide, 19));
      // Nonzero remains the complete input range, not a representative one.
      for (unsigned Value = 0; Value != 256; ++Value)
        EXPECT_EQ(C.eval(Selected, {llvm::APInt(8, Value)}).getZExtValue(),
                  Value ? Value : 19);
    }
  }
}

TEST(SymExprExtensionEquality, ConstantComparisonsMatchExhaustiveHostOracle) {
  for (unsigned Narrow = 1; Narrow <= 4; ++Narrow)
    for (unsigned Wide = Narrow + 1; Wide <= 8; ++Wide) {
      SymContext C;
      const auto X = C.mkVar("x", Narrow);
      const auto Z = C.mkZExt(X, Wide), S = C.mkSExt(X, Wide);
      for (unsigned Number = 0; Number != (1U << Wide); ++Number) {
        const auto K = C.mkConst(Wide, Number);
        const auto ZE = C.mkEq(Z, K), SE = C.mkEq(S, K);
        EXPECT_EQ(ZE, C.mkEq(K, Z));
        EXPECT_EQ(SE, C.mkEq(K, S));
        for (unsigned Value = 0; Value != (1U << Narrow); ++Value) {
          const int Signed = Value & (1U << (Narrow - 1))
                                 ? int(Value) - int(1U << Narrow)
                                 : int(Value);
          const unsigned Extended = unsigned(Signed) & ((1U << Wide) - 1);
          const auto Input = llvm::APInt(Narrow, Value);
          EXPECT_EQ(C.eval(ZE, {Input}).isOne(), Value == Number);
          EXPECT_EQ(C.eval(SE, {Input}).isOne(), Extended == Number);
        }
      }
    }
}

TEST(SymExprExtensionEquality, WideConstantsRetainTheirUpperAndSignBits) {
  for (unsigned Narrow :
       {1U, 8U, 31U, 32U, 63U, 64U, 65U, 127U, 128U, 255U, 256U, 511U, 512U}) {
    SymContext C;
    const auto X = C.mkVar("x", Narrow);
    const unsigned Wide = Narrow * 2;
    const auto Z = C.mkZExt(X, Wide), S = C.mkSExt(X, Wide);
    for (const auto &Value :
         {llvm::APInt(Narrow, 0), llvm::APInt(Narrow, 1),
          llvm::APInt::getSignMask(Narrow), llvm::APInt::getAllOnes(Narrow)}) {
      const auto Expected = C.mkEq(X, C.mkConst(Value));
      EXPECT_EQ(C.mkEq(Z, C.mkConst(Value.zext(Wide))), Expected);
      EXPECT_EQ(C.mkEq(S, C.mkConst(Value.sext(Wide))), Expected);
    }
    EXPECT_EQ(C.mkEq(Z, C.mkConst(llvm::APInt::getOneBitSet(Wide, Narrow))),
              C.mkFalse());
    EXPECT_EQ(C.mkEq(S, C.mkConst(llvm::APInt::getSignMask(Narrow).zext(Wide))),
              C.mkFalse());
    EXPECT_EQ(C.mkEq(S, C.mkConst(llvm::APInt::getSignMask(Wide))),
              C.mkFalse());
  }
}

TEST(SymExprExtensionEquality,
     EqualExtensionsContractButMixedFormsKeepMeaning) {
  SymContext C;
  const auto X = C.mkVar("x", 4), Y = C.mkVar("y", 4);
  const auto Z = C.mkVar("z", 3);
  EXPECT_EQ(C.mkEq(C.mkZExt(X, 32), C.mkZExt(Y, 32)), C.mkEq(X, Y));
  EXPECT_EQ(C.mkEq(C.mkSExt(X, 32), C.mkSExt(Y, 32)), C.mkEq(X, Y));
  const auto Mixed = C.mkEq(C.mkZExt(X, 8), C.mkSExt(Y, 8));
  const auto Unequal = C.mkEq(C.mkZExt(X, 8), C.mkZExt(Z, 8));
  for (unsigned A = 0; A != 16; ++A)
    for (unsigned B = 0; B != 16; ++B)
      for (unsigned D = 0; D != 8; ++D) {
        const llvm::APInt Inputs[] = {llvm::APInt(4, A), llvm::APInt(4, B),
                                      llvm::APInt(3, D)};
        EXPECT_EQ(C.eval(Mixed, Inputs).isOne(), A == (B < 8 ? B : B + 240));
        EXPECT_EQ(C.eval(Unequal, Inputs).isOne(), A == D);
      }
}

TEST(SymExprExtensionEquality, BoundedInspectionDoesNotTraverseTheSourceDAG) {
  using namespace neverd::limits::symbolic;
  SymContext C;
  auto X = C.mkVar("x", 8);
  const auto Divisor = C.mkVar("divisor", 8);
  for (unsigned I = 0; I != 4096; ++I)
    X = C.mkUDiv(X, Divisor);
  const auto Want = C.mkEq(X, C.mkConst(8, 7));
  const auto Extended = C.mkSExt(X, 64), K = C.mkConst(64, 7);
  const auto Before = C.numNodes();
  EXPECT_EQ(C.mkEq(Extended, K), Want);
  EXPECT_EQ(C.numNodes(), Before);

  for (unsigned Wide : {kSymExprExtensionCompareMaxWords * 64,
                        kSymExprExtensionCompareMaxWords * 64 + 1}) {
    const auto Z = C.mkZExt(X, Wide), S = C.mkSExt(X, Wide);
    const auto Seven = C.mkConst(Wide, 7);
    EXPECT_EQ(C.mkEq(Z, Seven) == Want,
              Wide == kSymExprExtensionCompareMaxWords * 64);
    EXPECT_EQ(C.mkEq(S, Seven) == Want,
              Wide == kSymExprExtensionCompareMaxWords * 64);
  }
}

TEST(SymExprExtensionEquality,
     ExtensionDepthLimitRetainsTheRemainingPredicate) {
  using namespace neverd::limits::symbolic;
  for (unsigned Count : {kSymExprExtensionEqualityMaxSteps,
                         kSymExprExtensionEqualityMaxSteps + 1}) {
    SymContext C;
    const auto X = C.mkVar("x", 4), Y = C.mkVar("y", 4);
    auto A = X, B = Y;
    for (unsigned I = 0; I != Count; ++I) {
      A = I % 2 ? C.mkZExt(A, I + 5) : C.mkSExt(A, I + 5);
      B = I % 2 ? C.mkZExt(B, I + 5) : C.mkSExt(B, I + 5);
    }
    const auto Result = C.mkEq(A, B);
    EXPECT_EQ(Result == C.mkEq(X, Y),
              Count == kSymExprExtensionEqualityMaxSteps);
    for (unsigned V = 0; V != 16; ++V)
      for (unsigned W = 0; W != 16; ++W)
        EXPECT_EQ(
            C.eval(Result, {llvm::APInt(4, V), llvm::APInt(4, W)}).isOne(),
            V == W);
  }
}
