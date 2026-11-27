//===- SymMBAResiduePowerTests.cpp - Bounded modular power decisions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check high-degree low-word products against independently multiplied
/// complete words, including even inputs and exhausted proof resources.
///
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/solver/Z3Solver.h"
#include "neverd/symbolic/SymMBA.h"

#include <algorithm>
#include <array>
#include <random>
#include <utility>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

struct PowerFixture {
  SymRef Input;
  unsigned Width, Observed, Count;
  unsigned Perturb = 0;

  llvm::APInt oracle(const std::array<llvm::APInt, 3> &Values) const {
    auto Result = llvm::APInt(Width, 3).shl(Width - Observed) * Values[2];
    for (unsigned I = 0; I < Count; ++I)
      Result *= (Values[0] + llvm::APInt(Width, I).shl((Observed + 2) / 3) +
                 llvm::APInt(Width, I == 0 ? Perturb : 0)) ^
                Values[1].shl((Observed + 2) / 3);
    return Result;
  }
};

PowerFixture fixture(SymContext &Ctx, unsigned Width, unsigned Observed,
                     unsigned Count, unsigned Perturb = 0) {
  const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  const auto Z = Ctx.mkVar("z", Width);
  const unsigned Split = (Observed + 2) / 3;
  const auto High = Ctx.mkShl(Y, Ctx.mkConst(Width, Split));
  llvm::SmallVector<SymRef, 128> Factors{
      Ctx.mkConst(llvm::APInt(Width, 3).shl(Width - Observed)), Z};
  for (unsigned I = 0; I < Count; ++I)
    Factors.push_back(Ctx.mkXor(
        Ctx.mkAdd(X, Ctx.mkConst(llvm::APInt(Width, I).shl(Split) +
                                 llvm::APInt(Width, I == 0 ? Perturb : 0))),
        High));
  // Z has independent parity. In particular, an odd Z cannot justify
  // treating the repeated X factors as odd before reducing their powers.
  return {Ctx.mkMul(Factors), Width, Observed, Count, Perturb};
}

TEST(SymMBAResiduePowers, LowWordsKeepEvenInputsAndSmallExponents) {
  for (unsigned Observed = 1; Observed <= 8; ++Observed)
    for (unsigned Count :
         {1u, 2u, 3u, 7u, 8u, 16u, 17u, 32u, 33u, 64u, 65u, 96u}) {
      SCOPED_TRACE(::testing::Message() << Observed << ':' << Count);
      SymContext Ctx;
      const auto F = fixture(Ctx, 8, Observed, Count);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
      EXPECT_LE(Budget.used(), 65536u);
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned Trial = 0; Trial < 1024; ++Trial) {
        const std::array Values{
            llvm::APInt(8, Trial), llvm::APInt(8, Trial / 4),
            llvm::APInt(8, std::array{0u, 1u, 2u, 255u}[Trial / 256])};
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values)) << Trial;
      }
    }
}

TEST(SymMBAResiduePowers, LargeGroupsRetainFullWidthWords) {
  std::mt19937_64 Random(0x504f574552574944);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Count : {64u, 65u, 96u, 128u, 192u, 256u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Count);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, 8, Count);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
      // The immediate odd neighbor retains a nonconstant high correction;
      // it is an equivalence control even when the bounded emitter declines.
      if (Count != 65) {
        ASSERT_NE(Result, F.Input) << "work=" << Budget.used();
        EXPECT_LT(detail::readingCost(Ctx, Result),
                  detail::readingCost(Ctx, F.Input));
      }
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned Trial = 0; Trial < 128; ++Trial) {
        std::array<llvm::APInt, 3> Values;
        for (auto &Value : Values) {
          Value = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random()).shl(Bit);
        }
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

TEST(SymMBAResiduePowers, LargeBoundedDecisionsDoNotRequireAffineExpansion) {
  std::mt19937_64 Random(0x504f574552353132);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Observed : {4u, 6u, 8u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Observed);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, Observed, 512);
      ASSERT_GT(Ctx.numOperands(F.Input), 512u);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage, true);
      EXPECT_LE(Budget.used(), 65536u);
      EXPECT_NE(Result, F.Input) << "work=" << Budget.used();
      EXPECT_LT(detail::readingCost(Ctx, Result),
                detail::readingCost(Ctx, F.Input));
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned Trial = 0; Trial < 64; ++Trial) {
        std::array<llvm::APInt, 3> Values;
        for (auto &Value : Values) {
          Value = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random()).shl(Bit);
        }
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

TEST(SymMBAResiduePowers, LargeGroupsReachTheSharedSimplifier) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Observed : {4u, 6u, 8u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Observed);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, Observed, 512);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
      EXPECT_TRUE(Result.Changed) << "work=" << Result.Work;
      EXPECT_LT(Result.Work, Options.MaxWork);
      EXPECT_LT(Result.SizeAfter, F.Count);
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned I = 0; I < 256; ++I) {
        const std::array Values{llvm::APInt(Width, I),
                                llvm::APInt(Width, I * 17),
                                llvm::APInt(Width, I / 4)};
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

TEST(SymMBAResiduePowers, PeriodNeighborsReachTheSharedSimplifier) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Observed : {4u, 6u, 8u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Observed);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, Observed, 513);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
      EXPECT_TRUE(Result.Changed) << "work=" << Result.Work;
      EXPECT_LT(Result.Work, Options.MaxWork);
      EXPECT_LT(Result.SizeAfter, F.Count);
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned I = 0; I < 256; ++I) {
        const std::array Values{llvm::APInt(Width, I),
                                llvm::APInt(Width, I * 17),
                                llvm::APInt(Width, I / 4)};
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

TEST(SymMBAResiduePowers, ProjectedFactorsRetainLowDisturbances) {
  for (unsigned Width : {8u, 32u, 65u, 257u})
    for (unsigned Perturb : {0u, 1u, 128u, 256u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Perturb);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, 8, 512, Perturb);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage, true);
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned I = 0; I < 256; ++I)
        for (unsigned J : {0u, 1u, 7u, 31u}) {
          const std::array Values{llvm::APInt(Width, I), llvm::APInt(Width, J),
                                  llvm::APInt(Width, I * 53)};
          EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
        }
      const std::array Witness{llvm::APInt(Width, 1), llvm::APInt(Width, 0),
                               llvm::APInt(Width, 1)};
      const auto Unperturbed = llvm::APInt(Width, 3).shl(Width - 8);
      EXPECT_EQ(F.oracle(Witness),
                Unperturbed * llvm::APInt(Width, 1 + Perturb));
      if (Perturb == 1 || Perturb == 128)
        EXPECT_NE(F.oracle(Witness), Unperturbed);
      else
        EXPECT_EQ(F.oracle(Witness), Unperturbed);
    }
}

static void checkLinearProducts(unsigned Count, bool RepeatOffsets,
                                llvm::ArrayRef<unsigned> Widths) {
  std::mt19937_64 Random(0x2516412);
  for (unsigned Bits : Widths) {
    SCOPED_TRACE(::testing::Message() << Count << "," << Bits);
    const unsigned Width = Bits <= 65 ? 193 : 321;
    const unsigned Shift = Bits + 1, Precision = Shift + Bits;
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    auto Field = Ctx.mkExtract(X, 62, Bits);
    const auto Other = Ctx.mkExtract(Y, 7, Bits);
    const auto High = Ctx.mkExtract(Y, Bits, Width - Bits);
    const auto Scale = llvm::APInt(Width, 5).shl(Width - Precision);
    // The top coefficient bits and the even offset both affect the product.
    auto Offset = [&](unsigned I) {
      const unsigned K = RepeatOffsets ? I % 7 : I;
      const bool Even = I == 20 || (RepeatOffsets && I == 72);
      auto A = llvm::APInt(Width, Even ? 6 : 2 * K + 1);
      if (I % 5 == 2)
        A.setBit(Bits);
      return A;
    };
    auto Weight = [&](unsigned I) {
      auto B = llvm::APInt(Width, 2 * I + 3);
      if (I % 7 == 1)
        B.setBit(Bits - 1);
      return (I % 5 == 3 ? -B : B).shl(Shift + I % 2);
    };
    llvm::SmallVector<SymRef, 128> Factors{Ctx.mkConst(Scale), Y};
    for (unsigned I = 0; I < Count; ++I) {
      const auto Low = I % 3 == 0   ? Ctx.mkNot(Field)
                       : I % 3 == 1 ? Other
                                    : Field;
      const auto Word = I % 3 == 0   ? Ctx.mkZExt(Low, Width)
                        : I % 3 == 1 ? Ctx.mkSExt(Low, Width)
                                     : Ctx.mkConcat(High, Low);
      const auto A = Ctx.mkConst(Offset(I)), B = Ctx.mkConst(Weight(I));
      Factors.push_back(Ctx.mkAdd(A, Ctx.mkMul(B, Word)));
    }
    const auto Input = Ctx.mkMul(Factors);
    MBAOptions Opts;
    Opts.MaxWork = 65536;
    const auto R = simplifyMBADeep(Ctx, Input, Opts);
    EXPECT_LT(R.SizeAfter, 64u);
    EXPECT_LT(R.Work, Opts.MaxWork);
    SymEvalPlan Plan(Ctx, R.Expr);
    for (unsigned J = 0; J < 32; ++J) {
      std::array<uint64_t, 6> XL, YL;
      std::generate(XL.begin(), XL.end(), [&] { return Random(); });
      std::generate(YL.begin(), YL.end(), [&] { return Random(); });
      const std::array<llvm::APInt, 2> Values{llvm::APInt(Width, XL),
                                              llvm::APInt(Width, YL)};
      auto Expected = Scale * Values[1];
      for (unsigned I = 0; I < Count; ++I) {
        auto V = I % 3 == 1 ? Values[1].extractBits(Bits, 7)
                            : Values[0].extractBits(Bits, 62);
        if (I % 3 == 0)
          V.flipAllBits();
        Expected *= Offset(I) + V.zext(Width) * Weight(I);
      }
      EXPECT_EQ(Plan.eval(Values), Expected);
    }
    if (Bits == 31 || Bits == 97) {
      Opts.MaxWork = 1;
      EXPECT_EQ(simplifyMBADeep(Ctx, Input, Opts).Expr, Input);
      Opts.MaxWork = 65536;
      Opts.MaxTableBytes = 1;
      EXPECT_EQ(simplifyMBADeep(Ctx, Input, Opts).Expr, Input);
    }
  }
}

