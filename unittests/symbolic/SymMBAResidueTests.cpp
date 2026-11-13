//===- SymMBAResidueTests.cpp - Modular product residue proofs ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check modular zero-product proofs against independent arithmetic and SMT.
///
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/symbolic/SymParse.h"

#include <algorithm>
#include <array>
#include <bit>
#include <random>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

SymRef product(SymContext &Ctx, SymRef Base, unsigned Count,
               const llvm::APInt &Scale, unsigned Start = 0, unsigned Step = 1,
               bool Disturb = false) {
  llvm::SmallVector<SymRef, 64> Factors{Ctx.mkConst(Scale)};
  for (unsigned I = 0; I < Count; ++I)
    Factors.push_back(Ctx.mkAdd(
        Base,
        Ctx.mkConst(Scale.getBitWidth(),
                    Start + Step * (Disturb && I + 1 == Count ? I - 1 : I))));
  return Ctx.mkMul(Factors);
}

llvm::APInt oracle(const llvm::APInt &Base, unsigned Count,
                   const llvm::APInt &Scale, unsigned Start = 0,
                   unsigned Step = 1, bool Disturb = false) {
  auto Result = Scale;
  for (unsigned I = 0; I < Count; ++I)
    Result *= Base + Start + Step * (Disturb && I + 1 == Count ? I - 1 : I);
  return Result;
}

TEST(SymMBAResidue, RecoversLongProductsAndOpaqueOrExpandedAdditiveBases) {
  std::mt19937_64 Random(0x52455349445545);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u}) {
    for (unsigned Count : {4u, 8u, 16u, 32u, 64u, 128u, 256u}) {
      for (unsigned Kind = 0; Kind < 3; ++Kind) {
        SymContext Ctx;
        auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
        auto Z = Ctx.mkVar("z", Width);
        auto Base = Kind == 0 ? X : Ctx.mkAdd(X, Y);
        if (Kind == 1)
          Base = Ctx.mkXor(Base, Ctx.mkMul(Y, Z));
        const unsigned Valuation = Count - std::popcount(Count);
        auto Scale = llvm::APInt(Width, 1).shl(
            Width > Valuation ? Width - Valuation : 0);
        auto Input = Ctx.mkAdd(
            product(Ctx, Base, Count, Scale, 11, 5),
            Ctx.mkAdd(Ctx.mkXor(X, Y),
                      Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkAnd(X, Y))));
        MBAOptions Opts;
        Opts.MaxWork = Count <= 64 ? 16384 : 65536;
        Opts.VerifySamples = 0;
        auto Result = simplifyMBADeep(Ctx, Input, Opts);
        ASSERT_EQ(Result.Expr, Ctx.mkAdd(X, Y))
            << Width << ": " << Count << ": " << Kind << ": "
            << Ctx.toString(Result.Expr);
        EXPECT_LT(Result.Work, Opts.MaxWork);
        EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
        for (unsigned Trial = 0; Trial < 32; ++Trial) {
          std::array<llvm::APInt, 3> Values;
          for (auto &Value : Values) {
            Value = llvm::APInt(Width, Random());
            for (unsigned Bit = 64; Bit < Width; Bit += 64)
              Value |= llvm::APInt(Width, Random()).shl(Bit);
          }
          auto Word = Kind == 0 ? Values[0] : Values[0] + Values[1];
          if (Kind == 1)
            Word ^= Values[1] * Values[2];
          auto Want = oracle(Word, Count, Scale, 11, 5) + Values[0] + Values[1];
          EXPECT_EQ(Want, Values[0] + Values[1]);
          EXPECT_EQ(Ctx.eval(Input, Values), Want);
          EXPECT_EQ(Ctx.eval(Result.Expr, Values), Want);
        }
      }
    }
  }
}

TEST(SymMBAResidue, ExhaustsFourFactorMultisetsAndAllSmallWordValues) {
  unsigned Proven = 0;
  for (unsigned A = 0; A < 8; ++A)
    for (unsigned B = A; B < 8; ++B)
      for (unsigned C = B; C < 8; ++C)
        for (unsigned D = C; D < 8; ++D)
          for (unsigned Scale : {1u, 2u, 4u, 8u}) {
            SymContext Ctx;
            auto X = Ctx.mkVar("x", 4);
            std::array<unsigned, 4> Offsets{A, B, C, D};
            llvm::SmallVector<SymRef, 8> Factors{Ctx.mkConst(4, Scale)};
            for (auto Offset : Offsets)
              Factors.push_back(Ctx.mkAdd(X, Ctx.mkConst(4, Offset)));
            auto Input = Ctx.mkMul(Factors);
            detail::WorkBudget Budget(65536);
            size_t Storage = 1 << 20;
            auto Output =
                detail::foldProductResidues(Ctx, Input, Budget, Storage);
            bool IsZero = true;
            for (uint64_t V = 0; V < 16; ++V) {
              uint64_t Want = Scale;
              for (auto Offset : Offsets)
                Want = (Want * (V + Offset)) & 15;
              IsZero &= Want == 0;
              EXPECT_EQ(Ctx.evalU64(Output, {V}), Want);
            }
            if (Ctx.isConstZero(Output)) {
              EXPECT_TRUE(IsZero);
              ++Proven;
            }
            if (IsZero && (B != 0 || (C != 0 && D != 0)))
              EXPECT_TRUE(Ctx.isConstZero(Output)) << A << B << C << D << Scale;
          }
  EXPECT_GT(Proven, 100u);
}

TEST(SymMBAResidue,
     RetainsMissingFactorsChangedOffsetsAndNeighborCoefficients) {
  for (unsigned Width : {8u, 32u, 64u, 257u}) {
    for (unsigned Kind = 0; Kind < 3; ++Kind) {
      SymContext Ctx;
      auto X = Ctx.mkVar("x", Width);
      auto Scale = llvm::APInt(Width, 1).shl(Width - 7);
      if (Kind == 2)
        ++Scale;
      auto Input = product(Ctx, X, Kind == 0 ? 7 : 8, Scale, 0, 1, Kind == 1);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 20;
      auto Output = detail::foldProductResidues(Ctx, Input, Budget, Storage);
      EXPECT_EQ(Output, Input);
      const auto Witness = oracle(llvm::APInt(Width, 1), Kind == 0 ? 7 : 8,
                                  Scale, 0, 1, Kind == 1);
      EXPECT_FALSE(Witness.isZero());
      EXPECT_EQ(Ctx.eval(Output, {llvm::APInt(Width, 1)}), Witness);
    }
  }
}

TEST(SymMBAResidue, CombinesBoundsWithoutIdentifyingDifferentBases) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SymContext Ctx;
    auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    auto A = Ctx.mkAdd(X, Y), B = Ctx.mkAdd(X, Ctx.mkNeg(Y));
    auto Scale = llvm::APInt(Width, 1).shl(Width - 6);
    auto Input = Ctx.mkMul(product(Ctx, A, 4, Scale),
                           product(Ctx, B, 4, llvm::APInt(Width, 1)));
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 20;
    EXPECT_TRUE(Ctx.isConstZero(
        detail::foldProductResidues(Ctx, Input, Budget, Storage)));
    for (unsigned I = 0; I < 256; ++I) {
      auto VX = llvm::APInt(Width, I), VY = llvm::APInt(Width, 73 * I + 11);
      EXPECT_TRUE((oracle(VX + VY, 4, Scale) *
                   oracle(VX - VY, 4, llvm::APInt(Width, 1)))
                      .isZero());
      EXPECT_TRUE(Ctx.eval(Input, {VX, VY}).isZero());
    }
    // Identifying x with y would invent a complete four-offset family. At
    // x=1,y=0 all four factors are odd, so this neighboring product is nonzero.
    auto Nonzero = Ctx.mkMul({Ctx.mkConst(llvm::APInt(Width, 1).shl(Width - 3)),
                              X, Ctx.mkAdd(X, Ctx.mkConst(Width, 2)),
                              Ctx.mkAdd(Y, Ctx.mkConst(Width, 1)),
                              Ctx.mkAdd(Y, Ctx.mkConst(Width, 3))});
    detail::WorkBudget OtherBudget(65536);
    Storage = 1 << 20;
    EXPECT_EQ(detail::foldProductResidues(Ctx, Nonzero, OtherBudget, Storage),
              Nonzero);
    EXPECT_FALSE(
        Ctx.eval(Nonzero, {llvm::APInt(Width, 1), llvm::APInt(Width, 0)})
            .isZero());
  }
}

