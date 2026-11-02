//===- SymKnownBitsTests.cpp - Independent bit-fact checks ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymKnownBits.h"

#include <type_traits>

using namespace neverd::symbolic;

namespace {
static_assert(!std::is_copy_constructible_v<SymKnownBits>);
static_assert(!std::is_move_constructible_v<SymKnownBits>);

void expectConstant(SymKnownBits &Facts, SymRef R, uint64_t Value) {
  unsigned Work = 256;
  auto K = Facts.query(R, Work);
  ASSERT_TRUE(K);
  ASSERT_TRUE(K->isConstant());
  EXPECT_EQ(K->One.getZExtValue(), Value);
}

void exhaustiveBytePairs(SymContext &C, llvm::ArrayRef<SymRef> Roots) {
  SymKnownBits Facts(C);
  const auto Before = C.numNodes();
  for (auto R : Roots) {
    SCOPED_TRACE(C.toString(R));
    unsigned Work = 256;
    auto K = Facts.query(R, Work);
    ASSERT_TRUE(K);
    ASSERT_FALSE(K->hasConflict());
    ASSERT_LE(C.width(R), 64U);
    SymEvalPlan Plan(C, R);
    const uint64_t Zero = K->Zero.getZExtValue();
    const uint64_t One = K->One.getZExtValue();
    for (uint64_t X = 0; X < 256; ++X)
      for (uint64_t Y = 0; Y < 256; ++Y) {
        const auto V = Plan.evalU64({X, Y});
        if ((V & Zero) != 0 || (V & One) != One) {
          ADD_FAILURE() << "unsound bit facts at " << X << ", " << Y;
          return;
        }
      }
  }
  EXPECT_EQ(C.numNodes(), Before);
}

TEST(SymKnownBits, ArithmeticStructuralAndComparisonFactsAreSound) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto A = C.mkAnd(X, C.mkConst(8, 7));
  auto B = C.mkAnd(Y, C.mkConst(8, 31));
  auto Sum = C.mkAdd(A, B);
  auto Negative = C.mkOr(A, C.mkConst(8, 224));
  auto Choice = C.mkIte(C.mkEq(X, Y), C.mkZExt(A, 16), C.mkZExt(B, 16));
  auto SignedChoice =
      C.mkIte(C.mkEq(X, Y), C.mkSExt(Negative, 16), C.mkConst(16, 65520));
  auto Roundtrip = C.mkEq(Choice, C.mkZExt(C.mkExtract(Choice, 0, 8), 16));
  auto SignedRoundtrip =
      C.mkEq(SignedChoice, C.mkSExt(C.mkExtract(SignedChoice, 0, 8), 16));
  auto SameExtension = C.mkEq(C.mkZExt(Sum, 16), C.mkSExt(Sum, 16));
  auto DifferentExtension =
      C.mkEq(C.mkZExt(Negative, 16), C.mkSExt(Negative, 16));
  // The shift count is a proved expression, not a literal. Its full value
  // matters even though truncation to the value width would yield four.
  auto Count = C.mkIte(SameExtension, C.mkConst(16, 260), C.mkConst(16, 1));
  SymRef Roots[] = {A,
                    B,
                    C.mkNot(Negative),
                    C.mkAnd(A, B),
                    C.mkOr(A, B),
                    C.mkXor(A, B),
                    Sum,
                    C.mkMul(A, B),
                    C.mkExtract(Sum, 2, 4),
                    C.mkConcat(A, B),
                    C.mkZExt(Sum, 32),
                    C.mkSExt(Negative, 16),
                    Choice,
                    SignedChoice,
                    C.mkUlt(A, C.mkConst(8, 8)),
                    C.mkUle(Sum, C.mkConst(8, 38)),
                    C.mkSlt(Negative, A),
                    C.mkSle(A, Negative),
                    Roundtrip,
                    SignedRoundtrip,
                    SameExtension,
                    DifferentExtension,
                    C.mkShl(A, Count),
                    C.mkLShr(A, Count),
                    C.mkAShr(Negative, Count)};
  exhaustiveBytePairs(C, Roots);
  SymKnownBits Facts(C);
  expectConstant(Facts, Roundtrip, 1);
  expectConstant(Facts, SignedRoundtrip, 1);
  expectConstant(Facts, SameExtension, 1);
  expectConstant(Facts, DifferentExtension, 0);
  expectConstant(Facts, C.mkShl(A, Count), 0);
  expectConstant(Facts, C.mkAShr(Negative, Count), 255);
}