TEST(SymMBAResiduePowers, LinearProductsPreserveAllLiveFieldBits) {
  checkLinearProducts(521, false, {1, 31, 64, 65, 97, 129});
}

TEST(SymMBAResiduePowers, RepeatedOffsetPowersKeepEvenCofactors) {
  checkLinearProducts(2111, true, {31, 65, 97});
}

TEST(SymMBAResiduePowers, MixedValuationsRetainTheCompleteLowWeightFactors) {
  constexpr unsigned Width = 257, Count = 521;
  std::mt19937_64 Random(0x54574f52494e4753);
  for (unsigned Bits : {7u, 32u, 65u, 97u}) {
    SCOPED_TRACE(Bits);
    const unsigned Shift = Bits + 1;
    const auto Scale = llvm::APInt(Width, 5).shl(Width - Bits - Shift);
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const auto Low = C.mkExtract(X, 19, Bits);
    const auto Upper = C.mkExtract(Y, Bits, Width - Bits);
    const auto Perturb = llvm::APInt::getSignMask(Bits);
    auto Offset = [&](unsigned I) {
      return llvm::APInt(Width, I == 41 && Bits != 65 ? 6 : 2 * (I % 7) + 1);
    };
    auto Weight = [&](unsigned I) {
      const bool Residual = I == 19 || I == 80 || I == 277;
      auto B = llvm::APInt(Width, 2 * (I % 5) + 1);
      return (I % 2 ? -B : B).shl(Residual ? 1 + I % 3 : Shift);
    };
    llvm::SmallVector<SymRef, 128> Factors{C.mkConst(Scale)};
    for (unsigned I = 0; I < Count; ++I) {
      const auto Field = I == 113 ? C.mkXor(Low, C.mkConst(Perturb)) : Low;
      const auto Word = I % 3 == 0   ? C.mkZExt(Field, Width)
                        : I % 3 == 1 ? C.mkSExt(Field, Width)
                                     : C.mkConcat(Upper, Field);
      Factors.push_back(
          C.mkAdd(C.mkConst(Offset(I)), C.mkMul(C.mkConst(Weight(I)), Word)));
    }
    const auto Input = C.mkMul(Factors);
    MBAOptions Options;
    Options.MaxWork = 65536;
    const auto R = simplifyMBADeep(C, Input, Options);
    EXPECT_LT(R.SizeAfter, 90u);
    EXPECT_LT(R.Work, Options.MaxWork);
    SymEvalPlan Plan(C, R.Expr);
    for (unsigned Trial = 0; Trial != 48; ++Trial) {
      std::array<llvm::APInt, 2> Values;
      for (auto &Value : Values) {
        std::array<uint64_t, 5> Limbs;
        std::generate(Limbs.begin(), Limbs.end(), [&] { return Random(); });
        Value = llvm::APInt(Width, Limbs);
      }
      if (Trial < 4) {
        Values[0] = Trial & 1 ? llvm::APInt::getAllOnes(Width)
                              : llvm::APInt::getZero(Width);
        Values[1] = Trial & 2 ? llvm::APInt::getAllOnes(Width)
                              : llvm::APInt::getZero(Width);
      }
      auto Expected = Scale;
      for (unsigned I = 0; I < Count; ++I) {
        auto Field = Values[0].extractBits(Bits, 19);
        if (I == 113)
          Field ^= Perturb;
        const auto Word =
            I % 3 == 0 ? Field.zext(Width)
            : I % 3 == 1
                ? Field.sext(Width)
                : Values[1].lshr(Bits).trunc(Width - Bits).concat(Field);
        Expected *= Offset(I) + Weight(I) * Word;
      }
      EXPECT_EQ(Plan.eval(Values), Expected) << Trial;
    }
    Options.MaxTableBytes = 1;
    EXPECT_EQ(simplifyMBADeep(C, Input, Options).Expr, Input);
  }
}

TEST(SymMBAResiduePowers, DifferentFieldWidthsKeepEveryObservableBit) {
  std::mt19937_64 Random(0x534c494345574944);
  struct ShapeCase {
    unsigned Width, Bits;
    bool Mixed, Terminal = false;
  };
  for (const auto [Width, Bits, Mixed, Terminal] :
       {ShapeCase{32, 8, false}, ShapeCase{32, 8, true},
        ShapeCase{129, 47, true}, ShapeCase{257, 67, true},
        ShapeCase{32, 8, true, true}, ShapeCase{129, 47, true, true},
        ShapeCase{257, 67, true, true}}) {
    SCOPED_TRACE(::testing::Message()
                 << Width << '/' << Mixed << '/' << Terminal);
    const unsigned Shift = Bits + 1, Count = Width == 32 ? 137 : 537;
    const auto Scale = llvm::APInt(Width, 3).shl(Width - Shift - Bits);
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    llvm::SmallVector<SymRef, 128> Factors{C.mkConst(Scale)};
    llvm::SmallVector<SymRef, 128> ExplicitFactors{C.mkConst(Scale)};
    auto Shape = [&](unsigned I) {
      const unsigned Extra = I % 5;
      const unsigned Size = Bits - Extra + I % 3;
      // One signed short field exposes its sign, another its concatenated
      // upper lane. A third window ends at the top of the source.
      return std::pair{I == 31 || I == 32 ? Size - 4 : Size,
                       Mixed && (I == 19 || I == 103) ? 2u : Shift + Extra};
    };
    auto Start = [&](unsigned I, unsigned Size) {
      return I == 127 || (Terminal && I % 4 != 0) ? Width - Size
                                                  : (I % 2 ? 0u : 3u);
    };
    auto Offset = [&](unsigned I) {
      return llvm::APInt(Width, I == 101 ? 6 : 1 + 2 * (I % 5));
    };
    auto Weight = [&](unsigned I, unsigned Zeros) {
      return llvm::APInt(Width, I % 2 ? -3 : 5, true).shl(Zeros);
    };
    for (unsigned I = 0; I < Count; ++I) {
      const auto [Size, Zeros] = Shape(I);
      const auto Source = I % 2 ? C.mkXor(X, Y) : X;
      const auto Field = C.mkExtract(Source, Start(I, Size), Size);
      const auto Extended =
          I % 3 == 0   ? C.mkZExt(Field, Width)
          : I % 3 == 1 ? C.mkSExt(Field, Width)
                       : C.mkConcat(C.mkExtract(Y, Size, Width - Size), Field);
      const auto A = C.mkConst(Offset(I)), B = C.mkConst(Weight(I, Zeros));
      auto Word = Extended;
      if (Terminal) {
        ExplicitFactors.push_back(C.mkAdd(A, C.mkMul(B, Extended)));
        if (Start(I, Size) == Width - Size && I % 3 != 2 && (I & 1)) {
          const auto Amount = C.mkConst(Width, Width - Size);
          Word = I % 3 ? C.mkAShr(Source, Amount) : C.mkLShr(Source, Amount);
        }
      }
      Factors.push_back(C.mkAdd(A, C.mkMul(B, Word)));
    }
    MBAOptions Options;
    Options.MaxWork = 65536;
    const auto Input = C.mkMul(Factors);
    const auto Result = simplifyMBADeep(C, Input, Options);
    EXPECT_LT(Result.Work, Options.MaxWork);
    if (Terminal) {
      // Ending slices of different sizes at the top creates distinct start
      // positions. A direct shift should be as compact as explicitly extending
      // each of those slices, and both forms should eliminate the long chain.
      const auto Explicit =
          simplifyMBADeep(C, C.mkMul(ExplicitFactors), Options);
      EXPECT_LT(Explicit.Work, Options.MaxWork);
      EXPECT_LE(Result.SizeAfter, Explicit.SizeAfter);
      EXPECT_LT(Result.SizeAfter * 8, Result.SizeBefore);
    } else {
      EXPECT_LT(Result.SizeAfter, 135u);
    }
    SymEvalPlan Original(C, Input), Plan(C, Result.Expr);
    for (unsigned Trial = 0; Trial < 48; ++Trial) {
      std::array<llvm::APInt, 2> V;
      for (auto &Value : V) {
        std::array<uint64_t, 5> Digits;
        std::generate(Digits.begin(), Digits.end(), [&] { return Random(); });
        Value = llvm::APInt(Width, Digits);
      }
      if (Trial < 4) {
        V[0] =
            Trial & 1 ? llvm::APInt::getAllOnes(Width) : llvm::APInt(Width, 0);
        V[1] =
            Trial & 2 ? llvm::APInt::getAllOnes(Width) : llvm::APInt(Width, 0);
      }
      auto Expected = Scale;
      for (unsigned I = 0; I < Count; ++I) {
        const auto [Size, Zeros] = Shape(I);
        const auto Field =
            (I % 2 ? V[0] ^ V[1] : V[0]).extractBits(Size, Start(I, Size));
        const auto Word =
            I % 3 == 0   ? Field.zext(Width)
            : I % 3 == 1 ? Field.sext(Width)
                         : V[1].extractBits(Width - Size, Size).concat(Field);
        Expected *= Offset(I) + Weight(I, Zeros) * Word;
      }
      EXPECT_EQ(Original.eval(V), Expected) << Trial;
      EXPECT_EQ(Plan.eval(V), Expected) << Trial;
    }
    Options.MaxTableBytes = 1;
    EXPECT_EQ(simplifyMBADeep(C, Input, Options).Expr, Input);
  }
}

