//===- SymExprLiteralCarrierTests.cpp - Exact constructor regressions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent bitvector evaluations and constructor refusal controls.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/APInt.h"

#include <algorithm>
#include <array>
#include <vector>

using namespace neverd::symbolic;

TEST(SymExpr, LiteralSelectionsFoldConversionsAndConstantBitwiseOperations) {
  for (unsigned Width : {2, 8, 16, 32, 64, 128}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const auto Condition = C.mkVar("condition", 1);
    const llvm::APInt Yes = llvm::APInt::getSignMask(Width) | 1;
    const llvm::APInt No = llvm::APInt(Width, 2);
    const llvm::APInt Mask(Width, 0xA5, false, true);
    const auto Select = C.mkIte(Condition, C.mkConst(Yes), C.mkConst(No));
    const auto Expected = [&](const llvm::APInt &T, const llvm::APInt &F) {
      return C.mkIte(Condition, C.mkConst(T), C.mkConst(F));
    };
    const auto ZeroExtended = C.mkZExt(Select, Width * 2);
    const auto SignExtended = C.mkSExt(Select, Width * 2);
    EXPECT_EQ(C.op(ZeroExtended), SymOp::ZExt);
    EXPECT_EQ(C.op(SignExtended), SymOp::SExt);
    EXPECT_EQ(C.mkNot(Select), Expected(~Yes, ~No));
    EXPECT_EQ(C.mkAnd(Select, C.mkConst(Mask)),
              Expected(Yes & Mask, No & Mask));
    EXPECT_EQ(C.mkOr(Select, C.mkConst(Mask)), Expected(Yes | Mask, No | Mask));
    EXPECT_EQ(C.mkXor(Select, C.mkConst(Mask)),
              Expected(Yes ^ Mask, No ^ Mask));
    for (unsigned Bit : {0, 1}) {
      const llvm::APInt Value = Bit ? Yes : No;
      const std::vector<llvm::APInt> Inputs{llvm::APInt(1, Bit)};
      EXPECT_EQ(C.eval(ZeroExtended, Inputs), Value.zext(Width * 2));
      EXPECT_EQ(C.eval(SignExtended, Inputs), Value.sext(Width * 2));
      EXPECT_EQ(C.eval(C.mkNot(ZeroExtended), Inputs), ~Value.zext(Width * 2));
      EXPECT_EQ(C.eval(C.mkNot(SignExtended), Inputs), ~Value.sext(Width * 2));
      EXPECT_EQ(C.eval(C.mkAnd(Select, C.mkConst(Mask)), Inputs), Value & Mask);
      EXPECT_EQ(C.eval(C.mkOr(Select, C.mkConst(Mask)), Inputs), Value | Mask);
      EXPECT_EQ(C.eval(C.mkXor(Select, C.mkConst(Mask)), Inputs), Value ^ Mask);
    }
  }
}

TEST(SymExpr, LiteralSelectionFoldingDoesNotDistributeDataValuedArms) {
  SymContext C;
  const auto Condition = C.mkVar("condition", 1);
  const auto X = C.mkVar("x", 16), Y = C.mkVar("y", 16);
  const auto Select = C.mkIte(Condition, X, Y);
  EXPECT_EQ(C.op(C.mkZExt(Select, 64)), SymOp::ZExt);
  EXPECT_EQ(C.op(C.mkSExt(Select, 64)), SymOp::SExt);
  EXPECT_EQ(C.op(C.mkNot(Select)), SymOp::Not);
  EXPECT_EQ(C.op(C.mkAnd(Select, C.mkConst(16, 0xF0))), SymOp::And);
}

TEST(SymExpr, LiteralCarrierExtensionsRetainEveryHighConstantBit) {
  for (unsigned Width : {4u, 8u, 64u, 128u}) {
    SymContext C;
    const auto P = C.mkVar("p", 1);
    const auto Selected = C.mkIte(P, C.mkConst(Width, 1), C.mkConst(Width, 2));
    const auto Widened = C.mkSExt(Selected, 2 * Width);
    EXPECT_EQ(Widened, C.mkZExt(Selected, 2 * Width));
    const auto High = llvm::APInt::getHighBitsSet(2 * Width, Width);
    const auto Tagged = C.mkOr(Widened, C.mkConst(High));
    const auto Toggled = C.mkXor(Widened, C.mkConst(High | 1));
    for (unsigned Value : {0u, 1u}) {
      const llvm::APInt Input(1, Value);
      const llvm::APInt Expected(2 * Width, Value ? 1 : 2);
      EXPECT_EQ(C.eval(Tagged, {Input}), Expected | High);
      EXPECT_EQ(C.eval(Toggled, {Input}), Expected ^ (High | 1));
    }
  }
}