TEST(SymMBAResidue, RequiresTheCompleteProofWorkAndStorage) {
  SymContext Ctx;
  auto X = Ctx.mkVar("x", 257);
  auto Input = product(Ctx, X, 16, llvm::APInt(257, 1).shl(242));
  detail::WorkBudget Full(65536);
  size_t Storage = 1 << 20;
  ASSERT_TRUE(
      Ctx.isConstZero(detail::foldProductResidues(Ctx, Input, Full, Storage)));
  ASSERT_GT(Full.used(), 1u);
  for (size_t Work : {size_t(0), size_t(1), Full.used() - 1}) {
    detail::WorkBudget Short(Work);
    size_t Bytes = 1 << 20;
    EXPECT_EQ(detail::foldProductResidues(Ctx, Input, Short, Bytes), Input);
    EXPECT_LE(Short.used(), Work);
  }
  for (size_t Bytes : {size_t(0), size_t(1)}) {
    detail::WorkBudget Plenty(65536);
    EXPECT_EQ(detail::foldProductResidues(Ctx, Input, Plenty, Bytes), Input);
  }
  detail::WorkBudget Exact(Full.used());
  Storage = 1 << 20;
  EXPECT_TRUE(
      Ctx.isConstZero(detail::foldProductResidues(Ctx, Input, Exact, Storage)));
  EXPECT_EQ(Exact.used(), Full.used());
}

TEST(SymMBAResidue, PreservesUnrelatedSummandsAndPartialBudgetSemantics) {
  for (size_t Work : {0u, 1u, 128u, 1024u, 16384u}) {
    SymContext Ctx;
    auto X = Ctx.mkVar("x", 8), Y = Ctx.mkVar("y", 8);
    auto Zero = product(Ctx, X, 8, llvm::APInt(8, 2));
    auto Nonzero = product(Ctx, Y, 7, llvm::APInt(8, 2));
    auto Input = Ctx.mkAdd({Zero, Nonzero, Ctx.mkXor(X, Y)});
    MBAOptions Opts;
    Opts.MaxWork = Work;
    Opts.VerifySamples = 0;
    auto Result = simplifyMBADeep(Ctx, Input, Opts);
    EXPECT_LE(Result.Work, Work);
    SymEvalPlan Output(Ctx, Result.Expr);
    for (uint64_t VX = 0; VX < 256; ++VX)
      for (uint64_t VY = 0; VY < 256; ++VY) {
        uint64_t Want = 2;
        for (unsigned I = 0; I < 7; ++I)
          Want = (Want * (VY + I)) & 255;
        Want = (Want + (VX ^ VY)) & 255;
        EXPECT_EQ(Output.evalU64({VX, VY}), Want);
      }
  }
}

TEST(SymMBAResidue, ReducesProductsOfHighWordPerturbations) {
  std::mt19937_64 Random(288);
  for (unsigned Width : {8u, 31u, 64u, 65u, 129u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const unsigned Count = Width == 129 ? 331 : 17;
    const unsigned Observed = Width - Width / 4;
    const auto Scale = llvm::APInt(Width, 3).shl(Width - Observed);
    const auto High =
        llvm::APInt::getHighBitsSet(Width, Width - (Observed + 1) / 2);
    const auto Shared = Ctx.mkXor(X, Y);
    llvm::SmallVector<SymRef, 32> Factors{Ctx.mkConst(Scale)};
    for (unsigned I = 0; I < Count; ++I) {
      const auto D =
          I % 2 ? Ctx.mkAnd(Y, Ctx.mkConst(High))
                : Ctx.mkMul(Shared, Ctx.mkConst(llvm::APInt(Width, 2 * I + 1)
                                                    .shl((Observed + 1) / 2)));
      Factors.push_back(
          Ctx.mkAdd(Ctx.mkConst(Width, I == 5 ? 2 : 2 * I + 1), D));
    }
    auto Input = Ctx.mkMul(Factors);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 20;
    const auto Output =
        detail::foldProductResidues(Ctx, Input, Budget, Storage);
    ASSERT_LT(Ctx.readabilityCost(Output), Ctx.readabilityCost(Input));
    EXPECT_LT(Budget.used(), 65536u);
    SymEvalPlan Plan(Ctx, Output);
    for (unsigned Trial = 0; Trial < 48; ++Trial) {
      llvm::APInt VX(Width, Random(), false, true),
          VY(Width, Random(), false, true);
      if (Width > 64) {
        VX.setBit(Width - 2);
        if (Trial & 1)
          VY.setBit(Width - 1);
      }
      auto Expected = Scale;
      for (unsigned I = 0; I < Count; ++I) {
        const auto D = I % 2
                           ? (VY & High)
                           : ((VX ^ VY) * (2 * I + 1)).shl((Observed + 1) / 2);
        Expected *= D + (I == 5 ? 2 : 2 * I + 1);
      }
      EXPECT_EQ(Plan.eval({VX, VY}), Expected);
    }
    for (auto [Work, Bytes] :
         {std::pair<size_t, size_t>{1, 1 << 20}, {65536, 1}}) {
      detail::WorkBudget Short(Work);
      EXPECT_EQ(detail::foldProductResidues(Ctx, Input, Short, Bytes), Input);
      EXPECT_LE(Short.used(), Work);
    }
  }
}

TEST(SymMBAResidue, KeepsObservableProductsOfTwoPerturbations) {
  SymContext Ctx;
  constexpr unsigned Width = 5;
  const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  auto Offset = [&](unsigned C, unsigned K, SymRef V) {
    return Ctx.mkAdd(Ctx.mkConst(Width, C),
                     Ctx.mkMul(Ctx.mkConst(Width, K), V));
  };
  for (const unsigned Weight : {4u, 8u}) {
    auto Input = Ctx.mkMul({Offset(1, 4, X), Offset(3, Weight, Y),
                            Offset(5, 16, X), Offset(7, 16, Y)});
    MBAOptions Opt;
    Opt.VerifySamples = 0;
    Opt.MaxWork = 65536;
    auto R = simplifyMBADeep(Ctx, Input, Opt);
    SymEvalPlan Plan(Ctx, R.Expr);
    for (unsigned A = 0; A < 32; ++A)
      for (unsigned B = 0; B < 32; ++B)
        EXPECT_EQ(Plan.evalU64({A, B}), ((1 + 4 * A) * (3 + Weight * B) *
                                         (5 + 16 * A) * (7 + 16 * B)) &
                                            31);
  }
}

TEST(SymMBAResidue, RetainsCompoundSummandsAroundAnExpandedMaskedProduct) {
  constexpr unsigned Width = 127;
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  const auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
  const std::array Words{Ctx.mkOr(X, Y), Ctx.mkXor(Y, W),
                         Ctx.mkAdd(Z, Ctx.mkAnd(X, W)),
                         Ctx.mkMul(Ctx.mkXor(W, X), Ctx.mkOr(Y, Z))};
  llvm::SmallVector<llvm::APInt, 32> Masks;
  llvm::SmallVector<SymRef, 32> Factors{Ctx.mkConst(Width, 48)};
  std::mt19937_64 Random(291);
  for (unsigned I = 0; I < 21; ++I) {
    Masks.push_back(llvm::APInt(Width, Random() | 1).shl(62 + I % 2));
    const auto D = Ctx.mkAnd(Ctx.mkConst(Masks.back()), Words[I % 4]);
    Factors.push_back(Ctx.mkAdd(Ctx.mkConst(Width, 2 * I + 1), D));
  }
  const auto Extra = Ctx.mkXor(Ctx.mkMul(X, Y), Ctx.mkMul(W, Z));
  const auto Input = Ctx.mkAdd(
      {Ctx.mkMul(Factors), Extra, Ctx.mkOr(X, Y), Ctx.mkAnd(X, Y),
       Ctx.mkXor(Z, W), Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkAnd(Z, W))});
  MBAOptions Opt;
  Opt.MaxWork = 65536;
  auto R = simplifyMBADeep(Ctx, Input, Opt);
  ASSERT_LT(R.SizeAfter, R.SizeBefore);
  EXPECT_LT(R.Work, Opt.MaxWork / 2);
  SymEvalPlan Plan(Ctx, R.Expr);
  auto RandomWord = [&]() {
    const auto High = llvm::APInt(Width, Random()).shl(64);
    return High | llvm::APInt(Width, Random());
  };
  for (unsigned I = 0; I < 32; ++I) {
    const std::array V{RandomWord(), RandomWord(), RandomWord(), RandomWord()};
    const std::array Word{V[0] | V[1], V[1] ^ V[3], V[2] + (V[0] & V[3]),
                          (V[3] ^ V[0]) * (V[1] | V[2])};
    llvm::APInt Expected(Width, 48);
    for (unsigned J = 0; J < Masks.size(); ++J)
      Expected *= (Word[J % 4] & Masks[J]) + (2 * J + 1);
    Expected += (V[0] * V[1]) ^ (V[3] * V[2]);
    EXPECT_EQ(Plan.eval(V), Expected + V[0] + V[1] + V[2] + V[3]);
  }
}