TEST(SymMBAResiduePowers, HighestFieldBitMovesIntoTheProductOffset) {
  constexpr unsigned Width = 257, Count = 65;
  for (unsigned Bits : {7u, 64u, 65u}) {
    SCOPED_TRACE(Bits);
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const auto Field = C.mkExtract(X, 3, Bits);
    const auto Flipped =
        C.mkXor(Field, C.mkConst(llvm::APInt::getSignMask(Bits)));
    const unsigned Shift = Bits + 1, Precision = Shift + Bits;
    const auto Scale = llvm::APInt(Width, 3).shl(Width - Precision);
    const auto Kept =
        C.mkAdd(C.mkConst(Width, 5), C.mkMul(C.mkConst(Width, 6), Y));
    llvm::SmallVector<SymRef, 128> Factors{C.mkConst(Scale), Kept};
    // Each pair contributes 1 - weight * 2^(Bits-1); the two linear
    // dependences on X cancel at the observed precision.
    auto Reference = Scale;
    for (unsigned I = 0; I < Count; ++I) {
      const auto Weight = llvm::APInt(Width, 2 * I + 1).shl(Shift);
      const auto Left = C.mkSExt(Field, Width);
      const auto Right =
          C.mkConcat(C.mkExtract(Y, Bits, Width - Bits), Flipped);
      Factors.push_back(
          C.mkAdd(C.mkConst(Width, 1), C.mkMul(C.mkConst(Weight), Left)));
      Factors.push_back(
          C.mkAdd(C.mkConst(Width, 1), C.mkMul(C.mkConst(-Weight), Right)));
      Reference *= 1 - Weight.shl(Bits - 1);
    }
    MBAOptions Options;
    Options.MaxWork = 65536;
    const auto R = simplifyMBADeep(C, C.mkMul(Factors), Options);
    llvm::SmallVector<uint32_t, 4> Live;
    C.collectVars(R.Expr, Live);
    ASSERT_EQ(Live.size(), 1u);
    EXPECT_EQ(Live.front(), C.varId(Y));
    EXPECT_LT(R.Work, Options.MaxWork);
    SymEvalPlan Plan(C, R.Expr);
    for (uint64_t I = 0; I != 32; ++I) {
      const std::array Values{llvm::APInt(Width, I).shl(4),
                              llvm::APInt(Width, 7 * I)};
      EXPECT_EQ(Plan.eval(Values), Reference * (5 + 6 * Values[1]));
    }
  }
}

TEST(SymMBAResiduePowers, DominantPrefixesRetainDisturbedAndIndependentWords) {
  std::mt19937_64 Random(0x524553494455414c);
  for (unsigned Width : {8u, 32u, 65u, 257u})
    for (unsigned Observed : {4u, 6u, 8u})
      for (unsigned Count : {511u, 512u, 513u, 514u}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Observed << ':' << Count);
        SymContext Ctx;
        const auto F = fixture(Ctx, Width, Observed, Count, 1);
        const auto Z = Ctx.mkVar("z", Width);
        // This multiplier can be even or zero independently of the selected
        // X parity. It also moves the total count off a unit-period boundary.
        const auto Other = Ctx.mkAdd(Z, Ctx.mkConst(Width, 3));
        const auto Input = Ctx.mkMul(F.Input, Other);
        MBAOptions Options;
        Options.MaxWork = 65536;
        Options.VerifySamples = 0;
        const auto Result = simplifyMBADeep(Ctx, Input, Options);
        EXPECT_TRUE(Result.Changed) << "work=" << Result.Work;
        EXPECT_LT(Result.Work, Options.MaxWork);
        EXPECT_LT(Result.SizeAfter, Result.SizeBefore / 2);
        SymEvalPlan Reduced(Ctx, Result.Expr);
        for (unsigned Trial = 0; Trial < 256; ++Trial) {
          std::array<llvm::APInt, 3> Values;
          for (auto &Value : Values) {
            Value = llvm::APInt(Width, 0);
            for (unsigned Bit = 0; Bit < Width; Bit += 64)
              Value |= llvm::APInt(Width, Random()).shl(Bit);
          }
          // Exhaust every byte of X alongside independently varied complete
          // words, including both branches and residual zero/even values.
          Values[0].clearLowBits(8);
          Values[0] |= llvm::APInt(Width, Trial);
          if (Trial < 4)
            Values[2] = std::array{llvm::APInt(Width, 0), llvm::APInt(Width, 1),
                                   llvm::APInt(Width, 2),
                                   -llvm::APInt(Width, 3)}[Trial];
          EXPECT_EQ(Reduced.eval(Values),
                    F.oracle(Values) * (Values[2] + llvm::APInt(Width, 3)));
        }
      }
}

TEST(SymMBAResiduePowers, ConstantResiduesRemainProductFactors) {
  for (unsigned Width : {16u, 32u, 65u, 257u})
    for (unsigned Residue : {0u, 1u, 2u, 3u, 255u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Residue);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, 8, 512);
      const auto X = Ctx.mkVar("x", Width), Z = Ctx.mkVar("z", Width);
      const auto Carrier =
          Ctx.mkAdd(Ctx.mkConst(Width, Residue),
                    Ctx.mkMul(Ctx.mkConst(llvm::APInt(Width, 1).shl(8)), X));
      llvm::SmallVector<SymRef, 16> Factors(Ctx.operands(F.Input));
      for (auto &Factor : Factors)
        if (Factor == Z)
          Factor = Carrier;
      const auto Input = Ctx.mkMul(Factors);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, Input, Options);
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned I = 0; I < 256; ++I) {
        std::array Values{llvm::APInt(Width, I), llvm::APInt(Width, I * 17),
                          llvm::APInt(Width, 0)};
        Values[2] = llvm::APInt(Width, Residue) + Values[0].shl(8);
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

TEST(SymMBAResiduePowers, JointResidualZerosReduceGroupPrecision) {
  for (unsigned Width : {8u, 32u, 65u, 257u})
    for (unsigned Offset : {2u, 3u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Offset);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, 8, 511);
      const auto Input =
          Ctx.mkMul(F.Input, Ctx.mkAdd(Ctx.mkVar("z", Width),
                                       Ctx.mkConst(Width, Offset)));
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, Input, Options);
      // Neither residual is always even. Their product has a guaranteed zero
      // bit only for the odd offset; a nearby even offset is a negative
      // control.
      if (Offset == 3) {
        EXPECT_TRUE(Result.Changed);
        EXPECT_LT(Result.Work, Options.MaxWork);
        EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
      }
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned X = 0; X < 256; ++X)
        for (unsigned Z : {0u, 1u, 2u, 3u, 254u, 255u}) {
          const std::array Values{llvm::APInt(Width, X),
                                  llvm::APInt(Width, X * 17),
                                  llvm::APInt(Width, Z)};
          EXPECT_EQ(Reduced.eval(Values),
                    F.oracle(Values) *
                        (Values[2] + llvm::APInt(Width, Offset)));
        }
    }
}

TEST(SymMBAResiduePowers, ProjectedDomainsKeepEveryVisibleInputBit) {
  for (unsigned Width : {16u, 32u, 65u, 257u})
    for (unsigned Shift : {7u, 8u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Shift);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, 8, 513);
      const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      const auto U = Ctx.mkVar("u", Width), V = Ctx.mkVar("v", Width);
      const auto First = Ctx.mkXor(X, Ctx.mkShl(Y, Ctx.mkConst(Width, 3)));
      const auto High = Ctx.mkConst(llvm::APInt(Width, 1).shl(Shift));
      const auto Changed =
          Ctx.mkXor(Ctx.mkAdd(First, Ctx.mkMul(High, U)), Ctx.mkMul(High, V));
      llvm::SmallVector<SymRef, 16> Factors(Ctx.operands(F.Input));
      const auto It = std::find(Factors.begin(), Factors.end(), First);
      ASSERT_NE(It, Factors.end());
      *It = Changed;
      const auto Input = Ctx.mkMul(Factors);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, Input, Options);
      if (Shift == 8) {
        EXPECT_TRUE(Result.Changed);
        EXPECT_LT(Result.Work, Options.MaxWork);
        EXPECT_LT(Result.SizeAfter, F.Count);
      }
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned Trial = 0; Trial < 256; ++Trial) {
        const std::array Values{
            llvm::APInt(Width, Trial), llvm::APInt(Width, Trial * 17),
            llvm::APInt(Width, Trial / 4), llvm::APInt(Width, Trial * 53),
            llvm::APInt(Width, Trial / 8)};
        auto Expected = llvm::APInt(Width, 3).shl(Width - 8) * Values[2];
        for (unsigned I = 0; I < F.Count; ++I) {
          auto Factor =
              (Values[0] + llvm::APInt(Width, I).shl(3)) ^ Values[1].shl(3);
          if (I == 0)
            Factor = (Factor + Values[3].shl(Shift)) ^ Values[4].shl(Shift);
          Expected *= Factor;
        }
        EXPECT_EQ(Reduced.eval(Values), Expected);
      }
    }
}

