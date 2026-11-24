//===- SymExprTests.cpp - Interning and canonicalisation ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Covers the two properties the rest of the optimiser is allowed to assume:
/// that structurally equal expressions are the same \c SymRef, and that every
/// way of writing the same value reaches the same node without any rewrite
/// pass running.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"

#include <algorithm>
#include <array>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::symbolic;

namespace {

constexpr uint32_t W32 = 32;

TEST(SymExpr, InterningMakesStructuralEqualityAPointerCompare) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  EXPECT_EQ(X, Ctx.mkVar("x", W32));
  EXPECT_NE(X, Y);
  EXPECT_EQ(Ctx.mkXor(X, Y), Ctx.mkXor(X, Y));
  EXPECT_EQ(Ctx.mkConst(W32, 7), Ctx.mkConst(W32, 7));
  // Same value, different width, different node.
  EXPECT_NE(Ctx.mkConst(W32, 7), Ctx.mkConst(8, 7));
}

TEST(SymExpr, NativeLiteralOverloadsKeepEveryWidthAndWordBoundary) {
  SymContext Ctx;
  std::mt19937_64 Random(0x6e6174697665636f);
  for (unsigned Width = 1; Width <= 257; ++Width)
    for (unsigned I = 0; I < 12; ++I) {
      const auto Value = I == 0   ? uint64_t(0)
                         : I == 1 ? ~uint64_t(0)
                                  : Random();
      const llvm::APInt Expected(Width, Value, false, true);
      const auto Literal = Ctx.mkConst(Width, Value);
      EXPECT_EQ(Ctx.constValue(Literal), Expected);
      EXPECT_EQ(Literal, Ctx.mkConst(Expected));
      SymEvalPlan Plan(Ctx, Literal);
      EXPECT_EQ(Plan.numSteps(), 1u);
      EXPECT_EQ(Plan.eval({}), Expected);
      if (Width <= 64) {
        EXPECT_EQ(Plan.evalU64({}), Expected.getZExtValue());
        EXPECT_EQ(Plan.evalU64With([](uint32_t) {
          ADD_FAILURE() << "a constant plan must not request an input";
          return uint64_t(0);
        }),
                  Expected.getZExtValue());
      }
    }
}

TEST(SymExpr, InterningRetainsFullIdentityAcrossGrowthCopyAndMove) {
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 64);
  const auto WideValue = llvm::APInt::getOneBitSet(128, 100);
  const auto Wide = Ctx.mkConst(WideValue);
  // These literals exercise hashes zero, UINT64_MAX, UINT64_MAX-1 and
  // 0xfffffffefffffffe, plus exact collisions with the variable and the first
  // wide literal. A hash match must never substitute for full identity.
  const std::array<uint64_t, 6> Values{
      0x96c84cbe741a547eULL, 0xfb9ece775e7489a1ULL, 0xfb9ece775e7489a0ULL,
      0xfb9ece785e7489a0ULL, 0xffffffffffffcfd1ULL, 0xffffffffffffefb0ULL};
  std::vector<SymRef> Literals;
  for (auto Value : Values) {
    const auto R = Ctx.mkConst(64, Value);
    EXPECT_NE(R, X);
    EXPECT_NE(R, Wide);
    for (auto Previous : Literals)
      EXPECT_NE(R, Previous);
    Literals.push_back(R);
  }
  const auto Sum = Ctx.mkAdd(X, Literals.front());
  for (uint64_t I = 0; I < 8192; ++I)
    Ctx.mkAdd(X, Ctx.mkConst(64, I));

  auto Copy = Ctx;
  auto Moved = std::move(Copy);
  SymContext Assigned;
  Assigned = Ctx;
  for (auto *Context : {&Ctx, &Moved, &Assigned}) {
    const auto Before = Context->numNodes();
    EXPECT_EQ(Context->mkVar("x", 64), X);
    EXPECT_EQ(Context->mkConst(WideValue), Wide);
    for (size_t I = 0; I < Values.size(); ++I) {
      EXPECT_EQ(Context->mkConst(64, Values[I]), Literals[I]);
      EXPECT_EQ(Context->constValue(Literals[I]), llvm::APInt(64, Values[I]));
    }
    EXPECT_EQ(Context->mkAdd(X, Literals.front()), Sum);
    EXPECT_EQ(Context->numNodes(), Before);
  }
  Moved.mkVar("after-copy", 64);
  EXPECT_FALSE(Ctx.hasVarName("after-copy"));
  EXPECT_FALSE(Assigned.hasVarName("after-copy"));
}

TEST(SymExpr, WideLiteralsKeepEveryBitAndWidthAcrossPoolGrowthAndCopies) {
  SymContext Original;
  std::vector<std::pair<llvm::APInt, SymRef>> Values;
  auto Insert = [&](llvm::APInt Value) {
    const auto Ref = Original.mkConst(Value);
    Values.emplace_back(Value, Ref);
    // Interning owns its value, even when the caller reuses its input buffer.
    Value.flipAllBits();
    EXPECT_EQ(Original.constValue(Ref), Values.back().first);
  };
  for (unsigned Width :
       {65u, 66u, 127u, 128u, 129u, 255u, 256u, 257u, 513u, 1025u}) {
    Insert(llvm::APInt::getZero(Width));
    Insert(llvm::APInt::getAllOnes(Width));
    for (unsigned Bit = 0; Bit < Width; ++Bit)
      Insert(llvm::APInt::getOneBitSet(Width, Bit));
  }
  // Every entry has a different width or value, including words whose only
  // difference is the last bit of a partial high limb.
  ASSERT_EQ(Original.numNodes(), Values.size());
  auto Copy = Original;
  SymContext Assigned;
  Assigned = Original;
  Original = SymContext();
  auto Moved = std::move(Copy);
  for (auto *Ctx : {&Moved, &Assigned}) {
    const auto Before = Ctx->numNodes();
    for (auto It = Values.rbegin(); It != Values.rend(); ++It) {
      EXPECT_EQ(Ctx->mkConst(It->first), It->second);
      EXPECT_EQ(Ctx->constValue(It->second), It->first);
    }
    EXPECT_EQ(Ctx->numNodes(), Before);
    const auto NewValue = llvm::APInt::getOneBitSet(2049, 2048);
    EXPECT_EQ(Ctx->constValue(Ctx->mkConst(NewValue)), NewValue);
  }
}

TEST(SymExpr, CollisionChainsSurviveIndependentCopiesAndOwnerDestruction) {
  // These different literal identities all have hash 0xfb58cd469c6a459e.
  constexpr std::array<std::pair<unsigned, uint64_t>, 5> Values{
      {{16, 0x3c5c}, {24, 0x3e66}, {32, 0x17e8}, {48, 0x13d4}, {64, 0x1000}}};
  SymContext Original;
  std::array<SymRef, Values.size()> Refs;
  for (size_t I = 0; I + 1 < Values.size(); ++I)
    Refs[I] = Original.mkConst(Values[I].first, Values[I].second);
  auto Copy = Original;
  Refs.back() = Copy.mkConst(Values.back().first, Values.back().second);
  EXPECT_EQ(Original.numNodes(), Values.size() - 1);
  EXPECT_EQ(Copy.numNodes(), Values.size());
  Original = SymContext();

  auto Moved = std::move(Copy);
  const auto X = Moved.mkVar("x", 64);
  for (uint64_t I = 0; I < 8192; ++I)
    Moved.mkAdd(X, Moved.mkConst(64, I));
  SymContext Assigned;
  Assigned = Moved;
  Moved = SymContext();
  const auto Before = Assigned.numNodes();
  for (size_t I = 0; I < Values.size(); ++I) {
    const auto [Width, Value] = Values[I];
    EXPECT_EQ(Assigned.mkConst(Width, Value), Refs[I]);
    EXPECT_EQ(Assigned.constValue(Refs[I]), llvm::APInt(Width, Value));
    for (size_t J = 0; J < I; ++J)
      EXPECT_NE(Refs[I], Refs[J]);
  }
  EXPECT_EQ(Assigned.numNodes(), Before);
}

TEST(SymExpr, LongOperandIdentityPreservesOrderAcrossGrowthAndCopies) {
  for (unsigned Count : {7u, 8u, 9u, 16u, 65u}) {
    SCOPED_TRACE(Count);
    SymContext Ctx;
    llvm::SmallVector<SymRef, 16> Inputs;
    for (unsigned I = 0; I < Count; ++I)
      Inputs.push_back(Ctx.mkVar("byte" + std::to_string(I), 8));
    const auto Forward = Ctx.mkConcat(Inputs);
    const auto Sum = Ctx.mkAdd(Inputs);
    llvm::SmallVector<SymRef, 16> Reversed(Inputs.rbegin(), Inputs.rend());
    const auto Backward = Ctx.mkConcat(Reversed);
    EXPECT_NE(Forward, Backward);
    EXPECT_EQ(Ctx.mkAdd(Reversed), Sum);
    for (unsigned I = 0; I < 2048; ++I)
      Ctx.mkVar("growth" + std::to_string(I), 32);
    auto Copy = Ctx;
    auto Moved = std::move(Copy);
    for (auto *Context : {&Ctx, &Moved}) {
      const auto Before = Context->numNodes();
      EXPECT_EQ(Context->mkConcat(Inputs), Forward);
      EXPECT_EQ(Context->mkConcat(Reversed), Backward);
      EXPECT_EQ(Context->mkAdd(Reversed), Sum);
      EXPECT_EQ(Context->numNodes(), Before);
      llvm::APInt Expected(8 * Count, 0);
      llvm::SmallVector<llvm::APInt, 16> Values;
      for (unsigned I = 0; I < Count; ++I) {
        Expected = Expected.shl(8) | (I + 1);
        Values.emplace_back(8, I + 1);
      }
      SymEvalPlan Plan(*Context, Forward);
      EXPECT_EQ(Plan.eval(Values), Expected);
    }
  }
}

TEST(SymExpr, LiteralOffsetsKeepCanonicalTermsAndWidths) {
  std::mt19937_64 Random(0x6f666673657473);
  for (unsigned Width : {1u, 8u, 16u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const llvm::APInt Three(Width, 3, false, true);
    const auto Mask = llvm::APInt::getLowBitsSet(Width, (Width + 1) / 2);
    const std::array Terms{X,
                           C.mkNot(X),
                           C.mkXor(X, Y),
                           C.mkAnd(X, Y),
                           C.mkAnd(C.mkConst(Mask), X),
                           C.mkAdd(X, Y),
                           C.mkMul(C.mkConst(Three), X),
                           C.mkMul({C.mkConst(Three), X, Y})};
    for (const auto &Offset : {llvm::APInt(Width, 0), llvm::APInt(Width, 1),
                               llvm::APInt::getAllOnes(Width),
                               llvm::APInt::getOneBitSet(Width, Width - 1)}) {
      const auto K = C.mkConst(Offset);
      for (size_t I = 0; I < Terms.size(); ++I) {
        const auto Root = C.mkAdd(K, Terms[I]);
        EXPECT_EQ(Root, C.mkAdd(Terms[I], K));
        EXPECT_EQ(Root, C.mkAdd({K, Terms[I], C.mkZero(Width)}));
        SymEvalPlan Plan(C, Root);
        for (unsigned Sample = 0; Sample < 32; ++Sample) {
          llvm::APInt A(Width, Random(), false, true);
          llvm::APInt B(Width, Random(), false, true);
          if (Sample & 1)
            A.setBit(Width - 1);
          if (Sample & 2)
            B.setBit(Width - 1);
          const std::array Values{A, B};
          const std::array Expected{A,        ~A,    A ^ B,     A & B,
                                    A & Mask, A + B, Three * A, Three * A * B};
          EXPECT_EQ(Plan.eval(Values), Offset + Expected[I]);
          if (Width <= 64) {
            const std::array Native{A.getZExtValue(), B.getZExtValue()};
            EXPECT_EQ(Plan.evalU64(Native),
                      (Offset + Expected[I]).getZExtValue());
          }
        }
      }
    }
  }
}

TEST(SymExpr, DistinctAddendBasesKeepCoefficientsAndCanonicalOrder) {
  std::mt19937_64 Random(0x64697374696e6374);
  for (unsigned Width : {1u, 8u, 16u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const llvm::APInt Three(Width, 3, false, true);
    const auto Mask = llvm::APInt::getLowBitsSet(Width, (Width + 1) / 2);
    const std::array Terms{X,
                           Y,
                           C.mkNot(X),
                           C.mkXor(X, Y),
                           C.mkAnd(X, Y),
                           C.mkAnd(C.mkConst(Mask), X),
                           C.mkAdd(X, Y),
                           C.mkMul(C.mkConst(Three), X),
                           C.mkNeg(X),
                           C.mkMul({C.mkConst(Three), X, Y}),
                           C.mkMul(X, Y)};
    for (size_t I = 0; I < Terms.size(); ++I)
      for (size_t J = 0; J < Terms.size(); ++J) {
        const auto Root = C.mkAdd(Terms[I], Terms[J]);
        EXPECT_EQ(Root, C.mkAdd(Terms[J], Terms[I]));
        EXPECT_EQ(Root, C.mkAdd({Terms[I], Terms[J], C.mkZero(Width)}));
        SymEvalPlan Plan(C, Root);
        for (unsigned Sample = 0; Sample < 16; ++Sample) {
          llvm::APInt A(Width, 0), B(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64) {
            A |= llvm::APInt(Width, Random(), false, true).shl(Bit);
            B |= llvm::APInt(Width, Random(), false, true).shl(Bit);
          }
          const std::array Values{A, B};
          const std::array Expected{
              A,     B,         ~A, A ^ B,         A & B, A & Mask,
              A + B, Three * A, -A, Three * A * B, A * B};
          EXPECT_EQ(Plan.eval(Values), Expected[I] + Expected[J]);
          if (Width <= 64) {
            const std::array Native{A.getZExtValue(), B.getZExtValue()};
            EXPECT_EQ(Plan.evalU64(Native),
                      (Expected[I] + Expected[J]).getZExtValue());
          }
        }
      }
  }
}

TEST(SymExpr, CommutativeOperandsAreSorted) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef Z = Ctx.mkVar("z", W32);

  EXPECT_EQ(Ctx.mkAdd(X, Y), Ctx.mkAdd(Y, X));
  EXPECT_EQ(Ctx.mkAnd(X, Y), Ctx.mkAnd(Y, X));
  EXPECT_EQ(Ctx.mkOr(X, Y), Ctx.mkOr(Y, X));
  EXPECT_EQ(Ctx.mkXor(X, Y), Ctx.mkXor(Y, X));
  EXPECT_EQ(Ctx.mkMul(X, Y), Ctx.mkMul(Y, X));

  // Associativity is flattened, so the grouping an obfuscator chose is gone.
  EXPECT_EQ(Ctx.mkAdd(Ctx.mkAdd(X, Y), Z), Ctx.mkAdd(X, Ctx.mkAdd(Y, Z)));
  EXPECT_EQ(Ctx.mkXor(Ctx.mkXor(Z, X), Y), Ctx.mkXor(X, Ctx.mkXor(Y, Z)));
}

TEST(SymExpr, ConstantsFoldOnConstruction) {
  SymContext Ctx;
  EXPECT_EQ(Ctx.mkAdd(Ctx.mkConst(W32, 3), Ctx.mkConst(W32, 4)),
            Ctx.mkConst(W32, 7));
  EXPECT_EQ(Ctx.mkMul(Ctx.mkConst(W32, 3), Ctx.mkConst(W32, 4)),
            Ctx.mkConst(W32, 12));
  EXPECT_EQ(Ctx.mkNot(Ctx.mkConst(W32, 0)), Ctx.mkOnes(W32));
  // Wrapping is modulo the width, not an overflow.
  EXPECT_EQ(Ctx.mkAdd(Ctx.mkOnes(W32), Ctx.mkConst(W32, 1)), Ctx.mkZero(W32));
}

TEST(SymExpr, SumsCollectLikeTerms) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  EXPECT_EQ(Ctx.mkAdd(X, X), Ctx.mkMul(Ctx.mkConst(W32, 2), X));
  EXPECT_EQ(Ctx.mkAdd(X, Ctx.mkMul(Ctx.mkConst(W32, 2), X)),
            Ctx.mkMul(Ctx.mkConst(W32, 3), X));
  EXPECT_EQ(Ctx.mkSub(X, X), Ctx.mkZero(W32));
  EXPECT_EQ(Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(W32, 3), X),
                      Ctx.mkMul(Ctx.mkOnes(W32), X)),
            Ctx.mkMul(Ctx.mkConst(W32, 2), X));
  // A shared base with a compound body still collects.
  SymRef XY = Ctx.mkXor(X, Y);
  EXPECT_EQ(Ctx.mkAdd(XY, XY), Ctx.mkMul(Ctx.mkConst(W32, 2), XY));
}

TEST(SymExpr, SumCollectionKeepsCanonicalOrderAcrossTableGrowth) {
  std::mt19937_64 Random(0x73756d73);
  for (unsigned Width : {1u, 7u, 8u, 16u, 32u, 64u, 65u, 257u}) {
    for (unsigned Count : {1u, 2u, 7u, 8u, 9u, 17u, 65u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Count);
      SymContext Ctx;
      llvm::SmallVector<SymRef, 16> Terms{Ctx.mkConst(Width, 7)};
      llvm::SmallVector<SymRef, 16> Cancellations;
      std::vector<llvm::APInt> Coefficients;
      for (unsigned I = 0; I < Count; ++I) {
        auto V = Ctx.mkVar("v" + std::to_string(I), Width);
        llvm::APInt Coefficient(Width, 2 + I % 7, false, true);
        if (Width > 64)
          Coefficient.setBit(Width - 1);
        Terms.push_back(Ctx.mkMul(Ctx.mkConst(Coefficient), V));
        Terms.push_back(Ctx.mkMul(Ctx.mkConst(Width, 3), Ctx.mkNeg(V)));
        Coefficients.push_back(Coefficient - 3);
        auto Shifted = Ctx.mkLShr(V, Ctx.mkConst(32, 1));
        auto Slice = Ctx.mkAnd(V, Ctx.mkConst(~llvm::APInt(Width, 1)));
        Cancellations.append({Shifted, Shifted, Ctx.mkNeg(Slice)});
      }
      auto Root = Ctx.mkAdd(Terms);
      for (unsigned Permutation = 0; Permutation < 8; ++Permutation) {
        std::shuffle(Terms.begin(), Terms.end(), Random);
        EXPECT_EQ(Ctx.mkAdd(Terms), Root);
      }
      // Combining the shift coefficients exposes masks, which must be
      // collected again against the existing negative masks after growth.
      EXPECT_EQ(Ctx.mkAdd(Cancellations), Ctx.mkZero(Width));
      SymEvalPlan Plan(Ctx, Root);
      for (unsigned Sample = 0; Sample < 32; ++Sample) {
        std::vector<llvm::APInt> Values;
        llvm::APInt Expected(Width, 7, false, true);
        for (unsigned I = 0; I < Count; ++I) {
          Values.emplace_back(Width, Random(), false, true);
          if (Width > 64 && (I + Sample) % 2)
            Values.back().setBit(Width - 1);
          Expected += Coefficients[I] * Values.back();
        }
        EXPECT_EQ(Plan.eval(Values), Expected);
      }
    }
  }
}

TEST(SymExpr, ShiftsByConstantsBecomeProductsSoSumsCanCollectThem) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);

  EXPECT_EQ(Ctx.mkShl(X, Ctx.mkConst(W32, 1)),
            Ctx.mkMul(Ctx.mkConst(W32, 2), X));
  // This is the point of the normalisation: `x + (x << 1)` is `3*x`.
  EXPECT_EQ(Ctx.mkAdd(X, Ctx.mkShl(X, Ctx.mkConst(W32, 1))),
            Ctx.mkMul(Ctx.mkConst(W32, 3), X));
  // A shift past the width is zero, matching QF_BV rather than any one CPU.
  EXPECT_EQ(Ctx.mkShl(X, Ctx.mkConst(W32, 32)), Ctx.mkZero(W32));
  EXPECT_EQ(Ctx.mkLShr(X, Ctx.mkConst(W32, 99)), Ctx.mkZero(W32));
}