TEST(SymMBAResidue, PreservesCompactReadingsOfTheLastTwoBits) {
  struct Example {
    unsigned Width, Cost;
    const char *Source;
  };
  const Example Cases[] = {
      {32, 16,
       "(768*(8+(819200)*(y|z))*(14+(507904)*((x&z)*(y|w)))*(6+(188416)*(x^y))"
       "*(12+(950272)*(x+(w^y)))*(1+(278528)*(x+(w^y)))*(5+(57344)*((x&z)*(y|w"
       ")))*(9+(90112)*(x^y))*(1+(-16384)*(z&w))*(3+(81920)*(x+(w^y)))*(1+(-50"
       "7904)*(~(w|x)))*(4+(-622592)*(~(w|x)))*(2+(311296)*((x&z)*(y|w)))*(3+("
       "1146880)*(x^y))*(10+(-409600)*(z&w))*(11+(106496)*(y|z))*(13+(-53248)*"
       "(z&w))*(7+(-114688)*(~(w|x))))+(x^y)+2*(x&y)"},
      {9, 17,
       "(48*(147+(-136*(z+(x&w))))*(173+(80*((w^x)*(y|z))))*"
       "(159+((y^w)&144))*(96+((z+(x&w))&504))*(43+((x|y)&488))*"
       "(71+((y^w)&112))*(53+(56*(x|y))))+(x|y)+(x&y)+(z^w)+2*(z&w)"},
      {8, 17,
       "(6*(11+(208)*(x^y))*(3+(80)*(z&w))*(7+(-112)*"
       "((x&z)*(y|w)))*(2+(272)*(z&w))*(5+(112)*(x+(w^y)))*"
       "(2+(-16)*(y|z))*(9+(176)*(~(w|x)))*(13+(-208)*"
       "(y|z))*(2+(304)*(x+(w^y))))+(x^y)+2*(x&y)"},
      {16, 36,
       "(48*(2+(4864)*(x+(w^y)))*(2+(-256)*(y|z))*"
       "(5+(1792)*(x+(w^y)))*(13+(-3328)*(y|z))*(11+(3328)*"
       "(x^y))*(2+(2176)*(z&w))*(3+(320)*(z&w))*(9+(2816)*"
       "(~(w|x)))*(7+(-896)*((x&z)*(y|w))))+(x^y)+2*(x&y)"}};
  for (const auto &Example : Cases) {
    SCOPED_TRACE(Example.Width);
    SymContext Ctx;
    const auto Input = parseSymExpr(Ctx, Example.Source, Example.Width);
    ASSERT_TRUE(Input);
    MBAOptions Opt;
    if (Example.Width == 32) {
      // The full top-column reading also fits a short interactive budget.
      SymContext Limited;
      const auto Parsed = parseSymExpr(Limited, Example.Source, Example.Width);
      ASSERT_TRUE(Parsed);
      Opt.MaxWork = 256;
      const auto Compact = simplifyMBADeep(Limited, Parsed.Root, Opt);
      EXPECT_LE(Compact.SizeAfter, Example.Cost);
      if (solver::proofBackendAvailable(solver::ProofBackend::Z3)) {
        solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 5000);
        EXPECT_EQ(Verify(Limited, Parsed.Root, Compact.Expr),
                  SynthVerification::Equivalent);
      }
    }
    Opt.MaxWork = 65536;
    const auto R = simplifyMBADeep(Ctx, Input.Root, Opt);
    EXPECT_LE(R.SizeAfter, Example.Cost);
    EXPECT_LT(R.Work, 8192u);
    std::mt19937_64 Random(19);
    SymEvalPlan Before(Ctx, Input.Root), After(Ctx, R.Expr);
    for (unsigned I = 0; I < 128; ++I) {
      const std::array<uint64_t, 4> Values{Random(), Random(), Random(),
                                           Random()};
      EXPECT_EQ(Before.evalU64(Values), After.evalU64(Values));
    }
  }
}

TEST(SymMBAResidue, ProvesHighPerturbationsAtFullWidth) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  constexpr unsigned Width = 65;
  SymContext Ctx;
  auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  auto Scaled = [&](unsigned C, unsigned Shift, SymRef Value) {
    return Ctx.mkMul(Ctx.mkConst(llvm::APInt(Width, C).shl(Shift)), Value);
  };
  auto Offset = [&](unsigned C, SymRef D) {
    return Ctx.mkAdd(Ctx.mkConst(Width, C), D);
  };
  auto Input =
      Scaled(3, Width - 10,
             Ctx.mkMul({Offset(3, Scaled(3, 4, Ctx.mkXor(X, Y))),
                        Offset(2, Ctx.mkAnd(Y, Ctx.mkConst(Width, 0x3c0))),
                        Offset(5, Scaled(7, 6, Ctx.mkOr(X, Y))),
                        Offset(9, Ctx.mkAnd(X, Ctx.mkConst(Width, 0x280)))}));
  detail::WorkBudget Budget(65536);
  size_t Storage = 1 << 20;
  auto Output = detail::foldProductResidues(Ctx, Input, Budget, Storage);
  ASSERT_LT(Ctx.readabilityCost(Output), Ctx.readabilityCost(Input));
  solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
  EXPECT_EQ(Verifier(Ctx, Input, Output), SynthVerification::Equivalent);
}