TEST(SymMBAResiduePowers, ExactAndShortResourcesKeepCompleteProofs) {
  for (unsigned Count : {64u, 512u}) {
    SCOPED_TRACE(Count);
    SymContext FullCtx;
    const auto F = fixture(FullCtx, 64, 8, Count);
    detail::WorkBudget FullBudget(65536);
    size_t FullStorage = 1 << 24;
    const auto Full =
        detail::foldResidueDecisions(FullCtx, F.Input, FullBudget, FullStorage);
    ASSERT_NE(Full, F.Input);
    const auto Work = FullBudget.used();
    const size_t Bytes = (1 << 24) - FullStorage;
    ASSERT_GT(Work, 1u);
    ASSERT_GT(Bytes, 1u);
    for (auto Limits : {std::pair<size_t, size_t>{0, Bytes},
                        {1, Bytes},
                        {Work, 0},
                        {Work - 1, Bytes},
                        {Work, Bytes - 1},
                        {Work, Bytes}}) {
      SymContext Ctx;
      const auto Input = fixture(Ctx, 64, 8, Count);
      detail::WorkBudget Budget(Limits.first);
      size_t Storage = Limits.second;
      const auto Result =
          detail::foldResidueDecisions(Ctx, Input.Input, Budget, Storage);
      EXPECT_LE(Budget.used(), Limits.first);
      EXPECT_LE(Storage, Limits.second);
      if (Limits.first == Work && Limits.second == Bytes)
        EXPECT_EQ(Result.index(), Full.index());
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned I = 0; I < 256; ++I) {
        const std::array Values{llvm::APInt(64, I), llvm::APInt(64, I * 17),
                                llvm::APInt(64, I * 51)};
        EXPECT_EQ(Reduced.eval(Values), Input.oracle(Values));
      }
    }
  }
}

TEST(SymMBAResiduePowers, ParitySubgroupsKeepIndependentMultipliers) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext ExactCtx;
    const auto ExactInput = fixture(ExactCtx, Width, 6, 64);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 24;
    const auto Exact = detail::foldResidueDecisions(ExactCtx, ExactInput.Input,
                                                    Budget, Storage, true);
    ASSERT_NE(Exact, ExactInput.Input);
    SymContext Ctx;
    const auto F = fixture(Ctx, Width, 6, 64);
    MBAOptions Options;
    Options.MaxWork = 65536;
    Options.VerifySamples = 0;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    ASSERT_TRUE(Result.Changed);
    EXPECT_LT(Result.Work, Options.MaxWork);
    EXPECT_LT(Result.SizeAfter, F.Count);
    SymEvalPlan Reduced(Ctx, Result.Expr);
    for (unsigned I = 0; I < 256; ++I) {
      const std::array Values{llvm::APInt(Width, I), llvm::APInt(Width, I * 17),
                              llvm::APInt(Width, I / 4)};
      EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
    }
  }
}

TEST(SymMBAResiduePowers, RetainedGroupsRespectExactAndShortResources) {
  auto MakeInput = [](SymContext &Ctx) {
    const auto F = fixture(Ctx, 65, 6, 513, 1);
    return Ctx.mkMul(F.Input,
                     Ctx.mkAdd(Ctx.mkVar("z", 65), Ctx.mkConst(65, 3)));
  };
  SymContext FullCtx;
  const auto Input = MakeInput(FullCtx);
  detail::WorkBudget FullBudget(65536);
  size_t FullStorage = 1 << 24;
  const auto Full = detail::foldResidueDecisions(FullCtx, Input, FullBudget,
                                                 FullStorage, true);
  ASSERT_NE(Full, Input);
  const size_t Work = FullBudget.used(), Bytes = (1 << 24) - FullStorage;
  ASSERT_GT(Work, 1u);
  ASSERT_GT(Bytes, 1u);
  for (auto Limits : {std::pair<size_t, size_t>{0, Bytes},
                      {1, Bytes},
                      {Work, 0},
                      {Work - 1, Bytes},
                      {Work, Bytes - 1},
                      {Work, Bytes}}) {
    SymContext Ctx;
    const auto Original = MakeInput(Ctx);
    const auto F = fixture(Ctx, 65, 6, 513, 1);
    detail::WorkBudget Budget(Limits.first);
    size_t Storage = Limits.second;
    const auto Result =
        detail::foldResidueDecisions(Ctx, Original, Budget, Storage, true);
    EXPECT_LE(Budget.used(), Limits.first);
    EXPECT_LE(Storage, Limits.second);
    if (Limits.first == Work && Limits.second == Bytes)
      EXPECT_EQ(Result.index(), Full.index());
    SymEvalPlan Reduced(Ctx, Result);
    for (unsigned I = 0; I < 256; ++I) {
      const std::array Values{llvm::APInt(65, I), llvm::APInt(65, I * 17),
                              llvm::APInt(65, I * 51)};
      EXPECT_EQ(Reduced.eval(Values),
                F.oracle(Values) * (Values[2] + llvm::APInt(65, 3)));
    }
  }
}

TEST(SymMBAResiduePowers, ResidualGroupsHaveIndependentSolverProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Count : {512u, 513u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Count);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, 4, Count, 1);
      const auto Input = Ctx.mkMul(
          F.Input, Ctx.mkAdd(Ctx.mkVar("z", Width), Ctx.mkConst(Width, 3)));
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, Input, Options);
      ASSERT_TRUE(Result.Changed);
      EXPECT_LT(Result.Work, Options.MaxWork);
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 10000);
      EXPECT_EQ(Verifier(Ctx, Input, Result.Expr),
                SynthVerification::Equivalent);
    }
}

TEST(SymMBAResiduePowers, NativeWidthsHaveIndependentSolverProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Count : {2u, 7u, 16u, 17u, 512u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Count);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, Count < 8 ? 8 : 4, Count);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
      // A complete period must exercise an actual rewrite in each native
      // width; unchanged neighbor controls alone would make this proof vacuous.
      if (Count == 16 || Count == 512)
        ASSERT_NE(Result, F.Input);
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 10000);
      EXPECT_EQ(Verifier(Ctx, F.Input, Result), SynthVerification::Equivalent);
    }
}

TEST(SymMBAResiduePowers, ParityGroupsRetainMultipleIndependentWords) {
  std::mt19937_64 Random(0x5041524954595245);
  for (unsigned Width : {8u, 32u, 65u, 257u})
    for (unsigned Count : {16u, 17u, 32u, 33u, 64u, 65u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Count);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, 4, Count, 1);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
      ASSERT_TRUE(Result.Changed);
      EXPECT_LT(Result.Work, Options.MaxWork);
      EXPECT_LT(Result.SizeAfter, Result.SizeBefore / 2);
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned Trial = 0; Trial < 256; ++Trial) {
        std::array<llvm::APInt, 3> Values;
        for (auto &Value : Values) {
          Value = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random()).shl(Bit);
        }
        Values[0].clearLowBits(8);
        Values[0] |= llvm::APInt(Width, Trial);
        if (Trial < 4)
          Values[2] = llvm::APInt(Width, Trial);
        ASSERT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

TEST(SymMBAResiduePowers, ParityMajorityDoesNotReplaceTheZeroBranchProof) {
  for (unsigned Count : {3u, 4u, 5u, 8u})
    for (unsigned Perturb : {0u, 1u, 2u}) {
      SCOPED_TRACE(::testing::Message() << Count << ':' << Perturb);
      SymContext Ctx;
      const auto F = fixture(Ctx, 8, 4, Count, Perturb);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage, true);
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned X = 0; X < 256; ++X)
        for (unsigned Y = 0; Y < 16; ++Y)
          for (unsigned Z : {0u, 1u, 2u, 255u}) {
            const std::array Values{llvm::APInt(8, X), llvm::APInt(8, Y),
                                    llvm::APInt(8, Z)};
            ASSERT_EQ(Reduced.eval(Values), F.oracle(Values));
          }
    }
}

TEST(SymMBAResiduePowers, SampledProductAndRewrittenTailKeepAllAddends) {
  constexpr unsigned Width = 257, Observed = 135;
  SymContext C;
  auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
  auto U = C.mkVar("u", Width), V = C.mkVar("v", Width);
  auto Two = C.mkConst(Width, 2);
  auto Sum = [&](SymRef A, SymRef B) {
    return C.mkAdd(C.mkXor(A, B), C.mkMul(Two, C.mkAnd(A, B)));
  };
  auto Product = [&](SymRef Body, unsigned Offset) {
    const auto Factor = C.mkAdd(C.mkConst(Width, Offset), Body);
    llvm::SmallVector<SymRef, 40> Factors(33, Factor);
    Factors.push_back(C.mkConst(llvm::APInt(Width, 3).shl(Width - Observed)));
    return C.mkMul(Factors);
  };
  const auto Original = C.mkAdd(Product(Sum(X, Y), 7), Sum(U, V));
  const auto P = Product(C.mkAdd(X, Y), 7), Tail = C.mkAdd(U, V);
  const auto Result = C.mkAdd(P, Tail);
  ASSERT_NE(Original, Result);
  EXPECT_TRUE(detail::agreeOnSamples(C, Original, Result, 12));
  EXPECT_FALSE(detail::agreeOnSamples(
      C, Original, C.mkAdd(Result, C.mkConst(Width, 1)), 12));
  EXPECT_FALSE(detail::agreeOnSamples(
      C, Original, C.mkAdd(Product(C.mkAdd(X, Y), 9), Tail), 12));
}

