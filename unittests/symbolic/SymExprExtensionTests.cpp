//===- SymExprExtensionTests.cpp - Width-preserving bitwise rules ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"

using namespace neverd::symbolic;

TEST(SymExprExtension, BitwiseContractionPreservesHighConstantBits) {
  for (unsigned Width : {1U, 3U, 8U, 32U, 64U, 128U, 256U}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const unsigned Wide = Width * 2;
    auto ZX = C.mkZExt(X, Wide), ZY = C.mkZExt(Y, Wide);
    auto Low = llvm::APInt::getLowBitsSet(Width, (Width + 1) / 2);
    auto WideLow = Low.zext(Wide);
    auto High = WideLow | llvm::APInt::getSignMask(Wide);
    EXPECT_EQ(C.mkAnd({ZX, ZY, C.mkConst(High)}),
              C.mkZExt(C.mkAnd({X, Y, C.mkConst(Low)}), Wide));
    EXPECT_EQ(C.mkOr({ZX, ZY, C.mkConst(WideLow)}),
              C.mkZExt(C.mkOr({X, Y, C.mkConst(Low)}), Wide));
    EXPECT_EQ(C.mkXor({ZX, ZY, C.mkConst(WideLow)}),
              C.mkZExt(C.mkXor({X, Y, C.mkConst(Low)}), Wide));
    auto HighOr = C.mkOr({ZX, ZY, C.mkConst(High)});
    auto HighXor = C.mkXor({ZX, ZY, C.mkConst(High)});
    auto Mixed = C.mkAnd(ZX, C.mkSExt(Y, Wide));
    for (const auto &A : {llvm::APInt(Width, 0), Low, ~Low})
      for (const auto &B : {llvm::APInt(Width, 0), Low, ~Low}) {
        EXPECT_EQ(C.eval(HighOr, {A, B}), A.zext(Wide) | B.zext(Wide) | High);
        EXPECT_EQ(C.eval(HighXor, {A, B}), A.zext(Wide) ^ B.zext(Wide) ^ High);
        EXPECT_EQ(C.eval(Mixed, {A, B}), A.zext(Wide) & B.sext(Wide));
      }
  }
}

TEST(SymExprExtension, LogicalShiftRetainsTheFullCountAcrossZeroExtension) {
  for (unsigned Width : {1U, 3U, 8U, 32U, 64U, 128U}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Count = C.mkVar("count", 256);
    auto Shift = C.mkLShr(C.mkZExt(X, Width * 2), Count);
    EXPECT_EQ(Shift, C.mkZExt(C.mkLShr(X, Count), Width * 2));
    for (const auto &N : {llvm::APInt(256, 0), llvm::APInt(256, Width - 1),
                          llvm::APInt(256, Width), llvm::APInt(256, 256),
                          llvm::APInt::getOneBitSet(256, 200)}) {
      auto A = llvm::APInt::getAllOnes(Width);
      auto Expected =
          N.uge(Width) ? llvm::APInt(Width, 0) : A.lshr(N.getZExtValue());
      EXPECT_EQ(C.eval(Shift, {A, N}), Expected.zext(Width * 2));
    }
  }
}
