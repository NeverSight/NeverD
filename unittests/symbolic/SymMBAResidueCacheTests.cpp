//===- SymMBAResidueCacheTests.cpp - Shared polynomial emission tests -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check complete polynomial cofactors reused by later output bits whose
/// support introduces another variable index, including bounded refusals.
///
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/solver/Z3Solver.h"

#include "llvm/ADT/SmallVector.h"

#include <array>
#include <random>
#include <utility>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

struct PolynomialCacheFixture {
  SymRef Input;
  unsigned Width;

  llvm::APInt oracle(const std::array<llvm::APInt, 3> &V) const {
    const auto One = llvm::APInt(Width, 1);
    const auto Parity = V[0] & One;
    const auto A = V[0].lshr(1) & One;
    const auto D = V[0].lshr(2) & One;
    const auto H = A & ((V[1] ^ V[2]) & One);
    // (1+4H)(1+8DH) = 1+4H+40DH because H is a bit. Every
    // factor disturbance is divisible by 64 and disappears under Scale.
    return llvm::APInt(Width, 3).shl(Width - 6) * Parity *
           (One + llvm::APInt(Width, 4) * H + llvm::APInt(Width, 40) * D * H);
  }
};

PolynomialCacheFixture polynomialCacheFixture(SymContext &Ctx, unsigned Width) {
  const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  const auto Z = Ctx.mkVar("z", Width);
  auto C = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
  const auto Parity = Ctx.mkAnd(X, C(1));
  const auto A = Ctx.mkAnd(Ctx.mkLShr(X, C(1)), C(1));
  const auto D = Ctx.mkAnd(Ctx.mkLShr(X, C(2)), C(1));
  const auto H = Ctx.mkAnd(A, Ctx.mkAnd(Ctx.mkXor(Y, Z), C(1)));
  const auto First =
      Ctx.mkAdd({C(1), Ctx.mkMul(C(4), H), Ctx.mkMul(C(64), Ctx.mkXor(X, Y))});
  const auto Second = Ctx.mkAdd({C(1), Ctx.mkMul(C(8), Ctx.mkAnd(D, H)),
                                 Ctx.mkMul(C(64), Ctx.mkXor(Y, Z))});
  const auto Scale = Ctx.mkConst(llvm::APInt(Width, 3).shl(Width - 6));
  return {Ctx.mkMul({Scale, Parity, First, Second}), Width};
}

std::array<llvm::APInt, 3> polynomialCacheValues(unsigned Width,
                                                 unsigned Trial) {
  std::array<llvm::APInt, 3> Values;
  for (unsigned I = 0; I < Values.size(); ++I) {
    auto Value = llvm::APInt(Width, 0);
    for (unsigned Bit = 0; Bit < Width; Bit += 64)
      Value |= llvm::APInt(Width,
                           0x9e3779b97f4a7c15ULL * (1 + I + (Trial >> 5) + Bit))
                   .shl(Bit);
    const uint64_t Mask = I ? 1 : 7;
    const uint64_t Low = I ? (Trial >> (I + 2)) & 1 : Trial & 7;
    Values[I] = (Value & ~llvm::APInt(Width, Mask)) | llvm::APInt(Width, Low);
  }
  return Values;
}

TEST(SymMBAResiduePolynomialCache, PreservesCofactorsWhenOutputSupportGrows) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto F = polynomialCacheFixture(Ctx, Width);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 24;
    const auto Result =
        detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
    ASSERT_NE(Result, F.Input);
    EXPECT_LT(detail::readingCost(Ctx, Result),
              detail::readingCost(Ctx, F.Input));
    EXPECT_LT(Budget.used(), 65536u);
    // H occurs in the low output before D joins the support of the next one.
    // Its old polynomial variable indices must retain the same shifted words.
    SymEvalPlan Reduced(Ctx, Result);
    for (unsigned Trial = 0; Trial < 128; ++Trial) {
      const auto Values = polynomialCacheValues(Width, Trial);
      EXPECT_EQ(Reduced.eval(Values), F.oracle(Values)) << Trial;
    }
  }
}

TEST(SymMBAResiduePolynomialCache, ExactAndShortResourcesKeepCompleteWords) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext FullCtx;
    const auto FullFixture = polynomialCacheFixture(FullCtx, Width);
    detail::WorkBudget FullBudget(65536);
    size_t FullStorage = 1 << 24;
    const auto Full = detail::foldResidueDecisions(FullCtx, FullFixture.Input,
                                                   FullBudget, FullStorage);
    ASSERT_NE(Full, FullFixture.Input);
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
      SCOPED_TRACE(::testing::Message()
                   << Limits.first << ':' << Limits.second);
      SymContext Ctx;
      const auto F = polynomialCacheFixture(Ctx, Width);
      detail::WorkBudget Budget(Limits.first);
      size_t Storage = Limits.second;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
      EXPECT_LE(Budget.used(), Limits.first);
      EXPECT_LE(Storage, Limits.second);
      if (Limits.first <= 1 || !Limits.second)
        EXPECT_EQ(Result, F.Input);
      if (Limits.first == Work && Limits.second == Bytes)
        EXPECT_EQ(Result.index(), Full.index());
      // A later optional refusal may retain a finished shorter reading. Either
      // result must still satisfy the independent complete-width expression.
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned Trial = 0; Trial < 64; ++Trial) {
        const auto Values = polynomialCacheValues(Width, Trial);
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values)) << Trial;
      }
    }
  }
}

