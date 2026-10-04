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