TEST(SymKnownBits, SumOrderingRequiresNoWrapAndTheExactOperandSubset) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto A = C.mkAnd(X, C.mkConst(8, 7));
  auto B = C.mkAnd(Y, C.mkConst(8, 31));
  auto D = C.mkLShr(X, C.mkConst(8, 5));
  auto Part = C.mkAdd(A, B), Whole = C.mkAdd({A, B, D});
  auto Contains = C.mkUle(Part, Whole), NotBelow = C.mkUlt(Whole, A);
  auto Wrap = C.mkUle(A, C.mkAdd(A, C.mkConst(8, 253)));
  auto WrongSubset = C.mkUle(C.mkAdd({A, B, D, D}), Whole);
  auto DifferentRoot = C.mkEq(C.mkZExt(A, 16), C.mkSExt(B, 16));
  SymRef Roots[] = {Contains, NotBelow, Wrap, WrongSubset, DifferentRoot};
  exhaustiveBytePairs(C, Roots);
  SymKnownBits Facts(C);
  expectConstant(Facts, Contains, 1);
  expectConstant(Facts, NotBelow, 0);
  for (auto R : {Wrap, WrongSubset, DifferentRoot}) {
    unsigned Work = 256;
    auto K = Facts.query(R, Work);
    ASSERT_TRUE(K);
    EXPECT_TRUE(K->isUnknown());
  }
}

TEST(SymKnownBits, DistributedLowArithmeticRequiresMatchingRootsAndHighBits) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto A = C.mkAnd(X, C.mkConst(8, 15));
  auto B = C.mkAnd(Y, C.mkConst(8, 7));
  auto WideProduct = C.mkMul(C.mkZExt(A, 16), C.mkZExt(B, 16));
  auto WideSum = C.mkAdd(C.mkZExt(A, 16), C.mkZExt(B, 16));
  auto NegativeA = C.mkOr(A, C.mkConst(8, 224));
  auto NegativeB = C.mkOr(B, C.mkConst(8, 240));
  SymRef Proven[] = {
      C.mkEq(WideProduct, C.mkZExt(C.mkMul(A, B), 16)),
      C.mkEq(WideSum, C.mkZExt(C.mkAdd(A, B), 16)),
      C.mkEq(C.mkMul(C.mkSExt(A, 16), C.mkSExt(B, 16)),
             C.mkSExt(C.mkMul(A, B), 16)),
      C.mkEq(C.mkAdd(C.mkSExt(NegativeA, 16), C.mkSExt(NegativeB, 16)),
             C.mkSExt(C.mkAdd(NegativeA, NegativeB), 16))};
  exhaustiveBytePairs(C, Proven);
  SymKnownBits Facts(C);
  for (auto R : Proven)
    expectConstant(Facts, R, 1);
  SymRef Unknown[] = {
      C.mkEq(WideProduct, C.mkZExt(C.mkMul(A, A), 16)),
      C.mkEq(C.mkMul(C.mkZExt(X, 16), C.mkZExt(Y, 16)),
             C.mkZExt(C.mkMul(X, Y), 16)),
      C.mkEq(
          C.mkLShr(C.mkMul(C.mkZExt(X, 16), C.mkZExt(Y, 16)), C.mkConst(16, 1)),
          C.mkZExt(C.mkLShr(C.mkMul(X, Y), C.mkConst(8, 1)), 16))};
  exhaustiveBytePairs(C, Unknown);
  for (auto R : Unknown) {
    unsigned Work = 256;
    auto K = Facts.query(R, Work);
    ASSERT_TRUE(K);
    EXPECT_TRUE(K->isUnknown());
  }
}