TEST(SymMBAResiduePolynomialCache, HasIndependentNativeWidthProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto F = polynomialCacheFixture(Ctx, Width);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 24;
    const auto Result =
        detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
    ASSERT_NE(Result, F.Input);
    solver::Z3SolverOptions Options;
    Options.TimeoutMs = 10000;
    solver::Z3Solver Verifier(Ctx, Options);
    EXPECT_EQ(Verifier.checkDistinct(F.Input, Result), solver::SatResult::Unsat)
        << Verifier.reasonUnknown();
  }
}

TEST(SymMBAResiduePolynomialCache, SingleBitArithmeticKeepsCompleteWords) {
  for (unsigned Width : {8u, 64u, 257u}) {
    auto input = [&](SymContext &Ctx) {
      const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      const auto Z = Ctx.mkVar("z", Width);
      return Ctx.mkMul({Ctx.mkConst(llvm::APInt(Width, 1).shl(Width - 1)),
                        Ctx.mkXor(Ctx.mkAdd(X, Y), Z),
                        Ctx.mkAdd(Ctx.mkMul(Y, Z), X)});
    };
    SymContext FullCtx;
    const auto Root = input(FullCtx);
    detail::WorkBudget FullBudget(65536);
    size_t FullStorage = 1 << 24;
    const auto Full =
        detail::foldResidueDecisions(FullCtx, Root, FullBudget, FullStorage);
    ASSERT_NE(Full, Root);
    const auto Work = FullBudget.used();
    const size_t Bytes = (1 << 24) - FullStorage;
    ASSERT_GT(Work, 1u);
    ASSERT_GT(Bytes, 1u);
    for (auto Limits : {std::pair<size_t, size_t>{0, Bytes},
                        {Work - 1, Bytes},
                        {Work, Bytes - 1},
                        {Work, Bytes}}) {
      SCOPED_TRACE(::testing::Message()
                   << Width << ':' << Limits.first << ':' << Limits.second);
      SymContext Ctx;
      const auto Original = input(Ctx);
      detail::WorkBudget Budget(Limits.first);
      size_t Storage = Limits.second;
      const auto Result =
          detail::foldResidueDecisions(Ctx, Original, Budget, Storage);
      EXPECT_LE(Budget.used(), Limits.first);
      EXPECT_LE(Storage, Limits.second);
      if (Limits.first == Work && Limits.second == Bytes)
        EXPECT_EQ(Result.index(), Full.index());
      SymEvalPlan Plan(Ctx, Result);
      for (unsigned Trial = 0; Trial < 128; ++Trial) {
        auto Values = polynomialCacheValues(Width, Trial);
        for (unsigned I = 0; I < Values.size(); ++I)
          Values[I] = (Values[I] & ~llvm::APInt(Width, 1)) |
                      llvm::APInt(Width, (Trial >> I) & 1);
        const auto Expected = llvm::APInt(Width, 1).shl(Width - 1) *
                              ((Values[0] + Values[1]) ^ Values[2]) *
                              (Values[1] * Values[2] + Values[0]);
        EXPECT_EQ(Plan.eval(Values), Expected);
      }
    }
  }
}