TEST(SymMBAResidue, PreservesAllCrossTermsOfTheRemainingLowFactors) {
  std::mt19937_64 Random(731);
  for (unsigned Width : {5u, 9u, 65u, 129u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto U = Ctx.mkXor(X, Y), V = Ctx.mkOr(X, Y);
    const auto Zero = llvm::APInt(Width, 0);
    const unsigned Lost = Width / 4;
    const auto Scale = llvm::APInt(Width, 3).shl(Lost);
    const unsigned Shift = (Width - Lost + 1) / 2;
    const auto High = llvm::APInt(Width, 1).shl(Shift);
    const auto Mask = -High;
    auto Offset = [&](unsigned C, SymRef D) {
      return Ctx.mkAdd(Ctx.mkConst(Width, C), D);
    };
    // The bare word and both low perturbations are correlated with the high
    // subset. Moving the scale into each low multiplier would be incorrect.
    const auto Input = Ctx.mkMul(
        {V, Offset(1, X), Offset(3, Ctx.mkMul(Ctx.mkConst(Width, 2), Y)),
         Offset(2, Ctx.mkMul(Ctx.mkConst(High), Ctx.mkNot(U))),
         Offset(3, Ctx.mkAnd(Ctx.mkConst(Mask), V)),
         Offset(5, Ctx.mkMul(Ctx.mkConst(-High), U)),
         Offset(7, Ctx.mkMul(Ctx.mkConst(High * 3), V)), Ctx.mkConst(Scale)});
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 20;
    const auto Output =
        detail::foldProductResidues(Ctx, Input, Budget, Storage);
    ASSERT_LT(Ctx.readabilityCost(Output), Ctx.readabilityCost(Input));
    EXPECT_LT(Budget.used(), 8192u);
    SymEvalPlan Plan(Ctx, Output);
    auto Check = [&](const llvm::APInt &A, const llvm::APInt &B) {
      const auto Uv = A ^ B, Vv = A | B;
      const auto Expected = Scale * Vv * (A + 1) * (B * 2 + 3) *
                            ((~Uv) * High + 2) * ((Mask & Vv) + 3) *
                            (5 - High * Uv) * (High * 3 * Vv + 7);
      EXPECT_EQ(Plan.eval({A, B}), Expected);
    };
    if (Width == 5) {
      for (unsigned A = 0; A < 32; ++A)
        for (unsigned B = 0; B < 32; ++B)
          Check(llvm::APInt(Width, A), llvm::APInt(Width, B));
    } else {
      auto Word = [&] {
        auto V = Zero;
        for (unsigned Bit = 0; Bit < Width; Bit += 64)
          V |= llvm::APInt(Width, Random(), false, true).shl(Bit);
        return V;
      };
      for (unsigned I = 0; I < 64; ++I)
        Check(Word(), Word());
    }
    for (auto [Work, Bytes] :
         {std::pair<size_t, size_t>{2, 1 << 20}, {65536, 1}}) {
      detail::WorkBudget Short(Work);
      EXPECT_EQ(detail::foldProductResidues(Ctx, Input, Short, Bytes), Input);
      EXPECT_LE(Short.used(), Work);
    }
  }
}

TEST(SymMBAResidue, TruncatesRepeatedMixedWordsWithoutLosingCrossTerms) {
  std::mt19937_64 Random(53491);
  for (unsigned Width : {6u, 16u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto U = Ctx.mkXor(X, Y), V = Ctx.mkMul(X, Ctx.mkOr(X, Y));
    const unsigned Observed = Width - Width / 5, Shift = (Observed + 3) / 4;
    const auto Scale = llvm::APInt(Width, 3).shl(Width - Observed);
    llvm::SmallVector<SymRef, 32> Factors{Ctx.mkConst(Scale), Y};
    llvm::SmallVector<llvm::APInt, 32> Weights;
    for (unsigned J = 0; J < 23; ++J) {
      auto Word = J % 2 ? V : U;
      if (J % 5 == 3)
        Word = Ctx.mkNot(Word);
      auto Weight =
          llvm::APInt(Width, 3 + 2 * J).shl(J == 2 ? 0 : Shift + J % 2);
      if (J % 3 == 0)
        Weight.negate();
      Weights.push_back(Weight);
      Factors.push_back(Ctx.mkAdd(Ctx.mkConst(Width, J == 4 ? 6 : 2 * J + 1),
                                  Ctx.mkMul(Ctx.mkConst(Weight), Word)));
    }
    const auto Input = Ctx.mkMul(Factors);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 20;
    const auto Output =
        detail::foldProductResidues(Ctx, Input, Budget, Storage);
    ASSERT_LT(Ctx.readabilityCost(Output), Ctx.readabilityCost(Input) / 2);
    EXPECT_LT(Budget.used(), 32768u);
    SymEvalPlan Plan(Ctx, Output);
    auto Check = [&](SymEvalPlan &Evaluator, const llvm::APInt &A,
                     const llvm::APInt &B) {
      auto Expected = Scale * B;
      for (unsigned J = 0; J < Weights.size(); ++J) {
        auto Word = J % 2 ? A * (A | B) : A ^ B;
        if (J % 5 == 3)
          Word.flipAllBits();
        Expected *= Weights[J] * Word + (J == 4 ? 6 : 2 * J + 1);
      }
      EXPECT_EQ(Evaluator.eval({A, B}), Expected);
    };
    if (Width == 6) {
      for (unsigned A = 0; A < 64; ++A)
        for (unsigned B = 0; B < 64; ++B)
          Check(Plan, llvm::APInt(Width, A), llvm::APInt(Width, B));
    } else {
      auto Word = [&] {
        auto Value = llvm::APInt(Width, 0);
        for (unsigned Bit = 0; Bit < Width; Bit += 64)
          Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
        return Value;
      };
      for (unsigned J = 0; J < 64; ++J)
        Check(Plan, Word(), Word());
    }
    for (auto [Work, Bytes] :
         {std::pair<size_t, size_t>{2, 1 << 20}, {65536, 1}}) {
      detail::WorkBudget Short(Work);
      EXPECT_EQ(detail::foldProductResidues(Ctx, Input, Short, Bytes), Input);
      EXPECT_LE(Short.used(), Work);
    }
    // Exercise a declined middle-product reconstruction followed by the
    // existing high-product path with the same remaining storage.
    for (size_t Bytes : {128u, 512u, 1024u, 2048u, 4096u}) {
      detail::WorkBudget Limited(65536);
      size_t Remaining = Bytes;
      const auto Candidate =
          detail::foldProductResidues(Ctx, Input, Limited, Remaining);
      EXPECT_LE(Remaining, Bytes);
      EXPECT_LE(Limited.used(), 65536u);
      SymEvalPlan Evaluator(Ctx, Candidate);
      Check(Evaluator, llvm::APInt(Width, 3), llvm::APInt::getAllOnes(Width));
      Check(Evaluator, llvm::APInt::getAllOnes(Width), llvm::APInt(Width, 7));
    }
  }
}

TEST(SymMBAResidue, DropsTheCubicOnlyWhenEveryFallingFactorialIsAnnihilated) {
  // The third forward difference of (1 + 4*x)^7 is 128 modulo 256.
  // Seven observed bits admit a quadratic; eight still require a cubic.
  for (unsigned Observed : {7u, 8u}) {
    constexpr unsigned Width = 65;
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width);
    const auto F =
        Ctx.mkAdd(Ctx.mkConst(Width, 1), Ctx.mkMul(Ctx.mkConst(Width, 4), X));
    const auto Scale = llvm::APInt(Width, 1).shl(Width - Observed);
    llvm::SmallVector<SymRef, 8> Factors(7, F);
    Factors.push_back(Ctx.mkConst(Scale));
    const auto Input = Ctx.mkMul(Factors);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 20;
    const auto Output =
        detail::foldProductResidues(Ctx, Input, Budget, Storage);
    ASSERT_NE(Output, Input);
    unsigned Uses = 0;
    llvm::SmallVector<SymRef, 16> Pending{Output};
    while (!Pending.empty()) {
      const auto Value = Pending.pop_back_val();
      Uses += Value == X;
      const auto Operands = Ctx.operands(Value);
      Pending.append(Operands.begin(), Operands.end());
    }
    EXPECT_EQ(Uses, Observed == 7 ? 2u : 3u);
    SymEvalPlan Plan(Ctx, Output);
    for (unsigned I = 0; I < (1u << Observed); ++I) {
      llvm::APInt Expected = Scale;
      for (unsigned J = 0; J < 7; ++J)
        Expected *= 1 + I * 4;
      EXPECT_EQ(Plan.eval({llvm::APInt(Width, I)}), Expected);
    }
  }
}

TEST(SymMBAResidue,
     KeepsACompletedPolynomialBesideADisjointMonomialWithinBudget) {
  constexpr unsigned Width = 257;
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  const auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
  const std::array Words{Ctx.mkXor(X, Y),
                         Ctx.mkXor(Ctx.mkAnd(X, Z), Ctx.mkOr(Y, W)),
                         Ctx.mkXor(Ctx.mkAdd(X, Y), Ctx.mkAnd(Z, W))};
  const auto Scale = llvm::APInt(Width, 3).shl(51);
  llvm::SmallVector<SymRef, 96> Factors{Ctx.mkConst(Scale)};
  llvm::SmallVector<llvm::APInt, 96> Weights, Offsets;
  for (unsigned I = 0; I < 89; ++I) {
    const int Weight = (int(I * 17 % 63) - 31) | 1;
    int Offset = (int(I * 13 % 61) - 31) | 1;
    if (I == 0 || I == 44) {
      ++Offset;
      if (!Offset)
        Offset = 2;
    }
    Weights.push_back(llvm::APInt(Width, Weight, true).shl(42));
    Offsets.push_back(llvm::APInt(Width, Offset, true));
    Factors.push_back(
        Ctx.mkAdd(Ctx.mkConst(Offsets.back()),
                  Ctx.mkMul(Ctx.mkConst(Weights.back()), Words[(I + 5) % 3])));
  }
  const auto Input =
      Ctx.mkAdd(Ctx.mkMul(Factors), Ctx.mkMul(Ctx.mkXor(Z, W), Ctx.mkOr(X, Y)));
  MBAOptions Opts;
  Opts.MaxWork = 65536;
  const auto R = simplifyMBADeep(Ctx, Input, Opts);
  EXPECT_LT(R.Work, Opts.MaxWork / 2);
  EXPECT_LT(R.SizeAfter, 200u);
  SymEvalPlan Plan(Ctx, R.Expr);
  std::mt19937_64 Random(87331);
  for (unsigned I = 0; I < 32; ++I) {
    std::array<llvm::APInt, 4> Inputs;
    for (auto &Value : Inputs) {
      Value = llvm::APInt(Width, 0);
      for (unsigned Bit = 0; Bit < Width; Bit += 64)
        Value |= llvm::APInt(Width, Random()).shl(Bit);
    }
    const auto &A = Inputs[0], &B = Inputs[1], &C = Inputs[2], &D = Inputs[3];
    const std::array Values{A ^ B, (A & C) ^ (B | D), (A + B) ^ (C & D)};
    auto Expected = Scale;
    for (unsigned J = 0; J < Weights.size(); ++J)
      Expected *= Offsets[J] + Weights[J] * Values[(J + 5) % 3];
    EXPECT_EQ(Plan.eval(Inputs), Expected + (C ^ D) * (A | B));
  }
}

TEST(SymMBAResidue, ProvesOverlappingTruncatedProductsAtFullWidth) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {16u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto U = Ctx.mkXor(X, Y), V = Ctx.mkOr(X, Y);
    llvm::SmallVector<SymRef, 16> Factors{
        Ctx.mkConst(llvm::APInt(Width, 1).shl(Width - 6))};
    for (unsigned I = 0; I < 8; ++I) {
      auto Word = I % 2 ? U : V;
      if (I == 3)
        Word = Ctx.mkNot(Word);
      auto Weight = llvm::APInt(Width, I * 2 + 1).shl(I == 1 ? 0 : 2);
      if (I % 3 == 1)
        Weight.negate();
      Factors.push_back(Ctx.mkAdd(Ctx.mkConst(Width, I == 5 ? 2 : 2 * I + 1),
                                  Ctx.mkMul(Ctx.mkConst(Weight), Word)));
    }
    const auto Input = Ctx.mkMul(Factors);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 20;
    const auto Output =
        detail::foldProductResidues(Ctx, Input, Budget, Storage);
    ASSERT_LT(Ctx.readabilityCost(Output), Ctx.readabilityCost(Input));
    solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 15000);
    EXPECT_EQ(Verify(Ctx, Input, Output), SynthVerification::Equivalent);
  }
}