TEST(SymKnownBits, LosslessProjectionsRetainTheExactSourceAndDiscardedBits) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto Sum = C.mkAdd(C.mkAnd(X, C.mkConst(8, 7)), C.mkAnd(Y, C.mkConst(8, 7)));
  auto Negative = C.mkOr(Sum, C.mkConst(8, 240));
  auto Roundtrip = [&](SymRef Source, unsigned Left, unsigned Right,
                       bool Signed) {
    auto Product = C.mkShl(Source, C.mkConst(16, Left));
    auto Count = C.mkConst(16, Right);
    return C.mkEq(Source,
                  Signed ? C.mkAShr(Product, Count) : C.mkLShr(Product, Count));
  };
  llvm::SmallVector<SymRef, 16> Proven{
      C.mkEq(Sum, C.mkAnd(Sum, C.mkConst(8, 31))), Roundtrip(Sum, 4, 4, false)};
  for (unsigned Shift = 1; Shift <= 3; ++Shift) {
    Proven.push_back(Roundtrip(Sum, Shift, Shift, true));
    Proven.push_back(Roundtrip(Negative, Shift, Shift, true));
  }
  SymKnownBits Facts(C);
  for (auto R : Proven)
    expectConstant(Facts, R, 1);
  exhaustiveBytePairs(C, Proven);

  SymRef Unknown[] = {
      C.mkEq(Sum, C.mkAnd(Sum, C.mkConst(8, 247))),
      C.mkEq(Y, C.mkAnd(Sum, C.mkConst(8, 31))),
      Roundtrip(Sum, 4, 4, true),
      Roundtrip(Negative, 4, 4, true),
      Roundtrip(Sum, 2, 1, true),
      Roundtrip(Sum, 3, 259, true),
      C.mkEq(Sum, C.mkAShr(C.mkMul(Sum, C.mkConst(8, 6)), C.mkConst(8, 1)))};
  exhaustiveBytePairs(C, Unknown);
  for (auto R : Unknown) {
    unsigned Work = 256;
    auto K = Facts.query(R, Work);
    ASSERT_TRUE(K);
    EXPECT_TRUE(K->isUnknown());
  }

  // An independent integer oracle checks both signed and unsigned roundtrips
  // for every byte pair, rather than relying only on the symbolic evaluator.
  for (unsigned A = 0; A < 256; ++A)
    for (unsigned B = 0; B < 256; ++B) {
      const unsigned S = (A & 7) + (B & 7);
      EXPECT_EQ((S & 31), S);
      EXPECT_EQ(((S << 4) & 255) >> 4, S);
      for (unsigned K = 1; K <= 3; ++K)
        for (unsigned V : {S, S | 240}) {
          const unsigned Shifted = (V << K) & 255;
          const int Signed = Shifted < 128 ? int(Shifted) : int(Shifted) - 256;
          // Every shifted value is divisible by 2^K, so division has no
          // rounding ambiguity and avoids implementation-defined signed >>.
          EXPECT_EQ(unsigned(Signed / int(1U << K)) & 255, V);
        }
    }
}

TEST(SymKnownBits, ProjectionProofsKeepWidthWorkAndContextLimits) {
  for (unsigned Width : {8U, 32U, 64U, 128U}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Sum = C.mkAdd(C.mkAnd(X, C.mkConst(Width, 7)),
                       C.mkAnd(Y, C.mkConst(Width, 7)));
    auto Count = C.mkConst(256, Width - 5);
    auto Roundtrip = C.mkEq(Sum, C.mkAShr(C.mkShl(Sum, Count), Count));
    const auto Nodes = C.numNodes();
    unsigned Remaining = 256;
    SymKnownBits Baseline(C);
    auto Result = Baseline.query(Roundtrip, Remaining);
    ASSERT_TRUE(Result);
    ASSERT_TRUE(Result->isConstant());
    ASSERT_TRUE(Result->One.isOne());
    const unsigned Used = 256 - Remaining;
    ASSERT_GT(Used, 0U);
    for (unsigned Limit : {0U, Used - 1, Used}) {
      SymKnownBits Fresh(C);
      unsigned Budget = Limit;
      auto K = Fresh.query(Roundtrip, Budget);
      if (Limit == Used) {
        ASSERT_TRUE(K);
        EXPECT_TRUE(K->isConstant() && K->One.isOne());
      } else {
        EXPECT_FALSE(K);
      }
      EXPECT_EQ(Budget, 0U);
      EXPECT_EQ(C.numNodes(), Nodes);
    }
  }
}