TEST(SymMBAResiduePolynomialCache, ColumnSumsKeepComplementedCarries) {
  std::mt19937_64 Random(0x636f6c756d6e);
  unsigned Rewrites = 0;
  for (unsigned Width : {8u, 64u, 257u})
    for (unsigned Observed : {3u, 6u, 8u})
      for (unsigned Mode = 0; Mode < 4; ++Mode) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Observed << ':' << Mode);
        const auto Scale = llvm::APInt(Width, 3).shl(Width - Observed);
        const auto Even = ~llvm::APInt(Width, 1);
        auto Input = [&](SymContext &Ctx) {
          const auto X = Ctx.mkVar("x", Width);
          auto Y = Ctx.mkVar("y", Width), Z = Ctx.mkVar("z", Width);
          if (Mode & 1)
            Y = Ctx.mkNot(Y);
          if (Mode & 2)
            Z = Ctx.mkNot(Z);
          llvm::SmallVector<SymRef, 16> Factors{Ctx.mkConst(Scale)};
          for (unsigned I = 0; I < 8; ++I)
            Factors.push_back(Ctx.mkXor(
                Ctx.mkAdd({X, Y, Z, Ctx.mkConst(Width, 2 * I)}),
                Ctx.mkMul(Ctx.mkAnd(Ctx.mkAdd(Y, Ctx.mkConst(Width, I)),
                                    Ctx.mkConst(Even)),
                          Ctx.mkAnd(Ctx.mkAdd(Z, Ctx.mkConst(Width, 2 * I)),
                                    Ctx.mkConst(Even)))));
          return Ctx.mkMul(Factors);
        };
        SymContext FullCtx;
        const auto Root = Input(FullCtx);
        detail::WorkBudget FullBudget(65536);
        size_t FullStorage = 1 << 24;
        const auto Full = detail::foldResidueDecisions(FullCtx, Root,
                                                       FullBudget, FullStorage);
        Rewrites += Full != Root;
        const auto Work = FullBudget.used();
        const size_t Bytes = (1 << 24) - FullStorage;
        ASSERT_GT(Work, 1u);
        ASSERT_GT(Bytes, 1u);
        for (auto Limits : {std::pair<size_t, size_t>{Work, Bytes},
                            {Work - 1, Bytes},
                            {Work, Bytes - 1}}) {
          SymContext Ctx;
          const auto Original = Input(Ctx);
          detail::WorkBudget Budget(Limits.first);
          size_t Storage = Limits.second;
          const auto Result =
              detail::foldResidueDecisions(Ctx, Original, Budget, Storage);
          EXPECT_LE(Budget.used(), Limits.first);
          EXPECT_LE(Storage, Limits.second);
          if (Limits.first == Work && Limits.second == Bytes)
            EXPECT_EQ(Ctx.toString(Result), FullCtx.toString(Full));
          SymEvalPlan Plan(Ctx, Result);
          for (unsigned Trial = 0; Trial < 128; ++Trial) {
            std::array<llvm::APInt, 3> Values;
            for (auto &Value : Values) {
              Value = llvm::APInt(Width, 0);
              for (unsigned Bit = 0; Bit < Width; Bit += 64)
                Value |= llvm::APInt(Width, Random(), false, true).shl(Bit);
              if (Trial < 2)
                Value = Trial ? llvm::APInt::getAllOnes(Width)
                              : llvm::APInt(Width, 0);
            }
            const auto Y = Mode & 1 ? ~Values[1] : Values[1];
            const auto Z = Mode & 2 ? ~Values[2] : Values[2];
            auto Expected = Scale;
            for (unsigned I = 0; I < 8; ++I)
              Expected *= (Values[0] + Y + Z + llvm::APInt(Width, 2 * I)) ^
                          (((Y + llvm::APInt(Width, I)) & Even) *
                           ((Z + llvm::APInt(Width, 2 * I)) & Even));
            EXPECT_EQ(Plan.eval(Values), Expected);
          }
        }
      }
  EXPECT_GT(Rewrites, 0u);
}

TEST(SymMBAResiduePolynomialCache, NarrowReadsKeepDistinctOriginalInputs) {
  for (unsigned Width : {8u, 64u, 257u})
    for (bool LargeIndices : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << LargeIndices);
      SymContext Ctx;
      if (LargeIndices)
        for (unsigned I = 0; I < 4096; ++I)
          Ctx.mkConst(128, 100000 + I);
      const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      const auto Z = Ctx.mkVar("z", Width);
      auto C = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
      const auto A = Ctx.mkAdd(C(1), Ctx.mkMul(C(4), Ctx.mkLShr(X, C(2))));
      const auto B = Ctx.mkAdd(C(1), Ctx.mkMul(C(4), Ctx.mkLShr(Y, C(2))));
      const auto Scale = llvm::APInt(Width, 3).shl(Width - 8);
      llvm::SmallVector<SymRef, 68> Factors{
          Ctx.mkConst(Scale), Ctx.mkAdd(C(1), Ctx.mkMul(C(4), Z))};
      for (unsigned I = 0; I < 32; ++I) {
        Factors.push_back(A);
        Factors.push_back(B);
      }
      const auto Root = Ctx.mkMul(Factors);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, Root, Budget, Storage);
      ASSERT_NE(Result, Root);
      EXPECT_LT(detail::readingCost(Ctx, Result),
                detail::readingCost(Ctx, Root));
      EXPECT_LT(Budget.used(), 65536u);
      // At wide widths, the eight-bit admission sees two opaque shifts.
      // Multiplication by four later requests six bits, exposing their two
      // different source words. Neither may reuse the other's decision key.
      SymEvalPlan Plan(Ctx, Result);
      for (unsigned Trial = 0; Trial < 256; ++Trial) {
        auto Values = polynomialCacheValues(Width, Trial);
        for (unsigned I = 0; I < Values.size(); ++I)
          Values[I] =
              (Values[I] & ~llvm::APInt(Width, 255)) |
              llvm::APInt(Width, ((Trial * 29 + I * 97) ^ (Trial >> I)) & 255);
        const auto One = llvm::APInt(Width, 1), Four = llvm::APInt(Width, 4);
        const auto Left = One + Four * Values[0].lshr(2);
        const auto Right = One + Four * Values[1].lshr(2);
        auto Expected = Scale * (One + Four * Values[2]);
        for (unsigned I = 0; I < 32; ++I)
          Expected *= Left * Right;
        EXPECT_EQ(Plan.eval(Values), Expected) << Trial;
      }
    }
}

} // namespace