TEST(SymMBAResidue, ProvesAMixedSubsetWithEvenAndNegativeCoefficients) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (const unsigned Width : {16u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    auto Offset = [&](unsigned C, int K, SymRef Word) {
      return Ctx.mkAdd(
          Ctx.mkConst(Width, C),
          Ctx.mkMul(Ctx.mkConst(llvm::APInt(Width, K, true)), Word));
    };
    const auto Xor = Ctx.mkXor(X, Y);
    const auto Input = Ctx.mkMul(
        {Ctx.mkConst(llvm::APInt(Width, 3).shl(Width - 6)), Offset(3, 1, X),
         Offset(5, 2, Y), Offset(2, -24, Ctx.mkNot(Xor)), Offset(5, 8, X),
         Offset(7, 40, Y), Offset(9, -8, Xor)});
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 20;
    const auto Output =
        detail::foldProductResidues(Ctx, Input, Budget, Storage);
    ASSERT_LT(Ctx.readabilityCost(Output), Ctx.readabilityCost(Input));
    solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 5000);
    EXPECT_EQ(Verify(Ctx, Input, Output), SynthVerification::Equivalent);
  }
}

TEST(SymMBAResidue, AbsorbsAnExactParityCorrectionIntoAnAdjacentSum) {
  for (const unsigned Width : {8u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
    const auto Kernel = Ctx.mkXor({X, Ctx.mkAnd(Y, Z), Ctx.mkOr(Z, W)});
    const auto High = llvm::APInt::getSignMask(Width);
    auto Make = [&](const llvm::APInt &Scale) {
      return Ctx.mkAdd({X, Y, Ctx.mkConst(Width, 19),
                        Ctx.mkMul(Ctx.mkConst(Scale), Kernel)});
    };
    const auto Input = Make(High);
    MBAOptions Options;
    Options.VerifySamples = 0;
    detail::WorkBudget Budget(65536);
    detail::SolveReport Report;
    const auto Out = detail::foldResidueParitySum(Ctx, Input, Options, Budget,
                                                  1 << 20, Report);
    ASSERT_LT(Ctx.readabilityCost(Out), Ctx.readabilityCost(Input));
    const auto Neighbor = Make(High.lshr(1));
    EXPECT_EQ(detail::foldResidueParitySum(Ctx, Neighbor, Options, Budget,
                                           1 << 20, Report),
              Neighbor);
    if (solver::proofBackendAvailable(solver::ProofBackend::Z3)) {
      solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 5000);
      EXPECT_EQ(Verify(Ctx, Input, Out), SynthVerification::Equivalent);
    }
  }
}

TEST(SymMBAResidue, CollectsDualViewsBesideIndependentSummands) {
  for (const unsigned Width : {9u, 65u}) {
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
    const auto Other = Ctx.mkXor(Y, W);
    const auto Union = Ctx.mkOr(X, Y);
    const auto Common = Ctx.mkAnd(X, Y);
    const auto Tail = Ctx.mkAdd({Union, Common, Other});
    MBAOptions Opt;
    Opt.VerifySamples = 0;
    Opt.MaxWork = 65536;
    const auto R = simplifyMBADeep(Ctx, Tail, Opt);
    EXPECT_EQ(R.Expr, Ctx.mkAdd({X, Y, Other}));
    EXPECT_LT(R.SizeAfter, R.SizeBefore);
    // A third input on just one side makes the pair ineligible.
    const auto Neighbor = Ctx.mkAdd({Ctx.mkOr({X, Y, Z}), Common, Other});
    const auto Out = simplifyMBADeep(Ctx, Neighbor, Opt).Expr;
    SymEvalPlan Before(Ctx, Neighbor), After(Ctx, Out);
    for (unsigned I = 0; I < 16; ++I) {
      const std::array Inputs{
          llvm::APInt(Width, I & 1), llvm::APInt(Width, I >> 1 & 1),
          llvm::APInt(Width, I >> 2 & 1), llvm::APInt(Width, I >> 3)};
      EXPECT_EQ(Before.eval(Inputs), After.eval(Inputs));
    }
  }
}