TEST(SymMBAResiduePowers, RepeatedFieldsKeepTheirLastSquareAndOddCopies) {
  for (unsigned Precision : {4u, 5u, 12u, 13u, 130u, 131u}) {
    const unsigned Bits = Precision / 2, Shift = Precision - Bits;
    const unsigned K = Shift - 1, Width = Precision + 7;
    for (unsigned Copies : {2u, 3u, 4u, 6u}) {
      SCOPED_TRACE(::testing::Message() << Precision << ':' << Copies);
      SymContext C;
      auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
      auto A = C.mkSExt(C.mkExtract(X, 1, Bits), Width);
      auto B = C.mkZExt(C.mkExtract(Y, 0, Bits), Width);
      auto Z = C.mkZExt(C.mkExtract(X, 0, Bits), Width);
      auto Weight = [&](int N, unsigned Zeros) {
        return C.mkConst(llvm::APInt(Width, N, true).shl(Zeros));
      };
      auto Affine = [&](int Offset, unsigned Zeros) {
        llvm::SmallVector<SymRef, 4> Terms{Weight(Offset, 0),
                                           C.mkMul(Weight(-3, Zeros), A),
                                           C.mkMul(Weight(5, K), B)};
        if (Copies == 6)
          Terms.push_back(C.mkMul(Weight(7, K), Z));
        return C.mkAdd(Terms);
      };
      auto F = Affine(-3, K), G = Affine(2, K);
      auto Neighbor = Affine(7, K - 1);
      auto Outer = Weight(3, Width - Precision);
      llvm::SmallVector<SymRef, 16> Factors{Outer, G, G, Neighbor};
      Factors.append(Copies, F);
      auto Input = C.mkMul(Factors);
      auto Fold = [&](size_t Work, size_t Bytes) {
        detail::WorkBudget Budget(Work);
        return detail::foldDistributedProduct(C, Input, Budget, Bytes, nullptr);
      };
      EXPECT_EQ(Fold(1, 1 << 20), Input);
      EXPECT_EQ(Fold(65536, 1), Input);
      auto Reduced = Fold(65536, 1 << 20);
      ASSERT_NE(Reduced, Input);
      SymEvalPlan Plan(C, Reduced);
      for (unsigned I = 0; I < 16; ++I)
        for (unsigned J = 0; J < 16; ++J) {
          std::array Values{llvm::APInt::getAllOnes(Width) - 2 * I,
                            llvm::APInt(Width, J)};
          if (I & 1)
            Values[1].setBit(Bits - 1);
          if (J & 1)
            Values[0].clearBit(Bits);
          const auto U = Values[0].extractBits(Bits, 1).sext(Width);
          const auto V = Values[1].trunc(Bits).zext(Width);
          auto Linear = 5 * V;
          if (Copies == 6)
            Linear += 7 * Values[0].trunc(Bits).zext(Width);
          const auto H = (Linear - 3 * U).shl(K);
          auto Expected = llvm::APInt(Width, 3).shl(Width - Precision) *
                          (2 + H) * (2 + H) *
                          (7 + Linear.shl(K) - (3 * U).shl(K - 1));
          for (unsigned N = 0; N < Copies; ++N)
            Expected *= H - 3;
          ASSERT_EQ(Plan.eval(Values), Expected);
        }
    }
  }
}

TEST(SymMBAResiduePowers, RepeatedPowersRetainQuadraticsAndResidualCopies) {
  for (const auto [Precision, D] : {std::pair{8u, 2u},
                                    {9u, 3u},
                                    {16u, 3u},
                                    {25u, 4u},
                                    {38u, 7u},
                                    {130u, 4u},
                                    {131u, 8u}}) {
    const unsigned Bits = Precision / 2, Shift = Precision - Bits;
    const unsigned Width = Precision + 5, K = Shift - D, Group = 1u << D;
    for (const unsigned Extra : {0u, 1u, Group - 1}) {
      SCOPED_TRACE(::testing::Message() << Precision << ':' << Extra);
      SymContext C;
      const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
      const auto A = C.mkSExt(C.mkExtract(X, 1, Bits), Width);
      const auto B = C.mkZExt(C.mkExtract(Y, 0, Bits), Width);
      auto literal = [&](int N, unsigned S = 0) {
        return C.mkConst(llvm::APInt(Width, N, true).shl(S));
      };
      auto affine = [&](int Offset, int AX, int BY) {
        return C.mkAdd({literal(Offset), C.mkMul(literal(AX, K), A),
                        C.mkMul(literal(BY, K), B)});
      };
      const int OtherOffset = Extra == 1 ? 2 : 5;
      const auto F = affine(-3, -5, 3), G = affine(OtherOffset, 7, 2);
      const auto Scale = literal(3, Width - Precision);
      llvm::SmallVector<SymRef, 64> Factors{Scale};
      Factors.append(Group + Extra, F);
      Factors.append(3 * Group, G);
      const auto Original = C.mkMul(Factors);
      size_t Bytes = 1 << 20;
      detail::WorkBudget Budget(65536);
      const auto Result =
          detail::foldDistributedProduct(C, Original, Budget, Bytes, nullptr);
      ASSERT_NE(Result, Original);
      SymEvalPlan Eval(C, Result);
      for (unsigned I = 0; I < 16; ++I)
        for (unsigned J = 0; J < 16; ++J) {
          std::array Values{llvm::APInt(Width, I), llvm::APInt(Width, J)};
          if (J & 1)
            Values[0].setBit(Bits);
          const auto U = Values[0].extractBits(Bits, 1).sext(Width);
          const auto V = Values[1].trunc(Bits).zext(Width);
          const auto First = (3 * V - 5 * U).shl(K) - 3;
          const auto Second = (7 * U + 2 * V).shl(K) + OtherOffset;
          auto Expected = llvm::APInt(Width, 3).shl(Width - Precision);
          for (unsigned N = 0; N < Group + Extra; ++N)
            Expected *= First;
          for (unsigned N = 0; N < 3 * Group; ++N)
            Expected *= Second;
          ASSERT_EQ(Eval.eval(Values), Expected);
        }
    }
  }
}

TEST(SymMBAResiduePowers, SparseFieldRunsStayFactoredWithinBudget) {
  constexpr unsigned Width = 257, Precision = 40, Shift = 15, Rows = 65;
  std::mt19937_64 Random(0x526f77506f776572ULL);
  for (bool EvenSingleton : {false, true}) {
    SCOPED_TRACE(EvenSingleton);
    SymContext C;
    const auto Z = C.mkVar("z", Width);
    llvm::SmallVector<SymRef, 16> Factors{
        C.mkConst(llvm::APInt(Width, 3).shl(Width - Precision))};
    for (unsigned I = 0; I < Rows; ++I) {
      const auto A = C.mkZExt(C.mkExtract(Z, 3 * I, 3), Width);
      const auto Field = C.mkExtract(Z, 3 * ((I + 1) % Rows), 3);
      const auto B = I % 2 ? C.mkSExt(Field, Width) : C.mkZExt(Field, Width);
      const bool Single = EvenSingleton && I == 0;
      const auto F =
          C.mkAdd({C.mkConst(Width, Single ? 2u : 2 * I + 1),
                   C.mkMul(C.mkConst(Width, (1 + 2 * (I % 3)) << Shift), A),
                   C.mkMul(C.mkConst(Width, (3 + I % 2) << Shift), B)});
      Factors.append(Single ? 1u : 8u + (I + 1 == Rows), F);
    }
    const auto Input = C.mkMul(Factors);
    MBAOptions Options;
    Options.MaxWork = 65536;
    const auto R = simplifyMBADeep(C, Input, Options);
    ASSERT_LT(R.Work, Options.MaxWork);
    ASSERT_LT(R.SizeAfter, R.SizeBefore / 2);
    SymEvalPlan Original(C, Input), Reduced(C, R.Expr);
    for (unsigned Trial = 0; Trial < 128; ++Trial) {
      std::array<uint64_t, 5> Words{};
      for (auto &Word : Words)
        Word = Trial == 0 ? 0 : Trial == 1 ? UINT64_MAX : Random();
      const std::array Values{llvm::APInt(Width, Words)};
      ASSERT_EQ(Reduced.eval(Values), Original.eval(Values)) << Trial;
    }
  }
}

TEST(SymMBAResiduePowers, RepeatedPowersKeepLiveFourthDegrees) {
  // With five observed bits (c + 2*h)^4 still contains 16*h^4.
  constexpr unsigned Width = 16;
  SymContext C;
  const auto X = C.mkZExt(C.mkVar("x", 4), Width);
  const auto Y = C.mkSExt(C.mkVar("y", 4), Width);
  const auto F = C.mkAdd({C.mkConst(Width, 3), C.mkMul(C.mkConst(Width, 6), X),
                          C.mkMul(C.mkConst(Width, 10), Y)});
  const auto Original = C.mkMul({C.mkConst(Width, 1u << 11), F, F, F, F});
  size_t Bytes = 1 << 20;
  detail::WorkBudget Budget(65536);
  const auto Folded =
      detail::foldDistributedProduct(C, Original, Budget, Bytes, nullptr);
  SymEvalPlan Plan(C, Folded);
  for (unsigned A = 0; A != 16; ++A)
    for (unsigned B = 0; B != 16; ++B) {
      std::array Values{llvm::APInt(4, A), llvm::APInt(4, B)};
      const auto V = 3 + 6 * Values[0].zext(Width) + 10 * Values[1].sext(Width);
      EXPECT_EQ(Plan.eval(Values),
                llvm::APInt(Width, 1u << 11) * V * V * V * V);
    }
}