TEST(SymKnownBits, ScaledOrderingRequiresOneBaseAndNoUnsignedWrap) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto Base =
      C.mkAdd(C.mkAnd(X, C.mkConst(8, 7)), C.mkAnd(Y, C.mkConst(8, 15)));
  auto Three = C.mkMul(Base, C.mkConst(8, 3));
  auto Five = C.mkMul(Base, C.mkConst(8, 5));
  SymRef Proven[] = {C.mkUle(Base, Five), C.mkUle(Three, Five),
                     C.mkUlt(Five, Three)};
  exhaustiveBytePairs(C, Proven);
  SymKnownBits Facts(C);
  expectConstant(Facts, Proven[0], 1);
  expectConstant(Facts, Proven[1], 1);
  expectConstant(Facts, Proven[2], 0);
  SymRef Unknown[] = {C.mkUle(Three, C.mkMul(Base, C.mkConst(8, 13))),
                      C.mkUle(Five, Three),
                      C.mkUle(Three, C.mkMul(Y, C.mkConst(8, 5)))};
  exhaustiveBytePairs(C, Unknown);
  for (auto R : Unknown) {
    unsigned Work = 256;
    auto K = Facts.query(R, Work);
    ASSERT_TRUE(K);
    EXPECT_TRUE(K->isUnknown());
  }
}

TEST(SymKnownBits, ModularProductsKeepCoefficientsMultiplicityAndWidth) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto A = C.mkAnd(X, C.mkConst(8, 3));
  auto B = C.mkAnd(Y, C.mkConst(8, 1));
  auto Inner = C.mkMul({C.mkConst(8, 3), A, B});
  auto Wide = C.mkMul(C.mkConst(16, 7), C.mkZExt(Inner, 16));
  auto Narrow = C.mkMul({C.mkConst(8, 21), A, B});
  SymRef Proven[] = {C.mkEq(Wide, C.mkZExt(Narrow, 16)),
                     C.mkEq(Wide, C.mkSExt(Narrow, 16))};
  exhaustiveBytePairs(C, Proven);
  SymKnownBits Facts(C);
  for (auto R : Proven)
    expectConstant(Facts, R, 1);
  auto Unbounded =
      C.mkMul(C.mkConst(16, 7), C.mkZExt(C.mkMul(C.mkConst(8, 3), X), 16));
  SymRef Unknown[] = {
      C.mkEq(Wide, C.mkZExt(C.mkMul({C.mkConst(8, 21), A, A}), 16)),
      C.mkEq(Wide, C.mkZExt(C.mkMul({C.mkConst(8, 20), A, B}), 16)),
      C.mkEq(Unbounded, C.mkZExt(C.mkMul(C.mkConst(8, 21), X), 16)),
      C.mkEq(Unbounded, C.mkMul(C.mkConst(16, 21), C.mkZExt(X, 16)))};
  exhaustiveBytePairs(C, Unknown);
  for (auto R : Unknown) {
    unsigned Work = 256;
    auto K = Facts.query(R, Work);
    ASSERT_TRUE(K);
    EXPECT_TRUE(K->isUnknown());
  }

  for (unsigned XValue = 0; XValue < 256; ++XValue)
    for (unsigned YValue = 0; YValue < 256; ++YValue) {
      const unsigned Product = (XValue & 3) * (YValue & 1);
      EXPECT_EQ(7 * ((3 * Product) & 255), (21 * Product) & 255);
      const unsigned Base = (XValue & 7) + (YValue & 15);
      EXPECT_LE(3 * Base, 5 * Base);
      EXPECT_LE(5 * Base, 255U);
    }
}

TEST(SymKnownBits, ModularProductQueriesChargeBothSidesAndStayReadOnly) {
  for (unsigned Width : {16U, 32U, 64U}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width);
    auto Bounded = C.mkAnd(X, C.mkConst(Width, 3));
    auto Inner = C.mkMul(C.mkConst(Width, 3), Bounded);
    auto Product = C.mkMul(C.mkConst(Width * 2, 7), C.mkZExt(Inner, Width * 2));
    auto Back = C.mkZExt(C.mkMul(C.mkConst(Width, 21), Bounded), Width * 2);
    auto Root = C.mkEq(Product, Back);
    const auto Nodes = C.numNodes();
    SymKnownBits Initial(C);
    unsigned Work = 256;
    auto K = Initial.query(Root, Work);
    ASSERT_TRUE(K);
    ASSERT_TRUE(K->isConstant() && K->One.isOne());
    const auto Used = 256 - Work;
    for (unsigned Budget : {Used - 1, Used}) {
      SymKnownBits Fresh(C);
      unsigned Remaining = Budget;
      const auto Result = Fresh.query(Root, Remaining);
      if (Budget == Used) {
        ASSERT_TRUE(Result);
        EXPECT_TRUE(Result->isConstant() && Result->One.isOne());
      } else {
        EXPECT_FALSE(Result);
      }
      EXPECT_EQ(Remaining, 0U);
      EXPECT_EQ(C.numNodes(), Nodes);
    }
  }
}