TEST(SymExpr, WideLogicalShiftsRetainSingleAndCombinedScales) {
  constexpr unsigned W = 4097, Shift = 4079;
  SymContext C;
  const auto X = C.mkVar("x", W), Amount = C.mkConst(W, Shift);
  const auto Right = C.mkLShr(X, Amount);
  const auto Scale = llvm::APInt::getOneBitSet(W, Shift);
  const auto Mask = C.mkConst(~llvm::APInt::getLowBitsSet(W, Shift));
  const auto Half = C.mkConst(Scale.lshr(1)), Two = C.mkConst(W, 2);
  EXPECT_EQ(C.mkMul(C.mkConst(Scale), Right), C.mkAnd(X, Mask));
  EXPECT_EQ(C.mkMul({Right, Half, Two}), C.mkAnd(X, Mask));
  const auto K = C.mkConst(Scale + 3);
  const auto Other = C.mkMul(K, Right);
  EXPECT_EQ(Other, C.mkMul(Right, K));
  EXPECT_EQ(C.op(Other), SymOp::Mul);
  EXPECT_EQ(C.operand(Other, 0), K);
  EXPECT_EQ(C.operand(Other, 1), Right);
  EXPECT_EQ(C.mkMul({C.mkZero(W), Right, Two}), C.mkZero(W));
  EXPECT_EQ(C.mkMul(C.mkConst(W, 1), Right), Right);
}

TEST(SymExpr, RightShiftDiscardsOnlyNeutralBitwiseWindows) {
  for (unsigned Width : {8U, 16U, 32U, 64U, 128U}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const unsigned Half = Width / 2;
    auto X = C.mkVar("x", Half), Y = C.mkVar("y", Half);
    auto High = C.mkShl(C.mkZExt(X, Width), C.mkConst(Width, Half));
    auto Low = C.mkZExt(Y, Width);
    auto Count = C.mkConst(128, Half);
    for (auto Packed : {C.mkOr(High, Low), C.mkXor(High, Low)})
      EXPECT_EQ(C.mkExtract(C.mkLShr(Packed, Count), 0, Half), X);
    auto Neutral =
        C.mkOr(Low, C.mkConst(llvm::APInt::getHighBitsSet(Width, Half)));
    EXPECT_EQ(C.mkExtract(C.mkLShr(C.mkAnd(High, Neutral), Count), 0, Half), X);
    // Several discarded terms may jointly cover more than one machine word.
    auto Narrow = C.mkVar("narrow", 1);
    auto Whole = C.mkVar("whole", Width);
    EXPECT_EQ(
        C.mkLShr(C.mkOr(Whole, C.mkZExt(Narrow, Width)), C.mkConst(Width, 1)),
        C.mkLShr(Whole, C.mkConst(Width, 1)));
  }
}

TEST(SymExpr, MaskedFieldCarriesUseTheLastEqualMaskBitBelowTheField) {
  SymContext C;
  constexpr unsigned W = 193, Start = 127, Size = 66;
  const auto X = C.mkVar("x", W), Y = C.mkVar("y", W);
  auto Live = llvm::APInt::getBitsSet(W, Start, Start + Size);
  Live.setBit(3);
  auto Other = llvm::APInt::getBitsSet(W, 1, Start);
  Other.clearBit(70);
  auto L = C.mkAnd(X, C.mkConst(Live));
  const auto R = C.mkAnd(Y, C.mkConst(Other));
  const auto Want = C.mkExtract(X, Start, Size);
  for (auto Joined : {C.mkAdd(L, R), C.mkOr(R, L), C.mkXor(L, R)}) {
    EXPECT_EQ(C.mkExtract(Joined, Start, Size), Want);
    const auto Rotated = C.mkRor(Joined, C.mkConst(W, 89));
    EXPECT_EQ(C.mkExtract(Rotated, Start - 89, Size), Want);
  }
  // A generate above the kill at bit 70 propagates into the field.
  Live.setBit(71);
  const auto Sum = C.mkAdd(C.mkAnd(X, C.mkConst(Live)), R);
  const auto Carry = C.mkExtract(Sum, Start, Size);
  EXPECT_NE(Carry, Want);
  const std::array<llvm::APInt, 2> Ones{llvm::APInt::getAllOnes(W),
                                        llvm::APInt::getAllOnes(W)};
  EXPECT_EQ(C.eval(Carry, Ones), (Live + Other).extractBits(Size, Start));
  EXPECT_NE(C.eval(Carry, Ones), Live.extractBits(Size, Start));
}

TEST(SymExpr, WidenedRotationsProjectInteriorAndWrappingFields) {
  SymContext C;
  const auto X = C.mkVar("x", 8);
  const auto L = C.mkShl(X, C.mkConst(8, 3));
  const auto R = C.mkLShr(X, C.mkConst(8, 5));
  for (auto Join : {C.mkOr(L, R), C.mkXor(L, R), C.mkAdd(L, R)})
    for (bool Signed : {false, true}) {
      const auto Wide = Signed ? C.mkSExt(Join, 16) : C.mkZExt(Join, 16);
      EXPECT_EQ(C.mkExtract(Wide, 4, 3), C.mkExtract(X, 1, 3));
      const auto Wrap = C.mkExtract(Wide, 1, 5);
      const auto CrossExtension = C.mkExtract(Wide, 6, 5);
      for (uint64_t I = 0; I < 256; ++I) {
        const std::array<uint64_t, 1> Args{I};
        const unsigned Word = ((I << 3) | (I >> 5)) & 255;
        const unsigned Extended = Signed && (Word & 128) ? Word | 0xff00 : Word;
        EXPECT_EQ(C.evalU64(Wrap, Args), (Word >> 1) & 31);
        EXPECT_EQ(C.evalU64(CrossExtension, Args), (Extended >> 6) & 31);
      }
    }
}

TEST(SymExpr, MaskedFieldSlicesMatchAllFourBitMasksAndInputs) {
  SymContext C;
  const auto X = C.mkVar("x", 4), Y = C.mkVar("y", 4);
  for (unsigned A = 0; A < 16; ++A)
    for (unsigned B = 0; B < 16; ++B) {
      const auto L = C.mkAnd(X, C.mkConst(4, A));
      const auto R = C.mkAnd(Y, C.mkConst(4, B));
      for (unsigned At = 0; At < 4; ++At)
        for (unsigned N = 1; N <= 4 - At; ++N) {
          SymEvalPlan Sum(C, C.mkExtract(C.mkAdd(L, R), At, N));
          SymEvalPlan Union(C, C.mkExtract(C.mkOr(L, R), At, N));
          SymEvalPlan Diff(C, C.mkExtract(C.mkXor(L, R), At, N));
          const auto Mask = (1u << N) - 1;
          for (uint64_t I = 0; I < 16; ++I)
            for (uint64_t J = 0; J < 16; ++J) {
              const std::array<uint64_t, 2> Args{I, J};
              const auto U = I & A, V = J & B;
              ASSERT_EQ(Sum.evalU64(Args), ((U + V) >> At) & Mask);
              ASSERT_EQ(Union.evalU64(Args), ((U | V) >> At) & Mask);
              ASSERT_EQ(Diff.evalU64(Args), ((U ^ V) >> At) & Mask);
            }
        }
    }
}

TEST(SymExpr, RightShiftWindowFoldsMatchIndependentByteArithmetic) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  for (unsigned Shift = 1; Shift < 8; ++Shift) {
    const unsigned Mask = (1U << Shift) - 1;
    auto Low = C.mkAnd(Y, C.mkConst(8, Mask));
    auto HighOnes = C.mkOr(Y, C.mkConst(8, 255U ^ Mask));
    auto Count = C.mkConst(8, Shift);
    const SymRef Results[] = {C.mkLShr(C.mkOr(X, Low), Count),
                              C.mkLShr(C.mkXor(X, Low), Count),
                              C.mkLShr(C.mkAnd(X, HighOnes), Count)};
    for (auto R : Results)
      EXPECT_EQ(R, C.mkLShr(X, Count));
    // Addition can carry out of the discarded bits; a partly retained OR
    // operand, a sign extension and a variable shift cannot be dropped.
    const SymRef Unchanged[] = {
        C.mkLShr(C.mkAdd(X, Low), Count),
        C.mkLShr(C.mkOr(X, C.mkAnd(Y, C.mkConst(8, Mask | (1U << Shift)))),
                 Count),
        C.mkLShr(C.mkOr(X, C.mkSExt(C.mkExtract(Y, 0, 4), 8)), Count),
        C.mkLShr(C.mkOr(X, Low), Y)};
    for (unsigned A = 0; A < 256; ++A)
      for (unsigned B = 0; B < 256; ++B) {
        uint64_t Values[] = {A, B};
        for (auto R : Results)
          ASSERT_EQ(C.evalU64(R, Values), A >> Shift);
        const unsigned SignedNibble = (B & 8) ? (B & 15) | 240 : B & 15;
        const unsigned Expected[] = {
            ((A + (B & Mask)) & 255) >> Shift,
            (A | (B & (Mask | (1U << Shift)))) >> Shift,
            (A | SignedNibble) >> Shift, B >= 8 ? 0 : (A | (B & Mask)) >> B};
        for (unsigned I = 0; I < std::size(Unchanged); ++I)
          ASSERT_EQ(C.evalU64(Unchanged[I], Values), Expected[I]);
      }
  }
}

TEST(SymExpr, RightShiftWindowDiscoveryIsBoundedAndKeepsFullCounts) {
  SymContext C;
  auto X = C.mkVar("x", 64);
  llvm::SmallVector<SymRef, 10> Terms{X};
  for (unsigned I = 0; I < 8; ++I)
    Terms.push_back(C.mkZExt(C.mkFreshVar(8), 64));
  auto Many = C.mkOr(Terms);
  auto Count = C.mkConst(64, 32);
  auto Result = C.mkLShr(Many, Count);
  EXPECT_EQ(C.op(Result), SymOp::LShr);
  EXPECT_EQ(C.operand(Result, 0), Many);
  Terms.pop_back();
  EXPECT_EQ(C.mkLShr(C.mkOr(Terms), Count), C.mkLShr(X, Count));
  auto Wide = C.mkOr(C.mkVar("wide", 256), C.mkZExt(C.mkVar("low", 8), 256));
  EXPECT_EQ(C.operand(C.mkLShr(Wide, C.mkConst(256, 128)), 0), Wide);
  EXPECT_EQ(C.mkLShr(Many, C.mkConst(llvm::APInt::getOneBitSet(128, 80))),
            C.mkZero(64));
  EXPECT_EQ(C.mkLShr(Many, C.mkZero(128)), Many);
}

TEST(SymExpr, XorClearsBitsForcedByTheSameOrMask) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    SymRef Y = Ctx.mkVar("y", Width);
    llvm::APInt Mask(Width, 0);
    for (unsigned I = 0; I < Width; I += 2)
      Mask.setBit(I);
    SymRef M = Ctx.mkConst(Mask);
    SymRef Keep = Ctx.mkConst(~Mask);
    EXPECT_EQ(Ctx.mkXor(Ctx.mkOr(X, M), M), Ctx.mkAnd(X, Keep));
    EXPECT_EQ(Ctx.mkXor(M, Ctx.mkOr({M, X, Y})),
              Ctx.mkAnd(Ctx.mkOr(X, Y), Keep));
    EXPECT_EQ(Ctx.mkXor(Ctx.mkOr(X, Ctx.mkZero(Width)), Ctx.mkZero(Width)), X);
    EXPECT_EQ(Ctx.mkXor(Ctx.mkOr(X, Ctx.mkOnes(Width)), Ctx.mkOnes(Width)),
              Ctx.mkZero(Width));
    llvm::APInt Values[] = {llvm::APInt::getAllOnes(Width), Mask};
    SymRef DifferentMask = Ctx.mkXor(Ctx.mkOr(X, M), Keep);
    EXPECT_EQ(Ctx.eval(DifferentMask, Values), (Values[0] | Mask) ^ ~Mask);
    EXPECT_NE(Ctx.eval(DifferentMask, Values), Values[0] & ~Mask);
  }
}

TEST(SymExpr, ForcedOrBitClearingMatchesSmallWordArithmetic) {
  for (unsigned Width : {1u, 2u, 3u, 4u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    unsigned Limit = 1u << Width;
    for (unsigned Mask = 0; Mask < Limit; ++Mask) {
      SymRef M = Ctx.mkConst(Width, Mask);
      SymRef Result = Ctx.mkXor(Ctx.mkOr(X, M), M);
      for (unsigned Value = 0; Value < Limit; ++Value) {
        uint64_t Values[] = {Value};
        EXPECT_EQ(Ctx.evalU64(Result, Values), (Value | Mask) ^ Mask);
      }
    }
  }
}

TEST(SymExpr, RotatesAcceptIndependentlySizedAmounts) {
  struct Case {
    uint32_t Width;
    llvm::APInt Amount;
    uint64_t Left;
    uint64_t Right;
  };
  const Case Cases[] = {
      {3, llvm::APInt(1, 1), 2, 4},
      {3, llvm::APInt(2, 3), 1, 1},
      {3, llvm::APInt(8, 4), 2, 4},
      {3, llvm::APInt::getOneBitSet(128, 64), 2, 4},
      {3, llvm::APInt::getOneBitSet(128, 127), 4, 2},
      {3, llvm::APInt::getAllOnes(128), 1, 1},
      {1, llvm::APInt::getAllOnes(128), 1, 1},
  };
  for (const Case &T : Cases) {
    SCOPED_TRACE(T.Width);
    SCOPED_TRACE(T.Amount.getBitWidth());
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", T.Width);
    SymRef Y = Ctx.mkVar("amount", T.Amount.getBitWidth());
    SymRef One = Ctx.mkConst(T.Width, 1);
    SymRef Amount = Ctx.mkConst(T.Amount);
    EXPECT_EQ(Ctx.mkRol(One, Amount), Ctx.mkConst(T.Width, T.Left));
    EXPECT_EQ(Ctx.mkRor(One, Amount), Ctx.mkConst(T.Width, T.Right));

    llvm::APInt Values[] = {llvm::APInt(T.Width, 1), T.Amount};
    EXPECT_EQ(Ctx.eval(Ctx.mkRol(X, Y), Values), llvm::APInt(T.Width, T.Left));
    EXPECT_EQ(Ctx.eval(Ctx.mkRor(X, Y), Values), llvm::APInt(T.Width, T.Right));
    if (T.Amount.getBitWidth() <= 64) {
      uint64_t ValuesU64[] = {1, T.Amount.getZExtValue()};
      EXPECT_EQ(Ctx.evalU64(Ctx.mkRol(X, Y), ValuesU64), T.Left);
      EXPECT_EQ(Ctx.evalU64(Ctx.mkRor(X, Y), ValuesU64), T.Right);
    }
  }
}

TEST(SymExpr, ShiftsKeepHighBitsOfIndependentlySizedAmounts) {
  const llvm::APInt Amounts[] = {llvm::APInt(16, 256),
                                 llvm::APInt::getOneBitSet(128, 64)};
  for (const llvm::APInt &Amount : Amounts) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", 8);
    SymRef Y = Ctx.mkVar("amount", Amount.getBitWidth());
    SymRef Value = Ctx.mkConst(8, 0x81);
    SymRef Count = Ctx.mkConst(Amount);
    EXPECT_EQ(Ctx.mkShl(Value, Count), Ctx.mkZero(8));
    EXPECT_EQ(Ctx.mkLShr(Value, Count), Ctx.mkZero(8));
    EXPECT_EQ(Ctx.mkAShr(Value, Count), Ctx.mkOnes(8));

    llvm::APInt Values[] = {llvm::APInt(8, 0x81), Amount};
    EXPECT_EQ(Ctx.eval(Ctx.mkShl(X, Y), Values), llvm::APInt(8, 0));
    EXPECT_EQ(Ctx.eval(Ctx.mkLShr(X, Y), Values), llvm::APInt(8, 0));
    EXPECT_EQ(Ctx.eval(Ctx.mkAShr(X, Y), Values), llvm::APInt(8, 0xff));
    if (Amount.getBitWidth() <= 64) {
      uint64_t ValuesU64[] = {0x81, Amount.getZExtValue()};
      EXPECT_EQ(Ctx.evalU64(Ctx.mkShl(X, Y), ValuesU64), 0u);
      EXPECT_EQ(Ctx.evalU64(Ctx.mkLShr(X, Y), ValuesU64), 0u);
      EXPECT_EQ(Ctx.evalU64(Ctx.mkAShr(X, Y), ValuesU64), 0xffu);
    }
  }
}

TEST(SymExpr, BitwiseIdentitiesHoldOnConstruction) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);

  EXPECT_EQ(Ctx.mkXor(X, X), Ctx.mkZero(W32));
  EXPECT_EQ(Ctx.mkAnd(X, X), X);
  EXPECT_EQ(Ctx.mkOr(X, X), X);
  EXPECT_EQ(Ctx.mkAnd(X, Ctx.mkZero(W32)), Ctx.mkZero(W32));
  EXPECT_EQ(Ctx.mkAnd(X, Ctx.mkOnes(W32)), X);
  EXPECT_EQ(Ctx.mkOr(X, Ctx.mkOnes(W32)), Ctx.mkOnes(W32));
  EXPECT_EQ(Ctx.mkOr(X, Ctx.mkZero(W32)), X);
  EXPECT_EQ(Ctx.mkNot(Ctx.mkNot(X)), X);
  EXPECT_EQ(Ctx.mkXor(X, Ctx.mkZero(W32)), X);
}

TEST(SymExpr, MaskedOrKeepsDisjointKnownBits) {
  SymContext Ctx;
  SymRef Quotient = Ctx.mkVar("quotient", 16);
  SymRef Top = Ctx.mkVar("top", 16);
  SymRef Status =
      Ctx.mkOr(Ctx.mkConst(16, 0x0400),
               Ctx.mkOr(Ctx.mkAnd(Quotient, Ctx.mkConst(16, 0x4300)),
                        Ctx.mkAnd(Top, Ctx.mkConst(16, 0x3800))));

  EXPECT_EQ(Ctx.mkAnd(Status, Ctx.mkConst(16, 0x0400)),
            Ctx.mkConst(16, 0x0400));
  EXPECT_FALSE(Ctx.isConst(Ctx.mkAnd(Status, Ctx.mkConst(16, 0x0100))));
  EXPECT_FALSE(Ctx.isConst(Ctx.mkAnd(Status, Ctx.mkConst(16, 0x0800))));
}

TEST(SymExpr, StructuralOperatorsCollapseWhereTheyCan) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);

  EXPECT_EQ(Ctx.mkExtract(X, 0, W32), X);
  EXPECT_EQ(Ctx.mkExtract(Ctx.mkExtract(X, 8, 16), 4, 8),
            Ctx.mkExtract(X, 12, 8));
  EXPECT_EQ(Ctx.mkZExt(Ctx.mkZExt(Ctx.mkExtract(X, 0, 8), 16), W32),
            Ctx.mkZExt(Ctx.mkExtract(X, 0, 8), W32));
  // Adjacent literals in a concatenation merge into one word.
  EXPECT_EQ(Ctx.mkConcat(Ctx.mkConst(8, 0xAB), Ctx.mkConst(8, 0xCD)),
            Ctx.mkConst(16, 0xABCD));
  EXPECT_EQ(
      Ctx.width(Ctx.mkConcat(Ctx.mkExtract(X, 0, 8), Ctx.mkExtract(X, 8, 8))),
      16u);
}

TEST(SymExpr, ZeroPrefixedWordsShareTheZeroExtensionForm) {
  for (uint32_t Bits : {1u, 7u, 8u, 13u, 24u, 32u, 63u, 64u, 127u, 255u})
    for (uint32_t Padding : {1u, 7u, 8u, 19u, 64u}) {
      SCOPED_TRACE(Bits);
      SCOPED_TRACE(Padding);
      SymContext Ctx;
      const auto X = Ctx.mkVar("x", Bits);
      const auto Value = Ctx.mkAdd(X, Ctx.mkConst(Bits, 3));
      const auto Expected = Ctx.mkZExt(Value, Bits + Padding);
      EXPECT_EQ(Ctx.mkConcat(Ctx.mkZero(Padding), Value), Expected);
      EXPECT_EQ(Ctx.mkConcat({Ctx.mkZero(Padding), Ctx.mkZero(5), Value}),
                Ctx.mkZExt(Value, Bits + Padding + 5));
      llvm::APInt Inputs[] = {llvm::APInt::getAllOnes(Bits)};
      EXPECT_EQ(Ctx.eval(Expected, Inputs),
                (Inputs[0] + llvm::APInt(64, 3).zextOrTrunc(Bits))
                    .zext(Bits + Padding));
    }
}