TEST(SymMBAResidue, HasIndependentFullWidthSMTProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SymContext Ctx;
    auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    auto Input =
        product(Ctx, Ctx.mkAdd(X, Y), 4, llvm::APInt(Width, 1).shl(Width - 3));
    detail::WorkBudget Budget(16384);
    size_t Storage = 1 << 20;
    auto Output = detail::foldProductResidues(Ctx, Input, Budget, Storage);
    ASSERT_TRUE(Ctx.isConstZero(Output));
    solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
    EXPECT_EQ(Verifier(Ctx, Input, Output), SynthVerification::Equivalent);
  }
}

TEST(SymMBAResidue, NestedAffineProductsKeepCompleteValuesWithinBudget) {
  std::mt19937_64 Random(0x63a971);
  for (unsigned Width : {6u, 9u, 32u, 65u, 257u, 513u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const std::array Words{Ctx.mkXor(X, Y),
                           Ctx.mkXor(Ctx.mkAdd(X, Y), Ctx.mkAnd(X, Y))};
    const unsigned Observed = Width <= 97 ? Width : Width / 3;
    const auto Scale = llvm::APInt(Width, 3).shl(Width - Observed);
    llvm::SmallVector<SymRef, 64> Factors{Ctx.mkConst(Scale)};
    for (unsigned I = 0; I < 47; ++I) {
      auto Word = Words[I % 2];
      for (unsigned J = 0; J < 3; ++J)
        Word = Ctx.mkAdd(
            Ctx.mkConst(Width, 3 + 2 * I + J),
            Ctx.mkMul(Ctx.mkConst(llvm::APInt(Width, J % 2 ? -3 : 5, true)),
                      Word));
      const auto Weight =
          llvm::APInt(Width, 2 * I + 1, false, true).shl((Observed + 5) / 6);
      Factors.push_back(Ctx.mkAdd(Ctx.mkConst(Width, 2 * I + 1),
                                  Ctx.mkMul(Ctx.mkConst(Weight), Word)));
    }
    const auto Input =
        Ctx.mkAdd({Ctx.mkMul(Factors), Ctx.mkMul(Ctx.mkConst(Width, 3), X),
                   Ctx.mkMul(Ctx.mkConst(Width, 5), Y),
                   Ctx.mkMul(Ctx.mkConst(Width, 7), Ctx.mkXor(X, Y))});
    MBAOptions Opts;
    Opts.MaxWork = 65536;
    const auto Result = simplifyMBADeep(Ctx, Input, Opts);
    EXPECT_LT(Result.Work, Opts.MaxWork);
    EXPECT_LT(Result.SizeAfter, 150u);
    SymEvalPlan Plan(Ctx, Result.Expr);
    auto Check = [&](const llvm::APInt &A, const llvm::APInt &B) {
      auto Expected = Scale;
      for (unsigned I = 0; I < 47; ++I) {
        auto V = I % 2 ? (A + B) ^ (A & B) : A ^ B;
        for (unsigned J = 0; J < 3; ++J)
          V = 3 + 2 * I + J + V * llvm::APInt(Width, J % 2 ? -3 : 5, true);
        Expected *= 2 * I + 1 +
                    V * llvm::APInt(Width, 2 * I + 1, false, true)
                            .shl((Observed + 5) / 6);
      }
      EXPECT_EQ(Plan.eval({A, B}), Expected + A * 3 + B * 5 + (A ^ B) * 7);
    };
    if (Width == 6) {
      for (unsigned A = 0; A < 64; ++A)
        for (unsigned B = 0; B < 64; ++B)
          Check(llvm::APInt(Width, A), llvm::APInt(Width, B));
    } else {
      auto Word = [&] {
        auto V = llvm::APInt(Width, 0);
        for (unsigned Bit = 0; Bit < Width; Bit += 64)
          V |= llvm::APInt(Width, Random(), false, true).shl(Bit);
        return V;
      };
      for (unsigned I = 0; I < 48; ++I)
        Check(Word(), Word());
    }
    for (size_t Bytes : {1u, 128u, 1024u, 8192u}) {
      detail::WorkBudget Short(128);
      size_t Remaining = Bytes;
      const auto Part = detail::foldProductResidues(Ctx, Ctx.mkMul(Factors),
                                                    Short, Remaining);
      EXPECT_LE(Short.used(), 128u);
      EXPECT_LE(Remaining, Bytes);
      SymEvalPlan Original(Ctx, Ctx.mkMul(Factors)), Limited(Ctx, Part);
      for (unsigned I : {0u, 1u, 7u, 63u}) {
        const std::array Inputs{llvm::APInt(Width, I),
                                llvm::APInt::getAllOnes(Width)};
        EXPECT_EQ(Limited.eval(Inputs), Original.eval(Inputs));
      }
    }
  }
}

TEST(SymMBAResidue, AffineCoordinatesPreserveEvenWeightsAndZeroContent) {
  for (unsigned Width : {5u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto U = Ctx.mkXor(X, Y);
    for (unsigned Cancel : {0u, 1u}) {
      llvm::SmallVector<SymRef, 16> Factors{
          Ctx.mkConst(llvm::APInt(Width, 3).shl(Width - 5))};
      for (unsigned I = 0; I < 7; ++I) {
        const auto Word = Ctx.mkAdd(
            Ctx.mkConst(Width, 3),
            Ctx.mkMul(Ctx.mkConst(Width, 2), I % 2 ? Ctx.mkNot(U) : U));
        const int Offset = Cancel && I == 0 ? -12 : 1 + 2 * I;
        Factors.push_back(
            Ctx.mkAdd(Ctx.mkConst(llvm::APInt(Width, Offset, true)),
                      Ctx.mkMul(Ctx.mkConst(Width, 4), Word)));
      }
      const auto Input = Ctx.mkMul(Factors);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 20;
      const auto Output =
          detail::foldProductResidues(Ctx, Input, Budget, Storage);
      SymEvalPlan Plan(Ctx, Output);
      for (unsigned A = 0; A < 32; ++A)
        for (unsigned B = 0; B < 32; ++B) {
          auto Expected = llvm::APInt(Width, 3).shl(Width - 5);
          for (unsigned I = 0; I < 7; ++I) {
            auto V = llvm::APInt(Width, A ^ B);
            if (I % 2)
              V.flipAllBits();
            const int Offset = Cancel && I == 0 ? -12 : int(1 + 2 * I);
            Expected *= llvm::APInt(Width, Offset, true) + (3 + V * 2) * 4;
          }
          EXPECT_EQ(Plan.eval({llvm::APInt(Width, A), llvm::APInt(Width, B)}),
                    Expected);
        }
    }
  }
}

TEST(SymMBAResidue, ProvesNestedAffineCoordinatesAtFullWidth) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {16u, 65u, 257u}) {
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto U = Ctx.mkXor(X, Y);
    llvm::SmallVector<SymRef, 16> Factors{
        Ctx.mkConst(llvm::APInt(Width, 3).shl(Width - 7))};
    for (unsigned I = 0; I < 11; ++I) {
      const auto Nested =
          Ctx.mkAdd(Ctx.mkConst(Width, I + 3),
                    Ctx.mkMul(Ctx.mkConst(llvm::APInt(Width, -5, true)), U));
      Factors.push_back(
          Ctx.mkAdd(Ctx.mkConst(Width, 2 * I + 1),
                    Ctx.mkMul(Ctx.mkConst(Width, (2 * I + 1) * 4), Nested)));
    }
    const auto Input = Ctx.mkMul(Factors);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 20;
    const auto Output =
        detail::foldProductResidues(Ctx, Input, Budget, Storage);
    ASSERT_LT(Ctx.readabilityCost(Output), Ctx.readabilityCost(Input));
    solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 5000);
    EXPECT_EQ(Verify(Ctx, Input, Output), SynthVerification::Equivalent);
  }
}