TEST(SymKnownBits, WideFactsAgreeWithArbitraryPrecisionEvaluation) {
  for (unsigned Width : {1U, 3U, 8U, 31U, 64U, 127U, 128U}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Mask = llvm::APInt::getLowBitsSet(Width, (Width + 1) / 2);
    auto A = C.mkAnd(X, C.mkConst(Mask));
    auto B = C.mkOr(Y, C.mkConst(~Mask));
    SymKnownBits Facts(C);
    for (auto R : {C.mkMul(A, A), C.mkAdd(A, A), C.mkNot(B),
                   C.mkIte(C.mkUlt(X, Y), A, C.mkAnd(B, C.mkConst(Mask)))}) {
      unsigned Work = 256;
      auto K = Facts.query(R, Work);
      ASSERT_TRUE(K);
      SymEvalPlan Plan(C, R);
      for (const auto &Left : {llvm::APInt(Width, 0), llvm::APInt(Width, 1),
                               Mask, ~Mask, llvm::APInt::getAllOnes(Width)})
        for (const auto &Right : {llvm::APInt(Width, 0), Mask, ~Mask}) {
          auto Value = Plan.eval({Left, Right});
          EXPECT_TRUE((Value & K->Zero).isZero());
          EXPECT_EQ(Value & K->One, K->One);
        }
    }
  }
}

TEST(SymKnownBits, WorkCacheAndContextBoundariesAreExplicit) {
  SymContext C;
  auto X = C.mkVar("x", 32);
  auto R = C.mkAnd(X, C.mkConst(32, 15));
  SymKnownBits Cold(C);
  unsigned Work = 256;
  auto Expected = Cold.query(R, Work);
  ASSERT_TRUE(Expected);
  const unsigned Used = 256 - Work;
  ASSERT_GT(Used, 2U);
  for (unsigned Limit : {0U, Used - 1, Used}) {
    SymKnownBits Fresh(C);
    Work = Limit;
    auto Result = Fresh.query(R, Work);
    EXPECT_EQ(Result.has_value(), Limit == Used);
    EXPECT_EQ(Work, 0U);
  }
  Work = 2;
  auto Cached = Cold.query(R, Work);
  ASSERT_TRUE(Cached);
  EXPECT_EQ(Cached->Zero, Expected->Zero);
  EXPECT_EQ(Work, 0U);
  Work = 1;
  EXPECT_FALSE(Cold.query(R, Work));
  EXPECT_EQ(Work, 0U);

  // Equal node indices in a second context carry unrelated facts.
  SymContext Other;
  auto Y = Other.mkVar("y", 32);
  auto S = Other.mkOr(Y, Other.mkConst(32, 240));
  ASSERT_EQ(R.index(), S.index());
  SymKnownBits Separate(Other);
  Work = 256;
  auto Distinct = Separate.query(S, Work);
  ASSERT_TRUE(Distinct);
  EXPECT_EQ(Distinct->One, llvm::APInt(32, 240));
  EXPECT_TRUE(Distinct->Zero.isZero());
  auto Later = C.mkOr(R, C.mkConst(32, 128));
  Work = 256;
  ASSERT_TRUE(Cold.query(Later, Work));
}