TEST(SymMBAResiduePowers, SmallOddPowerBlocksKeepTheirFullResidues) {
  constexpr unsigned Width = 13;
  SymContext C;
  const auto X = C.mkVar("x", 4), Y = C.mkVar("y", 4);
  const auto A = C.mkZExt(X, Width), B = C.mkSExt(Y, Width);
  auto literal = [&](int V) { return C.mkConst(llvm::APInt(Width, V, true)); };
  const auto F =
      C.mkAdd({literal(-3), C.mkMul(literal(6), A), C.mkMul(literal(-2), B)});
  const auto G =
      C.mkAdd({literal(5), C.mkMul(literal(-10), A), C.mkMul(literal(14), B)});
  llvm::SmallVector<SymRef, 64> Factors{literal(8)};
  Factors.append(16, F);
  Factors.append(30, G);
  const auto Original = C.mkMul(Factors);
  size_t Bytes = 1 << 20;
  detail::WorkBudget Budget(65536);
  const auto Result =
      detail::foldDistributedProduct(C, Original, Budget, Bytes, nullptr);
  ASSERT_NE(Result, Original);
  SymEvalPlan Plan(C, Result);
  for (unsigned I = 0; I < 16; ++I)
    for (unsigned J = 0; J < 16; ++J) {
      std::array Values{llvm::APInt(4, I), llvm::APInt(4, J)};
      const auto U = Values[0].zext(Width), V = Values[1].sext(Width);
      auto Expected = llvm::APInt(Width, 8);
      for (unsigned K = 0; K < 16; ++K)
        Expected *= 6 * U - 2 * V - 3;
      for (unsigned K = 0; K < 30; ++K)
        Expected *= 5 - 10 * U + 14 * V;
      ASSERT_EQ(Plan.eval(Values), Expected);
    }
}

TEST(SymMBAResiduePowers, PackedProductsCompeteWithExpandedNativeArithmetic) {
  constexpr unsigned Width = 16;
  SymContext C;
  const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
  const auto A = C.mkZExt(C.mkExtract(X, 1, 4), Width);
  const auto B = C.mkZExt(C.mkExtract(Y, 1, 2), Width);
  const auto D = C.mkSExt(C.mkExtract(Y, 1, 2), Width);
  const auto E = C.mkSExt(C.mkExtract(X, 1, 3), Width);
  const auto H = C.mkZExt(C.mkExtract(Y, 0, 2), Width);
  auto literal = [&](int V) { return C.mkConst(llvm::APInt(Width, V, true)); };
  auto scale = [&](int V, SymRef R) { return C.mkMul(literal(V), R); };
  llvm::SmallVector<SymRef, 80> Factors{literal(-384)};
  Factors.append(16, C.mkAdd({literal(7), scale(4, A), scale(-4, B)}));
  Factors.append(
      26, C.mkAdd({literal(5), scale(28, D), scale(-12, A), scale(12, E)}));
  Factors.append(24, C.mkAdd({literal(3), scale(4, E), scale(12, H)}));
  const auto Original = C.mkMul(Factors);
  MBAOptions Options;
  Options.MaxWork = 65536;
  const auto Result = simplifyMBADeep(C, Original, Options);
  EXPECT_LT(Result.Work, Options.MaxWork);
  // The packed form costs 149. The fully expanded arithmetic reading costs 100.
  EXPECT_LE(C.readabilityCost(Result.Expr), 100u);
  SymEvalPlan Before(C, Original), After(C, Result.Expr);
  for (unsigned XV = 0; XV < 32; ++XV)
    for (unsigned YV = 0; YV < 8; ++YV) {
      std::array Values{llvm::APInt(Width, XV), llvm::APInt(Width, YV)};
      ASSERT_EQ(After.eval(Values), Before.eval(Values));
    }
}

TEST(SymMBAResiduePowers, DistributedFieldsCountEachScalarFactorOnce) {
  for (const unsigned Bits : {4u, 17u, 65u}) {
    const unsigned Width = 3 * Bits + 2, Shift = Bits + 1;
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto LowX = Ctx.mkExtract(X, 2, Bits);
    const auto LowY = Ctx.mkExtract(Y, 1, Bits);
    const auto SX = Ctx.mkSExt(LowX, Width);
    const auto CY = Ctx.mkConcat(Ctx.mkExtract(X, 0, Width - Bits), LowY);
    const auto Scale = llvm::APInt(Width, 3).shl(Width - Shift - Bits);
    llvm::SmallVector<SymRef, 128> Factors{Ctx.mkConst(Scale)};
    constexpr unsigned Degree = 113;
    for (unsigned I = 0; I < Degree; ++I) {
      const unsigned Offset = I == 29 ? 6 : 1 + 2 * (I % 5);
      const unsigned K = I % 4 + 1, J = 3 + I % 7;
      // One lower-weight term makes the whole factor a residual; its other
      // field must remain inside it too. The near miss is not linear.
      const auto A = llvm::APInt(Width, K).shl(I == 37 ? 1 : Shift);
      const auto B = -llvm::APInt(Width, J).shl(Shift);
      Factors.push_back(
          Ctx.mkAdd({Ctx.mkConst(Width, Offset), Ctx.mkMul(Ctx.mkConst(A), SX),
                     Ctx.mkMul(Ctx.mkConst(B), CY)}));
    }
    const auto Input = Ctx.mkMul(Factors);
    size_t Storage = 1u << 20;
    detail::WorkBudget Tiny(8);
    EXPECT_EQ(detail::foldProductResidues(Ctx, Input, Tiny, Storage), Input);
    EXPECT_LE(Tiny.used(), 8u);

    detail::WorkBudget Budget(65536);
    Storage = 1u << 20;
    const auto Reduced =
        detail::foldProductResidues(Ctx, Input, Budget, Storage);
    ASSERT_NE(Reduced, Input);
    EXPECT_LT(Ctx.readabilityCost(Reduced), Ctx.readabilityCost(Input) / 4);
    if (Bits == 4 && solver::proofBackendAvailable(solver::ProofBackend::Z3)) {
      solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 10000);
      EXPECT_EQ(Verify(Ctx, Input, Reduced), SynthVerification::Equivalent);
    }
    SymEvalPlan Plan(Ctx, Reduced);
    std::mt19937_64 Random(0x668d5);
    for (unsigned Trial = 0; Trial < 256; ++Trial) {
      auto Draw = [&] {
        llvm::SmallVector<uint64_t, 4> Words(llvm::APInt::getNumWords(Width));
        for (auto &Word : Words)
          Word = Random();
        return llvm::APInt(Width, Words);
      };
      const std::array Values{Draw(), Draw()};
      const auto A = Values[0].extractBits(Bits, 2).sext(Width);
      const auto B =
          Values[0].trunc(Width - Bits).concat(Values[1].extractBits(Bits, 1));
      auto Expected = Scale;
      for (unsigned I = 0; I < Degree; ++I)
        Expected *= (I == 29 ? 6 : 1 + 2 * (I % 5)) +
                    (A * (I % 4 + 1)).shl(I == 37 ? 1 : Shift) -
                    (B * (3 + I % 7)).shl(Shift);
      EXPECT_EQ(Plan.eval(Values), Expected)
          << Bits << " bits, trial " << Trial;
    }
  }
}

TEST(SymMBAResiduePowers, UniformAndPartialMasksRespectTheObservedRing) {
  for (unsigned Bits : {4u, 65u}) {
    const unsigned Shift = Bits + 1, Width = 3 * Bits + 4;
    const auto Ones = llvm::APInt::getLowBitsSet(Width, Bits).shl(Shift);
    const auto Sparse = llvm::APInt::getOneBitSet(Width, Shift + Bits / 2);
    const auto Scale = llvm::APInt(Width, 5).shl(Width - Shift - Bits);
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const auto SX = C.mkSExt(C.mkExtract(X, 0, Bits), Width);
    const auto ZY = C.mkZExt(C.mkExtract(Y, 0, Bits), Width);
    auto offset = [&](unsigned I) {
      const auto Mask = I == 13       ? Sparse
                        : (I / 3) % 2 ? Ones
                                      : llvm::APInt(Width, 0);
      return (llvm::APInt::getOneBitSet(Width, Width - 2) | Mask) +
             (I == 17 ? 6u : 1u + 2 * (I % 5));
    };
    auto toggleMask = [&](unsigned I) {
      if (I == 19)
        return Sparse;
      return (I / 3) % 2 ? Ones : llvm::APInt::getOneBitSet(Width, Width - 3);
    };
    llvm::SmallVector<SymRef, 128> Factors{C.mkConst(Scale)};
    for (unsigned I = 0; I < 113; ++I) {
      const auto A =
          llvm::APInt(Width, 2 * (I % 4) + 1).shl(I == 31 ? 1 : Shift);
      const auto B = -llvm::APInt(Width, 2 * (I % 3) + 3).shl(Shift);
      const auto D =
          C.mkAdd(C.mkMul(C.mkConst(A), SX), C.mkMul(C.mkConst(B), ZY));
      const auto K = C.mkConst(offset(I));
      if (I % 4 == 3)
        Factors.push_back(C.mkXor(K, C.mkAnd(C.mkConst(toggleMask(I)), D)));
      else
        Factors.push_back(I % 3 == 0   ? C.mkAnd(K, C.mkNot(D))
                          : I % 3 == 1 ? C.mkXor(K, D)
                                       : C.mkOr(K, D));
    }
    const auto Product = C.mkMul(Factors);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1u << 20;
    const auto Reduced =
        detail::foldProductResidues(C, Product, Budget, Storage);
    ASSERT_LT(C.readabilityCost(Reduced), C.readabilityCost(Product) / 4);
    EXPECT_LT(Budget.used(), 65536u);
    SymEvalPlan Plan(C, Reduced);
    std::mt19937_64 Random(0x1d93c2);
    for (unsigned Trial = 0; Trial < 256; ++Trial) {
      std::array<llvm::APInt, 2> Values;
      for (auto &V : Values) {
        llvm::SmallVector<uint64_t, 4> Words(llvm::APInt::getNumWords(Width));
        for (auto &Word : Words)
          Word = Random();
        V = llvm::APInt(Width, Words);
      }
      if (Bits == 4) {
        Values[0] = Trial / 16;
        Values[1] = Trial % 16;
      }
      const auto Left = Values[0].trunc(Bits).sext(Width);
      const auto Right = Values[1].trunc(Bits).zext(Width);
      auto Expected = Scale;
      for (unsigned I = 0; I < 113; ++I) {
        const auto D = (Left * (2 * (I % 4) + 1)).shl(I == 31 ? 1 : Shift) -
                       (Right * (2 * (I % 3) + 3)).shl(Shift);
        const auto K = offset(I);
        if (I % 4 == 3)
          Expected *= K ^ (toggleMask(I) & D);
        else
          Expected *= I % 3 == 0 ? K & ~D : I % 3 == 1 ? K ^ D : K | D;
      }
      EXPECT_EQ(Plan.eval(Values), Expected)
          << Bits << " bits, trial " << Trial;
    }
  }
}