TEST(SymMBAResidue, GroupsCoordinatesOnlyThroughTheirLiveLowBits) {
  std::mt19937_64 Random(0xa1685);
  for (unsigned Width : {6u, 9u, 32u, 65u, 257u, 513u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto U = Ctx.mkXor(X, Y);
    const unsigned Observed = Width == 6 ? 6 : Width * 2 / 3;
    const auto Scale = llvm::APInt(Width, 3).shl(Width - Observed);
    for (unsigned Variant = 0; Variant < 4; ++Variant) {
      SCOPED_TRACE(Variant);
      const bool Neighbor = Variant & 1, AbsorbedScale = Variant & 2;
      llvm::SmallVector<SymRef, 64> Factors;
      if (!AbsorbedScale)
        Factors.push_back(Ctx.mkConst(Scale));
      for (unsigned I = 0; I < 47; ++I) {
        const unsigned Shift = std::max(1u, Observed / 5) + I % 2;
        const unsigned Bits = Observed - Shift;
        const auto Low = llvm::APInt::getLowBitsSet(Width, Bits);
        const auto K = Ctx.mkConst(Neighbor && I == 0 ? Low.lshr(1) : Low);
        const auto High = Ctx.mkConst(~Low);
        SymRef View;
        switch (I % 5) {
        case 0:
          View = Ctx.mkAnd(U, K);
          break;
        case 1:
          View = Ctx.mkOr(U, High);
          break;
        case 2:
          View = Ctx.mkXor({X, Y, K});
          break;
        case 3:
          View = Ctx.mkXor({X, Y, High});
          break;
        default:
          View = Ctx.mkAdd(
              {X, Ctx.mkConst(Width, 3),
               Ctx.mkMul(Ctx.mkConst(llvm::APInt::getOneBitSet(Width, Bits)),
                         Y)});
        }
        auto Offset = llvm::APInt(Width, 2 * I + 1, false, true);
        auto Weight = Offset.shl(Shift);
        if (AbsorbedScale && I == 0) {
          Offset *= Scale;
          Weight *= Scale;
        }
        Factors.push_back(Ctx.mkAdd(Ctx.mkConst(Offset),
                                    Ctx.mkMul(Ctx.mkConst(Weight), View)));
      }
      const auto Input = Ctx.mkMul(Factors);
      MBAOptions Opts;
      Opts.MaxWork = 65536;
      const auto R = simplifyMBADeep(Ctx, Input, Opts);
      ASSERT_LT(R.Work, Opts.MaxWork);
      ASSERT_LT(R.SizeAfter, 100u);
      SymEvalPlan Result(Ctx, R.Expr);
      auto Check = [&](llvm::APInt A, llvm::APInt B) {
        auto Expected = Scale;
        for (unsigned I = 0; I < 47; ++I) {
          const unsigned Shift = std::max(1u, Observed / 5) + I % 2;
          const unsigned Bits = Observed - Shift;
          const auto Low = llvm::APInt::getLowBitsSet(Width, Bits);
          auto V = A ^ B;
          switch (I % 5) {
          case 0:
            V &= Neighbor && I == 0 ? Low.lshr(1) : Low;
            break;
          case 1:
            V |= ~Low;
            break;
          case 2:
            V ^= Low;
            break;
          case 3:
            V ^= ~Low;
            break;
          default:
            V = A + 3 + B.shl(Bits);
          }
          Expected *= 2 * I + 1 +
                      V * llvm::APInt(Width, 2 * I + 1, false, true).shl(Shift);
        }
        EXPECT_EQ(Result.eval({A, B}), Expected);
      };
      if (Width == 6) {
        for (unsigned A = 0; A < 64; ++A)
          for (unsigned B = 0; B < 64; ++B)
            Check(llvm::APInt(Width, A), llvm::APInt(Width, B));
      } else {
        auto Word = [&] {
          llvm::APInt V(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            V |= llvm::APInt(Width, Random(), false, true).shl(Bit);
          return V;
        };
        for (unsigned I = 0; I < 48; ++I)
          Check(Word(), Word());
      }
      detail::WorkBudget Short(128);
      size_t Storage = 256;
      const auto Part = detail::foldProductResidues(Ctx, Input, Short, Storage);
      EXPECT_EQ(Part, Input);
      EXPECT_LE(Short.used(), 128u);
    }
  }
}

TEST(SymMBAResidue, ProvesLivePrefixAndNeighborAtFullWidth) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {16u, 65u, 257u}) {
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto U = Ctx.mkXor(X, Y);
    for (bool Neighbor : {false, true}) {
      llvm::SmallVector<SymRef, 16> Factors{
          Ctx.mkConst(llvm::APInt(Width, 3).shl(Width - 7))};
      for (unsigned I = 0; I < 13; ++I) {
        const auto Mask =
            llvm::APInt::getLowBitsSet(Width, Neighbor && I == 0 ? 4 : 5);
        auto V = I % 2 ? Ctx.mkXor(U, Ctx.mkConst(~Mask))
                       : Ctx.mkAnd(U, Ctx.mkConst(Mask));
        Factors.push_back(
            Ctx.mkAdd(Ctx.mkConst(Width, 2 * I + 1),
                      Ctx.mkMul(Ctx.mkConst(Width, (2 * I + 1) * 4), V)));
      }
      const auto Input = Ctx.mkMul(Factors);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 20;
      const auto Output =
          detail::foldProductResidues(Ctx, Input, Budget, Storage);
      ASSERT_LT(Ctx.readabilityCost(Output), Ctx.readabilityCost(Input));
      solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 5000);
      EXPECT_EQ(Verify(Ctx, Input, Output), SynthVerification::Equivalent);
    }
  }
}

TEST(SymMBAResidue, DynamicMasksRetainTheLastObservedBit) {
  for (unsigned Width : {7u, 16u, 65u, 257u}) {
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto U = Ctx.mkXor(X, Y);
    const auto Scale = llvm::APInt(Width, 3).shl(Width - 6);
    for (bool Fixed : {false, true})
      for (bool Live : {false, true}) {
        llvm::SmallVector<SymRef, 16> Factors{Ctx.mkConst(Scale)};
        // The coefficient is four times an odd number. Only the low
        // four bits of each word can reach the six observed result bits.
        for (unsigned I = 0; I < 13; ++I) {
          const unsigned Mask = Live && I == 5 ? 8 : ((2 * I + 1) << 4) | 16;
          const auto Term = Ctx.mkAdd(Y, Ctx.mkConst(Width, 2 * I + 1));
          const auto High = Ctx.mkAnd(Term, Ctx.mkConst(Width, Mask));
          // A live literal on the same n-ary operation must also survive.
          const auto One = Ctx.mkConst(Width, 1);
          const auto Word =
              I % 4 == 0   ? Ctx.mkOr(Fixed ? Ctx.mkOr(U, One) : U, High)
              : I % 4 == 1 ? Ctx.mkXor(Fixed ? Ctx.mkXor(U, One) : U, High)
              : I % 4 == 2
                  ? Ctx.mkAnd(Fixed ? Ctx.mkAnd(U, Ctx.mkNot(One)) : U,
                              Ctx.mkNot(High))
                  : Ctx.mkAnd(U, Ctx.mkOr(Term, Ctx.mkConst(Width, 15)));
          Factors.push_back(
              Ctx.mkAdd(Ctx.mkConst(Width, 2 * I + 1),
                        Ctx.mkMul(Ctx.mkConst(Width, 4 * (2 * I + 1)), Word)));
        }
        auto Input = Ctx.mkMul(Factors);
        MBAOptions Opts;
        Opts.MaxWork = 65536;
        auto Result = simplifyMBADeep(Ctx, Input, Opts);
        EXPECT_LT(Result.Work, Opts.MaxWork);
        EXPECT_LT(Result.SizeAfter, Result.SizeBefore / 2);
        SymEvalPlan Eval(Ctx, Result.Expr);
        // Only four input bits reach a coefficient times four modulo 64.
        // Exhaust the full seven-bit words, then all 256 live input pairs
        // at each larger width using full-width result values.
        const unsigned Count = Width == 7 ? 128 : 16;
        for (unsigned A = 0; A < Count; ++A)
          for (unsigned B = 0; B < Count; ++B) {
            unsigned Expected = 3;
            for (unsigned I = 0; I < 13; ++I) {
              const auto Mask = Live && I == 5 ? 8u : ((2 * I + 1) << 4) | 16;
              const auto Term = B + 2 * I + 1, High = Term & Mask, V = A ^ B;
              const auto Word = I % 4 == 0   ? (V | (Fixed ? 1u : 0u)) | High
                                : I % 4 == 1 ? (V ^ (Fixed ? 1u : 0u)) ^ High
                                : I % 4 == 2 ? (V & ~(Fixed ? 1u : 0u)) & ~High
                                             : V & (Term | 15);
              Expected *= (2 * I + 1) * (1 + 4 * Word);
            }
            ASSERT_EQ(Eval.eval({llvm::APInt(Width, A), llvm::APInt(Width, B)}),
                      llvm::APInt(Width, Expected, false, true).shl(Width - 6));
          }
        if (Width != 7 &&
            solver::proofBackendAvailable(solver::ProofBackend::Z3)) {
          // Preserve the ordinary factor coordinate in the proof and
          // truncate each multiplication to its six observable bits.
          llvm::SmallVector<SymRef, 16> LowFactors{Ctx.mkConst(6, 3)};
          for (auto F : llvm::ArrayRef(Factors).drop_front())
            LowFactors.push_back(Ctx.mkExtract(F, 0, 6));
          const auto Expected =
              Ctx.mkConcat(Ctx.mkMul(LowFactors), Ctx.mkZero(Width - 6));
          solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 30000);
          EXPECT_EQ(Verify(Ctx, Expected, Result.Expr),
                    SynthVerification::Equivalent);
        }
      }
  }
}