TEST(SymExpr, ExtendedConcatGroupingPreservesEveryNonzeroAndUnknownBit) {
  SymContext Ctx;
  const auto High = Ctx.mkVar("high", 8), Low = Ctx.mkVar("low", 8);
  const auto Pair = Ctx.mkConcat(High, Low);
  const auto Flat = Ctx.mkConcat({Ctx.mkZero(8), High, Low});
  const auto Grouped = Ctx.mkConcat(Ctx.mkZExt(High, 16), Low);
  EXPECT_EQ(Flat, Ctx.mkZExt(Pair, 24));
  EXPECT_EQ(Grouped, Flat);
  EXPECT_EQ(Ctx.mkConcat(Ctx.mkZero(8), Grouped), Ctx.mkZExt(Pair, 32));
  const auto Interior = Ctx.mkConcat(High, Ctx.mkZExt(Low, 16));
  const auto Nonzero = Ctx.mkConcat(Ctx.mkConst(8, 0x93), Low);
  EXPECT_NE(Pair, Ctx.mkZExt(Low, 16));
  EXPECT_NE(Nonzero, Ctx.mkZExt(Low, 16));
  for (uint64_t A = 0; A != 256; ++A)
    for (uint64_t B = 0; B != 256; ++B) {
      const uint64_t Inputs[] = {A, B};
      EXPECT_EQ(Ctx.evalU64(Flat, Inputs), (A << 8) | B);
      EXPECT_EQ(Ctx.evalU64(Grouped, Inputs), (A << 8) | B);
      EXPECT_EQ(Ctx.evalU64(Interior, Inputs), (A << 16) | B);
      EXPECT_EQ(Ctx.evalU64(Nonzero, Inputs), 0x9300 | B);
    }
}

TEST(SymExpr, NonLowBitwiseWindowsRetainProvedConstants) {
  for (unsigned Width : {16u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    const unsigned Low = Width - 8;
    const auto Mask = llvm::APInt::getBitsSet(Width, Low, Width);
    const auto Literal = llvm::APInt(Width, 0x5a).shl(Low);
    auto Slice = [&](SymRef Value) {
      return Ctx.constantWindow(Value, Low, 8);
    };
    EXPECT_EQ(Slice(Ctx.mkAnd(X, Ctx.mkConst(~Mask))),
              std::optional<llvm::APInt>(llvm::APInt(8, 0)));
    EXPECT_EQ(Slice(Ctx.mkOr(X, Ctx.mkConst(Mask))),
              std::optional<llvm::APInt>(llvm::APInt(8, 0xff)));
    EXPECT_EQ(Slice(Ctx.mkXor(Ctx.mkAnd(X, Ctx.mkConst(~Mask)),
                              Ctx.mkConst(Literal))),
              std::optional<llvm::APInt>(llvm::APInt(8, 0x5a)));
    EXPECT_EQ(Slice(Ctx.mkNot(Ctx.mkOr(X, Ctx.mkConst(Mask)))),
              std::optional<llvm::APInt>(llvm::APInt(8, 0)));
    EXPECT_FALSE(Slice(Ctx.mkXor(X, Ctx.mkConst(Literal))));
  }
}

TEST(SymExpr, ConstantWindowsFollowStructuralCarriersWithoutLosingUnknowns) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8);
  SymRef Y = Ctx.mkVar("y", 8);
  SymRef Wide = Ctx.mkZExt(X, 32);
  EXPECT_EQ(Ctx.constantWindow(Ctx.mkNot(Wide), 8, 16),
            std::optional<llvm::APInt>(llvm::APInt(16, 0xffff)));
  SymRef Packed = Ctx.mkConcat(Ctx.mkConst(8, 0xa5), X);
  EXPECT_EQ(Ctx.constantWindow(Ctx.mkXor(Packed, Ctx.mkZExt(Y, 16)), 8, 8),
            std::optional<llvm::APInt>(llvm::APInt(8, 0xa5)));
  SymRef Crossed = Ctx.mkExtract(Ctx.mkNot(Wide), 4, 24);
  SymRef Mixed = Ctx.mkXor(Crossed, Ctx.mkZExt(Y, 24));
  EXPECT_EQ(Ctx.constantWindow(Mixed, 12, 8),
            std::optional<llvm::APInt>(llvm::APInt(8, 0xff)));

  const SymRef Unknown[] = {
      Ctx.mkExtract(Ctx.mkNot(Wide), 7, 2),
      Ctx.mkExtract(Ctx.mkNot(Ctx.mkSExt(X, 32)), 8, 8),
      Ctx.mkExtract(Ctx.mkXor(Packed, Ctx.mkZExt(Y, 16)), 4, 8)};
  for (SymRef Value : Unknown)
    EXPECT_FALSE(Ctx.constantWindow(Value, 0, Ctx.width(Value)));
  for (uint64_t A = 0; A != 256; ++A) {
    uint64_t Values[] = {A, A ^ 0x5a};
    EXPECT_EQ(Ctx.evalU64(Unknown[0], Values), ((~A) >> 7) & 3);
    EXPECT_EQ(Ctx.evalU64(Unknown[1], Values), A < 128 ? 0xffu : 0u);
    EXPECT_EQ(Ctx.evalU64(Unknown[2], Values),
              (((0xa500 | A) ^ (A ^ 0x5a)) >> 4) & 0xff);
  }
}

TEST(SymExpr, NonLowBitwiseWindowsMatchEverySmallInputAndMask) {
  for (unsigned M = 0; M != 16; ++M)
    for (unsigned N = 0; N != 16; ++N) {
      SymContext Ctx;
      SymRef X = Ctx.mkVar("x", 4), Y = Ctx.mkVar("y", 4);
      SymRef MX = Ctx.mkAnd(X, Ctx.mkConst(4, M));
      SymRef NY = Ctx.mkOr(Y, Ctx.mkConst(4, N));
      const SymRef Expressions[] = {Ctx.mkAnd(MX, NY), Ctx.mkOr(MX, NY),
                                    Ctx.mkXor(MX, NY), Ctx.mkNot(MX)};
      for (unsigned Low = 1; Low != 4; ++Low)
        for (unsigned Bits = 1; Bits <= 4 - Low; ++Bits) {
          const uint64_t Mask = (1u << Bits) - 1;
          llvm::SmallVector<std::optional<llvm::APInt>, 4> Windows;
          for (SymRef Expression : Expressions)
            Windows.push_back(Ctx.constantWindow(Expression, Low, Bits));
          for (uint64_t A = 0; A != 16; ++A)
            for (uint64_t B = 0; B != 16; ++B) {
              const uint64_t Expected[] = {(A & M) & (B | N), (A & M) | (B | N),
                                           (A & M) ^ (B | N), ~(A & M)};
              for (unsigned I = 0; I != std::size(Expected); ++I)
                if (Windows[I])
                  ASSERT_EQ(Windows[I]->getZExtValue(),
                            (Expected[I] >> Low) & Mask)
                      << A << ',' << B << ',' << M << ',' << N << ',' << Low
                      << ',' << Bits << ',' << I;
            }
        }
    }
}

TEST(SymExpr, ConstantWindowSearchDoesNotExpandDeepOrWideUnknownExpressions) {
  SymContext Ctx;
  SymRef Root = Ctx.mkVar("root", 64);
  for (unsigned I = 0; I != 96; ++I) {
    SymRef Other = Ctx.mkFreshVar(64, "other");
    Root = I % 2 ? Ctx.mkOr(Root, Other) : Ctx.mkAnd(Root, Other);
  }
  llvm::SmallVector<SymRef, 512> Wide;
  for (unsigned I = 0; I != 512; ++I)
    Wide.push_back(Ctx.mkFreshVar(64, "wide"));
  SymRef Many = Ctx.mkXor(Wide);
  for (SymRef Value : {Root, Many}) {
    const size_t Before = Ctx.numNodes();
    EXPECT_FALSE(Ctx.constantWindow(Value, 17, 13));
    EXPECT_EQ(Ctx.numNodes(), Before);
  }
}

TEST(SymExpr, ConstantWindowBudgetIncludesWideConstantsAndDepthRejections) {
  SymContext Ctx;
  constexpr unsigned Width = 64 * 512;
  SymRef Wide = Ctx.mkAnd(Ctx.mkVar("wide", Width),
                          Ctx.mkConst(llvm::APInt::getLowBitsSet(Width, 16)));
  // A small result must not authorize copying an arbitrarily wide constant.
  EXPECT_FALSE(Ctx.constantWindow(Wide, 20, 8));

  for (unsigned Count : {4u, 512u}) {
    SymContext Local;
    llvm::SmallVector<SymRef, 512> Operands;
    for (unsigned I = 0; I != Count; ++I)
      Operands.push_back(Local.mkFreshVar(64, "leaf"));
    SymRef Deep = Local.mkXor(Operands);
    // The wide node sits at depth 31, just before the recursion cutoff.
    for (unsigned I = 0; I != 30; ++I) {
      SymRef Other = Local.mkFreshVar(64, "other");
      Deep = I % 2 ? Local.mkAnd(Deep, Other) : Local.mkOr(Deep, Other);
    }
    SymRef Known = Local.mkNot(Local.mkZExt(Local.mkFreshVar(8, "low"), 64));
    SymRef Root = Local.mkOr(Deep, Known);
    ASSERT_EQ(Local.operand(Root, 0), Deep);
    const size_t Before = Local.numNodes();
    auto Window = Local.constantWindow(Root, 16, 8);
    if (Count == 4)
      EXPECT_EQ(Window, std::optional<llvm::APInt>(llvm::APInt(8, 0xff)));
    else
      EXPECT_FALSE(Window);
    EXPECT_EQ(Local.numNodes(), Before);
  }
}

TEST(SymExpr, ConstantWindowSharesActualWorkWithoutRelaxingQueryCeilings) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("byte", 8);
  SymRef Value = Ctx.mkZExt(X, 64);
  unsigned Remaining = 1000;
  const auto Expected = std::optional<llvm::APInt>(llvm::APInt(8, 0));
  const auto Before = Ctx.numNodes();
  EXPECT_EQ(Ctx.constantWindow(Value, 32, 8, Remaining), Expected);
  const unsigned Used = 1000 - Remaining;
  ASSERT_GT(Used, 0U);
  ASSERT_LT(Used, 256U);
  unsigned Exact = Used, Short = Used - 1, Zero = 0;
  EXPECT_EQ(Ctx.constantWindow(Value, 32, 8, Exact), Expected);
  EXPECT_EQ(Exact, 0U);
  EXPECT_FALSE(Ctx.constantWindow(Value, 32, 8, Short));
  EXPECT_FALSE(Ctx.constantWindow(Value, 32, 8, Zero));
  EXPECT_EQ(Ctx.constantWindow(Value, 32, 8, Remaining), Expected);
  EXPECT_EQ(Remaining, 1000 - 2 * Used);
  unsigned Unknown = 1000;
  EXPECT_FALSE(Ctx.constantWindow(Value, 0, 8, Unknown));
  EXPECT_LT(Unknown, 1000U);
  EXPECT_EQ(Ctx.numNodes(), Before);

  constexpr unsigned WideBits = 64 * 512;
  SymRef Wide = Ctx.mkConst(llvm::APInt::getLowBitsSet(WideBits, 16));
  unsigned Large = 10000;
  EXPECT_FALSE(Ctx.constantWindow(Wide, 20, 8, Large));
  EXPECT_GE(Large, 10000 - 256U);
}

TEST(SymExpr, ConstantWindowRejectsInvalidAndOversizedQueries) {
  SymContext Ctx;
  SymRef Value = Ctx.mkConst(128, 123);
  const size_t Before = Ctx.numNodes();
  EXPECT_FALSE(Ctx.constantWindow({}, 0, 8));
  EXPECT_FALSE(Ctx.constantWindow(SymRef(uint32_t(Before + 1)), 0, 8));
  EXPECT_FALSE(Ctx.constantWindow(Value, 0, 0));
  EXPECT_FALSE(Ctx.constantWindow(Value, 0, 65));
  EXPECT_FALSE(Ctx.constantWindow(Value, 127, 2));
  EXPECT_FALSE(Ctx.constantWindow(Value, UINT32_MAX, 8));
  EXPECT_EQ(Ctx.constantWindow(Value, 64, 64),
            std::optional<llvm::APInt>(llvm::APInt(64, 0)));
  EXPECT_EQ(Ctx.numNodes(), Before);
}

TEST(SymExpr, ConstantWindowsDoNotProjectAcrossArithmeticCarries) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 16);
  SymRef Sum = Ctx.mkAdd(X, Ctx.mkOne(16));
  SymRef Window = Ctx.mkExtract(Ctx.mkNot(Sum), 8, 8);
  EXPECT_FALSE(Ctx.constantWindow(Window, 0, 8));
  for (uint64_t A : {0u, 0xfeu, 0xffu, 0x100u, 0x7fffu, 0xffffu}) {
    uint64_t Values[] = {A};
    EXPECT_EQ(Ctx.evalU64(Window, Values), (~(A + 1) >> 8) & 0xff);
  }
}

TEST(SymExpr, ByteStateReassemblyPreservesMixedKnownAndUnknownWindows) {
  for (unsigned Width : {16u, 32u, 64u, 128u, 256u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    const auto Mask = llvm::APInt::getSplat(Width, llvm::APInt(16, 0x00ff));
    const auto Fixed = llvm::APInt::getSplat(Width, llvm::APInt(16, 0xa500));
    SymRef Word = Ctx.mkOr(Ctx.mkAnd(X, Ctx.mkConst(Mask)), Ctx.mkConst(Fixed));
    llvm::SmallVector<SymRef, 32> Bytes;
    for (unsigned Low = Width; Low != 0; Low -= 8) {
      SymRef Byte = Ctx.mkExtract(Word, Low - 8, 8);
      if (Low % 16 == 0)
        EXPECT_EQ(Ctx.constantWindow(Byte, 0, 8),
                  std::optional<llvm::APInt>(llvm::APInt(8, 0xa5)));
      else
        EXPECT_FALSE(Ctx.constantWindow(Byte, 0, 8));
      Bytes.push_back(Byte);
    }
    SymRef Roundtrip = Ctx.mkConcat(Bytes);
    EXPECT_EQ(Roundtrip, Word);
    for (unsigned A = 0; A != 256; ++A) {
      const auto Value = llvm::APInt::getSplat(Width, llvm::APInt(8, A));
      llvm::APInt Values[] = {Value};
      EXPECT_EQ(Ctx.eval(Roundtrip, Values), (Value & Mask) | Fixed);
    }
  }
}

TEST(SymExpr, LowPrefixProjectsModularArithmeticAndBitwiseOperations) {
  for (uint32_t Width : {1u, 3u, 8u, 16u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    SymRef Y = Ctx.mkVar("y", Width);
    SymRef WideX = Ctx.mkZExt(X, Width * 2);
    SymRef WideY = Ctx.mkSExt(Y, Width * 2);
    auto Low = [&](SymRef R) { return Ctx.mkExtract(R, 0, Width); };
    EXPECT_EQ(Low(Ctx.mkAdd(WideX, WideY)), Ctx.mkAdd(X, Y));
    EXPECT_EQ(Low(Ctx.mkSub(WideX, WideY)), Ctx.mkSub(X, Y));
    EXPECT_EQ(Low(Ctx.mkMul(WideX, WideY)), Ctx.mkMul(X, Y));
    EXPECT_EQ(Low(Ctx.mkAnd(WideX, WideY)), Ctx.mkAnd(X, Y));
    EXPECT_EQ(Low(Ctx.mkOr(WideX, WideY)), Ctx.mkOr(X, Y));
    EXPECT_EQ(Low(Ctx.mkXor(WideX, WideY)), Ctx.mkXor(X, Y));
    EXPECT_EQ(Low(Ctx.mkNot(WideX)), Ctx.mkNot(X));

    // Widening an unsigned all-ones word does not make it wide -1. Its low
    // product nevertheless implements modular negation at the original width.
    SymRef Coefficient =
        Ctx.mkConst(llvm::APInt::getAllOnes(Width).zext(Width * 2));
    EXPECT_EQ(Low(Ctx.mkMul(WideY, Coefficient)), Ctx.mkNeg(Y));
  }
}

TEST(SymExpr, ProjectedWordOperationsAgreeForEverySmallInputIncludingHighBits) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 4);
  SymRef Y = Ctx.mkVar("y", 4);
  const SymRef Wide[] = {Ctx.mkAdd(X, Y), Ctx.mkSub(X, Y), Ctx.mkMul(X, Y),
                         Ctx.mkAnd(X, Y), Ctx.mkOr(X, Y),  Ctx.mkXor(X, Y),
                         Ctx.mkNot(X)};
  for (unsigned Width : {1u, 2u, 3u}) {
    uint64_t Mask = (1u << Width) - 1;
    for (unsigned A = 0; A != 16; ++A) {
      for (unsigned B = 0; B != 16; ++B) {
        const uint64_t Expected[] = {A + B, uint64_t(A) - B, A * B,       A & B,
                                     A | B, A ^ B,           ~uint64_t(A)};
        uint64_t Values[] = {A, B};
        for (unsigned I = 0; I != std::size(Wide); ++I)
          EXPECT_EQ(Ctx.evalU64(Ctx.mkExtract(Wide[I], 0, Width), Values),
                    Expected[I] & Mask);
      }
    }
  }
}

TEST(SymExpr, ProjectionRetainsExtensionWhenItsInputIsNarrowerThanTheResult) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 3);
  SymRef Y = Ctx.mkVar("y", 4);
  SymRef SX = Ctx.mkSExt(X, 8);
  SymRef ZY = Ctx.mkZExt(Y, 8);
  const SymRef Wide[] = {Ctx.mkAdd(SX, ZY), Ctx.mkSub(SX, ZY),
                         Ctx.mkMul(SX, ZY), Ctx.mkAnd(SX, ZY),
                         Ctx.mkOr(SX, ZY),  Ctx.mkXor(SX, ZY)};
  for (unsigned A = 0; A != 8; ++A) {
    for (unsigned B = 0; B != 16; ++B) {
      const uint64_t SignedA = A < 4 ? A : uint64_t(A) - 8;
      const uint64_t Expected[] = {SignedA + B, SignedA - B, SignedA * B,
                                   SignedA & B, SignedA | B, SignedA ^ B};
      uint64_t Values[] = {A, B};
      for (unsigned I = 0; I != std::size(Wide); ++I)
        EXPECT_EQ(Ctx.evalU64(Ctx.mkExtract(Wide[I], 0, 5), Values),
                  Expected[I] & 31);
    }
  }
}

TEST(SymExpr, ProjectionPreservesHighProductsDivisionAndShiftAmounts) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8);
  SymRef Y = Ctx.mkVar("y", 8);
  SymRef Product = Ctx.mkMul(X, Y);
  SymRef HighProduct = Ctx.mkExtract(Product, 4, 4);
  EXPECT_EQ(Ctx.op(HighProduct), SymOp::Extract);
  EXPECT_EQ(Ctx.operand(HighProduct, 0), Product);
  uint64_t ProductValues[] = {15, 15};
  EXPECT_EQ(Ctx.evalU64(HighProduct, ProductValues), 14u);

  SymRef Division = Ctx.mkUDiv(X, Y);
  SymRef LowDivision = Ctx.mkExtract(Division, 0, 4);
  EXPECT_EQ(Ctx.op(LowDivision), SymOp::Extract);
  uint64_t DivisionValues[] = {16, 3};
  EXPECT_EQ(Ctx.evalU64(LowDivision, DivisionValues), 5u);

  SymRef Shift = Ctx.mkShl(X, Y);
  SymRef LowShift = Ctx.mkExtract(Shift, 0, 4);
  EXPECT_EQ(Ctx.op(LowShift), SymOp::Extract);
  uint64_t ShiftValues[] = {1, 16};
  EXPECT_EQ(Ctx.evalU64(LowShift, ShiftValues), 0u);

  SymRef Right = Ctx.mkExtract(Ctx.mkLShr(X, Y), 0, 4);
  uint64_t RightValues[] = {16, 1};
  EXPECT_EQ(Ctx.evalU64(Right, RightValues), 8u);
}

TEST(SymExpr, LowPrefixWalkHandlesDeepSharedWordExpressionsIteratively) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 16);
  SymRef Y = Ctx.mkVar("y", 16);
  SymRef Root = X;
  uint64_t Expected = 173;
  for (unsigned I = 0; I != 10000; ++I) {
    Root = Ctx.mkXor(Ctx.mkAdd(Root, Y), Ctx.mkConst(16, 0x35));
    Expected = ((Expected + 29) ^ 0x35) & 0xff;
  }
  SymRef Low = Ctx.mkExtract(Root, 0, 8);
  EXPECT_EQ(Ctx.op(Low), SymOp::Xor);
  uint64_t Values[] = {173, 29};
  EXPECT_EQ(Ctx.evalU64(Low, Values), Expected);
}

