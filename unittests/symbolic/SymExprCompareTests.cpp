//===- SymExprCompareTests.cpp - Subtraction predicate recovery -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/expr/SymExprCompare.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymMBA.h"

using namespace neverd::symbolic;

namespace {

SymRef signedWord(SymContext &C, SymRef X, SymRef Y) {
  SymRef D = C.mkSub(X, Y);
  return C.mkXor(D, C.mkAnd(C.mkXor(X, Y), C.mkXor(X, D)));
}

SymRef borrowWord(SymContext &C, SymRef X, SymRef Y) {
  return C.mkOr(C.mkAnd(C.mkNot(X), Y),
                C.mkAnd(C.mkNot(C.mkXor(X, Y)), C.mkSub(X, Y)));
}

TEST(SymExprCompare, HighestBitRecoversSignedAndUnsignedComparisons) {
  for (unsigned W : {1u, 3u, 8u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(W);
    SymContext C;
    SymRef X = C.mkVar("x", W), Y = C.mkVar("y", W);
    SymRef S = signedWord(C, X, Y), U = borrowWord(C, X, Y);
    SCOPED_TRACE(C.toString(U));
    SymRef Count = C.mkConst(16, W - 1);
    EXPECT_EQ(C.mkLShr(S, Count), C.mkZExt(C.mkSlt(X, Y), W));
    EXPECT_EQ(C.mkLShr(U, Count), C.mkZExt(C.mkUlt(X, Y), W));
    EXPECT_EQ(C.mkExtract(S, W - 1, 1), C.mkSlt(X, Y));
    EXPECT_EQ(C.mkExtract(U, W - 1, 1), C.mkUlt(X, Y));
    EXPECT_EQ(C.mkAShr(S, Count), C.mkSExt(C.mkSlt(X, Y), W));
    EXPECT_EQ(C.mkAShr(U, C.mkConst(16, W + 1)), C.mkSExt(C.mkUlt(X, Y), W));
  }
}

TEST(SymExprCompare, SplitSignAndOverflowViewsShareTheSamePredicate) {
  for (unsigned W : {3u, 8u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(W);
    SymContext C;
    SymRef Y = C.mkVar("y", W), X = C.mkVar("x", W);
    SymRef D = C.mkSub(X, Y);
    SymRef V = C.mkAnd(C.mkXor(Y, X), C.mkXor(D, X));
    SymRef Count = C.mkConst(16, W - 1);
    SymRef Expected = C.mkSlt(X, Y);
    EXPECT_EQ(C.mkXor(C.mkLShr(D, Count), C.mkLShr(V, Count)),
              C.mkZExt(Expected, W));
    EXPECT_EQ(C.mkXor(C.mkExtract(V, W - 1, 1), C.mkExtract(D, W - 1, 1)),
              Expected);
    EXPECT_EQ(C.mkXor(C.mkSlt(V, C.mkZero(W)), C.mkSlt(D, C.mkZero(W))),
              Expected);
    EXPECT_EQ(C.mkXor(C.mkZExt(C.mkSlt(V, C.mkZero(W)), 8),
                      C.mkZExt(C.mkExtract(D, W - 1, 1), 8)),
              C.mkZExt(Expected, 8));
  }
}

TEST(SymExprCompare, WrongDifferenceAndProjectionRemainDistinct) {
  for (unsigned W : {3u, 8u, 32u, 64u, 128u, 256u}) {
    SymContext C;
    SymRef X = C.mkVar("x", W), Y = C.mkVar("y", W);
    SymRef Z = C.mkVar("z", W), D = C.mkSub(Z, Y);
    SymRef Wrong = C.mkXor(D, C.mkAnd(C.mkXor(X, Y), C.mkXor(X, D)));
    SymRef Expected = C.mkZExt(C.mkSlt(X, Y), W);
    EXPECT_NE(C.mkLShr(Wrong, C.mkConst(16, W - 1)), Expected);
    SymRef S = signedWord(C, X, Y);
    EXPECT_NE(C.mkLShr(S, C.mkConst(16, W - 2)), Expected);
    EXPECT_NE(C.mkLShr(S, C.mkVar("count", 16)), Expected);
    EXPECT_NE(C.mkLShr(S, C.mkConst(16, W - 1)), C.mkZExt(C.mkSlt(Y, X), W));
    EXPECT_EQ(C.mkLShr(S, C.mkConst(16, W)), C.mkZero(W));
  }
}

TEST(SymExprCompare, CanonicalDifferencesKeepCompoundOperands) {
  for (unsigned W : {1u, 3u, 8u, 32u, 128u, 256u}) {
    SymContext C;
    SymRef A = C.mkVar("a", W), B = C.mkVar("b", W);
    SymRef V = C.mkVar("v", W), Z = C.mkVar("z", W);
    for (SymRef X : {C.mkAdd(A, B), C.mkMul(A, B)}) {
      for (SymRef Y : {C.mkAdd(V, Z), C.mkNeg(C.mkAdd(V, Z)),
                       C.mkMul(C.mkConst(W, 3), V)}) {
        SCOPED_TRACE(W);
        SCOPED_TRACE(C.toString(X) + " vs " + C.toString(Y));
        SymRef S = signedWord(C, X, Y);
        SymRef U = borrowWord(C, X, Y);
        EXPECT_EQ(C.mkExtract(S, W - 1, 1), C.mkSlt(X, Y));
        EXPECT_EQ(C.mkExtract(U, W - 1, 1), C.mkUlt(X, Y));
      }
    }
  }
}

TEST(SymExprCompare, ExhaustiveSmallWordsKeepFullValueConsumers) {
  for (unsigned W : {1u, 3u, 8u}) {
    SymContext C;
    SymRef X = C.mkVar("x", W), Y = C.mkVar("y", W);
    SymRef D = C.mkSub(X, Y);
    SymRef V = C.mkAnd(C.mkXor(X, Y), C.mkXor(X, D));
    SymRef S = C.mkXor(D, V), U = borrowWord(C, X, Y);
    SymRef SP = C.mkExtract(S, W - 1, 1);
    SymRef UP = C.mkExtract(U, W - 1, 1);
    const unsigned Limit = 1u << W;
    for (unsigned A = 0; A < Limit; ++A) {
      for (unsigned B = 0; B < Limit; ++B) {
        llvm::APInt AV(W, A), BV(W, B), DV = AV - BV;
        llvm::SmallVector<llvm::APInt, 2> Values{AV, BV};
        EXPECT_EQ(C.eval(SP, Values), llvm::APInt(1, AV.slt(BV)));
        EXPECT_EQ(C.eval(UP, Values), llvm::APInt(1, AV.ult(BV)));
        EXPECT_EQ(C.eval(D, Values), DV);
        EXPECT_EQ(C.eval(V, Values), (AV ^ BV) & (AV ^ DV));
        EXPECT_EQ(C.eval(S, Values), DV ^ ((AV ^ BV) & (AV ^ DV)));
        EXPECT_EQ(C.eval(U, Values), (~AV & BV) | (~(AV ^ BV) & DV));
      }
    }
  }
}

TEST(SymExprCompare, DeepAndWideRefusalDoesNotInternOrRecurse) {
  SymContext C;
  SymRef X = C.mkVar("x", 32), Y = C.mkVar("y", 32);
  SymRef E = signedWord(C, X, Y);
  for (unsigned I = 0; I < 10000; ++I)
    E = C.mkXor(C.mkAnd(E, X), C.mkAnd(E, C.mkNot(X)));
  const size_t BeforeDeep = C.numNodes();
  EXPECT_FALSE(detail::recoverSignComparison(C, E));
  EXPECT_EQ(C.numNodes(), BeforeDeep);
  EXPECT_EQ(C.evalU64(E, {17, 23}),
            (uint32_t(17 - 23) ^ ((17 ^ 23) & (17 ^ uint32_t(17 - 23)))));
  llvm::SmallVector<SymRef, 80> Arms;
  for (unsigned I = 0; I < 80; ++I)
    Arms.push_back(C.mkVar("arm" + std::to_string(I), 32));
  SymRef Wide = C.mkOr(Arms);
  const size_t BeforeWide = C.numNodes();
  EXPECT_FALSE(detail::recoverSignComparison(C, Wide));
  EXPECT_EQ(C.numNodes(), BeforeWide);
}

TEST(SymExprCompare, ComplementAndBorrowXorFormsRecoverExactly) {
  for (unsigned W : {3u, 8u, 64u, 256u}) {
    SymContext C;
    SymRef X = C.mkVar("x", W), Y = C.mkVar("y", W);
    SymRef D = C.mkSub(X, Y);
    SymRef U = C.mkXor(D, C.mkAnd(C.mkXor(D, Y), C.mkXor(X, Y)));
    EXPECT_EQ(C.mkExtract(U, W - 1, 1), C.mkUlt(X, Y));
    EXPECT_EQ(C.mkExtract(C.mkNot(U), W - 1, 1), C.mkNot(C.mkUlt(X, Y)));
    SymRef S = signedWord(C, X, Y);
    EXPECT_EQ(C.mkSlt(S, C.mkZero(W)), C.mkSlt(X, Y));
    EXPECT_EQ(C.mkSlt(C.mkNot(S), C.mkZero(W)), C.mkNot(C.mkSlt(X, Y)));
  }
}

TEST(SymExprCompare, FullShiftCountAndBooleanCarrierWidthsRemainObservable) {
  SymContext C;
  SymRef X = C.mkVar("x", 32), Y = C.mkVar("y", 32);
  SymRef D = C.mkSub(X, Y);
  SymRef V = C.mkAnd(C.mkXor(X, Y), C.mkXor(X, D));
  SymRef S = C.mkXor(D, V);
  llvm::APInt LargeCount(256, 31);
  LargeCount.setBit(128);
  EXPECT_EQ(C.mkLShr(S, C.mkConst(LargeCount)), C.mkZero(32));
  EXPECT_EQ(C.mkAShr(S, C.mkConst(LargeCount)), C.mkSExt(C.mkSlt(X, Y), 32));
  SymRef OneBit = C.mkVar("bit", 1);
  EXPECT_EQ(C.mkAShr(OneBit, C.mkVar("count", 256)), OneBit);
  SymRef WideFlag = C.mkNe(C.mkZExt(C.mkVar("three_bits", 3), 8), C.mkZero(8));
  SymRef Sign = C.mkSlt(D, C.mkZero(32));
  SymRef Unrelated = C.mkXor(Sign, WideFlag);
  EXPECT_NE(Unrelated, C.mkSlt(X, Y));
  const size_t Before = C.numNodes();
  EXPECT_FALSE(detail::recoverObservedComparison(
      C, SymOp::Xor, {Sign, WideFlag}, llvm::APInt(1, 0)));
  EXPECT_EQ(C.numNodes(), Before);
}

TEST(SymExprCompare, NegatedPackedEqualityDiscardsUnobservedLanes) {
  for (unsigned W : {8u, 32u, 64u, 128u}) {
    SCOPED_TRACE(W);
    SymContext C;
    const auto Noise = C.mkVar("noise", W);
    const auto A = C.mkVar("a", 1), B = C.mkVar("b", 1);
    const llvm::APInt Mask(W, 0x90), Target(W, 0x80);
    const auto Packed = C.mkOr({C.mkAnd(Noise, C.mkConst(~Mask)),
                                C.mkMul(C.mkZExt(A, W), C.mkConst(W, 0x10)),
                                C.mkMul(C.mkZExt(B, W), C.mkConst(W, 0x80))});
    const auto Encoded = C.mkNeg(C.mkOr(C.mkConst(~Mask), C.mkNot(Packed)));
    const auto Result =
        simplifyMBADeep(C, C.mkEq(Encoded, C.mkConst(Target + 1))).Expr;
    EXPECT_EQ(Result, C.mkAnd(C.mkNot(A), B)) << C.toString(Result);
    for (unsigned N = 0; N != 256; ++N)
      for (unsigned Bits = 0; Bits != 4; ++Bits) {
        const llvm::APInt Values[] = {llvm::APInt(W, N),
                                      llvm::APInt(1, Bits & 1),
                                      llvm::APInt(1, Bits >> 1)};
        EXPECT_EQ(C.eval(Result, Values), llvm::APInt(1, Bits == 2));
        // The original word still has all its bits for a full-value consumer.
        const auto Word =
            (llvm::APInt(W, N) & ~Mask) |
            llvm::APInt(W, (Bits & 1 ? 0x10 : 0) | (Bits & 2 ? 0x80 : 0));
        EXPECT_EQ(C.eval(Packed, Values), Word);
      }
  }
}

TEST(SymExprCompare, LiteralEqualityPeelsBijectiveWordOperations) {
  for (unsigned W : {1u, 4u, 8u, 64u, 128u}) {
    SCOPED_TRACE(W);
    SymContext C;
    const auto X = C.mkVar("x", W);
    for (unsigned Mode = 0; Mode != 4; ++Mode) {
      auto Encoded = X;
      if (Mode & 1)
        Encoded = C.mkNot(Encoded);
      if (Mode & 2)
        Encoded = C.mkNeg(Encoded);
      const auto Top = llvm::APInt::getSignMask(W);
      for (auto Target : {llvm::APInt(W, 0), llvm::APInt(W, 1), Top,
                          llvm::APInt::getAllOnes(W)}) {
        const auto Equal = C.mkEq(Encoded, C.mkConst(Target));
        const auto Unequal = C.mkNe(C.mkConst(Target), Encoded);
        for (unsigned I = 0; I != 256; ++I) {
          auto Value = llvm::APInt(W, I) ^ (I & 1 ? Top : llvm::APInt(W, 0));
          auto Expected = Value;
          if (Mode & 1)
            Expected = ~Expected;
          if (Mode & 2)
            Expected = -Expected;
          EXPECT_EQ(C.eval(Equal, {Value}), llvm::APInt(1, Expected == Target));
          EXPECT_EQ(C.eval(Unequal, {Value}),
                    llvm::APInt(1, Expected != Target));
        }
      }
    }
    // Multiplication by two is not injective, including when it wraps to
    // zero in a one-bit word. It cannot use the negation rule.
    const auto Doubled = C.mkMul(X, C.mkConst(W, 2));
    const auto IsZero = C.mkEq(Doubled, C.mkZero(W));
    const auto Half = llvm::APInt::getSignMask(W);
    EXPECT_EQ(C.eval(IsZero, {Half}), llvm::APInt(1, 1));
  }
}

TEST(SymExprCompare, MaskedPackedEqualityKeepsOverlappingObservedLanes) {
  SymContext C;
  const auto A = C.mkVar("a", 1), B = C.mkVar("b", 1), D = C.mkVar("d", 1);
  const auto Packed = C.mkOr({C.mkMul(C.mkZExt(A, 32), C.mkConst(32, 0x10)),
                              C.mkMul(C.mkZExt(B, 32), C.mkConst(32, 0x10)),
                              C.mkMul(C.mkZExt(D, 32), C.mkConst(32, 0x80))});
  for (unsigned Mask : {0x10u, 0x80u, 0x90u})
    for (unsigned Target : {0u, 0x10u, 0x80u, 0x90u, 0x100u}) {
      const auto Predicate = C.mkEq(
          C.mkAnd(C.mkNot(Packed), C.mkConst(32, Mask)), C.mkConst(32, Target));
      for (unsigned Bits = 0; Bits != 8; ++Bits) {
        const llvm::APInt Values[] = {llvm::APInt(1, Bits & 1),
                                      llvm::APInt(1, (Bits >> 1) & 1),
                                      llvm::APInt(1, Bits >> 2)};
        const auto Word = (Bits & 3 ? 0x10 : 0) | (Bits & 4 ? 0x80 : 0);
        EXPECT_EQ(C.eval(Predicate, Values),
                  llvm::APInt(1, ((~Word) & Mask) == Target));
      }
    }
}

TEST(SymExprCompare, LiteralPackedEqualityChecksForcedAndWideBits) {
  SymContext C;
  const auto A = C.mkVar("a", 1), B = C.mkVar("b", 1);
  const auto Low = llvm::APInt::getOneBitSet(128, 3);
  const auto High = llvm::APInt::getOneBitSet(128, 93);
  const llvm::APInt Forced(128, 2);
  const auto Packed =
      C.mkOr({C.mkConst(Forced), C.mkMul(C.mkZExt(A, 128), C.mkConst(Low)),
              C.mkMul(C.mkZExt(B, 128), C.mkConst(High))});
  EXPECT_EQ(C.mkEq(Packed, C.mkConst(Forced | High)), C.mkAnd(C.mkNot(A), B));
  EXPECT_EQ(C.mkNe(C.mkConst(Forced | Low), Packed),
            C.mkNot(C.mkAnd(A, C.mkNot(B))));
  EXPECT_EQ(C.mkEq(Packed, C.mkConst(High)), C.mkFalse());
  EXPECT_EQ(C.mkEq(Packed, C.mkConst(Forced | llvm::APInt(128, 1))),
            C.mkFalse());
  const auto Overlap = C.mkOr(Packed, C.mkConst(Low));
  for (unsigned Bits = 0; Bits != 4; ++Bits) {
    const llvm::APInt Values[] = {llvm::APInt(1, Bits & 1),
                                  llvm::APInt(1, Bits >> 1)};
    const auto Expected =
        Forced | Low | (Bits & 2 ? High : llvm::APInt(128, 0));
    EXPECT_EQ(C.eval(Overlap, Values), Expected);
    EXPECT_EQ(C.eval(C.mkEq(Overlap, C.mkConst(Forced | Low | High)), Values),
              llvm::APInt(1, (Bits & 2) != 0));
  }
}

} // namespace