TEST(SymMBAResiduePowers, BitwiseOffsetsPreserveLiveOverlapAndLowerCarries) {
  constexpr unsigned Width = 21, Bits = 4, Shift = 5;
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  const auto SX = Ctx.mkSExt(Ctx.mkExtract(X, 1, Bits), Width);
  const auto ZY = Ctx.mkZExt(Ctx.mkExtract(Y, 1, Bits), Width);
  const auto Scale = llvm::APInt(Width, 3).shl(Width - Bits - Shift);
  llvm::SmallVector<SymRef, 128> Factors{Ctx.mkConst(Scale)};
  for (unsigned I = 0; I < 117; ++I) {
    const auto A =
        Ctx.mkMul(Ctx.mkConst(Width, 3u << (I == 29 ? 1 : Shift)), SX);
    const auto B = Ctx.mkMul(Ctx.mkConst(Width, 5u << Shift), ZY);
    const unsigned Offset =
        (I == 17 ? 6 : 3) | (1u << 18) | (I % 13 == 0 ? 1u << Shift : 0);
    auto C = Ctx.mkConst(Width, Offset), Sum = Ctx.mkAdd(A, B);
    Factors.push_back(I % 2 ? Ctx.mkOr(C, Sum) : Ctx.mkXor(C, Sum));
  }
  const auto Root = Ctx.mkAdd(Ctx.mkMul(Factors), X);
  size_t Storage = 1u << 20;
  detail::WorkBudget Budget(65536);
  const auto Reduced = detail::foldProductResidues(Ctx, Root, Budget, Storage);
  ASSERT_LT(Ctx.readabilityCost(Reduced), Ctx.readabilityCost(Root) / 3);
  EXPECT_LT(Budget.used(), 65536u);
  SymEvalPlan Plan(Ctx, Reduced);
  for (unsigned Xv = 0; Xv < 32; ++Xv) {
    for (unsigned Yv = 0; Yv < 32; ++Yv) {
      const std::array Inputs{llvm::APInt(Width, Xv), llvm::APInt(Width, Yv)};
      const auto A = Inputs[0].extractBits(Bits, 1).sext(Width);
      const auto B = Inputs[1].extractBits(Bits, 1).zext(Width);
      auto Expected = Scale;
      for (unsigned I = 0; I < 117; ++I) {
        const auto D = (A * 3).shl(I == 29 ? 1 : Shift) + (B * 5).shl(Shift);
        const llvm::APInt C(Width, (I == 17 ? 6u : 3u) | (1u << 18) |
                                       (I % 13 == 0 ? 1u << Shift : 0));
        Expected *= I % 2 ? C | D : C ^ D;
      }
      EXPECT_EQ(Plan.eval(Inputs), Expected + Inputs[0]);
    }
  }
}

enum class FieldMasks { Input, ScaledInput, ScaledResult, CarryWindow };

void checkMaskedWeightedDeltas(FieldMasks Mode) {
  const bool Scaled = Mode != FieldMasks::Input;
  const bool MaskResult =
      Mode == FieldMasks::ScaledResult || Mode == FieldMasks::CarryWindow;
  const bool CarryWindow = Mode == FieldMasks::CarryWindow;
  auto trailing = [](unsigned I) { return 1 + I % 3; };
  for (unsigned Bits : {4u, 65u}) {
    const unsigned Shift = Bits + 1, Width = 3 * Bits + 7;
    const auto Full = llvm::APInt::getLowBitsSet(Width, Bits).shl(Shift);
    const auto Outside = llvm::APInt::getOneBitSet(Width, Width - 2);
    const auto Scale = llvm::APInt(Width, 3).shl(Width - Shift - Bits);
    auto mask = [&](unsigned I) {
      if (CarryWindow) {
        const auto Top =
            llvm::APInt::getOneBitSet(Width, Shift + Bits - trailing(I));
        auto Selected = I % 7 ? Full ^ Top : Top;
        if (I == 15)
          Selected ^= llvm::APInt::getOneBitSet(Width, Shift);
        return Selected | Outside | 3;
      }
      if (MaskResult && I % 8 == 2)
        return llvm::APInt::getAllOnes(Width);
      if (I >= 10 && I < 15)
        return Outside | llvm::APInt::getOneBitSet(Width, Shift + Bits / 2);
      return (I % 5 ? Full | Outside : Outside) | 3;
    };
    auto resultMask = [&](unsigned I) {
      if (CarryWindow) {
        const auto Low =
            llvm::APInt::getOneBitSet(Width, Shift + trailing(I) - 1);
        auto Selected = I % 11 ? Full ^ Low : Low;
        if (I == 15)
          Selected ^= llvm::APInt::getOneBitSet(Width, Shift + trailing(I));
        return Selected | Outside | 5;
      }
      if (I >= 9 && I <= 16)
        return (Full ^ llvm::APInt::getOneBitSet(Width, Shift + Bits / 2)) |
               Outside;
      return (I % 7 ? Full | Outside : Outside) | 5;
    };
    auto resultScale = [&](unsigned I) {
      if (CarryWindow && I % 13 == 0)
        return llvm::APInt::getOneBitSet(Width, Bits);
      return I % 6 == 0 ? llvm::APInt::getOneBitSet(Width, Bits - 1) + 5
                        : llvm::APInt(Width, I % 9 == 0 ? -3 : 1, true);
    };
    auto fieldScale = [&](unsigned I) {
      if (CarryWindow)
        return llvm::APInt(Width, I % 2 ? -5 : 3, true).shl(trailing(I));
      if (!Scaled)
        return llvm::APInt(Width, I == 38 ? 3 : 1);
      if (I % 4 == 0)
        return llvm::APInt::getOneBitSet(Width, Bits - 1) + 3;
      return llvm::APInt(Width, I % 4 == 1 ? -5 : I % 4 == 2 ? 6 : 9, true);
    };
    auto offset = [&](unsigned I) {
      return (I % 3 ? Full : Outside) + (I == 23 ? 6 : 1 + 2 * (I % 7));
    };
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const auto SX = C.mkSExt(C.mkExtract(X, 0, Bits), Width);
    const auto ZY = C.mkZExt(C.mkExtract(Y, 0, Bits), Width);
    llvm::SmallVector<SymRef, 128> Factors{C.mkConst(Scale)};
    for (unsigned I = 0; I < 97; ++I) {
      const auto A =
          llvm::APInt(Width, 1 + 2 * (I % 4)).shl(I == 29 ? 1 : Shift);
      const auto B = -llvm::APInt(Width, 3 + 2 * (I % 3)).shl(Shift);
      const auto Sum =
          C.mkAdd(C.mkMul(C.mkConst(A), SX), C.mkMul(C.mkConst(B), ZY));
      auto D = C.mkAnd(C.mkConst(mask(I)), Sum);
      D = C.mkMul(Scaled && I == 43 ? X : C.mkConst(fieldScale(I)), D);
      if (MaskResult)
        D = C.mkMul(C.mkConst(resultScale(I)),
                    C.mkAnd(C.mkConst(resultMask(I)), D));
      const auto K = C.mkConst(offset(I));
      switch (I % 5) {
      case 0:
        Factors.push_back(C.mkOr(K, D));
        break;
      case 1:
        Factors.push_back(C.mkXor(K, D));
        break;
      case 2:
        Factors.push_back(C.mkAnd(K, C.mkNot(D)));
        break;
      case 3:
        Factors.push_back(C.mkAdd(K, D));
        break;
      default:
        Factors.push_back(C.mkSub(K, D));
        break;
      }
    }
    const auto Product = C.mkMul(Factors);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1u << 20;
    const auto Reduced =
        detail::foldProductResidues(C, Product, Budget, Storage);
    ASSERT_LT(C.readabilityCost(Reduced), C.readabilityCost(Product) / 4);
    SymEvalPlan Plan(C, Reduced);
    std::mt19937_64 Random(0xfc3029);
    for (unsigned I = 0; I < 256; ++I) {
      std::array<llvm::APInt, 2> Values;
      for (auto &V : Values) {
        llvm::SmallVector<uint64_t, 4> Data(llvm::APInt::getNumWords(Width));
        for (auto &Word : Data)
          Word = Random();
        V = llvm::APInt(Width, Data);
      }
      if (Bits == 4) {
        Values[0] = I / 16;
        Values[1] = I % 16;
      }
      const auto L = Values[0].trunc(Bits).sext(Width);
      const auto R = Values[1].trunc(Bits).zext(Width);
      auto Want = Scale;
      for (unsigned J = 0; J < 97; ++J) {
        auto D = mask(J) & ((L * (1 + 2 * (J % 4))).shl(J == 29 ? 1 : Shift) -
                            (R * (3 + 2 * (J % 3))).shl(Shift));
        D *= Scaled && J == 43 ? Values[0] : fieldScale(J);
        if (MaskResult)
          D = resultScale(J) * (D & resultMask(J));
        const auto K = offset(J);
        switch (J % 5) {
        case 0:
          Want *= K | D;
          break;
        case 1:
          Want *= K ^ D;
          break;
        case 2:
          Want *= K & ~D;
          break;
        case 3:
          Want *= K + D;
          break;
        default:
          Want *= K - D;
          break;
        }
      }
      EXPECT_EQ(Plan.eval(Values), Want) << Bits << " bits, trial " << I;
    }
  }
}