TEST(SymExpr, ByteSlicesReassembleComputedWordsAfterLowPrefixProjection) {
  for (uint32_t Width : {16u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width);
    SymRef Y = Ctx.mkVar("y", Width);
    const SymRef Words[] = {Ctx.mkAdd(X, Y), Ctx.mkSub(X, Y), Ctx.mkMul(X, Y),
                            Ctx.mkAnd(X, Y), Ctx.mkOr(X, Y),  Ctx.mkXor(X, Y),
                            Ctx.mkNot(X)};
    for (SymRef Word : Words) {
      llvm::SmallVector<SymRef, 32> Bytes;
      for (uint32_t Low = Width; Low != 0; Low -= 8)
        Bytes.push_back(Ctx.mkExtract(Word, Low - 8, 8));
      EXPECT_EQ(Ctx.mkConcat(Bytes), Word);
      SymRef Prefix = Bytes.back();
      for (uint32_t Low = 8; Low < Width; Low += 8) {
        Prefix = Ctx.mkConcat(Ctx.mkExtract(Word, Low, 8), Prefix);
        EXPECT_EQ(Prefix, Ctx.mkExtract(Word, 0, Low + 8));
      }
      EXPECT_EQ(Prefix, Word);
      for (uint32_t Split : {1u, Width / 2, Width - 1})
        EXPECT_EQ(Ctx.mkConcat(Ctx.mkExtract(Word, Split, Width - Split),
                               Ctx.mkExtract(Word, 0, Split)),
                  Word);

      // Only the actual low prefix may reconcile with the upper slices.
      Bytes.back() = Ctx.mkXor(Bytes.back(), Ctx.mkOne(8));
      SymRef Different = Ctx.mkConcat(Bytes);
      EXPECT_NE(Different, Word);
      llvm::APInt Values[] = {llvm::APInt(Width, 0x18f),
                              llvm::APInt(Width, 0x175)};
      EXPECT_EQ(Ctx.eval(Different, Values),
                Ctx.eval(Word, Values) ^ llvm::APInt(Width, 1));
    }
  }
}

TEST(SymExpr, SelectFoldsToItsConditionWhenTheArmsAreTheTruthValues) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef C = Ctx.mkUlt(X, Ctx.mkConst(W32, 10));

  EXPECT_EQ(Ctx.mkIte(C, Ctx.mkTrue(), Ctx.mkFalse()), C);
  EXPECT_EQ(Ctx.mkIte(C, Ctx.mkFalse(), Ctx.mkTrue()), Ctx.mkNot(C));
  EXPECT_EQ(Ctx.mkIte(Ctx.mkTrue(), X, Ctx.mkZero(W32)), X);
  EXPECT_EQ(Ctx.mkIte(C, X, X), X);
}

TEST(SymExpr, DagSizeCountsSharedSubtermsOnce) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef Shared = Ctx.mkXor(X, Y);

  // Doubling a shared node adds the product and its coefficient, not a second
  // copy of the subtree.  A tree-shaped count would call this expression twice
  // the size it costs to work with.
  size_t Before = Ctx.dagSize(Shared);
  size_t After = Ctx.dagSize(Ctx.mkAdd(Shared, Shared));
  EXPECT_EQ(Before, 3u);
  EXPECT_EQ(After, Before + 2);
}

TEST(SymExpr, ReadabilityCostCountsEveryPrintedUse) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef Z = Ctx.mkVar("z", W32);
  SymRef W = Ctx.mkVar("w", W32);
  SymRef Shared = Ctx.mkXor(X, Y);

  // Populate the cache before appending the expressions below: a later query
  // must extend it rather than recompute or return a stale prefix.
  EXPECT_EQ(Ctx.readabilityCost(Shared), 3u);

  SymRef Twice = Ctx.mkOr(Ctx.mkAnd(Shared, Z), Ctx.mkAnd(Shared, W));
  EXPECT_EQ(Ctx.dagSize(Twice), 8u);
  EXPECT_EQ(Ctx.readabilityCost(Twice), 11u);

  // The all-ones factor is how the graph spells a sign, so it is not a printed
  // quantity in the cost model.
  EXPECT_EQ(Ctx.readabilityCost(Ctx.mkNeg(Shared)), 4u);
}

TEST(SymExpr, CollectVarsReportsEveryReachableVariableOnce) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  SymRef E = Ctx.mkAdd(Ctx.mkXor(X, Y), Ctx.mkMul(X, Ctx.mkConst(W32, 3)));

  llvm::SmallVector<uint32_t, 4> Vars;
  Ctx.collectVars(E, Vars);
  ASSERT_EQ(Vars.size(), 2u);
  EXPECT_EQ(Ctx.varInfo(Vars[0]).Name, "x");
  EXPECT_EQ(Ctx.varInfo(Vars[1]).Name, "y");
}

TEST(SymExpr, CollectVarsUnionsSharedRootsAndExistingSparseIds) {
  SymContext C;
  const auto X = C.mkVar("x", 32);
  C.mkVar("unused", 257);
  const auto Y = C.mkVar("y", 32);
  const auto Z = C.mkVar("z", 32);
  const auto Kept = C.mkVar("kept", 128);
  const auto Shared = C.mkMul(C.mkXor(X, Y), C.mkConst(32, 7));
  const llvm::SmallVector<SymRef, 4> Roots{C.mkAdd(Shared, Z),
                                           C.mkXor(Shared, Z), Shared, X};
  llvm::SmallVector<uint32_t, 8> Vars{C.varId(Kept), C.varId(Y), C.varId(Y)};
  C.collectVars(Roots, Vars);
  const llvm::SmallVector<uint32_t, 4> Expected{0, 2, 3, 4};
  EXPECT_EQ(Vars, Expected);
  std::reverse(Vars.begin(), Vars.end());
  Vars.push_back(C.varId(X));
  C.collectVars(llvm::ArrayRef<SymRef>{}, Vars);
  EXPECT_EQ(Vars, Expected);
}

TEST(SymExpr, ReadabilityPrefixInspectionPreservesLazyAndCopiedCaches) {
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 64);
  const auto Sum = Ctx.mkAdd(X, Ctx.mkConst(64, 7));
  const auto Before = Ctx.numNodes();
  EXPECT_EQ(Ctx.numCachedReadabilityNodes(), 0u);
  EXPECT_EQ(Ctx.numNodes(), Before);
  EXPECT_EQ(Ctx.readabilityCost(Sum), 3u);
  EXPECT_EQ(Ctx.numCachedReadabilityNodes(), Before);

  Ctx.mkConst(llvm::APInt(256, 19));
  EXPECT_EQ(Ctx.numCachedReadabilityNodes(), Before);
  auto Copy = Ctx;
  EXPECT_EQ(Copy.numCachedReadabilityNodes(), Before);
  EXPECT_EQ(Copy.readabilityCost(Sum), 3u);
  EXPECT_EQ(Copy.numCachedReadabilityNodes(), Copy.numNodes());
  EXPECT_EQ(Ctx.numCachedReadabilityNodes(), Before);
  EXPECT_EQ(Ctx.readabilityCost(Sum), 3u);
  EXPECT_EQ(Ctx.numCachedReadabilityNodes(), Ctx.numNodes());
  EXPECT_EQ(Ctx.numNodes(), Before + 1);
}

TEST(SymExpr, FindVarSeesDeclarationsAndNothingElse) {
  SymContext Ctx;
  SymRef X32 = Ctx.mkVar("x", W32);
  EXPECT_TRUE(Ctx.hasVarName("x"));
  EXPECT_FALSE(Ctx.hasVarName("nope"));
  EXPECT_EQ(Ctx.findVarRef("x"), X32);
  EXPECT_FALSE(Ctx.findVarRef("nope"));
  ASSERT_TRUE(Ctx.findVar("x").has_value());
  EXPECT_EQ(*Ctx.findVar("x"), Ctx.varId(X32));
  EXPECT_EQ(Ctx.varInfo(*Ctx.findVar("x")).Width, W32);
  EXPECT_FALSE(Ctx.findVar("nope").has_value());

  Ctx.mkVar("x", 8);
  EXPECT_TRUE(Ctx.hasVarName("x"));
  EXPECT_FALSE(Ctx.findVar("x").has_value());
  EXPECT_FALSE(Ctx.findVarRef("x"));
}

TEST(SymExpr, FreshVariablesCarryExplicitOrigin) {
  SymContext Ctx;
  SymRef Input = Ctx.mkVar("reg$0", W32);
  SymRef Havoc = Ctx.mkFreshVar(W32, "reg$0$");

  EXPECT_FALSE(Ctx.varInfo(Ctx.varId(Input)).Fresh);
  EXPECT_TRUE(Ctx.varInfo(Ctx.varId(Havoc)).Fresh);
}

TEST(SymExpr, StructuredInputNeverMutatesAPlainVariableIdentity) {
  SymContext Ctx;
  const SymInputOrigin Origin{SymInputKind::Register, 24, 4, 0};

  SymRef Plain = Ctx.mkVar("reg$24", W32);
  SymRef Input = Ctx.mkInputVar("reg$24", W32, Origin);
  EXPECT_NE(Input, Plain);
  EXPECT_EQ(Plain, Ctx.mkVar("reg$24", W32));
  EXPECT_FALSE(Ctx.varInfo(Ctx.varId(Plain)).InputOrigin.has_value());
  EXPECT_EQ(Input, Ctx.varRef(Ctx.varId(Input)));
  EXPECT_TRUE(Ctx.hasVarName("reg$24"));
  EXPECT_FALSE(Ctx.findVar("reg$24").has_value());
  EXPECT_FALSE(Ctx.findVarRef("reg$24"));
  EXPECT_FALSE(Ctx.varRef(Ctx.numVars()).isValid());
  EXPECT_FALSE(Ctx.mkInputVar("zero-width", 0,
                              SymInputOrigin{SymInputKind::Register, 0, 1, 0})
                   .isValid());

  const SymVarInfo &Info = Ctx.varInfo(Ctx.varId(Input));
  ASSERT_TRUE(Info.InputOrigin.has_value());
  EXPECT_EQ(*Info.InputOrigin, Origin);
  EXPECT_FALSE(Info.Fresh);

  SymContext ReverseCtx;
  SymRef InputFirst = ReverseCtx.mkInputVar("reg$24", W32, Origin);
  SymRef PlainAfter = ReverseCtx.mkVar("reg$24", W32);
  EXPECT_NE(PlainAfter, InputFirst);
  EXPECT_FALSE(
      ReverseCtx.varInfo(ReverseCtx.varId(PlainAfter)).InputOrigin.has_value());
}

TEST(SymExpr, SameInputNameAndWidthKeepsDifferentOriginsIndependent) {
  SymContext Ctx;
  const SymInputOrigin RegisterOrigin{SymInputKind::Register, 24, 4, 0};
  const SymInputOrigin TemporaryOrigin{SymInputKind::Temporary, 24, 4, 0};

  SymRef Register = Ctx.mkInputVar("machine-input", W32, RegisterOrigin);
  SymRef Temporary = Ctx.mkInputVar("machine-input", W32, TemporaryOrigin);

  EXPECT_NE(Register, Temporary);
  EXPECT_NE(Ctx.varId(Register), Ctx.varId(Temporary));
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Register)).InputOrigin, RegisterOrigin);
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Temporary)).InputOrigin, TemporaryOrigin);
  EXPECT_TRUE(Ctx.hasVarName("machine-input"));
  EXPECT_FALSE(Ctx.findVar("machine-input").has_value());
}

TEST(SymExpr, OneNameAtDifferentWidthsHasDistinctConsistentVariables) {
  SymContext Ctx;
  SymRef Byte = Ctx.mkVar("same-name", 8);
  SymRef Word = Ctx.mkVar("same-name", 16);

  EXPECT_NE(Ctx.varId(Byte), Ctx.varId(Word));
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Byte)).Width, Ctx.width(Byte));
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Word)).Width, Ctx.width(Word));
  EXPECT_EQ(Byte, Ctx.mkVar("same-name", 8));
  EXPECT_EQ(Word, Ctx.mkVar("same-name", 16));
}

TEST(SymExpr, OneInputNameAtDifferentWidthsKeepsDistinctMetadata) {
  SymContext Ctx;
  const SymInputOrigin ByteOrigin{SymInputKind::Register, 0, 1, 0};
  const SymInputOrigin WordOrigin{SymInputKind::Register, 16, 2, 0};
  SymRef Byte = Ctx.mkInputVar("same-input", 8, ByteOrigin);
  SymRef Word = Ctx.mkInputVar("same-input", 16, WordOrigin);

  ASSERT_TRUE(Ctx.isVar(Byte));
  ASSERT_TRUE(Ctx.isVar(Word));
  EXPECT_NE(Ctx.varId(Byte), Ctx.varId(Word));
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Byte)).Width, Ctx.width(Byte));
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Word)).Width, Ctx.width(Word));
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Byte)).InputOrigin, ByteOrigin);
  EXPECT_EQ(Ctx.varInfo(Ctx.varId(Word)).InputOrigin, WordOrigin);
}

TEST(SymExpr, SubstitutionRebuildsThroughTheCanonicalisingBuilders) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  // Replacing y by x turns `x + y` into `x + x`, which the builders collect
  // into `2*x` rather than leaving as a sum of equal terms.
  SymRef Sum = Ctx.mkAdd(X, Y);
  EXPECT_EQ(Ctx.substituteVar(Sum, Ctx.varId(Y), X),
            Ctx.mkMul(Ctx.mkConst(W32, 2), X));
  EXPECT_EQ(Ctx.substituteVar(Sum, Ctx.varId(Y), Ctx.mkZero(W32)), X);
}

TEST(SymExpr, SharedRootSubstitutionKeepsSimultaneousReplacements) {
  std::mt19937_64 Random(0x7375627374697475);
  for (unsigned Width : {8u, 64u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const auto Z = C.mkVar("z", Width), Shared = C.mkAdd(X, Y);
    const auto Left = C.mkXor(Shared, Z), Right = C.mkAnd(Shared, Z);
    const auto Product = C.mkMul(Left, Right);
    const std::array Roots{Product, Shared, Product, Left, X, Y};
    std::unordered_map<uint32_t, SymRef> Replacements{{X.index(), Y},
                                                      {Y.index(), Z}};
    for (bool ReplaceShared : {false, true}) {
      if (ReplaceShared)
        Replacements.emplace(Shared.index(), X);
      const auto Results = C.substituteAll(Roots, Replacements);
      ASSERT_EQ(Results.size(), Roots.size());
      EXPECT_EQ(Results[0], Results[2]);
      EXPECT_EQ(Results[4], Y);
      EXPECT_EQ(Results[5], Z);
      for (unsigned Trial = 0; Trial < 64; ++Trial) {
        std::array<llvm::APInt, 3> Values;
        for (auto &V : Values) {
          V = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            V |= llvm::APInt(64, Random()).zextOrTrunc(Width).shl(Bit);
        }
        const auto S = ReplaceShared ? Values[0] : Values[1] + Values[2];
        const auto L = S ^ Values[2];
        const auto P = L * (S & Values[2]);
        const std::array Expected{P, S, P, L, Values[1], Values[2]};
        for (size_t I = 0; I < Results.size(); ++I)
          EXPECT_EQ(C.eval(Results[I], Values), Expected[I]);
      }
    }
  }
}

TEST(SymExpr, BatchedSubstitutionPreservesDifferentOperandArities) {
  std::mt19937_64 Random(0x6172697479626174);
  const std::array Counts{3u, 17u, 33u, 5u, 65u};
  for (unsigned Width : {8u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const auto Z = C.mkVar("z", Width), Shared = C.mkXor(X, Y);
    llvm::SmallVector<SymRef, 8> Terms, Roots;
    for (unsigned I = 0; I < Counts.back(); ++I)
      Terms.push_back(C.mkXor(C.mkAdd(Shared, C.mkConst(Width, I)), Z));
    for (unsigned Count : Counts)
      Roots.push_back(C.mkAdd(llvm::ArrayRef(Terms).take_front(Count)));
    const std::unordered_map<uint32_t, SymRef> Replacements{{X.index(), Y},
                                                            {Y.index(), Z}};
    const auto Results = C.substituteAll(Roots, Replacements);
    ASSERT_EQ(Results.size(), Counts.size());
    for (unsigned Trial = 0; Trial < 32; ++Trial) {
      std::array<llvm::APInt, 3> Values;
      for (auto &V : Values) {
        V = llvm::APInt(Width, 0);
        for (unsigned Bit = 0; Bit < Width; Bit += 64)
          V |= llvm::APInt(64, Random()).zextOrTrunc(Width).shl(Bit);
      }
      const auto ReplacedShared = Values[1] ^ Values[2];
      for (size_t R = 0; R < Results.size(); ++R) {
        llvm::APInt Expected(Width, 0);
        for (unsigned I = 0; I < Counts[R]; ++I)
          Expected += (ReplacedShared + I) ^ Values[2];
        EXPECT_EQ(C.eval(Results[R], Values), Expected);
      }
    }
  }
}

TEST(SymExpr, LateSubstitutionKeysPreserveEarlierRootsAndReplacementBodies) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const auto X = C.mkVar("earlier_x", Width);
    const auto Y = C.mkVar("earlier_y", Width);
    auto Earlier = X;
    for (unsigned I = 0; I < 48; ++I)
      Earlier = C.mkXor(C.mkAdd(Earlier, Y), C.mkConst(Width, I + 1));
    const auto Interior = C.mkAnd(Earlier, Y);
    const auto Placeholder = C.mkVar("later_placeholder", Width);
    const auto Root = C.mkAdd(Interior, Placeholder);
    const std::array Roots{Earlier, Interior, Root, Placeholder, Root, X};
    // Replacements are simultaneous: Interior inside Placeholder's new body
    // keeps its original meaning, while a direct Interior root becomes Y.
    const auto Expected = C.mkAdd(Y, Interior);
    const auto Nodes = C.numNodes();
    EXPECT_EQ(
        C.substituteAll(
            Roots, {{Interior.index(), Y}, {Placeholder.index(), Interior}}),
        (std::vector<SymRef>{Earlier, Y, Expected, Interior, Expected, X}));
    EXPECT_EQ(C.numNodes(), Nodes);
    // An interior replacement wins over a descendant replacement. The old
    // roots still participate independently in the same batch.
    EXPECT_EQ(C.substituteAll({Earlier, Interior, Root},
                              {{Earlier.index(), X}, {Interior.index(), Y}}),
              (std::vector<SymRef>{X, Y, C.mkAdd(Y, Placeholder)}));
  }
}

TEST(SymExpr, AdditionFastPathsKeepPackedWordIdentities) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto Forced = Ctx.mkConst(Width, 3);
    const auto Packed = Ctx.mkOr(Forced, X);
    EXPECT_EQ(Ctx.mkAdd(Ctx.mkConst(-llvm::APInt(Width, 3)), Packed),
              Ctx.mkAnd(X, Ctx.mkConst(~llvm::APInt(Width, 3))));
    const auto Union = Ctx.mkOr(X, Y);
    EXPECT_EQ(Ctx.mkAdd(Union, Ctx.mkNeg(X)), Ctx.mkXor(Union, X));
  }
}

TEST(SymExpr, RebuildingMergedMasksRetainsDemandedSimplification) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width);
    auto C = [&](uint64_t Value) { return Ctx.mkConst(Width, Value); };
    const auto Sum = Ctx.mkAdd(Ctx.mkAnd(X, C(15)), Ctx.mkAnd(X, C(31)));
    // Neither sparse mask demands a contiguous prefix. Their union does,
    // but mask merging deliberately defers that additional normalization.
    const auto Merged = Ctx.mkOr(Ctx.mkAnd(Sum, C(5)), Ctx.mkAnd(Sum, C(10)));
    const auto Rebuilt = Ctx.rebuild(Merged, Ctx.operands(Merged));
    EXPECT_LT(Ctx.readabilityCost(Rebuilt), Ctx.readabilityCost(Merged));
    SymEvalPlan Plan(Ctx, Rebuilt);
    for (uint64_t Value = 0; Value < 256; ++Value) {
      const llvm::APInt Input(Width, Value);
      EXPECT_EQ(Plan.eval({Input}), llvm::APInt(Width, (2 * Value) & 15));
    }
  }
}

//===----------------------------------------------------------------------===//
// Evaluation
//===----------------------------------------------------------------------===//