TEST(SymMBAResidue, CombinesPartialWordsOnlyAcrossUnobservableBits) {
  for (unsigned Width : {7u, 16u, 65u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto Axis =
        Ctx.mkXor(Ctx.mkAdd(X, Ctx.mkConst(llvm::APInt(Width, -3, true))),
                  Ctx.mkMul(X, Y));
    const auto Scale = Ctx.mkConst(llvm::APInt(Width, 3).shl(Width - 6));
    for (bool Live : {false, true}) {
      SCOPED_TRACE(Live);
      llvm::SmallVector<SymRef, 16> Factors{Scale};
      for (unsigned I = 0; I < 13; ++I) {
        // The coefficient reads four low bits. The middle sign extension
        // in the neighbor changes bit three for some inputs.
        const unsigned Bits = Live && I == 6 ? 3 : 4 + I % 2;
        const auto Low = Ctx.mkExtract(Axis, 0, Bits);
        SymRef Word;
        switch (I % 3) {
        case 0:
          Word = Ctx.mkSExt(Low, Width);
          break;
        case 1:
          Word = Ctx.mkZExt(Low, Width);
          break;
        default:
          Word = Ctx.mkConcat(Ctx.mkExtract(Y, Bits, Width - Bits), Low);
        }
        Factors.push_back(Ctx.mkAdd(Ctx.mkConst(Width, 2 * I + 1),
                                    Ctx.mkMul(Ctx.mkConst(Width, 4), Word)));
      }
      const auto Input = Ctx.mkMul(Factors);
      MBAOptions Opts;
      Opts.MaxWork = 65536;
      const auto R = simplifyMBADeep(Ctx, Input, Opts);
      ASSERT_LT(R.SizeAfter, 60u);
      ASSERT_LT(R.Work, Opts.MaxWork);
      SymEvalPlan Result(Ctx, R.Expr);
      if (Width == 7) {
        bool SawLiveBit = false;
        for (unsigned A = 0; A < 128; ++A)
          for (unsigned B = 0; B < 128; ++B) {
            unsigned V = (A - 3) ^ (A * B);
            unsigned Clean = 6, Expected = 6;
            for (unsigned I = 0; I < 13; ++I) {
              const unsigned Part = Live && I == 6 ? ((V & 7) ^ 4) - 4 : V;
              Expected *= 2 * I + 1 + 4 * Part;
              Clean *= 2 * I + 1 + 4 * V;
            }
            SawLiveBit |= ((Expected ^ Clean) & 127) != 0;
            ASSERT_EQ(Result.eval({llvm::APInt(7, A), llvm::APInt(7, B)}),
                      llvm::APInt(7, Expected, false, true));
          }
        EXPECT_EQ(SawLiveBit, Live);
      } else if (solver::proofBackendAvailable(solver::ProofBackend::Z3)) {
        solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 10000);
        EXPECT_EQ(Verify(Ctx, Input, R.Expr), SynthVerification::Equivalent);
      }
      detail::WorkBudget Short(128);
      size_t Storage = 256;
      EXPECT_EQ(detail::foldProductResidues(Ctx, Input, Short, Storage), Input);
      EXPECT_LE(Short.used(), 128u);
    }
  }
}

TEST(SymMBAResidue, StreamsTopColumnAndRetainsLowerCofactors) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  constexpr unsigned Width = 32;
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  for (unsigned Delta : {0u, 1u}) {
    // The literal product has ten low zeros. The second factor removes two
    // of those zeros from its cofactor; lowering its shift by one makes bit
    // 30 live even though every pair of disturbances still vanishes.
    const auto A =
        Ctx.mkAdd(Ctx.mkConst(Width, 2),
                  Ctx.mkMul(Ctx.mkConst(Width, 1u << 22), Ctx.mkNot(X)));
    const auto B =
        Ctx.mkAdd(Ctx.mkConst(Width, 12),
                  Ctx.mkMul({Ctx.mkConst(Width, 1u << (23 - Delta)), X, Y}));
    const auto C =
        Ctx.mkAdd(Ctx.mkConst(Width, 13),
                  Ctx.mkMul(Ctx.mkConst(Width, 1u << 21), Ctx.mkXor(X, Y)));
    const auto Input = Ctx.mkMul({Ctx.mkConst(Width, 128), A, B, C});
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 20;
    const auto Output =
        detail::foldHighProductTerms(Ctx, Input, Budget, Storage, nullptr);
    ASSERT_LT(Ctx.readabilityCost(Output), Ctx.readabilityCost(Input));
    solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 5000);
    EXPECT_EQ(Verify(Ctx, Input, Output), SynthVerification::Equivalent);
  }
}

TEST(SymMBAResidue, ProvesColumnCofactorsWithMultipleOrMissingPartners) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  SymContext Ctx;
  constexpr unsigned Width = 32;
  const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  const auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
  const auto Sign = Ctx.mkConst(llvm::APInt::getSignMask(Width));
  for (unsigned Shape : {0u, 1u, 2u}) {
    const std::array Words{Shape == 1 ? Z : X, Ctx.mkOr(X, Y),
                           Ctx.mkAnd(Shape == 2 ? X : Y, Z),
                           Ctx.mkAnd(Shape == 2 ? X : Y, W)};
    llvm::SmallVector<SymRef, 4> Factors;
    for (auto V : Words)
      Factors.push_back(Ctx.mkAdd(Ctx.mkConst(Width, 1), Ctx.mkMul(Sign, V)));
    const auto Input = Ctx.mkMul(Factors);
    detail::WorkBudget Budget(4096);
    size_t Storage = 8192;
    const auto Output =
        detail::foldHighProductTerms(Ctx, Input, Budget, Storage, nullptr);
    EXPECT_LT(Ctx.readabilityCost(Output), Ctx.readabilityCost(Input));
    solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 5000);
    EXPECT_EQ(Verify(Ctx, Input, Output), SynthVerification::Equivalent);
  }
}

} // namespace
