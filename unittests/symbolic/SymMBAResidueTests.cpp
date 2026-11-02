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

} // namespace