TEST(SymExpr, EvaluationAgreesAcrossAllThreeEntryPoints) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  SymRef Y = Ctx.mkVar("y", W32);
  // (x ^ y) + 2*(x & y), which is x + y written the long way round.
  SymRef E = Ctx.mkAdd(Ctx.mkXor(X, Y),
                       Ctx.mkMul(Ctx.mkConst(W32, 2), Ctx.mkAnd(X, Y)));
  ASSERT_TRUE(Ctx.fitsU64(E));

  SymEvalPlan Plan(Ctx, E);
  ASSERT_TRUE(Plan.fitsU64());

  std::mt19937_64 Rng(20260812);
  for (unsigned I = 0; I < 512; ++I) {
    uint64_t A = Rng() & 0xFFFFFFFFu;
    uint64_t B = Rng() & 0xFFFFFFFFu;
    uint64_t Want = (A + B) & 0xFFFFFFFFu;

    std::vector<uint64_t> VarsU64(Ctx.numVars(), 0);
    VarsU64[Ctx.varId(X)] = A;
    VarsU64[Ctx.varId(Y)] = B;

    std::vector<llvm::APInt> VarsAP(Ctx.numVars(), llvm::APInt(W32, 0));
    VarsAP[Ctx.varId(X)] = llvm::APInt(W32, A);
    VarsAP[Ctx.varId(Y)] = llvm::APInt(W32, B);

    EXPECT_EQ(Ctx.evalU64(E, VarsU64), Want);
    EXPECT_EQ(Ctx.eval(E, VarsAP).getZExtValue(), Want);
    EXPECT_EQ(Plan.evalU64(VarsU64), Want);
    EXPECT_EQ(Plan.eval(VarsAP).getZExtValue(), Want);
  }
}

TEST(SymExpr, EvaluationPlanReportsTheVariablesItReads) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  Ctx.mkVar("unused", W32);
  SymRef Y = Ctx.mkVar("y", W32);

  SymEvalPlan Plan(Ctx, Ctx.mkAnd(X, Y));
  ASSERT_EQ(Plan.vars().size(), 2u);
  EXPECT_EQ(Plan.vars()[0], Ctx.varId(X));
  EXPECT_EQ(Plan.vars()[1], Ctx.varId(Y));
  EXPECT_GT(Plan.numSteps(), 0u);
}

TEST(SymExpr, NativeAssociativeEvaluationMatchesFullWidthArithmetic) {
  std::mt19937_64 Random(0x6e617279);
  for (unsigned Width = 1; Width <= 64; ++Width) {
    for (unsigned Count : {2u, 3u, 5u, 6u, 7u, 8u, 33u, 257u}) {
      SCOPED_TRACE(::testing::Message()
                   << "width=" << Width << " operands=" << Count);
      SymContext Ctx;
      llvm::SmallVector<SymRef, 8> Inputs;
      for (unsigned I = 0; I < Count; ++I)
        Inputs.push_back(Ctx.mkVar("v" + std::to_string(I), Width));
      const std::array Roots{Ctx.mkAdd(Inputs), Ctx.mkMul(Inputs),
                             Ctx.mkAnd(Inputs), Ctx.mkOr(Inputs),
                             Ctx.mkXor(Inputs)};
      for (unsigned Operation = 0; Operation < Roots.size(); ++Operation) {
        SymEvalPlan Plan(Ctx, Roots[Operation]);
        for (unsigned Sample = 0; Sample < 64; ++Sample) {
          std::vector<uint64_t> Values(Count);
          std::vector<llvm::APInt> Wide;
          for (auto &Value : Values) {
            Value = Sample == 0 ? ~uint64_t(0) : Sample == 1 ? 0 : Random();
            Wide.emplace_back(Width, Value, /*isSigned=*/false,
                              /*implicitTrunc=*/true);
          }
          llvm::APInt Expected = Wide.front();
          for (unsigned I = 1; I < Count; ++I) {
            switch (Operation) {
            case 0:
              Expected += Wide[I];
              break;
            case 1:
              Expected *= Wide[I];
              break;
            case 2:
              Expected &= Wide[I];
              break;
            case 3:
              Expected |= Wide[I];
              break;
            case 4:
              Expected ^= Wide[I];
              break;
            }
          }
          EXPECT_EQ(Plan.evalU64(Values), Expected.getZExtValue());
          EXPECT_EQ(Plan.eval(Wide), Expected);
        }
      }
    }
  }
}

TEST(SymExpr, NativeArithmeticKeepsMixedWidthConsumersExact) {
  std::mt19937_64 Random(0x6d6f64756c7573);
  for (unsigned Width = 1; Width <= 64; ++Width) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto Root =
        Ctx.mkNot(Ctx.mkAdd({Ctx.mkMul(X, Y), X, Ctx.mkConst(Width, 3)}));
    const unsigned WideWidth = Width + 65;
    const auto Scale = llvm::APInt(WideWidth, 3).shl(65);
    const auto WideRoot =
        Ctx.mkMul(Ctx.mkZExt(Root, WideWidth), Ctx.mkConst(Scale));
    SymEvalPlan Native(Ctx, Root), Wide(Ctx, WideRoot);
    ASSERT_TRUE(Native.fitsU64());
    ASSERT_FALSE(Wide.fitsU64());
    for (unsigned Trial = 0; Trial < 64; ++Trial) {
      const std::array<uint64_t, 2> Raw{Trial == 0 ? UINT64_MAX : Random(),
                                        Trial == 1 ? 0 : Random()};
      const std::array Values{llvm::APInt(Width, Raw[0], false, true),
                              llvm::APInt(Width, Raw[1], false, true)};
      const auto Expected = ~(Values[0] * Values[1] + Values[0] +
                              llvm::APInt(Width, 3, false, true));
      EXPECT_EQ(Native.evalU64(Raw), Expected.getZExtValue());
      EXPECT_EQ(Native.eval(Values), Expected);
      EXPECT_EQ(Wide.eval(Values), Expected.zext(WideWidth) * Scale);
    }
  }
}

TEST(SymExpr, NativeResiduesKeepSharedFullWidthUses) {
  std::mt19937_64 Random(0x72657369647565);
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    for (unsigned Observed : {1u, 3u, 6u}) {
      SCOPED_TRACE(Observed);
      SymContext Ctx;
      const auto X = Ctx.mkVar("x", Width);
      Ctx.mkVar("unused", Width);
      const auto Y = Ctx.mkVar("y", Width), Z = Ctx.mkVar("z", Width);
      const llvm::APInt One(Width, 1), Three(Width, 3);
      const auto Offset = One.shl(Observed), Toggle = Three.shl(Observed);
      const auto Mask = ~One.shl(Observed + 1);
      const auto Scale = Three.shl(Width - Observed);
      const auto Hidden = Ctx.mkMul(
          Ctx.mkOr(Ctx.mkAnd(Ctx.mkXor(Ctx.mkAdd(X, Ctx.mkConst(Offset)),
                                       Ctx.mkConst(Toggle)),
                             Ctx.mkConst(Mask)),
                   Ctx.mkConst(Offset)),
          Ctx.mkConst(Offset + One));
      const auto Product = Ctx.mkMul({Ctx.mkConst(Scale), Ctx.mkAdd(Hidden, Y),
                                      Ctx.mkAdd(Z, Ctx.mkConst(Three))});
      const std::array Roots{
          Product,
          Ctx.mkAdd(Product, Ctx.mkLShr(Hidden, Ctx.mkConst(Width, Observed))),
          Ctx.mkAdd(Product, Ctx.mkZExt(Ctx.mkUlt(Hidden, Y), Width)),
          Ctx.mkAdd(Product, Ctx.mkUDiv(Hidden, Y))};
      for (size_t Kind = 0; Kind < Roots.size(); ++Kind) {
        SCOPED_TRACE(Kind);
        SymEvalPlan Plan(Ctx, Roots[Kind]);
        ASSERT_TRUE(Plan.fitsU64());
        EXPECT_EQ(Plan.numSteps(), Ctx.dagSize(Roots[Kind]));
        for (unsigned Trial = 0; Trial < 128; ++Trial) {
          const std::array Values{
              llvm::APInt(Width, Trial == 0 ? UINT64_MAX : Random(), false,
                          true),
              One, llvm::APInt(Width, Trial == 1 ? 0 : Random(), false, true),
              llvm::APInt(Width, Random(), false, true)};
          const auto Full =
              ((((Values[0] + Offset) ^ Toggle) & Mask) | Offset) *
              (Offset + One);
          auto Expected = Scale * (Full + Values[2]) * (Values[3] + Three);
          if (Kind == 1)
            Expected += Full.lshr(Observed);
          else if (Kind == 2)
            Expected += llvm::APInt(Width, Full.ult(Values[2]));
          else if (Kind == 3)
            Expected += Values[2].isZero() ? llvm::APInt::getAllOnes(Width)
                                           : Full.udiv(Values[2]);
          std::array<unsigned, 4> Calls{};
          EXPECT_EQ(Plan.evalU64With([&](uint32_t Id) {
            ++Calls.at(Id);
            return Values.at(Id).getZExtValue();
          }),
                    Expected.getZExtValue());
          EXPECT_EQ(Calls, (std::array<unsigned, 4>{1, 0, 1, 1}));
          Calls.fill(0);
          EXPECT_EQ(Plan.evalWith([&](uint32_t Id) {
            ++Calls.at(Id);
            return &Values.at(Id);
          }),
                    Expected);
          EXPECT_EQ(Calls, (std::array<unsigned, 4>{1, 0, 1, 1}));
        }
        EXPECT_EQ(Plan.numSteps(), Ctx.dagSize(Roots[Kind]));
      }
    }
  }
}

TEST(SymExpr, RepeatedProductResiduesRespectAllObserverWidths) {
  for (unsigned Width : {8u, 16u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    auto Z = Ctx.mkVar("z", Width);
    const uint64_t Mask = UINT64_MAX >> (64 - Width);
    const uint64_t Scale = uint64_t(3) << (Width - 5);
    auto Shared = Ctx.mkAdd(X, Ctx.mkConst(Width, 33));
    llvm::SmallVector<SymRef, 64> Factors{Ctx.mkConst(Width, Scale)};
    for (unsigned I = 0; I < 97; ++I) {
      auto Sum = Ctx.mkAdd(X, Ctx.mkConst(Width, I));
      auto V = (I & 1) ? Ctx.mkXor(Sum, Y) : Ctx.mkAdd(Sum, Z);
      Factors.push_back(Ctx.mkOr(V, Ctx.mkOne(Width)));
    }
    const auto Root = Ctx.mkAdd(Ctx.mkMul(Factors),
                                Ctx.mkLShr(Shared, Ctx.mkConst(Width, 3)));
    const auto Work = Ctx.dagSize(Root);
    SymEvalPlan Plan(Ctx, Root);
    std::mt19937_64 Random(288);
    for (unsigned Trial = 0; Trial < 128; ++Trial) {
      const std::array<uint64_t, 3> Values{
          (Trial < 2 ? uint64_t(0) - Trial : Random()) & Mask, Random() & Mask,
          Random() & Mask};
      auto Product = Scale;
      for (unsigned I = 0; I < 97; ++I) {
        const auto Sum = Values[0] + I;
        Product *= ((I & 1) ? Sum ^ Values[1] : Sum + Values[2]) | 1;
      }
      const auto Expected = (Product + (((Values[0] + 33) & Mask) >> 3)) & Mask;
      std::array<unsigned, 3> Calls{};
      EXPECT_EQ(Plan.evalU64With([&](uint32_t Id) {
        ++Calls.at(Id);
        return Values.at(Id);
      }),
                Expected);
      EXPECT_EQ(Calls, (std::array<unsigned, 3>{1, 1, 1}));
      EXPECT_EQ(Plan.eval({llvm::APInt(Width, Values[0]),
                           llvm::APInt(Width, Values[1]),
                           llvm::APInt(Width, Values[2])})
                    .getZExtValue(),
                Expected);
      EXPECT_EQ(Plan.numSteps(), Work);
    }
  }
}

TEST(SymExpr, EvaluationPlansReadEveryReachableInputOnce) {
  for (unsigned Width : {8u, 64u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width);
    Ctx.mkVar("unused", Width);
    const auto Y = Ctx.mkVar("y", Width), Z = Ctx.mkVar("z", Width);
    const llvm::APInt One(Width, 1), Seven(Width, 7);
    const auto Root = Ctx.mkMul(
        Ctx.mkAdd(Ctx.mkMul(X, Ctx.mkConst(Seven)), Y),
        Ctx.mkAdd(Z, Ctx.mkConst(One)));
    SymEvalPlan Plan(Ctx, Root);
    for (const auto &A : {llvm::APInt(Width, 0), One,
                          llvm::APInt::getAllOnes(Width)}) {
      const std::array Values{A, Seven, A + Seven, ~A};
      const auto Expected = (A * Seven + Values[2]) * (Values[3] + One);
      std::array<unsigned, 4> Calls{};
      EXPECT_EQ(Plan.evalWith([&](uint32_t Id) {
                  ++Calls.at(Id);
                  return &Values.at(Id);
                }),
                Expected);
      EXPECT_EQ(Calls, (std::array<unsigned, 4>{1, 0, 1, 1}));
      if (Width <= 64) {
        Calls.fill(0);
        EXPECT_EQ(Plan.evalU64With([&](uint32_t Id) {
                    ++Calls.at(Id);
                    return Values.at(Id).getZExtValue();
                  }),
                  Expected.getZExtValue());
        EXPECT_EQ(Calls, (std::array<unsigned, 4>{1, 0, 1, 1}));
      }
    }
  }
}

TEST(SymExpr, WideConstantSlotsSurviveInterruptedInputLookup) {
  for (unsigned Width : {8u, 64u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width);
    const auto Literal =
        llvm::APInt(Width, 7) | llvm::APInt::getOneBitSet(Width, Width - 3);
    const auto Root = Ctx.mkAdd(X, Ctx.mkConst(Literal));
    SymEvalPlan Plan(Ctx, Root);
    EXPECT_THROW(Plan.evalWith([](uint32_t) -> const llvm::APInt * {
      throw std::runtime_error("interrupted lookup");
    }),
                 std::runtime_error);
    for (const auto &Value : {llvm::APInt(Width, 5), llvm::APInt(Width, 0),
                              llvm::APInt::getAllOnes(Width),
                              llvm::APInt::getOneBitSet(Width, Width - 1)}) {
      const auto Actual = Plan.evalWith([&](uint32_t Id) {
        EXPECT_EQ(Id, Ctx.varId(X));
        return &Value;
      });
      ASSERT_EQ(Actual, Value + Literal);
    }
  }
}

TEST(SymExpr, EvaluationPlansKeepInterleavedConstantSlots) {
  for (unsigned Width : {8u, 16u, 64u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width);
    const auto K = llvm::APInt::getOneBitSet(Width, Width - 1) | 7;
    const llvm::APInt L(Width, 19), Three(Width, 3);
    const auto Root =
        Ctx.mkXor(Ctx.mkAdd(X, Ctx.mkConst(K)),
                  Ctx.mkMul(Ctx.mkConst(Three), Ctx.mkXor(X, Ctx.mkConst(L))));
    SymEvalPlan Plan(Ctx, Root);
    EXPECT_EQ(Plan.numSteps(), Ctx.dagSize(Root));
    for (const auto &Value : {llvm::APInt(Width, 0), llvm::APInt(Width, 5),
                              llvm::APInt::getAllOnes(Width),
                              llvm::APInt::getOneBitSet(Width, Width - 2)}) {
      const auto Expected = (Value + K) ^ (Three * (Value ^ L));
      EXPECT_EQ(Plan.evalWith([&](uint32_t) { return &Value; }), Expected);
      if (Width <= 64)
        EXPECT_EQ(
            Plan.evalU64With([&](uint32_t) { return Value.getZExtValue(); }),
            Expected.getZExtValue());
    }
    EXPECT_EQ(
        Plan.evalWith([](uint32_t) -> const llvm::APInt * { return nullptr; }),
        K ^ (Three * L));
  }
}

TEST(SymExpr, EvaluationPlanKeepsSharedInputsAfterContextGrowth) {
  for (unsigned Width : {8u, 64u, 65u, 257u}) {
    SymContext Ctx;
    auto X = Ctx.mkVar("x", Width);
    Ctx.mkVar("unused", Width);
    auto Y = Ctx.mkVar("y", Width);
    auto Shared = Ctx.mkAdd(X, Ctx.mkMul(Ctx.mkConst(Width, 3), Y));
    auto Root = Shared;
    for (unsigned I = 0; I < 40; ++I)
      Root = Ctx.mkXor(Ctx.mkAdd(Root, Ctx.mkConst(Width, I)), Shared);
    SymEvalPlan Plan(Ctx, Root);
    // A plan uses reachable handles, not a prefix of the growing context.
    for (unsigned I = 0; I < 1024; ++I)
      Ctx.mkAdd(X, Ctx.mkConst(Width, I));
    ASSERT_EQ(Plan.vars().size(), 2u);
    EXPECT_EQ(Plan.vars()[0], Ctx.varId(X));
    EXPECT_EQ(Plan.vars()[1], Ctx.varId(Y));
    std::vector<llvm::APInt> Values{llvm::APInt(Width, 197),
                                    llvm::APInt::getAllOnes(Width),
                                    llvm::APInt(Width, 211)};
    const auto Base = Values[0] + 3 * Values[2];
    auto Want = Base;
    for (unsigned I = 0; I < 40; ++I)
      Want = (Want + I) ^ Base;
    EXPECT_EQ(Plan.eval(Values), Want);
    unsigned Reads = 0;
    EXPECT_EQ(Plan.evalWith([&](uint32_t Id) {
      EXPECT_NE(Id, 1u);
      ++Reads;
      return &Values[Id];
    }),
              Want);
    EXPECT_EQ(Reads, 2u);
    if (Width <= 64) {
      std::vector<uint64_t> Narrow{197, ~uint64_t(0), 211};
      EXPECT_EQ(Plan.evalU64(Narrow), Want.getZExtValue());
    }
  }
}

TEST(SymExpr, NativeNormalizedResultsRetainWidthBoundaries) {
  std::mt19937_64 Random(0x6e6f726d616c);
  for (unsigned Width = 1; Width <= 64; ++Width) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto Shift = Ctx.mkVar("shift", 64);
    const auto Sum = Ctx.mkAdd(X, Y);
    const auto Product = Ctx.mkMul(X, Ctx.mkConst(Width, 3));
    const std::array Roots{
        Ctx.mkLShr(Sum, Shift), Ctx.mkURem(Sum, Y),
        Ctx.mkEq(Sum, X),       Ctx.mkUlt(Sum, Y),
        Ctx.mkUle(Sum, X),      Ctx.mkIte(Ctx.mkEq(X, Y), Product, Sum),
        Ctx.mkZExt(X, 64)};
    for (unsigned Operation = 0; Operation < Roots.size(); ++Operation) {
      SCOPED_TRACE(Operation);
      SymEvalPlan Plan(Ctx, Roots[Operation]);
      for (unsigned Trial = 0; Trial < 128; ++Trial) {
        const std::array<uint64_t, 3> Values{
            Trial == 0 || Trial == 2 ? ~uint64_t(0) : Random(),
            Trial < 2    ? 0
            : Trial == 2 ? ~uint64_t(0)
                         : Random(),
            std::array<uint64_t, 5>{0, Width - 1, Width, 64,
                                    ~uint64_t(0)}[Trial % 5]};
        const llvm::APInt A(Width, Values[0], false, true);
        const llvm::APInt B(Width, Values[1], false, true);
        const auto Total = A + B;
        const std::array Expected{
            Values[2] >= Width ? uint64_t(0)
                               : Total.lshr(unsigned(Values[2])).getZExtValue(),
            (B.isZero() ? Total : Total.urem(B)).getZExtValue(),
            uint64_t(Total == A),
            uint64_t(Total.ult(B)),
            uint64_t(Total.ule(A)),
            (A == B ? A * llvm::APInt(Width, 3, false, true) : Total)
                .getZExtValue(),
            A.getZExtValue()};
        EXPECT_EQ(Plan.evalU64(Values), Expected[Operation]);
        EXPECT_EQ(Plan.evalU64With([&](uint32_t Id) { return Values[Id]; }),
                  Expected[Operation]);
      }
    }
  }
}

TEST(SymExpr, ComposedRotationsUseEveryCountBitAndKeepDynamicCounts) {
  for (const unsigned W : {5u, 8u, 65u, 513u}) {
    SCOPED_TRACE(W);
    SymContext C;
    const auto X = C.mkVar("x", W);
    const auto First = llvm::APInt::getOneBitSet(129, 93) + 3;
    const auto Second = llvm::APInt(16, 2 * W + 1);
    const unsigned A = First.urem(W), B = Second.urem(W);
    const auto Word = llvm::APInt::getSignMask(W) | llvm::APInt(W, 11);
    for (bool LeftFirst : {false, true})
      for (bool LeftSecond : {false, true}) {
        const auto Inner = LeftFirst ? C.mkRol(X, C.mkConst(First))
                                     : C.mkRor(X, C.mkConst(First));
        const auto Result = LeftSecond ? C.mkRol(Inner, C.mkConst(Second))
                                       : C.mkRor(Inner, C.mkConst(Second));
        const unsigned Start =
            ((LeftFirst ? W - A : A) + (LeftSecond ? W - B : B)) % W;
        if (A && B)
          EXPECT_EQ(Result, C.mkRor(X, C.mkConst(W, Start)));
        EXPECT_EQ(C.eval(Result, {Word}), Word.rotr(Start));
        for (unsigned Low : {0u, 1u, W / 2})
          for (unsigned Size : {1u, W - Low}) {
            const auto Field = C.mkExtract(Result, Low, Size);
            EXPECT_EQ(C.eval(Field, {Word}),
                      Word.rotr(Start).extractBits(Size, Low));
            const unsigned At = (Start + Low) % W;
            if (At + Size <= W)
              EXPECT_EQ(Field, C.mkExtract(X, At, Size));
          }
        EXPECT_EQ(C.mkRol(C.mkRor(X, C.mkConst(First)), C.mkConst(First)), X);
      }
    const auto N = C.mkVar("count", 129);
    const auto Dynamic = C.mkRol(C.mkRor(X, N), C.mkConst(Second));
    EXPECT_EQ(C.eval(Dynamic, {Word, First}), Word.rotr(A).rotl(B));
    EXPECT_EQ(C.eval(Dynamic, {Word, llvm::APInt(129, 0)}), Word.rotl(B));
  }
}