TEST(SymKnownBits, WidthDepthAndFanInRefuseWithoutInventingFacts) {
  SymContext C;
  SymKnownBits Facts(C);
  unsigned Work = 1000;
  EXPECT_FALSE(Facts.query({}, Work));
  EXPECT_FALSE(Facts.query(SymRef(123456), Work));
  auto Wide = C.mkVar("wide", 129);
  EXPECT_FALSE(Facts.query(Wide, Work));
  EXPECT_EQ(Work, 1000U);
  EXPECT_FALSE(Facts.query(C.mkExtract(Wide, 0, 8), Work));
  EXPECT_LT(Work, 1000U);

  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto Deep = X;
  for (unsigned I = 0; I < 2048; ++I)
    Deep = C.mkLShr(Deep, Y);
  Work = 1000;
  auto Depth = Facts.query(Deep, Work);
  ASSERT_TRUE(Depth);
  EXPECT_TRUE(Depth->isUnknown());
  EXPECT_GE(Work, 1000U - SymKnownBits::MaxQueryWork);
  // Unsupported division is an unknown value, not a guessed operation.
  Work = 256;
  auto Opaque = Facts.query(C.mkUDiv(X, Y), Work);
  ASSERT_TRUE(Opaque);
  EXPECT_TRUE(Opaque->isUnknown());
  llvm::SmallVector<SymRef, 160> Terms;
  for (unsigned I = 0; I < 160; ++I)
    Terms.push_back(C.mkVar("term" + std::to_string(I), 32));
  Work = 1000;
  EXPECT_FALSE(Facts.query(C.mkAdd(Terms), Work));
  EXPECT_EQ(Work, 1000U - SymKnownBits::MaxQueryWork);
}

TEST(SymKnownBits, AFullCacheKeepsEarlierFactsAndDoesNotGrow) {
  SymContext C;
  SymKnownBits Facts(C);
  for (unsigned I = 0; I < SymKnownBits::MaxCachedFacts; ++I) {
    unsigned Work = 2;
    ASSERT_TRUE(Facts.query(C.mkConst(32, I), Work));
    ASSERT_EQ(Work, 0U);
  }
  auto R = C.mkAnd(C.mkVar("x", 32), C.mkConst(32, 7));
  unsigned Work = 256;
  auto Result = Facts.query(R, Work);
  ASSERT_TRUE(Result);
  Work = 2;
  EXPECT_FALSE(Facts.query(R, Work));
  Work = 2;
  EXPECT_TRUE(Facts.query(C.mkConst(32, 123), Work));
}
} // namespace

TEST(SymKnownBits, FlattenedMasksKeepEveryFactorAndTheOriginalMask) {
  for (unsigned Width : {8U, 32U, 64U, 128U}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Top = C.mkConst(llvm::APInt::getSignMask(Width));
    auto Restricted = C.mkXor(Top, C.mkAnd(Y, Top));
    auto Source = C.mkAnd(X, Restricted);
    auto Mask = C.mkConst(llvm::APInt::getHighBitsSet(Width, Width / 2));
    auto Equal = C.mkEq(Source, C.mkAnd(Mask, Source));
    const auto Nodes = C.numNodes();
    unsigned Remaining = 256;
    SymKnownBits Facts(C);
    auto K = Facts.query(Equal, Remaining);
    ASSERT_TRUE(K);
    ASSERT_TRUE(K->isConstant() && K->One.isOne());
    const auto Used = 256 - Remaining;
    for (unsigned Limit : {0U, Used - 1, Used}) {
      SymKnownBits Fresh(C);
      unsigned Work = Limit;
      auto R = Fresh.query(Equal, Work);
      EXPECT_EQ(bool(R), Limit == Used);
      EXPECT_EQ(Work, 0U);
      EXPECT_EQ(C.numNodes(), Nodes);
    }
    auto SourceMask = C.mkConst(Width, 1), WiderMask = C.mkConst(Width, 3);
    SymRef Unknown[] = {
        C.mkEq(Source, C.mkAnd(Mask, C.mkAnd(Y, Restricted))),
        C.mkEq(Source, C.mkAnd(C.mkNot(Top), Source)),
        C.mkEq(C.mkAnd({SourceMask, X, Y}), C.mkAnd({WiderMask, X, Y}))};
    for (auto R : Unknown) {
      unsigned Work = 256;
      auto U = Facts.query(R, Work);
      ASSERT_TRUE(U);
      EXPECT_TRUE(U->isUnknown());
    }
    if (Width == 8) {
      exhaustiveBytePairs(C, {Equal});
      exhaustiveBytePairs(C, Unknown);
      for (unsigned A = 0; A < 256; ++A)
        for (unsigned B = 0; B < 256; ++B) {
          const auto S = A & (128U ^ (B & 128U));
          EXPECT_EQ(S & 240U, S);
        }
    }
  }
}

