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

#include <array>
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
  SymContext FullCtx;
  const auto FullFixture = polynomialCacheFixture(FullCtx, 64);
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
    SCOPED_TRACE(::testing::Message() << Limits.first << ':' << Limits.second);
    SymContext Ctx;
    const auto F = polynomialCacheFixture(Ctx, 64);
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
      const auto Values = polynomialCacheValues(64, Trial);
      EXPECT_EQ(Reduced.eval(Values), F.oracle(Values)) << Trial;
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

} // namespace