TEST(SymExpr, OnlyComplementaryLogicalShiftsRotateTheCompleteWord) {
  SymContext C;
  const auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  const auto Shift = C.mkConst(8, 5);
  const auto L = C.mkShl(X, C.mkConst(8, 3)), R = C.mkLShr(X, Shift);
  const auto Rotation = C.mkRor(X, Shift);
  EXPECT_EQ(C.mkOr(L, R), Rotation);
  EXPECT_EQ(C.mkXor(R, L), Rotation);
  EXPECT_EQ(C.mkAdd(L, R), Rotation);
  const auto Other = C.mkOr(L, C.mkLShr(Y, Shift));
  const auto Signed = C.mkXor(L, C.mkAShr(X, Shift));
  const auto Overlap = C.mkAdd(L, C.mkLShr(X, C.mkConst(8, 4)));
  const auto Multiple = C.mkOr(C.mkMul(X, C.mkConst(8, 24)), R);
  for (unsigned I = 0; I < 256; ++I) {
    const unsigned J = I ^ 0xff;
    const std::array<uint64_t, 2> Inputs{I, J};
    EXPECT_EQ(C.evalU64(Other, Inputs), ((I << 3) | (J >> 5)) & 255);
    const auto SignShift = llvm::APInt(8, I).ashr(5).getZExtValue();
    EXPECT_EQ(C.evalU64(Signed, Inputs), ((I << 3) ^ SignShift) & 255);
    EXPECT_EQ(C.evalU64(Overlap, Inputs), ((I << 3) + (I >> 4)) & 255);
    EXPECT_EQ(C.evalU64(Multiple, Inputs), ((I * 24) | (I >> 5)) & 255);
  }
}

TEST(SymExpr, WideScaledFieldsRespectEveryConsumerAndPartialProductLimb) {
  for (const unsigned Bits : {7u, 63u, 65u, 129u, 513u}) {
    SCOPED_TRACE(Bits);
    SymContext C;
    const unsigned W = 2 * Bits + 3;
    const auto X = C.mkVar("x", Bits), Y = C.mkVar("y", Bits + 3);
    const auto Packed = C.mkConcat(Y, X);
    const auto Signed = C.mkSExt(X, W), Unsigned = C.mkZExt(Y, W);
    // One consumer observes only three bits, another sees the sign and
    // both joined lanes. A second shift straddles the product's last limb.
    const auto Scale = llvm::APInt::getOneBitSet(W, W - 3);
    const auto WideScale = llvm::APInt(W, 3).shl(W - Bits - 1);
    const auto Low = C.mkMul({C.mkConst(Scale), Signed, Unsigned, Packed});
    const auto Full = C.mkXor(Packed, C.mkAdd(Signed, Unsigned));
    const auto Boundary = C.mkMul({C.mkConst(WideScale), Signed, Packed});
    SymEvalPlan Narrow(C, Low);
    SymEvalPlan Shared(C, C.mkAdd({Low, Full, Boundary}));
    for (unsigned I : {0u, 1u, 2u, 3u, 0u, 2u}) {
      const auto A =
          I & 1 ? llvm::APInt::getAllOnes(Bits) : llvm::APInt(Bits, I);
      const auto B = I & 2 ? llvm::APInt::getSignMask(Bits + 3) | 3
                           : llvm::APInt(Bits + 3, I);
      const std::array<llvm::APInt, 2> Values{A, B};
      const auto P = B.concat(A), S = A.sext(W), U = B.zext(W);
      const auto L = Scale * S * U * P;
      EXPECT_EQ(Narrow.eval(Values), L);
      EXPECT_EQ(Shared.eval(Values), L + (P ^ (S + U)) + WideScale * S * P);
    }
  }
}

TEST(SymExpr, WideConstantRotationCountsIncludeEveryActiveLimb) {
  SymContext C;
  for (unsigned W : {65u, 128u, 257u, 1024u}) {
    const auto X = C.mkVar("x", W);
    const auto Pattern = llvm::APInt::getSignMask(W) | llvm::APInt(W, 5);
    for (auto Count :
         {llvm::APInt(W, 0), llvm::APInt(W, W + 1),
          llvm::APInt(W, ~uint64_t(0)), llvm::APInt::getOneBitSet(W, 64) + W,
          llvm::APInt::getSignMask(W) + 7}) {
      const auto Literal = C.mkConst(Count);
      const auto Amount = Count.urem(W);
      EXPECT_EQ(C.constantRotateAmount(Literal, W), Amount);
      EXPECT_EQ(C.constValue(C.mkRol(C.mkConst(Pattern), Literal)),
                Pattern.rotl(Amount));
      if (!Amount)
        EXPECT_EQ(C.mkRor(X, Literal), X);
    }
  }
}

TEST(SymExpr, NativeRotationsAndSignExtensionMatchEveryWordWidth) {
  std::mt19937_64 Random(0xf012e);
  for (unsigned Width = 1; Width <= 64; ++Width) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width);
    const auto Count = Ctx.mkVar("count", 64);
    SymEvalPlan Left(Ctx, Ctx.mkRol(X, Count)), Right(Ctx, Ctx.mkRor(X, Count));
    SymEvalPlan Signed(Ctx, Ctx.mkSExt(X, 64));
    const std::array<uint64_t, 7> Counts{
        0,         Width - 1,         Width,       Width + 1,
        2 * Width, uint64_t(1) << 63, ~uint64_t(0)};
    for (uint64_t Amount : Counts) {
      for (uint64_t Value : {uint64_t(0), uint64_t(1) << (Width - 1),
                             ~uint64_t(0), uint64_t(Random())}) {
        const llvm::APInt Word(Width, Value, false, true);
        const std::array<uint64_t, 2> Inputs{Value, Amount};
        EXPECT_EQ(Left.evalU64(Inputs),
                  Word.rotl(Amount % Width).getZExtValue());
        EXPECT_EQ(Right.evalU64(Inputs),
                  Word.rotr(Amount % Width).getZExtValue());
        EXPECT_EQ(Signed.evalU64(Inputs), Word.sextOrTrunc(64).getZExtValue());
      }
    }
  }
}

TEST(SymExpr, U64ArithmeticShiftByZeroKeepsTheWholeWord) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 64);
  SymRef Amount = Ctx.mkVar("amount", 64);
  SymEvalPlan Plan(Ctx, Ctx.mkAShr(X, Amount));
  ASSERT_TRUE(Plan.fitsU64());

  std::vector<uint64_t> Vars(Ctx.numVars(), 0);
  Vars[Ctx.varId(X)] = 0x8000000000000001ull;
  Vars[Ctx.varId(Amount)] = 0;
  EXPECT_EQ(Plan.evalU64(Vars), 0x8000000000000001ull);
}

TEST(SymExpr, ZeroExtendedUnsignedRemaindersUseTheirNarrowOperands) {
  std::mt19937_64 Random(0x7a65787472656d);
  for (auto [Left, Right] : {std::pair{1u, 1u},
                             {4u, 8u},
                             {8u, 4u},
                             {16u, 32u},
                             {32u, 16u},
                             {64u, 64u},
                             {65u, 128u},
                             {257u, 129u}}) {
    SCOPED_TRACE(::testing::Message() << Left << ':' << Right);
    const auto Inner = std::max(Left, Right);
    const auto Outer = 2 * Inner;
    SymContext C;
    auto X = C.mkVar("x", Left), Y = C.mkVar("y", Right);
    auto R = C.mkURem(C.mkZExt(X, Outer), C.mkZExt(Y, Outer));
    EXPECT_EQ(
        R, C.mkZExt(C.mkURem(C.mkZExt(X, Inner), C.mkZExt(Y, Inner)), Outer));
    EXPECT_EQ(C.mkExtract(R, 0, Inner),
              C.mkURem(C.mkZExt(X, Inner), C.mkZExt(Y, Inner)));
    SymEvalPlan Plan(C, R);
    for (unsigned Sample = 0; Sample != 128; ++Sample) {
      llvm::APInt A(Left, 0), B(Right, 0);
      for (unsigned Bit = 0; Bit < Left; Bit += 64)
        A |= llvm::APInt(64, Random()).zextOrTrunc(Left).shl(Bit);
      for (unsigned Bit = 0; Bit < Right; Bit += 64)
        B |= llvm::APInt(64, Random()).zextOrTrunc(Right).shl(Bit);
      if (Sample == 0)
        B.clearAllBits();
      auto N = A.zextOrTrunc(Inner), D = B.zextOrTrunc(Inner);
      const auto Oracle = D.isZero() ? N : N.urem(D);
      const llvm::APInt Values[] = {A, B};
      EXPECT_EQ(Plan.eval(Values), Oracle.zext(Outer));
    }
  }
}

TEST(SymExpr, ZeroExtendedRemaindersCoverEveryBytePairIncludingZero) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto R = C.mkURem(C.mkZExt(X, 16), C.mkZExt(Y, 16));
  SymEvalPlan Plan(C, R);
  for (unsigned A = 0; A != 256; ++A)
    for (unsigned B = 0; B != 256; ++B) {
      const llvm::APInt Values[] = {llvm::APInt(8, A), llvm::APInt(8, B)};
      const auto Oracle = B ? A % B : A;
      ASSERT_EQ(Plan.eval(Values), llvm::APInt(16, Oracle));
    }
}

TEST(SymExpr, RemainderNarrowingKeepsUnknownHighBitsAndWideDivideByZero) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto Wide = C.mkVar("wide", 16);
  auto HighNumerator = C.mkURem(Wide, C.mkZExt(Y, 16));
  auto HighDivisor = C.mkURem(C.mkZExt(X, 16), Wide);
  EXPECT_EQ(C.op(HighNumerator), SymOp::URem);
  EXPECT_EQ(C.op(HighDivisor), SymOp::URem);
  const llvm::APInt Values[] = {llvm::APInt(8, 255), llvm::APInt(8, 127),
                                llvm::APInt(16, 257)};
  EXPECT_EQ(C.eval(HighNumerator, Values), llvm::APInt(16, 3));
  EXPECT_EQ(C.eval(HighDivisor, Values), llvm::APInt(16, 255));
  auto Quotient = C.mkUDiv(C.mkZExt(X, 16), C.mkZExt(Y, 16));
  const llvm::APInt Zero[] = {llvm::APInt(8, 255), llvm::APInt(8, 0),
                              llvm::APInt(16, 0)};
  EXPECT_EQ(C.eval(Quotient, Zero), llvm::APInt::getAllOnes(16));
  EXPECT_NE(Quotient, C.mkZExt(C.mkUDiv(X, Y), 16));
}

TEST(SymExpr, ScaledUnsignedProductSlicesUseOnlyTheirRequiredWidth) {
  SymContext C;
  auto X = C.mkVar("scaled_x", 32), Y = C.mkVar("scaled_y", 32);
  auto Left =
      C.mkZExt(C.mkMul(C.mkConst(64, uint64_t(1) << 32), C.mkZExt(X, 64)), 128);
  auto Right = C.mkZExt(C.mkMul(C.mkConst(64, 256), C.mkZExt(Y, 64)), 128);
  auto Slice = C.mkExtract(C.mkMul(Left, Right), 64, 64);
  auto Product = C.mkMul(C.mkZExt(X, 64), C.mkZExt(Y, 64));
  ASSERT_EQ(Slice, C.mkZExt(C.mkExtract(Product, 24, 40), 64));
  SymEvalPlan Plan(C, Slice);
  ASSERT_TRUE(Plan.fitsU64());
  std::mt19937_64 Random(0x7363616c6564756c);
  for (unsigned Sample = 0; Sample < 512; ++Sample) {
    const uint64_t A =
        Sample < 4 ? (Sample & 1 ? UINT32_MAX : 0) : uint32_t(Random());
    const uint64_t B =
        Sample < 4 ? (Sample & 2 ? UINT32_MAX : 0) : uint32_t(Random());
    const uint64_t Inputs[] = {A, B};
    EXPECT_EQ(Plan.evalU64(Inputs), (A * B) >> 24);
  }
}