TEST(SymKnownBits, NonwrappingSumIntervalsRetainExactUnsignedEndpoints) {
  for (unsigned Width : {8U, 32U, 64U, 128U}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    llvm::SmallVector<SymRef, 8> Terms{C.mkConst(Width, 1)};
    for (unsigned Bit = 0; Bit < 4; ++Bit)
      Terms.push_back(C.mkZExt(C.mkExtract(Bit & 1 ? X : Y, Bit, 1), Width));
    auto Sum = C.mkAdd(Terms);
    auto Six = C.mkConst(Width, 6), Five = C.mkConst(Width, 5);
    auto EqualBound = C.mkUle(Sum, Five);
    SymKnownBits Facts(C);
    expectConstant(Facts, EqualBound, 1);
    expectConstant(Facts, C.mkUlt(Sum, Six), 1);
    expectConstant(Facts, C.mkUle(Six, Sum), 0);
    expectConstant(Facts, C.mkUlt(Five, Sum), 0);
    expectConstant(Facts, C.mkUle(C.mkConst(Width, 1), Sum), 1);
    SymRef Unknown[] = {
        C.mkUlt(Sum, Five), C.mkUle(Five, Sum),
        C.mkUlt(C.mkAdd(Sum, C.mkConst(llvm::APInt::getAllOnes(Width) - 2)),
                Five),
        C.mkUle(C.mkAdd(Sum, C.mkConst(llvm::APInt::getAllOnes(Width) - 2)),
                Five)};
    for (auto R : Unknown) {
      unsigned Work = 256;
      auto K = Facts.query(R, Work);
      ASSERT_TRUE(K);
      EXPECT_TRUE(K->isUnknown());
    }
    if (Width == 8) {
      exhaustiveBytePairs(C, {EqualBound});
      exhaustiveBytePairs(C, Unknown);
      for (unsigned A = 0; A < 256; ++A)
        for (unsigned B = 0; B < 256; ++B)
          EXPECT_LE(1 + (B & 1) + ((A >> 1) & 1) + ((B >> 2) & 1) +
                        ((A >> 3) & 1),
                    5U);
    }
    const auto Nodes = C.numNodes();
    unsigned Remaining = 256;
    SymKnownBits Baseline(C);
    ASSERT_TRUE(Baseline.query(EqualBound, Remaining));
    const auto Used = 256 - Remaining;
    for (unsigned Limit : {0U, Used - 1, Used}) {
      SymKnownBits Fresh(C);
      unsigned Work = Limit;
      auto K = Fresh.query(EqualBound, Work);
      EXPECT_EQ(bool(K), Limit == Used);
      EXPECT_EQ(Work, 0U);
      EXPECT_EQ(C.numNodes(), Nodes);
    }
  }
}

TEST(SymKnownBits, LiteralDivisionAndRemainderBoundTheirResults) {
  // SymContext division is total (x/0 is all ones, x%0 is x), so only a
  // nonzero literal divisor may use ordinary unsigned bounds.
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto Quotient = C.mkUDiv(X, C.mkConst(8, 251));
  auto Small = C.mkURem(C.mkAnd(X, C.mkConst(8, 96)), C.mkConst(8, 204));
  exhaustiveBytePairs(
      C, {Quotient, Small, C.mkURem(X, C.mkConst(8, 152)),
          C.mkUDiv(C.mkOr(X, C.mkConst(8, 0x80)), C.mkConst(8, 3)),
          C.mkUDiv(X, C.mkConst(8, 0)), C.mkURem(X, C.mkConst(8, 0)),
          C.mkUDiv(X, Y), C.mkURem(X, Y), C.mkURem(Y, C.mkOr(X, Y))});
  SymKnownBits Facts(C);
  unsigned Work = 256;
  auto K = Facts.query(Quotient, Work);
  ASSERT_TRUE(K);
  EXPECT_EQ(K->Zero.getZExtValue(), 0xFEU);
  // A dividend below the divisor is its own remainder.
  Work = 256;
  K = Facts.query(Small, Work);
  ASSERT_TRUE(K);
  EXPECT_EQ(K->Zero.getZExtValue(), 0x9FU);
  for (auto Dynamic : {C.mkUDiv(X, Y), C.mkURem(X, Y)}) {
    Work = 256;
    K = Facts.query(Dynamic, Work);
    ASSERT_TRUE(K);
    EXPECT_TRUE(K->isUnknown());
  }
}