TEST(SymMBAResiduePowers, MasksOnWeightedDeltasPreserveCarriesAndHoles) {
  checkMaskedWeightedDeltas(FieldMasks::Input);
}

TEST(SymMBAResiduePowers, ScaledMaskedDeltasKeepCarriesAndVariableMultipliers) {
  checkMaskedWeightedDeltas(FieldMasks::ScaledInput);
}

TEST(SymMBAResiduePowers, MasksBeforeAndAfterScalingKeepTheirCarryOrder) {
  checkMaskedWeightedDeltas(FieldMasks::ScaledResult);
}

TEST(SymMBAResiduePowers, EvenScalesLimitWhichMaskHolesCanAffectCarries) {
  checkMaskedWeightedDeltas(FieldMasks::CarryWindow);
}

TEST(SymMBAResiduePowers,
     SparseMasksObserveShiftsAndCarriesAtProductPrecision) {
  for (unsigned Bits : {8u, 65u}) {
    const unsigned Precision = 2 * Bits, Width = 3 * Bits + 5;
    const auto Scale = llvm::APInt::getOneBitSet(Width, Width - Precision);
    const auto Visible = llvm::APInt::getLowBitsSet(Width, Bits - 1);
    const auto Noise = llvm::APInt::getOneBitSet(Width, Precision + 2);
    llvm::APInt Stripe(Width, 0);
    for (unsigned I = 0; I < Bits - 1; I += 2)
      Stripe.setBit(I);
    for (bool KeepCarry : {false, true}) {
      SymContext C;
      const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
      const auto SX = C.mkSExt(C.mkExtract(X, 0, Bits), Width);
      const auto ZY = C.mkZExt(C.mkExtract(Y, 0, Bits), Width);
      llvm::SmallVector<SymRef, 32> Factors{C.mkConst(Scale)};
      auto Constant = Scale;
      for (unsigned I = 0; I < 31; ++I) {
        const auto W = llvm::APInt(Width, 2 * I + 1).shl(Bits);
        const auto Sum =
            C.mkAdd(C.mkMul(C.mkConst(W), SX), C.mkMul(C.mkConst(-W), ZY));
        auto A = Stripe, B = (Visible ^ Stripe).shl(1);
        auto K =
            llvm::APInt(Width, 2) - llvm::APInt::getOneBitSet(Width, Bits + 1);
        if (I % 3 == 1) {
          K = 6;
          A = Visible & ~llvm::APInt(Width, 7);
          B = 15;
        }
        if (KeepCarry && !I) {
          // An odd part of three carries a retained bit into a disjoint hole.
          K = 6;
          A = 1;
          B = 4;
        }
        const auto Input = C.mkAnd(C.mkConst(A.shl(Bits) | Noise | 1), Sum);
        const auto Delta = C.mkAnd(C.mkConst(B.shl(Bits) | Noise | 3),
                                   C.mkMul(C.mkConst(K), Input));
        auto Offset = llvm::APInt(Width, 2 * I + 1);
        if (I % 3 == 2) {
          Offset |= B.shl(Bits);
          Factors.push_back(C.mkOr(C.mkConst(Offset), Delta));
        } else {
          Factors.push_back(C.mkAdd(C.mkConst(Offset), Delta));
        }
        Constant *= Offset;
      }
      detail::WorkBudget Budget(65536);
      size_t Storage = 1u << 20;
      const auto Root = C.mkMul(Factors);
      const auto Reduced =
          detail::foldProductResidues(C, Root, Budget, Storage);
      if (!KeepCarry) {
        EXPECT_EQ(Reduced, C.mkConst(Constant));
      } else {
        EXPECT_FALSE(C.isConst(Reduced));
        SymEvalPlan Original(C, Root), Candidate(C, Reduced);
        for (unsigned I = 0; I < 16; ++I) {
          const std::array<llvm::APInt, 2> Input = {llvm::APInt(Width, I),
                                                    llvm::APInt(Width, I / 3)};
          EXPECT_EQ(Candidate.eval(Input), Original.eval(Input));
        }
      }
    }
  }
}

TEST(SymMBAResiduePowers, RecoversAnEmbeddedProductScaleAtTheOriginalWidth) {
  for (unsigned Width : {32u, 129u}) {
    SymContext C;
    const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width),
               Z = C.mkVar("z", Width);
    const auto A = C.mkZExt(C.mkExtract(X, 3, 8), Width),
               B = C.mkSExt(C.mkExtract(Y, 7, 8), Width);
    auto literal = [&](uint64_t K) { return C.mkConst(Width, K); };
    const auto Weight =
        C.mkAdd(C.mkMul(literal(256), A), C.mkMul(literal(768), B));
    const auto P = C.mkAdd(literal(5), Weight), Q = C.mkOr(literal(7), Weight);
    const auto Scale = llvm::APInt::getOneBitSet(Width, Width - 16);
    for (bool OddCoefficient : {false, true}) {
      // Moving the common power outside exposes the product precision.
      // The lower-weight factor still multiplies the entire projected result.
      const auto Embedded =
          C.mkAdd(C.mkConst(Scale * 17), C.mkMul(C.mkConst(-(Scale * 3)), X));
      llvm::SmallVector<SymRef, 16> Factors{
          Embedded, C.mkAdd(literal(3), C.mkMul(literal(12), Y))};
      if (OddCoefficient)
        Factors.push_back(literal(11));
      for (unsigned I = 0; I != 521; ++I)
        Factors.push_back(I % 2 ? P : Q);
      const auto Root = C.mkAdd(Z, C.mkMul(Factors));
      auto fold = [&](SymRef E) {
        detail::WorkBudget Budget(65536);
        size_t Storage = 1u << 20;
        return detail::foldProductResidues(C, E, Budget, Storage);
      };
      const auto Small = fold(Root);
      ASSERT_NE(Small, Root);
      // A single low offset bit removes the hidden scale.
      Factors[0] = C.mkAdd(Embedded, literal(1));
      const auto Neighbor = C.mkAdd(Z, C.mkMul(Factors)),
                 NeighborResult = fold(Neighbor);
      SymEvalPlan Original(C, Root), Result(C, Small), Other(C, Neighbor),
          OtherResult(C, NeighborResult);
      bool ChangedValue = false;
      for (unsigned I = 0; I != 32; ++I) {
        const std::array<llvm::APInt, 3> Input = {
            llvm::APInt(Width, 0x75937a29U * I),
            llvm::APInt(Width, 0x8d112f1dU * (I + 1)),
            llvm::APInt(Width, 0xf042531bU - I)};
        const auto Expected = Original.eval(Input), Close = Other.eval(Input);
        EXPECT_EQ(Result.eval(Input), Expected);
        EXPECT_EQ(OtherResult.eval(Input), Close);
        ChangedValue |= Expected != Close;
      }
      EXPECT_TRUE(ChangedValue);
    }
  }
}

TEST(SymMBAResiduePowers, HighestBitWeightsShareOneParityProduct) {
  constexpr unsigned Width = 32, Bits = 5, Shift = 5;
  const auto HighWeight = llvm::APInt(Width, 1u << (Bits + Shift - 1));
  const auto Outer = llvm::APInt(Width, 1u << (Width - Bits - Shift));
  for (unsigned Bias : {0u, 7u, 16u, 31u}) {
    SymContext C;
    llvm::SmallVector<SymRef, 4> Fields, Carriers;
    for (auto Name : {"a", "b", "c", "d"}) {
      auto Field = C.mkExtract(C.mkVar(Name, Width), 0, Bits);
      Fields.push_back(Field);
      Carriers.push_back(
          C.mkMul(C.mkConst(HighWeight), C.mkZExt(Field, Width)));
    }
    llvm::SmallVector<SymRef, 64> Factors{C.mkConst(Outer)};
    for (unsigned I = 0; I < 34; ++I) {
      const unsigned A = I < 32 ? I % 4 : 2 * (I - 32);
      const auto Offset = C.mkConst(Width, 1u + (I == 0 ? Bias << Shift : 0u));
      Factors.push_back(C.mkAdd({Offset, Carriers[A], Carriers[(A + 1) % 4]}));
    }
    const auto Root = C.mkMul(Factors);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1u << 20;
    const auto Reduced = detail::foldProductResidues(C, Root, Budget, Storage);
    const auto Parity = C.mkXor(Fields);
    const auto K = C.mkConst(Bits, 1u << (Bits - 1));
    const auto Upper = C.mkAdd(C.mkConst(Bits, Bias), C.mkMul(K, Parity));
    const auto Reference =
        C.mkConcat(Upper, C.mkConst(Width - Bits, Outer.getZExtValue()));
    EXPECT_LE(C.readabilityCost(Reduced), C.readabilityCost(Reference));
    SymEvalPlan Plan(C, Reduced);
    for (unsigned Mask = 0; Mask < 16; ++Mask) {
      std::array<llvm::APInt, 4> Values;
      unsigned Odd = 0;
      for (unsigned I = 0; I < Values.size(); ++I) {
        Values[I] = llvm::APInt(Width, 0x7ffffff0u + ((Mask >> I) & 1));
        Odd ^= (Mask >> I) & 1;
      }
      const auto Want =
          Outer * (1u + (Bias << Shift) + Odd * HighWeight.getZExtValue());
      EXPECT_EQ(Plan.eval(Values), Want);
    }
  }
}

} // namespace