TEST(SymExpr, ScaledProductWindowsCoverEveryBytePairAndWideCarriers) {
  SymContext C;
  auto X = C.mkVar("byte_x", 8), Y = C.mkVar("byte_y", 8);
  auto A = C.mkZExt(C.mkMul(C.mkConst(16, 16), C.mkZExt(X, 16)), 32);
  auto B = C.mkZExt(C.mkMul(C.mkConst(16, 64), C.mkZExt(Y, 16)), 32);
  auto Slice = C.mkExtract(C.mkMul(A, B), 12, 16);
  auto Narrow = C.mkMul(C.mkZExt(X, 16), C.mkZExt(Y, 16));
  ASSERT_EQ(Slice, C.mkZExt(C.mkExtract(Narrow, 2, 14), 16));
  SymEvalPlan Plan(C, Slice);
  ASSERT_TRUE(Plan.fitsU64());
  for (unsigned Left = 0; Left < 256; ++Left)
    for (unsigned Right = 0; Right < 256; ++Right) {
      const uint64_t Inputs[] = {Left, Right};
      ASSERT_EQ(Plan.evalU64(Inputs), (Left * Right) >> 2);
    }
  std::mt19937_64 Random(0x70726f6477696465);
  for (unsigned Width : {1u, 7u, 16u, 32u, 65u, 129u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Wide;
    auto L = Wide.mkVar("left", Width), R = Wide.mkVar("right", Width);
    const unsigned Carrier = 4 * Width + 8;
    auto Product = Wide.mkMul(Wide.mkZExt(L, Carrier), Wide.mkZExt(R, Carrier));
    auto Result = Wide.mkExtract(Product, Width, 2 * Width);
    for (unsigned Sample = 0; Sample < 32; ++Sample) {
      llvm::APInt Left(Width, 0), Right(Width, 0);
      for (unsigned Bit = 0; Bit < Width; Bit += 64) {
        Left |= llvm::APInt(64, Random()).zextOrTrunc(Width).shl(Bit);
        Right |= llvm::APInt(64, Random()).zextOrTrunc(Width).shl(Bit);
      }
      const llvm::APInt Inputs[] = {Left, Right};
      auto Oracle = (Left.zext(Carrier) * Right.zext(Carrier))
                        .extractBits(2 * Width, Width);
      EXPECT_EQ(Wide.eval(Result, Inputs), Oracle);
    }
  }
}

TEST(SymExpr, ProductWindowsKeepCoefficientsCarriesAndMultipleFactors) {
  for (unsigned Coefficient : {1u, 3u, 8u})
    for (unsigned Shift : {0u, 2u, 6u}) {
      SCOPED_TRACE(Coefficient);
      SCOPED_TRACE(Shift);
      SymContext C;
      auto X = C.mkVar("x", 4), Y = C.mkVar("y", 4), Z = C.mkVar("z", 4);
      auto Scaled = C.mkMul(C.mkConst(8, 1u << Shift), C.mkZExt(X, 8));
      auto Product = C.mkMul({C.mkConst(32, Coefficient), C.mkZExt(Scaled, 32),
                              C.mkZExt(Y, 32), C.mkZExt(Z, 32)});
      for (unsigned Low : {1u, 4u, 8u, 20u, 24u}) {
        SCOPED_TRACE(Low);
        auto Slice = C.mkExtract(Product, Low, 8);
        SymEvalPlan Plan(C, Slice);
        for (unsigned A : {0u, 1u, 7u, 15u})
          for (unsigned B : {0u, 1u, 7u, 15u})
            for (unsigned D : {0u, 1u, 7u, 15u}) {
              const llvm::APInt Inputs[] = {
                  llvm::APInt(4, A), llvm::APInt(4, B), llvm::APInt(4, D)};
              // Independent modular arithmetic retains the original inner
              // byte wrap, then forms the complete outer product.
              const uint32_t Oracle =
                  Coefficient * ((A << Shift) & 255u) * B * D;
              ASSERT_EQ(Plan.eval(Inputs),
                        llvm::APInt(8, uint8_t(Oracle >> Low)));
            }
      }
      if (Coefficient == 8 && Shift == 2) {
        auto Narrow =
            C.mkMul({C.mkZExt(X, 12), C.mkZExt(Y, 12), C.mkZExt(Z, 12)});
        EXPECT_EQ(C.mkExtract(Product, 9, 12),
                  C.mkZExt(C.mkExtract(Narrow, 4, 8), 12));
        EXPECT_EQ(C.mkExtract(Product, 20, 8), C.mkZero(8));
      }
    }
}

TEST(SymExpr, ProductWindowKeepsInnerOverflowAndSignedHighBits) {
  SymContext C;
  auto X = C.mkVar("full_x", 64), Y = C.mkVar("small_y", 32);
  auto Wrapped = C.mkMul(C.mkConst(64, uint64_t(1) << 32), X);
  auto ShiftedY = C.mkMul(C.mkConst(64, 256), C.mkZExt(Y, 64));
  auto Actual = C.mkExtract(
      C.mkMul(C.mkZExt(Wrapped, 128), C.mkZExt(ShiftedY, 128)), 64, 64);
  const llvm::APInt Inputs[] = {llvm::APInt(64, uint64_t(1) << 40),
                                llvm::APInt(32, UINT32_MAX)};
  EXPECT_EQ(C.eval(Actual, Inputs), llvm::APInt(64, 0));
  auto Incorrect =
      C.mkExtract(C.mkMul(C.mkZExt(X, 128), C.mkZExt(Y, 128)), 24, 64);
  EXPECT_NE(C.eval(Actual, Inputs), C.eval(Incorrect, Inputs));
  auto Signed =
      C.mkExtract(C.mkMul(C.mkSExt(Y, 128), C.mkZExt(X, 128)), 64, 64);
  auto Oracle = (Inputs[1].sext(128) * Inputs[0].zext(128)).extractBits(64, 64);
  EXPECT_EQ(C.eval(Signed, Inputs), Oracle);

  SymContext Inner;
  auto P = Inner.mkVar("p", 32), Q = Inner.mkVar("q", 32);
  auto Truncated =
      Inner.mkMul(Inner.mkConst(40, 1u << 16), Inner.mkZExt(P, 40));
  auto Slice = Inner.mkExtract(
      Inner.mkMul(Inner.mkZExt(Truncated, 128), Inner.mkZExt(Q, 128)), 64, 64);
  const llvm::APInt WideInputs[] = {llvm::APInt(32, uint32_t(1) << 31),
                                    llvm::APInt(32, UINT32_MAX)};
  EXPECT_EQ(Inner.eval(Slice, WideInputs), llvm::APInt(64, 0));
  auto LostWrap = Inner.mkExtract(
      Inner.mkMul(Inner.mkZExt(P, 128), Inner.mkZExt(Q, 128)), 48, 64);
  EXPECT_NE(Inner.eval(Slice, WideInputs), Inner.eval(LostWrap, WideInputs));
}

TEST(SymExpr, ProductWindowsFactorZeroConcatenationLanes) {
  SymContext C;
  auto X = C.mkVar("x", 32), Y = C.mkVar("y", 32);
  auto Left = C.mkConcat({X, C.mkZero(32)});
  auto Right = C.mkConcat({Y, C.mkZero(8)});
  auto Product = C.mkMul(C.mkZExt(Left, 104), C.mkZExt(Right, 104));
  auto Actual = C.mkExtract(Product, 64, 32);
  auto Expected =
      C.mkExtract(C.mkMul(C.mkZExt(X, 64), C.mkZExt(Y, 64)), 24, 32);
  ASSERT_EQ(Actual, Expected);
  ASSERT_TRUE(C.fitsU64(Actual));
  SymEvalPlan Plan(C, Actual);
  ASSERT_TRUE(Plan.fitsU64());
  std::mt19937_64 Random(0x636f6e6361747072);
  for (unsigned I = 0; I < 512; ++I) {
    uint64_t A = I < 4
                     ? std::array<uint32_t, 4>{0, 1, 0x80000000, UINT32_MAX}[I]
                     : uint32_t(Random());
    uint64_t B = I < 4 ? UINT32_MAX : uint32_t(Random());
    auto Oracle = ((A * B) >> 24) & UINT32_MAX;
    EXPECT_EQ(Plan.evalU64({A, B}), Oracle);
  }
}

TEST(SymExpr, ZeroConcatProductWindowsKeepAllLowerCarries) {
  for (unsigned LeftPad : {1u, 7u, 32u})
    for (unsigned RightPad : {1u, 8u}) {
      SCOPED_TRACE(LeftPad);
      SCOPED_TRACE(RightPad);
      SymContext C;
      auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
      auto Carrier = 17 + LeftPad + RightPad;
      auto Left = C.mkConcat({X, C.mkZero(LeftPad)});
      auto Right = C.mkConcat({Y, C.mkZero(RightPad)});
      auto Actual = C.mkExtract(
          C.mkMul(C.mkZExt(Left, Carrier), C.mkZExt(Right, Carrier)),
          LeftPad + RightPad + 4, 8);
      auto Product = C.mkMul(C.mkZExt(X, 16), C.mkZExt(Y, 16));
      ASSERT_EQ(Actual, C.mkExtract(Product, 4, 8));
      SymEvalPlan Plan(C, Actual);
      for (unsigned A = 0; A < 256; ++A)
        for (unsigned B = 0; B < 256; ++B) {
          const llvm::APInt Inputs[] = {llvm::APInt(8, A), llvm::APInt(8, B)};
          ASSERT_EQ(Plan.eval(Inputs), llvm::APInt(8, uint8_t((A * B) >> 4)));
        }
    }
  std::mt19937_64 Random(0x7a65726f6c616e65);
  for (unsigned Width : {1u, 7u, 32u, 65u, 129u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Carrier = 2 * Width + 73;
    auto Left = C.mkConcat({X, C.mkZero(64)});
    auto Right = C.mkConcat({Y, C.mkZero(8)});
    auto Low = 72 + Width / 2;
    auto Actual = C.mkExtract(
        C.mkMul(C.mkZExt(Left, Carrier), C.mkZExt(Right, Carrier)), Low, Width);
    auto Product = C.mkMul(C.mkZExt(X, 2 * Width), C.mkZExt(Y, 2 * Width));
    ASSERT_EQ(Actual, C.mkExtract(Product, Width / 2, Width));
    SymEvalPlan Plan(C, Actual);
    for (unsigned I = 0; I < 64; ++I) {
      llvm::APInt A(Width, 0), B(Width, 0);
      for (unsigned Bit = 0; Bit < Width; Bit += 64) {
        A |= llvm::APInt(64, Random()).zextOrTrunc(Width).shl(Bit);
        B |= llvm::APInt(64, Random()).zextOrTrunc(Width).shl(Bit);
      }
      auto Oracle = (A.zext(Carrier).shl(64) * B.zext(Carrier).shl(8))
                        .extractBits(Width, Low);
      const llvm::APInt Inputs[] = {A, B};
      EXPECT_EQ(Plan.eval(Inputs), Oracle);
    }
  }
}

TEST(SymExpr, ZeroConcatFactoringPreservesNonzeroLanesAndInnerWrap) {
  for (unsigned Tail : {0u, 1u, 0x80u, 0xFFu})
    for (unsigned Low : {1u, 7u, 8u, 16u, 24u}) {
      SymContext C;
      auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
      auto Left = C.mkConcat({X, C.mkConst(8, Tail)});
      auto Right = C.mkConcat({Y, C.mkZero(8)});
      auto Actual = C.mkExtract(
          C.mkMul(C.mkZExt(Left, 48), C.mkZExt(Right, 48)), Low, 16);
      SymEvalPlan Plan(C, Actual);
      for (unsigned A : {0u, 1u, 0x7Fu, 0x80u, 0xFFu})
        for (unsigned B : {0u, 1u, 0x7Fu, 0x80u, 0xFFu}) {
          auto Oracle = (uint64_t((A << 8) | Tail) * (B << 8)) >> Low;
          const llvm::APInt Inputs[] = {llvm::APInt(8, A), llvm::APInt(8, B)};
          EXPECT_EQ(Plan.eval(Inputs), llvm::APInt(16, uint16_t(Oracle)));
        }
    }
  SymContext C;
  auto X = C.mkVar("full_x", 64), Y = C.mkVar("y", 32);
  auto Wrapped = C.mkExtract(C.mkConcat({X, C.mkZero(32)}), 0, 64);
  auto Actual =
      C.mkExtract(C.mkMul(C.mkZExt(Wrapped, 128),
                          C.mkZExt(C.mkConcat({Y, C.mkZero(8)}), 128)),
                  64, 64);
  const llvm::APInt Inputs[] = {llvm::APInt(64, uint64_t(1) << 40),
                                llvm::APInt(32, UINT32_MAX)};
  EXPECT_EQ(C.eval(Actual, Inputs), llvm::APInt(64, 0));
  auto Incorrect =
      C.mkExtract(C.mkMul(C.mkZExt(X, 128), C.mkZExt(Y, 128)), 24, 64);
  EXPECT_NE(C.eval(Actual, Inputs), C.eval(Incorrect, Inputs));
}

TEST(SymExpr, DivisionAndRemainderFollowTheBitvectorTheory) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", W32);
  std::vector<llvm::APInt> V{llvm::APInt(W32, 7)};

  // Totalised the way QF_BV totalises them, so a solver bridge needs no
  // correction terms: udiv by zero is all-ones, and the remainder is x.
  EXPECT_EQ(Ctx.eval(Ctx.mkUDiv(X, Ctx.mkZero(W32)), V),
            llvm::APInt::getAllOnes(W32));
  EXPECT_EQ(Ctx.eval(Ctx.mkURem(X, Ctx.mkZero(W32)), V), llvm::APInt(W32, 7));
  EXPECT_EQ(Ctx.eval(Ctx.mkUDiv(X, Ctx.mkConst(W32, 2)), V),
            llvm::APInt(W32, 3));
}

TEST(SymExpr, AWordWiderThanSixtyFourBitsIsOrdinary) {
  SymContext Ctx;
  constexpr uint32_t W256 = 256;
  SymRef X = Ctx.mkVar("x", W256);

  // Everything the canonicaliser does at 32 bits it does at 256, which is what
  // makes an EVM word a first-class citizen rather than a special case.
  EXPECT_EQ(Ctx.mkAdd(X, X), Ctx.mkMul(Ctx.mkConst(W256, 2), X));
  EXPECT_EQ(Ctx.mkXor(X, Ctx.mkOnes(W256)), Ctx.mkNot(X));
  EXPECT_EQ(Ctx.mkSub(X, X), Ctx.mkZero(W256));

  // `~x + 1` and `-x` are the same value but deliberately not the same node:
  // Not stays primitive because the MBA solver needs it as a generator of the
  // bitwise algebra.  Bridging the two is the simplifier's job.
  SymRef E = Ctx.mkAdd(Ctx.mkNot(X), Ctx.mkConst(W256, 1));
  EXPECT_NE(E, Ctx.mkNeg(X));
  EXPECT_FALSE(Ctx.fitsU64(E));

  llvm::APInt Big = llvm::APInt(W256, 1).shl(200) + 12345;
  std::vector<llvm::APInt> V(Ctx.numVars(), llvm::APInt(W256, 0));
  V[Ctx.varId(X)] = Big;
  EXPECT_EQ(Ctx.eval(E, V), -Big);
  EXPECT_EQ(SymEvalPlan(Ctx, E).eval(V), -Big);
}

TEST(SymExpr, PreparedWideArithmeticKeepsEveryInputAcrossEvaluations) {
  std::mt19937_64 Random(0x4150494e54524555);
  for (unsigned Width : {1u, 8u, 64u, 65u, 127u, 128u, 129u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const auto Z = C.mkVar("z", Width);
    const auto Mask = llvm::APInt::getAllOnes(Width).lshr(1);
    const auto M = C.mkConst(Mask), K = C.mkConst(Width, 3);
    const auto Root = C.mkAdd({C.mkMul({K, X, Y, Z, X}), C.mkAnd({X, Y, Z, M}),
                               C.mkOr({X, Y, Z, M}), C.mkXor({X, Y, Z, M})});
    SymEvalPlan Plan(C, Root);
    for (unsigned Trial = 0; Trial < 128; ++Trial) {
      std::array<llvm::APInt, 3> Inputs, Values;
      for (unsigned I = 0; I < Inputs.size(); ++I) {
        auto &Input = Inputs[I];
        Input = llvm::APInt(Width, 0);
        for (unsigned Bit = 0; Bit < Width; Bit += 64)
          Input |= llvm::APInt(64, Random()).zextOrTrunc(Width).shl(Bit);
        if (Trial % 8 == 0)
          Input.clearAllBits();
        else if (Trial % 8 == 1)
          Input.setAllBits();
        else if (Trial % 8 == 2)
          Input = llvm::APInt::getSignMask(Width) + I;
        if (I == 1 && Trial % 3 == 0)
          Input = Input.zextOrTrunc(8);
        Values[I] = I == 2 && Trial % 7 == 0 ? llvm::APInt(Width, 0)
                                             : Input.zextOrTrunc(Width);
      }
      auto Expected = llvm::APInt(Width, 3, false, true) * Values[0] *
                      Values[1] * Values[2] * Values[0];
      Expected += Values[0] & Values[1] & Values[2] & Mask;
      Expected += Values[0] | Values[1] | Values[2] | Mask;
      Expected += Values[0] ^ Values[1] ^ Values[2] ^ Mask;
      EXPECT_EQ(Plan.evalWith([&](uint32_t I) -> const llvm::APInt * {
        return I == 2 && Trial % 7 == 0 ? nullptr : &Inputs[I];
      }),
                Expected);
    }
  }
}

TEST(SymExpr, NativeProductsKeepAllInputsAfterModularOverflow) {
  for (unsigned Width : {1u, 8u, 16u, 31u, 32u, 63u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const auto Z = C.mkVar("z", Width);
    llvm::SmallVector<SymRef, 72> Factors(64, X);
    Factors.append({Y, Z});
    SymEvalPlan Plan(C, C.mkMul(Factors));
    for (uint64_t First : std::array<uint64_t, 5>{0, 1, 2, 3, ~uint64_t(0)}) {
      const std::array<uint64_t, 3> Values{First, 7, 13};
      std::array<unsigned, 3> Reads{};
      const auto Result = Plan.evalU64With([&](uint32_t Id) {
        ++Reads[Id];
        return Values[Id];
      });
      EXPECT_EQ(Reads, (std::array<unsigned, 3>{1, 1, 1}));
      llvm::APInt Expected(Width, 1);
      const llvm::APInt Input(Width, First, false, true);
      for (unsigned I = 0; I < 64; ++I)
        Expected *= Input;
      Expected *= llvm::APInt(Width, Values[1], false, true);
      Expected *= llvm::APInt(Width, Values[2], false, true);
      EXPECT_EQ(Result, Expected.getZExtValue());
    }
  }
}

TEST(SymExpr, WideSignedCoefficientsKeepTheirSignAcrossWordBoundaries) {
  std::mt19937_64 Random(0x5731436f656666);
  for (unsigned Width : {65u, 129u, 257u, 513u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const auto X = C.mkVar("x", Width);
    llvm::SmallVector<uint64_t, 9> Words(llvm::APInt::getNumWords(Width));
    for (auto &Word : Words)
      Word = Random();
    const llvm::APInt Input(Width, Words);
    for (const auto &Magnitude :
         {llvm::APInt(Width, 5), llvm::APInt(Width, UINT64_MAX >> 1),
          llvm::APInt::getOneBitSet(Width, 63) + 1,
          llvm::APInt::getOneBitSet(Width, 64) + 3}) {
      for (unsigned Shift : {0u, 1u, 63u, 64u, Width - 1}) {
        for (const auto &Coefficient :
             {Magnitude.shl(Shift), (-Magnitude).shl(Shift)}) {
          SymEvalPlan Plan(C, C.mkMul(C.mkConst(Coefficient), X));
          EXPECT_EQ(Plan.eval({Input}), Coefficient * Input);
          EXPECT_EQ(Plan.eval({~Input}), Coefficient * ~Input);
        }
      }
    }
  }
}

TEST(SymExpr, WideScaledProductsObserveEveryDemandedPrefixAndSharedUse) {
  std::mt19937_64 Random(0x7072656669786573);
  for (unsigned Width : {65u, 128u, 257u, 513u})
    for (unsigned Bits :
         {1u, 7u, 32u, 63u, 64u, 65u, 127u, 128u, 129u, 171u, 193u, 257u}) {
      if (Bits > Width)
        continue;
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Bits);
      SymContext C;
      const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
      const auto Z = C.mkVar("z", Width);
      const auto Scale = llvm::APInt(Width, 3).shl(Width - Bits);
      const auto Mask = llvm::APInt::getAllOnes(Width).shl(4);
      llvm::SmallVector<SymRef, 16> Factors{C.mkConst(Scale)};
      for (unsigned I = 0; I < 12; ++I) {
        const auto Left = C.mkAdd(C.mkXor(X, C.mkConst(Width, 1)),
                                  C.mkConst(Width, 2 * I + 1));
        const auto Right =
            C.mkMul(C.mkAnd(C.mkAdd(Y, C.mkConst(Width, I)), C.mkConst(Mask)),
                    C.mkOr(C.mkNot(Z), C.mkConst(Width, 4 * I + 1)));
        Factors.push_back(C.mkXor(Left, Right));
      }
      const auto Product = C.mkMul(Factors);
      const auto Shifted = C.mkLShr(Factors[3], C.mkConst(Width, Width - 1));
      // A second user can demand the whole word or expose a high bit through
      // a non-prefix operation. Neither may inherit the product's precision.
      SymEvalPlan ProductPlan(C, Product);
      SymEvalPlan SharedPlan(C, C.mkAdd(Product, Factors[3]));
      SymEvalPlan ShiftPlan(C, C.mkAdd(Product, Shifted));
      for (unsigned Trial = 0; Trial < 128; ++Trial) {
        std::array<llvm::APInt, 3> Inputs;
        for (unsigned J = 0; J < Inputs.size(); ++J) {
          auto &V = Inputs[J];
          V = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            V |= llvm::APInt(Width, Random()).shl(Bit);
          if (Trial < 4)
            V = Trial == 0   ? llvm::APInt(Width, 0)
                : Trial == 1 ? llvm::APInt::getAllOnes(Width)
                : Trial == 2 ? llvm::APInt::getOneBitSet(Width, Bits - 1)
                             : llvm::APInt::getSignMask(Width) + J;
        }
        auto Expected = Scale;
        llvm::APInt Shared(Width, 0);
        for (unsigned I = 0; I < 12; ++I) {
          const auto Left = (Inputs[0] ^ llvm::APInt(Width, 1)) + 2 * I + 1;
          const auto Right = ((Inputs[1] + I) & Mask) *
                             (~Inputs[2] | llvm::APInt(Width, 4 * I + 1));
          const auto Factor = Left ^ Right;
          Expected *= Factor;
          if (I == 2)
            Shared = Factor;
        }
        std::array<unsigned, 3> Reads{};
        EXPECT_EQ(ProductPlan.evalWith([&](uint32_t Id) {
          ++Reads[Id];
          return &Inputs[Id];
        }),
                  Expected);
        EXPECT_EQ(Reads, (std::array<unsigned, 3>{1, 1, 1}));
        EXPECT_EQ(SharedPlan.eval(Inputs), Expected + Shared);
        EXPECT_EQ(ShiftPlan.eval(Inputs), Expected + Shared.lshr(Width - 1));
      }
    }
}

TEST(SymExpr, AffineProductPlansKeepLiveLaneBitsAndRepeatedFactors) {
  std::mt19937_64 Random(0xaff1ae);
  for (unsigned Bits : {34u, 63u, 64u, 65u, 127u, 128u, 129u, 171u, 257u}) {
    constexpr unsigned W = 513;
    const unsigned FieldWidth = Bits - 33;
    SymContext C;
    const auto X = C.mkVar("x", W), Y = C.mkVar("y", W);
    const auto Field = C.mkExtract(X, 9, FieldWidth);
    const auto Upper = C.mkExtract(Y, FieldWidth, W - FieldWidth);
    const auto Packed = C.mkConcat(Upper, Field);
    const auto Scale = llvm::APInt(W, 5).shl(W - Bits);
    for (unsigned Shift : {32u, 33u, 34u}) {
      SCOPED_TRACE(::testing::Message() << Bits << ',' << Shift);
      const auto B = (-llvm::APInt(W, 7)).shl(Shift);
      auto Factor = [&](SymRef Word) {
        return C.mkAdd(C.mkConst(W, 3),
                       C.mkMul(C.mkConst(B), C.mkAdd(C.mkConst(W, 11), Word)));
      };
      const auto A = Factor(Packed), D = Factor(C.mkSExt(Field, W));
      const auto E = Factor(C.mkZExt(Field, W));
      const auto Root = C.mkMul({C.mkConst(Scale), A, A, D, E});
      SymEvalPlan Plan(C, Root);
      const auto Steps = C.dagSize(Root);
      EXPECT_EQ(Plan.numSteps(), Steps);
      EXPECT_THROW((void)Plan.evalWith([&](uint32_t) -> const llvm::APInt * {
        throw std::runtime_error("missing input");
      }),
                   std::runtime_error);
      for (unsigned Trial = 0; Trial < 64; ++Trial) {
        std::array<llvm::APInt, 2> Inputs;
        for (auto &V : Inputs) {
          std::array<uint64_t, 9> Words;
          for (auto &Word : Words)
            Word = Random();
          V = llvm::APInt(W, Words);
        }
        const auto Low = Inputs[0].extractBits(FieldWidth, 9);
        const auto Both =
            Inputs[1].extractBits(W - FieldWidth, FieldWidth).concat(Low);
        auto Eval = [&](const llvm::APInt &V) { return 3 + B * (11 + V); };
        unsigned Lookups = 0;
        const auto Actual = Plan.evalWith([&](uint32_t Id) {
          ++Lookups;
          return &Inputs[Id];
        });
        EXPECT_EQ(Actual, Scale * Eval(Both) * Eval(Both) * Eval(Low.sext(W)) *
                              Eval(Low.zext(W)));
        EXPECT_EQ(Lookups, 2u);
        EXPECT_EQ(Plan.numSteps(), Steps);
      }
    }
  }
}

TEST(SymExpr, RotatedProductFieldsKeepWrappedSignedAndDynamicBits) {
  constexpr unsigned W = 513;
  SymContext C;
  const auto X = C.mkVar("x", W), Y = C.mkVar("y", W);
  auto Count = llvm::APInt::getOneBitSet(W, 150) + 7;
  const unsigned Turn = Count.urem(W);
  const auto Right = C.mkRor(X, C.mkConst(Count));
  const auto Left = C.mkRol(X, C.mkConst(W, W - Turn));
  const unsigned Size = 39;
  ASSERT_LT(Turn + Size, W);
  const auto Slice = C.mkExtract(X, Turn, Size);
  const auto End = C.mkRor(X, C.mkConst(W, W - 5));
  const auto Signed = C.mkAShr(X, C.mkConst(W, W - 5));
  const auto Dynamic = C.mkRor(X, Y);
  const auto Narrow = C.mkZExt(Slice, W);
  const auto Sign = C.mkSExt(Slice, W);
  const auto Scale = llvm::APInt::getOneBitSet(W, W - 77);
  const std::array Sources{Right, Left, Narrow, Sign, End, Signed, Dynamic};
  const std::array Shifts{40u, 43u, 41u, 38u, 70u, 71u, 20u};
  llvm::SmallVector<SymRef> Factors{C.mkConst(Scale)};
  for (unsigned I = 0; I < Sources.size(); ++I) {
    const auto Weight = llvm::APInt(W, 2 * I + 1).shl(Shifts[I]);
    Factors.push_back(C.mkAdd(C.mkConst(W, 2 * I + 3),
                              C.mkMul(C.mkConst(Weight), Sources[I])));
  }
  Factors.push_back(Factors[4]);
  const auto Product = C.mkMul(Factors);
  SymEvalPlan Plan(C, Product);
  std::mt19937_64 Random(0xda237);
  for (unsigned I = 0; I < 64; ++I) {
    std::array<uint64_t, 9> Words;
    for (auto &Word : Words)
      Word = Random();
    const auto A = I == 0 ? llvm::APInt::getSignMask(W) : llvm::APInt(W, Words);
    const auto B = llvm::APInt(W, Random());
    const auto Field = A.extractBits(Size, Turn);
    const std::array Values{A.rotr(Turn),     A.rotl(W - Turn), Field.zext(W),
                            Field.sext(W),    A.rotr(W - 5),    A.ashr(W - 5),
                            A.rotr(B.urem(W))};
    auto Expected = Scale;
    for (unsigned J = 0; J < Values.size(); ++J) {
      const auto Factor =
          (Values[J] * (2 * J + 1)).shl(Shifts[J]) + (2 * J + 3);
      Expected *= Factor;
      if (J == 3)
        Expected *= Factor;
    }
    EXPECT_EQ(Plan.eval({A, B}), Expected);
  }
}

TEST(SymExpr, AffineEvaluationGroupsOnlyVanishingHigherPowers) {
  constexpr unsigned W = 513;
  SymContext C;
  const auto X = C.mkVar("x", W), Y = C.mkVar("y", W);
  std::mt19937_64 Random(0x63478);
  // Some offsets share an even multiple of the same variable. Other rows
  // have distinct weights or retain a complete, repeated factor.
  auto Magnitude = [](unsigned I) { return 2 * (I < 8 ? I % 2 : I) + 1; };
  auto Offset = [](unsigned I) { return I == 5 ? 6u : 2 * I + 1; };
  for (unsigned Bits : {65u, 127u, 129u, 207u, 257u}) {
    const unsigned Fourth = (Bits + 3) / 4;
    const unsigned Third = (Bits + 2) / 3;
    const auto Scale = llvm::APInt(W, 3).shl(W - Bits);
    for (unsigned Shift :
         {Fourth - 1, Fourth, Third - 1, Third, (Bits + 1) / 2}) {
      llvm::SmallVector<SymRef> Factors{C.mkConst(Scale)};
      for (unsigned I = 0; I < 12; ++I) {
        const auto B = (-llvm::APInt(W, Magnitude(I))).shl(I == 8 ? 0 : Shift);
        const auto F = C.mkAdd(C.mkConst(W, Offset(I)),
                               C.mkMul(C.mkConst(B), I % 2 ? X : Y));
        Factors.push_back(F);
        if (I == 5)
          Factors.push_back(F);
      }
      SymEvalPlan Plan(C, C.mkMul(Factors));
      for (unsigned Trial = 0; Trial < 32; ++Trial) {
        std::array<llvm::APInt, 2> Inputs;
        for (auto &V : Inputs) {
          std::array<uint64_t, 9> Words;
          for (auto &Word : Words)
            Word = Random();
          V = llvm::APInt(W, Words);
        }
        auto Expected = Scale;
        for (unsigned I = 0; I < 12; ++I) {
          auto Term =
              (Inputs[I % 2 ? 0 : 1] * Magnitude(I)).shl(I == 8 ? 0 : Shift);
          Term = llvm::APInt(W, Offset(I)) - Term;
          Expected *= Term;
          if (I == 5)
            Expected *= Term;
        }
        EXPECT_EQ(Plan.eval(Inputs), Expected);
      }
    }
  }
}

TEST(SymExpr, WideProductsKeepDynamicCoefficientsAndSharedFactorPrecision) {
  std::mt19937_64 Random(0x6c6f777072656669);
  for (unsigned W : {65u, 128u, 257u})
    for (unsigned Bits : {7u, 64u, 65u}) {
      SCOPED_TRACE(W);
      SCOPED_TRACE(Bits);
      SymContext C;
      const auto X = C.mkVar("x", W), Y = C.mkVar("y", W);
      const auto Z = C.mkVar("z", W);
      const auto Scale = llvm::APInt::getOneBitSet(W, W - Bits);
      const auto Factor = C.mkXor(Y, C.mkAdd(X, Z));
      for (bool Perturbed : {false, true}) {
        SCOPED_TRACE(Perturbed);
        const auto Offset = Scale * 3 + unsigned(Perturbed);
        const auto Coeff =
            C.mkAdd(C.mkConst(Offset), C.mkMul(C.mkConst(Scale), Z));
        const auto Product = C.mkMul({Factor, Y, Coeff});
        ASSERT_NE(C.operand(Product, 0), Coeff);
        SymEvalPlan ProductPlan(C, Product);
        SymEvalPlan SharedPlan(C, C.mkXor(Product, Factor));
        for (unsigned I = 0; I < 48; ++I) {
          std::array<llvm::APInt, 3> V;
          for (auto &Word : V) {
            std::array<uint64_t, 5> Limbs;
            for (auto &Limb : Limbs)
              Limb = Random();
            Word = llvm::APInt(W, Limbs);
          }
          if (!I)
            V[0] = llvm::APInt::getSignMask(W);
          const auto F = V[1] ^ (V[0] + V[2]);
          const auto Expected = F * V[1] * (Offset + Scale * V[2]);
          EXPECT_EQ(ProductPlan.eval(V), Expected);
          EXPECT_EQ(SharedPlan.eval(V), Expected ^ F);
        }
      }
    }
}

TEST(SymExpr, WideBitwiseVariablePairsShareTheVariadicIdentity) {
  for (unsigned Width : {65u, 257u, 32769u}) {
    SymContext C;
    const auto Y = C.mkVar("y", Width), X = C.mkVar("x", Width);
    const auto Zero = C.mkZero(Width), Ones = C.mkOnes(Width);
    EXPECT_EQ(C.mkAnd(X, Y), C.mkAnd({Y, Ones, X, X}));
    EXPECT_EQ(C.mkOr(X, Y), C.mkOr({Y, Zero, Y, X}));
    EXPECT_EQ(C.mkXor(X, Y), C.mkXor({Y, X, X, Zero, X}));
    EXPECT_EQ(C.mkAnd(Y, Y), Y);
    EXPECT_EQ(C.mkOr(X, X), X);
    EXPECT_EQ(C.mkXor(Y, Y), Zero);
  }
}

TEST(SymExpr, ConcatenatedProductScalePreservesPartialAndSharedDemands) {
  constexpr unsigned Width = 385, High = 7;
  std::mt19937_64 Random(0x434f4e434154);
  for (unsigned Bits : {8u, 63u, 64u, 65u, 97u, 129u}) {
    SCOPED_TRACE(Bits);
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const auto Z = C.mkVar("z", Width);
    const auto Tail = llvm::APInt(Width - High, 3).shl(Width - Bits);
    const auto Coeff = C.mkConcat(C.mkExtract(X, 9, High), C.mkConst(Tail));
    const auto A = C.mkXor(X, Y), B = C.mkAdd(Y, Z);
    const auto Product = C.mkMul({A, B, Z, Coeff});
    // Truncation can require less than the full product. A second observer
    // still reads the high bits of B from the same plan.
    const unsigned Demand = Width - Bits / 2;
    const auto Narrow = C.mkZExt(C.mkExtract(Product, 0, Demand), Width);
    SymEvalPlan FullPlan(C, Product), PartialPlan(C, C.mkXor(Narrow, B));
    const auto Binary = C.mkMul(B, Coeff);
    SymEvalPlan BinaryPlan(C, C.mkXor(Binary, B));
    for (unsigned I = 0; I != 32; ++I) {
      std::array<llvm::APInt, 3> Values;
      for (auto &V : Values) {
        std::array<uint64_t, 7> Limbs;
        std::generate(Limbs.begin(), Limbs.end(), [&] { return Random(); });
        V = llvm::APInt(Width, Limbs);
      }
      const auto Sum = Values[1] + Values[2];
      const auto Scale = Values[0].extractBits(High, 9).concat(Tail);
      const auto Expected = (Values[0] ^ Values[1]) * Sum * Values[2] * Scale;
      EXPECT_EQ(FullPlan.eval(Values), Expected);
      EXPECT_EQ(BinaryPlan.eval(Values), (Scale * Sum) ^ Sum);
      EXPECT_EQ(PartialPlan.eval(Values),
                Expected.trunc(Demand).zext(Width) ^ Sum);
    }
  }
}

TEST(SymExpr, IncrementalProductsKeepCanonicalCoefficientsAndRepeatedFactors) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 65u, 128u, 257u})
    for (uint64_t Coefficient : {0u, 1u, 6u, 37u}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Coefficient);
      SymContext C;
      auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
      auto Z = C.mkVar("z", Width);
      auto Scale = C.mkConst(Width, Coefficient);
      llvm::SmallVector<SymRef, 64> Factors{Scale};
      auto Left = Scale, Right = Scale;
      for (unsigned I = 0; I != 64; ++I) {
        auto Factor = I % 3 == 0 ? Z : I % 3 == 1 ? X : C.mkNot(Y);
        Factors.push_back(Factor);
        Left = C.mkMul(Left, Factor);
        Right = C.mkMul(Factor, Right);
        EXPECT_EQ(Left, Right);
        EXPECT_EQ(Left, C.mkMul(Factors));
      }
      std::vector<llvm::APInt> Values{llvm::APInt(Width, 3, false, true),
                                      llvm::APInt(Width, 2, false, true),
                                      llvm::APInt(Width, 5, false, true)};
      auto Want = llvm::APInt(Width, Coefficient, false, true);
      for (unsigned I = 0; I != 64; ++I)
        Want *= I % 3 == 0 ? Values[2] : I % 3 == 1 ? Values[0] : ~Values[1];
      EXPECT_EQ(C.eval(Left, Values), Want);
      // A new literal still combines with the existing coefficient, including
      // modular zero and wide constants; merging two products stays canonical.
      auto Extra = C.mkConst(llvm::APInt::getSignMask(Width));
      Factors.push_back(Extra);
      EXPECT_EQ(C.mkMul(Left, Extra), C.mkMul(Factors));
      EXPECT_EQ(C.mkMul(Left, C.mkMul(X, Z)), C.mkMul({Left, X, Z}));
    }
}

TEST(SymExpr, BinaryProductsPreserveOpaqueOperandsAndModularOverflow) {
  std::mt19937_64 Random(0x61746f6d70616972ULL);
  for (unsigned Width : {1u, 3u, 8u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const auto Left = C.mkAnd(X, C.mkConst(~llvm::APInt(Width, 1)));
    const auto Right = C.mkNot(Y);
    const auto Product = C.mkMul(Left, Right);
    const auto Square = C.mkMul(Right, Right);
    const auto Coefficient = llvm::APInt::getSignMask(Width) | 1;
    const auto Scaled = C.mkMul(Right, C.mkConst(Coefficient));
    EXPECT_EQ(Product, C.mkMul(Right, Left));
    EXPECT_EQ(Square, C.mkMul({Right, C.mkConst(Width, 1), Right}));
    EXPECT_EQ(C.mkMul(C.mkZero(Width), Right), C.mkZero(Width));
    EXPECT_EQ(C.mkMul(Right, C.mkConst(Width, 1)), Right);
    for (unsigned I = 0; I != 64; ++I) {
      llvm::SmallVector<uint64_t, 8> XWords((Width + 63) / 64);
      llvm::SmallVector<uint64_t, 8> YWords(XWords.size());
      for (auto &Word : XWords)
        Word = Random();
      for (auto &Word : YWords)
        Word = Random();
      llvm::APInt A(Width, XWords), B(Width, YWords);
      if (I == 0)
        A = llvm::APInt::getSignMask(Width);
      if (I == 1)
        B = llvm::APInt::getAllOnes(Width);
      const llvm::APInt Inputs[] = {A, B};
      EXPECT_EQ(C.eval(Product, Inputs), (A & ~llvm::APInt(Width, 1)) * ~B);
      EXPECT_EQ(C.eval(Square, Inputs), ~B * ~B);
      EXPECT_EQ(C.eval(Scaled, Inputs), Coefficient * ~B);
    }
  }
}

} // namespace

TEST(SymExpr, ConditionalConstantWindowsIgnorePredicatesAndRespectWork) {
  SymContext C;
  const auto A = C.mkVar("first", 64), B = C.mkVar("second", 64);
  auto PredicateValue = A;
  for (unsigned I = 0; I < 256; ++I)
    PredicateValue = C.mkXor(C.mkAdd(PredicateValue, B),
                             C.mkLShr(PredicateValue, C.mkConst(64, 7)));
  const auto Predicate = C.mkEq(PredicateValue, C.mkZero(64));
  const auto Inner =
      C.mkIte(Predicate, C.mkConst(64, 0xEE49), C.mkConst(64, 0xEE4B));
  const auto Value =
      C.mkIte(C.mkEq(B, C.mkZero(64)), Inner, C.mkConst(64, 0xEE48));
  const auto Before = C.numNodes();
  unsigned Remaining = 256;
  EXPECT_EQ(C.constantWindow(Value, 32, 32, Remaining), llvm::APInt(32, 0));
  EXPECT_EQ(C.numNodes(), Before);
  const auto Used = 256 - Remaining;
  ASSERT_GT(Used, 0u);
  ASSERT_LT(Used, 32u);
  unsigned Exact = Used, Short = Used - 1;
  EXPECT_EQ(C.constantWindow(Value, 32, 32, Exact), llvm::APInt(32, 0));
  EXPECT_EQ(Exact, 0u);
  EXPECT_FALSE(C.constantWindow(Value, 32, 32, Short));
  EXPECT_EQ(C.mkExtract(Value, 32, 32), C.mkZero(32));
  EXPECT_FALSE(C.constantWindow(Value, 0, 8));
}

TEST(SymExpr, ConditionalSlicesKeepDifferentAndUnknownArms) {
  SymContext C;
  const auto Condition = C.mkVar("condition", 1);
  const auto Unknown = C.mkVar("unknown", 64);
  const auto Same = C.mkIte(Condition, C.mkConst(64, 0x12345678000000AA),
                            C.mkConst(64, 0x12345678000000BB));
  EXPECT_EQ(C.mkExtract(Same, 32, 32), C.mkConst(32, 0x12345678));
  const auto Different = C.mkIte(Condition, C.mkConst(64, 0x12345678000000AA),
                                 C.mkConst(64, 0xFEDCBA98000000BB));
  const auto Partial =
      C.mkIte(Condition, C.mkConst(64, 0x12345678000000AA), Unknown);
  for (const auto Value : {Different, Partial}) {
    EXPECT_FALSE(C.constantWindow(Value, 32, 32));
    const auto Slice = C.mkExtract(Value, 32, 32);
    EXPECT_FALSE(C.isConst(Slice));
    for (uint64_t Selected : {0u, 1u})
      for (uint64_t Input : {UINT64_C(0), UINT64_C(0xA5A5A5A512345678)}) {
        const uint64_t Values[] = {Selected, Input};
        EXPECT_EQ(C.evalU64(Slice, Values), C.evalU64(Value, Values) >> 32);
      }
  }
}

TEST(SymExpr, PlainVariableIndexKeepsLeafIdentityAcrossGrowthAndCopies) {
  SymContext C;
  const std::string Name("value\0suffix", 12);
  const auto First = C.mkVar(Name, 8);
  const auto Wide = C.mkVar(Name, 64);
  const auto Prefix = C.mkVar("value", 8);
  EXPECT_NE(First, Wide);
  EXPECT_NE(First, Prefix);
  for (unsigned I = 0; I < 2048; ++I)
    C.mkVar("other_" + std::to_string(I), I % 2 ? 8 : 64);
  SymContext Copy(C);
  for (auto *Context : {&C, &Copy}) {
    const auto Nodes = Context->numNodes();
    EXPECT_EQ(Context->mkVar(Name, 8), First);
    EXPECT_EQ(Context->mkVar(Name, 64), Wide);
    EXPECT_EQ(Context->mkVar("value", 8), Prefix);
    EXPECT_EQ(Context->varRef(Context->varId(First)), First);
    EXPECT_EQ(Context->numNodes(), Nodes);
    EXPECT_EQ(Context->varInfo(Context->varId(First)).Name, Name);
  }
  const auto New = Copy.mkVar("copy_only", 32);
  EXPECT_EQ(Copy.width(New), 32u);
  EXPECT_FALSE(C.hasVarName("copy_only"));
}

TEST(SymExpr, SingleVariableSubstitutionRetainsUnrelatedPayloads) {
  SymContext C;
  const auto Frame = C.mkVar("substitution_frame", 64);
  const auto Data = C.mkVar("substitution_data", 64);
  const auto Key = C.mkVar("substitution_key", 64);
  auto Payload = Data;
  for (unsigned I = 0; I < 48; ++I)
    Payload = C.mkXor(C.mkAdd(Payload, Data), Key);
  const auto Root = C.mkAdd(Frame, Payload);
  const auto Aligned =
      C.mkOr(C.mkAnd(Frame, C.mkConst(64, uint64_t(-16))), C.mkConst(64, 8));
  const auto Expected = C.mkAdd(Aligned, Payload);
  const std::vector<SymRef> Roots{Payload, Root, Key, Frame};
  const auto Before = C.numNodes();
  const auto Results = C.substituteAll(Roots, {{Frame.index(), Aligned}});
  EXPECT_EQ(Results, (std::vector<SymRef>{Payload, Expected, Key, Aligned}));
  EXPECT_EQ(C.numNodes(), Before);
  EXPECT_EQ(C.substituteVar(Payload, C.varId(Frame), Aligned), Payload);
}

TEST(SymExpr, VariableMembershipMatchesStructuralCollection) {
  SymContext C;
  const auto A = C.mkVar("membership_first", 32);
  const auto Missing = C.mkVar("membership_gap", 32);
  const auto B = C.mkVar("membership_last", 32);
  auto Value = B;
  for (unsigned I = 0; I < 32; ++I) {
    Value = C.mkXor(C.mkAdd(Value, B), C.mkShl(A, C.mkConst(32, I % 31)));
    llvm::SmallVector<uint32_t> Variables;
    C.collectVars(Value, Variables);
    const auto Before = C.numNodes();
    for (uint32_t Id = 0; Id <= C.numVars(); ++Id)
      EXPECT_EQ(C.containsVariable(Value, Id),
                std::find(Variables.begin(), Variables.end(), Id) !=
                    Variables.end());
    EXPECT_FALSE(C.containsVariable(Value, C.varId(Missing)));
    EXPECT_FALSE(C.containsVariable(Value, uint32_t(-1)));
    EXPECT_EQ(C.numNodes(), Before);
  }
  const auto Later = C.mkVar("membership_appended", 32);
  const auto Extended = C.mkAdd(Value, Later);
  EXPECT_FALSE(C.containsVariable(Value, C.varId(Later)));
  EXPECT_TRUE(C.containsVariable(Extended, C.varId(Later)));
  auto Copy = C;
  EXPECT_TRUE(Copy.containsVariable(Extended, C.varId(Later)));
  EXPECT_FALSE(Copy.containsVariable(Extended, C.varId(Missing)));
  EXPECT_FALSE(C.containsVariable(C.mkZero(32), C.varId(A)));
}

TEST(SymExpr, VariableMembershipDoesNotRestrictOtherSubstitutionKeys) {
  SymContext C;
  const auto A = C.mkVar("substitution_left", 64);
  const auto B = C.mkVar("substitution_right", 64);
  const auto Part = C.mkXor(A, B);
  const auto Root = C.mkAdd(Part, A);
  EXPECT_EQ(C.substitute(Root, {{Part.index(), C.mkConst(64, 17)}}),
            C.mkAdd(C.mkConst(64, 17), A));
  EXPECT_EQ(C.substitute(Root, {{A.index(), C.mkOne(64)},
                                {B.index(), C.mkConst(64, 3)}}),
            C.mkConst(64, 3));
}

TEST(SymExpr, DiscardedPackedFieldsDoNotGrowAcrossUpdates) {
  for (unsigned Width : {16u, 32u, 64u, 128u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const auto Mask = C.mkConst(Width, 0x7300);
    const auto Input = C.mkVar("status", Width);
    const auto Preserved = C.mkAnd(Input, Mask);
    auto Status = Preserved;
    for (unsigned I = 0; I < 128; ++I) {
      const auto Bit = C.mkVar("bit" + std::to_string(I), 1);
      const auto Field = C.mkMul(C.mkConst(Width, 16), C.mkZExt(Bit, Width));
      EXPECT_EQ(C.mkAnd(Field, Mask), C.mkZero(Width));
      Status = C.mkAnd(C.mkOr(Status, Field), Mask);
    }
    EXPECT_EQ(Status, Preserved) << C.toString(Status);
    // Observed bits and carries must survive; zero-extension is essential.
    const auto Wide = C.mkVar("wide", Width);
    const auto Field = C.mkMul(C.mkConst(Width, 16), Wide);
    EXPECT_FALSE(C.isConst(C.mkAnd(Field, Mask)));
    const auto Low = C.mkZExt(C.mkVar("low", 4), Width);
    EXPECT_FALSE(
        C.isConst(C.mkAnd(C.mkAdd(Low, C.mkOne(Width)), C.mkConst(Width, 16))));
  }
}
