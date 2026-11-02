//===- SymMBAWidePairTests.cpp - Exact two-bit product correction tests ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independently check variable two-bit corrections, mixed masks, normalized
/// products, nearby noncongruences and cumulative resource refusal.
///
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/solver/Z3Solver.h"

#include <array>
#include <limits>
#include <random>
#include <tuple>
#include <utility>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

llvm::APInt power(llvm::APInt Value, unsigned Exponent) {
  llvm::APInt Result(Value.getBitWidth(), 1);
  for (; Exponent; Exponent >>= 1) {
    if (Exponent & 1)
      Result *= Value;
    Value *= Value;
  }
  return Result;
}

struct Fixture {
  SymRef Input;
  unsigned Width, Kind;
  bool Neighbor;

  llvm::APInt oracle(const std::array<llvm::APInt, 3> &V) const {
    auto Factor = [&](bool Shifted) {
      auto X = V[0] + (Shifted ? 64 + unsigned(Neighbor) : 0);
      auto Y = V[1] + (Shifted ? (Kind == 3 ? 32 : 64) : 0);
      auto Z = V[2] + (Shifted ? (Kind == 3 ? 32 : 128) : 0);
      const auto Mask = ~llvm::APInt(Width, 3);
      auto High = Y & Mask;
      if (Kind == 1 || Kind == 2)
        High = Y & llvm::APInt(Width, Kind == 1 ? 0x7c : 0xbc);
      else if (Kind == 3)
        High = (Y & ~llvm::APInt(Width, 1)) * (Z & ~llvm::APInt(Width, 1));
      else if (Kind == 4)
        High = ((Y + 7) & ~llvm::APInt(Width, 7)) + (Z & Mask);
      if (Kind == 5)
        return X ^ High ^ (Z & Mask) ^ llvm::APInt(Width, Shifted ? 128 : 64);
      return Kind == 6 ? X + High : X ^ High;
    };
    return llvm::APInt(Width, 3).shl(Width - 8) * power(Factor(false), 63) *
           power(Factor(true), 65);
  }
};

Fixture fixture(SymContext &Ctx, unsigned Width, unsigned Kind,
                bool Neighbor = false) {
  auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  auto Z = Ctx.mkVar("z", Width);
  auto C = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
  auto Mask = Ctx.mkConst(~llvm::APInt(Width, 3));
  auto Factor = [&](bool Shifted) {
    auto A = Ctx.mkAdd(X, C(Shifted ? 64 + unsigned(Neighbor) : 0));
    auto B = Ctx.mkAdd(Y, C(Shifted ? (Kind == 3 ? 32 : 64) : 0));
    auto D = Ctx.mkAdd(Z, C(Shifted ? (Kind == 3 ? 32 : 128) : 0));
    auto High = Ctx.mkAnd(B, Mask);
    if (Kind == 1 || Kind == 2)
      High = Ctx.mkAnd(B, C(Kind == 1 ? 0x7c : 0xbc));
    else if (Kind == 3) {
      auto Even = Ctx.mkConst(~llvm::APInt(Width, 1));
      High = Ctx.mkMul(Ctx.mkAnd(B, Even), Ctx.mkAnd(D, Even));
    } else if (Kind == 4)
      High = Ctx.mkAdd(
          Ctx.mkAnd(Ctx.mkAdd(B, C(7)), Ctx.mkConst(~llvm::APInt(Width, 7))),
          Ctx.mkAnd(D, Mask));
    if (Kind == 5)
      return Ctx.mkXor({A, High, Ctx.mkAnd(D, Mask), C(Shifted ? 128 : 64)});
    return Kind == 6 ? Ctx.mkAdd(A, High) : Ctx.mkXor(A, High);
  };
  auto A = Factor(false), B = Factor(true);
  llvm::SmallVector<SymRef, 128> Factors(63, A);
  Factors.append(65, B);
  Factors.push_back(Ctx.mkConst(llvm::APInt(Width, 3).shl(Width - 8)));
  return {Ctx.mkMul(Factors), Width, Kind, Neighbor};
}

TEST(SymMBAWidePairs, CompletedParityProofKeepsReconstructionWithinBudget) {
  constexpr size_t WorkLimit = 9500;
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto F = fixture(Ctx, Width, 3);
    detail::WorkBudget Budget(WorkLimit);
    size_t Storage = 1 << 24;
    const auto Result =
        detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
    ASSERT_NE(Result, F.Input);
    EXPECT_LT(Budget.used(), WorkLimit);
    SymEvalPlan Reduced(Ctx, Result);
    for (unsigned I = 0; I < 256; ++I) {
      const std::array Values{llvm::APInt(Width, I), llvm::APInt(Width, 3 * I),
                              llvm::APInt(Width, 7 * I)};
      EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
    }
  }
}

TEST(SymMBAWidePairs, SharedAllowancesPreserveDecisionWorkAndMemo) {
  constexpr size_t Unlimited = std::numeric_limits<size_t>::max();
  auto Run = [](size_t Limit, size_t Previous, size_t LocalLimit, size_t Bytes,
                bool Memo) {
    SymContext Ctx;
    const auto F = fixture(Ctx, 32, 3);
    detail::WorkBudget Budget(Limit);
    EXPECT_TRUE(Budget.consume(Previous));
    if (Memo) {
      Budget.MemoOwner = &Ctx;
      Budget.MemoCapacity = 16;
    }
    bool Complete = false;
    const auto Result = detail::foldResidueDecisions(
        Ctx, F.Input, Budget, Bytes, true, LocalLimit, nullptr, &Complete);
    SymEvalPlan Plan(Ctx, Result);
    for (unsigned I : {0u, 1u, 7u, 127u, 255u}) {
      const std::array Values{llvm::APInt(32, I), llvm::APInt(32, 3 * I),
                              llvm::APInt(32, 7 * I)};
      EXPECT_EQ(Plan.eval(Values), F.oracle(Values));
    }
    return std::tuple{Ctx.toString(Result),
                      Budget.used() - Previous,
                      Bytes,
                      bool(Budget.Residues),
                      Complete,
                      Budget.exhausted()};
  };
  const auto Complete = Run(65536, 0, 65536, 1 << 24, true);
  const auto Required = std::get<1>(Complete);
  ASSERT_GT(Required, 1u);
  ASSERT_LT(Required, 65536u);
  EXPECT_TRUE(std::get<3>(Complete));
  for (size_t Allowance :
       {size_t(0), size_t(1), size_t(2), size_t(64), size_t(1024), Required - 1,
        Required, Required + 1, size_t(65536)})
    for (size_t Bytes : {size_t(0), size_t(256), size_t(1 << 24)})
      for (bool Memo : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Allowance << ':' << Bytes << ':' << Memo);
        // Previously consumed request work must leave exactly the same local
        // proof, refusal, memo opportunity and remaining storage.
        EXPECT_EQ(Run(Allowance, 0, 65536, Bytes, Memo),
                  Run(Allowance + 37, 37, 65536, Bytes, Memo));
        // An unlimited request still obeys the same local proof allowance.
        EXPECT_EQ(Run(65536, 0, Allowance, Bytes, Memo),
                  Run(Unlimited, 0, Allowance, Bytes, Memo));
      }
  const auto Saturated = Run(Unlimited, Unlimited - 1, 65536, 1 << 24, true);
  EXPECT_EQ(std::get<0>(Saturated), std::get<0>(Complete));
  EXPECT_EQ(std::get<1>(Saturated), 1u);
  EXPECT_EQ(std::get<2>(Saturated), std::get<2>(Complete));
  EXPECT_EQ(std::get<3>(Saturated), std::get<3>(Complete));
  EXPECT_EQ(std::get<4>(Saturated), std::get<4>(Complete));
  EXPECT_FALSE(std::get<5>(Saturated));
}

TEST(SymMBAWidePairs, StandaloneSelectorDoesNotProveOtherFactorsOdd) {
  for (unsigned Width : {8u, 32u, 64u, 257u})
    for (bool Complement : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Complement);
      SymContext Ctx;
      const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      auto Selector = Ctx.mkAnd(X, Ctx.mkOne(Width));
      if (Complement)
        Selector = Ctx.mkXor(Selector, Ctx.mkOne(Width));
      const auto Scale = llvm::APInt(Width, 3).shl(Width - 8);
      llvm::SmallVector<SymRef, 128> Factors{Ctx.mkConst(Scale), Selector};
      Factors.append(127, Y);
      Factors.push_back(Ctx.mkAdd(Y, Ctx.mkConst(Width, 128)));
      const auto Input = Ctx.mkMul(Factors);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, Input, Budget, Storage);
      EXPECT_LE(Budget.used(), 65536u);
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned XV = 0; XV < 4; ++XV)
        for (unsigned Low = 0; Low < 256; ++Low) {
          const auto YV = llvm::APInt(Width, 1).shl(Width - 1) + Low;
          const std::array Values{llvm::APInt(Width, XV), YV};
          const auto Selected = llvm::APInt(Width, (XV & 1) ^ Complement);
          EXPECT_EQ(Reduced.eval(Values),
                    Scale * Selected * power(YV, 127) * (YV + 128));
        }
    }
}

TEST(SymMBAWidePairs, CorrectionsPreserveMasksProductsAndXorCrossTerms) {
  std::mt19937_64 Random(0x57494445434f5252);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Kind = 0; Kind < 7; ++Kind)
      for (bool Neighbor : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Kind << ':' << Neighbor);
        SymContext Ctx;
        auto F = fixture(Ctx, Width, Kind, Neighbor);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        auto Result =
            detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
        if (!Neighbor)
          ASSERT_NE(Result, F.Input);
        EXPECT_LE(Budget.used(), 65536u);
        SymEvalPlan Reduced(Ctx, Result);
        for (unsigned Trial = 0; Trial < 512; ++Trial) {
          std::array<llvm::APInt, 3> Values;
          for (auto &V : Values) {
            V = llvm::APInt(Width, Random());
            for (unsigned Bit = 64; Bit < Width; Bit += 64)
              V |= llvm::APInt(Width, Random()).shl(Bit);
          }
          if (Trial < 256)
            Values = {llvm::APInt(Width, Trial), llvm::APInt(Width, 3 * Trial),
                      llvm::APInt(Width, 7 * Trial)};
          ASSERT_EQ(Reduced.eval(Values), F.oracle(Values));
        }
      }
}

TEST(SymMBAWidePairs, ExhaustsNarrowCorrectionsAndTheirCarryBoundaries) {
  for (unsigned Kind = 0; Kind < 7; ++Kind) {
    SCOPED_TRACE(Kind);
    SymContext Ctx;
    auto F = fixture(Ctx, 8, Kind);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 24;
    auto Result = detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
    ASSERT_NE(Result, F.Input);
    SymEvalPlan Reduced(Ctx, Result);
    for (unsigned X = 0; X < 256; ++X)
      for (unsigned Y = 0; Y < 256; ++Y) {
        const std::array<llvm::APInt, 3> Values{
            llvm::APInt(8, X), llvm::APInt(8, Y), llvm::APInt(8, X + 3 * Y)};
        ASSERT_EQ(Reduced.eval(Values), F.oracle(Values)) << X << ':' << Y;
      }
  }
}

TEST(SymMBAWidePairs, CorrectionsHaveIndependentFullWidthProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Kind : {0u, 1u, 2u, 3u, 5u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Kind);
      SymContext Ctx;
      auto F = fixture(Ctx, Width, Kind);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
      ASSERT_NE(Result, F.Input);
      // This independent 128-factor proof has a wall-clock timeout. Leave
      // room for shared CI runners while still requiring an unsat result;
      // production simplification and its resource limits are unchanged.
      constexpr unsigned ProofTimeoutMs = 30000;
      solver::Z3SolverOptions Options;
      Options.TimeoutMs = ProofTimeoutMs;
      solver::Z3Solver Verifier(Ctx, Options);
      // Each verifier checks one complete formula, allowing one-shot
      // preprocessing instead of treating the difference as an assumption.
      ASSERT_TRUE(Verifier.assertDistinct(F.Input, Result));
      EXPECT_EQ(Verifier.check(), solver::SatResult::Unsat)
          << Verifier.reasonUnknown();
    }
}

TEST(SymMBAWidePairs, RefusalsKeepTheFullWordAndCumulativeBounds) {
  for (auto Limits : {std::pair<size_t, size_t>{0, 1 << 24},
                      {1, 1 << 24},
                      {8192, 1 << 24},
                      {65536, 0},
                      {65536, 256},
                      {65536, 1 << 24}}) {
    SymContext Ctx;
    auto F = fixture(Ctx, 64, 3);
    detail::WorkBudget Budget(Limits.first);
    size_t Storage = Limits.second;
    auto Result = detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
    EXPECT_LE(Budget.used(), Limits.first);
    EXPECT_LE(Storage, Limits.second);
    SymEvalPlan Reduced(Ctx, Result);
    for (unsigned I = 0; I < 256; ++I) {
      const std::array<llvm::APInt, 3> Values{
          llvm::APInt(64, I), llvm::APInt(64, I * 17), llvm::APInt(64, I * 51)};
      EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
    }
  }
}

TEST(SymMBAWidePairs, QuadraticCarryCountsPreserveAllObservedBits) {
  std::mt19937_64 Random(0x51554144434f554e);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Observed : {6u, 7u, 8u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Observed);
      SymContext Ctx;
      auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      const std::array Parts{X, Y};
      const unsigned Split = (Observed + 2) / 3;
      const auto Scale = llvm::APInt(Width, 3).shl(Width - Observed);
      llvm::SmallVector<SymRef, 32> Factors{Ctx.mkConst(Scale)};
      // Odd multiplicities retain both correction bits. Distinct offsets keep
      // this a larger source product rather than repeated identical factors.
      for (unsigned Copy = 0; Copy < 23; ++Copy)
        for (unsigned I = 0; I < Parts.size(); ++I)
          Factors.push_back(Ctx.mkAdd(
              Ctx.mkConst(Width, 1),
              Ctx.mkMul(
                  Ctx.mkConst(Width, uint64_t(1) << Split),
                  Ctx.mkAdd(Parts[I],
                            Ctx.mkConst(Width, Copy * Parts.size() + I)))));
      auto Input = Ctx.mkMul(Factors);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldResidueDecisions(Ctx, Input, Budget, Storage);
      ASSERT_NE(Result, Input) << "work=" << Budget.used()
                               << " cost=" << detail::readingCost(Ctx, Input);
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned Trial = 0; Trial < 512; ++Trial) {
        std::array<llvm::APInt, 2> V;
        for (auto &Value : V) {
          Value = llvm::APInt(Width, Random());
          for (unsigned Bit = 64; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random()).shl(Bit);
        }
        // Exhaust both low residues independently, then retain random
        // complete words to catch accidental narrowing of the scaled result.
        if (Trial < 16)
          V = {llvm::APInt(Width, Trial & 3),
               llvm::APInt(Width, (Trial >> 2) & 3)};
        const auto &Values = V;
        auto Expected = Scale;
        for (unsigned Copy = 0; Copy < 23; ++Copy)
          for (unsigned I = 0; I < Values.size(); ++I)
            Expected *= llvm::APInt(Width, 1) +
                        (Values[I] + Copy * Values.size() + I).shl(Split);
        EXPECT_EQ(Reduced.eval(V), Expected);
      }
      if (Width <= 64 &&
          solver::proofBackendAvailable(solver::ProofBackend::Z3)) {
        solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 10000);
        EXPECT_EQ(Verifier(Ctx, Input, Result), SynthVerification::Equivalent);
      }
    }
}

struct PartialFixture {
  SymRef Input;
  unsigned Width, Degree;
  bool Neighbor;
  unsigned Observed, High, Complement;

  llvm::APInt oracle(const std::array<llvm::APInt, 3> &V) const {
    auto Result = llvm::APInt(Width, 3).shl(Width - Observed);
    const auto Mask = ~llvm::APInt(Width, 1);
    const auto ZMask = ~llvm::APInt(Width, (1u << (High - 1)) - 1);
    const auto X = Complement == 1   ? V[0] ^ llvm::APInt(Width, 1)
                   : Complement == 2 ? ~V[0]
                                     : V[0];
    for (unsigned I = 0; I < Degree; ++I)
      Result *= (X + 2 * I + unsigned(Neighbor && I == 0)) ^
                (((V[1] + I) & Mask) * ((V[2] + 2 * I) & ZMask));
    return Result;
  }
};

PartialFixture partialFixture(SymContext &Ctx, unsigned Width, unsigned Degree,
                              bool Neighbor = false, unsigned Observed = 6,
                              unsigned High = 2, unsigned Complement = 0,
                              bool ReverseConstruction = false) {
  auto X = Ctx.mkVar("x", Width);
  const auto Y = Ctx.mkVar("y", Width);
  const auto Z = Ctx.mkVar("z", Width);
  if (Complement)
    X = Complement == 1 ? Ctx.mkXor(X, Ctx.mkConst(Width, 1)) : Ctx.mkNot(X);
  const auto Mask = Ctx.mkConst(~llvm::APInt(Width, 1));
  const auto ZMask = Ctx.mkConst(~llvm::APInt(Width, (1u << (High - 1)) - 1));
  llvm::SmallVector<SymRef, 32> Factors{
      Ctx.mkConst(llvm::APInt(Width, 3).shl(Width - Observed))};
  for (unsigned Step = 0; Step < Degree; ++Step) {
    const unsigned I = ReverseConstruction ? Degree - 1 - Step : Step;
    auto A =
        Ctx.mkAdd(X, Ctx.mkConst(Width, 2 * I + unsigned(Neighbor && I == 0)));
    auto B = Ctx.mkAnd(Ctx.mkAdd(Y, Ctx.mkConst(Width, I)), Mask);
    auto C = Ctx.mkAnd(Ctx.mkAdd(Z, Ctx.mkConst(Width, 2 * I)), ZMask);
    Factors.push_back(Ctx.mkXor(A, Ctx.mkMul(B, C)));
  }
  return {Ctx.mkMul(Factors), Width, Degree,    Neighbor,
          Observed,           High,  Complement};
}

TEST(SymMBAWidePairs, NarrowNonlinearResiduesKeepTheirCrossTerms) {
  std::mt19937_64 Random(0x4e4152524f575052);
  for (unsigned Width : {8u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Degree : {23u, 30u})
      for (unsigned Complement : {0u, 1u, 2u})
        for (bool Neighbor : {false, true}) {
          SCOPED_TRACE(::testing::Message() << Width << ':' << Degree << ':'
                                            << Complement << ':' << Neighbor);
          SymContext Ctx;
          const auto F =
              partialFixture(Ctx, Width, Degree, Neighbor, 6, 3, Complement);
          detail::WorkBudget Budget(65536);
          size_t Storage = 1 << 24;
          const auto Result =
              detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
          if (Width <= 64 && !Neighbor) {
            ASSERT_NE(Result, F.Input);
            EXPECT_LT(Budget.used(), 32768u);
            EXPECT_LE(2 * detail::readingCost(Ctx, Result),
                      detail::readingCost(Ctx, F.Input));
          }
          EXPECT_LE(Budget.used(), 65536u);
          SymEvalPlan Reduced(Ctx, Result);
          for (unsigned Trial = 0; Trial < 128; ++Trial) {
            std::array<llvm::APInt, 3> Values;
            for (auto &V : Values) {
              V = llvm::APInt(Width, 0);
              for (unsigned Bit = 0; Bit < Width; Bit += 64)
                V |= llvm::APInt(Width, Random()).shl(Bit);
              if (Trial < 3)
                V = Trial == 2 ? llvm::APInt::getAllOnes(Width)
                               : llvm::APInt(Width, Trial);
            }
            ASSERT_EQ(Reduced.eval(Values), F.oracle(Values));
          }
        }
}

TEST(SymMBAWidePairs, ExhaustsNarrowProductSignaturesAndCorrections) {
  SymContext Ctx;
  const auto F = partialFixture(Ctx, 8, 23, false, 6, 3, 1);
  detail::WorkBudget Budget(65536);
  size_t Storage = 1 << 24;
  const auto Result =
      detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
  ASSERT_NE(Result, F.Input);
  SymEvalPlan Reduced(Ctx, Result);
  for (uint64_t X = 0; X < 64; ++X)
    for (uint64_t Y = 0; Y < 64; ++Y)
      for (uint64_t Z = 0; Z < 64; ++Z) {
        uint64_t Expected = 12;
        for (unsigned I = 0; I < 23; ++I)
          Expected *= ((X ^ 1) + 2 * I) ^
                      (((Y + I) & ~uint64_t(1)) * ((Z + 2 * I) & ~uint64_t(3)));
        ASSERT_EQ(Reduced.evalU64({X, Y, Z}), Expected & 255)
            << X << ':' << Y << ':' << Z;
      }
}

TEST(SymMBAWidePairs, PreparedParityPrefixesKeepTheirProductAndContext) {
  auto Run = [](unsigned Width, bool Prepare) {
    SymContext Ctx;
    const auto F = partialFixture(Ctx, Width, 96, false, 6, 3, 1);
    detail::WorkBudget Budget(1 << 20);
    Budget.MemoOwner = &Ctx;
    Budget.MemoCapacity = 16;
    size_t Storage = 1 << 24;
    if (Prepare)
      EXPECT_EQ(
          detail::foldProjectedProductResidues(Ctx, F.Input, Budget, Storage),
          F.Input);
    const auto Before = Budget.used();
    const auto Result =
        detail::foldResidualProductResidues(Ctx, F.Input, Budget, Storage);
    const auto Work = Budget.used() - Before;
    EXPECT_NE(Result, F.Input);
    SymEvalPlan Plan(Ctx, Result);
    for (unsigned I = 0; I < 128; ++I) {
      const std::array V{llvm::APInt(Width, I), llvm::APInt(Width, 3 * I),
                         llvm::APInt(Width, 7 * I)};
      EXPECT_EQ(Plan.eval(V), F.oracle(V));
    }
    return std::pair{Work, detail::readingCost(Ctx, Result)};
  };
  for (unsigned Width : {8u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    const auto Cold = Run(Width, false), Prepared = Run(Width, true);
    EXPECT_LT(Prepared.first, Cold.first);
    EXPECT_LE(Prepared.second, Cold.second);
  }
  for (unsigned Kind = 0; Kind < 4; ++Kind) {
    SCOPED_TRACE(Kind);
    SymContext Ctx, Other;
    const auto F = partialFixture(Ctx, 64, 96, false, 6, 3, 1);
    detail::WorkBudget Budget(1 << 20);
    Budget.MemoOwner = &Ctx;
    Budget.MemoCapacity = 16;
    size_t Storage = 1 << 24;
    EXPECT_EQ(
        detail::foldProjectedProductResidues(Ctx, F.Input, Budget, Storage),
        F.Input);
    auto &Target = Kind == 1 ? Other : Ctx;
    const auto G = Kind < 2 ? partialFixture(Target, Kind == 1 ? 32 : 64, 96,
                                             Kind == 0, 6, 3, 1)
                            : F;
    if (Kind == 1)
      ASSERT_EQ(G.Input.index(), F.Input.index());
    if (Kind == 2)
      Storage = 0;
    if (Kind == 3)
      EXPECT_TRUE(Budget.consume(Budget.available()));
    const auto Result =
        detail::foldResidualProductResidues(Target, G.Input, Budget, Storage);
    if (Kind >= 2)
      EXPECT_EQ(Result, G.Input);
    SymEvalPlan Plan(Target, Result);
    for (unsigned I = 0; I < 128; ++I) {
      const std::array V{llvm::APInt(G.Width, I), llvm::APInt(G.Width, 3 * I),
                         llvm::APInt(G.Width, 7 * I)};
      EXPECT_EQ(Plan.eval(V), G.oracle(V));
    }
  }
}

TEST(SymMBAWidePairs, UnfinishedProductsPreserveIndependentLinearTails) {
  std::mt19937_64 Random(0x4c494e4541525441);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 257u})
    for (unsigned High : {2u, 3u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << High);
      SymContext Ctx;
      const auto F = partialFixture(Ctx, Width, 192, false, 8, High, 1);
      const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      const auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
      const auto Input =
          Ctx.mkAdd({F.Input, Ctx.mkXor(X, Y),
                     Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkAnd(X, Y)),
                     Ctx.mkOr(Z, W), Ctx.mkAnd(Z, W)});
      MBAOptions Options;
      Options.MaxWork = 65536;
      const auto Result = simplifyMBADeep(Ctx, Input, Options);
      ASSERT_TRUE(Result.Changed);
      EXPECT_LE(Result.SizeAfter + 10, Result.SizeBefore);
      EXPECT_LE(Result.Work, Options.MaxWork);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned Trial = 0; Trial < 256; ++Trial) {
        std::array<llvm::APInt, 4> Values;
        for (auto &Value : Values) {
          Value = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random()).shl(Bit);
        }
        Values[0].clearLowBits(8);
        Values[0] |= llvm::APInt(Width, Trial);
        const auto Expected = F.oracle({Values[0], Values[1], Values[2]}) +
                              Values[0] + Values[1] + Values[2] + Values[3];
        ASSERT_EQ(Reduced.eval(Values), Expected);
      }
    }
}

TEST(SymMBAWidePairs,
     BatchedCorrectionsKeepOddCompletionsAndEvenResidualQuality) {
  std::mt19937_64 Random(0x434f525245435453);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Degree : {48u, 65u})
      for (unsigned Complement : {0u, 1u, 2u})
        for (bool Neighbor : {false, true}) {
          SCOPED_TRACE(::testing::Message() << Width << ':' << Degree << ':'
                                            << Complement << ':' << Neighbor);
          SymContext Ctx;
          const auto F =
              partialFixture(Ctx, Width, Degree, Neighbor, Degree == 65 ? 7 : 8,
                             2, Complement, Complement == 2);
          const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
          const auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
          const auto Input =
              Ctx.mkAdd({F.Input, Ctx.mkXor(X, Y),
                         Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkAnd(X, Y)),
                         Ctx.mkOr(Z, W), Ctx.mkAnd(Z, W)});
          MBAOptions Options;
          Options.MaxWork = 65536;
          const auto Result = simplifyMBADeep(Ctx, Input, Options);
          EXPECT_LE(Result.Work, Options.MaxWork);
          EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
          EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
          if (!Neighbor && Width <= 64) {
            if (Degree == 65) {
              EXPECT_LT(Result.Work, Options.MaxWork);
              EXPECT_LT(Result.SizeAfter, Result.SizeBefore / 4);
            } else if (Complement == 2) {
              // A refused partial-even proof must preserve enough of the
              // shared allowance for the established independent rewrites.
              EXPECT_LE(Result.SizeAfter + 19, Result.SizeBefore);
            }
          }
          SymEvalPlan Reduced(Ctx, Result.Expr);
          for (unsigned Trial = 0; Trial < 128; ++Trial) {
            std::array<llvm::APInt, 4> Values;
            for (auto &Value : Values) {
              Value = llvm::APInt(Width, 0);
              for (unsigned Bit = 0; Bit < Width; Bit += 64)
                Value |= llvm::APInt(Width, Random()).shl(Bit);
              if (Trial < 3)
                Value = Trial == 1 ? llvm::APInt::getAllOnes(Width)
                                   : llvm::APInt(Width, Trial == 2);
            }
            const auto Expected = F.oracle({Values[0], Values[1], Values[2]}) +
                                  Values[0] + Values[1] + Values[2] + Values[3];
            ASSERT_EQ(Reduced.eval(Values), Expected);
          }
        }
}

TEST(SymMBAWidePairs, ReassociatedComplementsRetainExactCorrections) {
  std::mt19937_64 Random(0x5349474e41545552);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 257u})
    for (unsigned Degree : {15u, 23u, 31u})
      for (unsigned Complement : {1u, 2u})
        for (bool Neighbor : {false, true}) {
          SCOPED_TRACE(::testing::Message() << Width << ':' << Degree << ':'
                                            << Complement << ':' << Neighbor);
          SymContext Ctx;
          const auto F =
              partialFixture(Ctx, Width, Degree, Neighbor, 6, 2, Complement);
          detail::WorkBudget Budget(65536);
          size_t Storage = 1 << 24;
          const auto Result =
              detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
          if (!Neighbor) {
            ASSERT_NE(Result, F.Input);
            EXPECT_LT(detail::readingCost(Ctx, Result),
                      detail::readingCost(Ctx, F.Input));
          }
          EXPECT_LE(Budget.used(), 65536u);
          SymEvalPlan Reduced(Ctx, Result);
          for (unsigned Trial = 0; Trial < 256; ++Trial) {
            std::array<llvm::APInt, 3> Values;
            for (auto &Value : Values) {
              Value = llvm::APInt(Width, 0);
              for (unsigned Bit = 0; Bit < Width; Bit += 64)
                Value |= llvm::APInt(Width, Random()).shl(Bit);
            }
            Values[0].clearLowBits(8);
            Values[0] |= llvm::APInt(Width, Trial);
            ASSERT_EQ(Reduced.eval(Values), F.oracle(Values));
          }
        }
}

TEST(SymMBAWidePairs, ExhaustsReassociatedComplementLowWords) {
  SymContext Ctx;
  const auto F = partialFixture(Ctx, 8, 15, false, 6, 2, 1);
  detail::WorkBudget Budget(65536);
  size_t Storage = 1 << 24;
  const auto Result =
      detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
  ASSERT_NE(Result, F.Input);
  SymEvalPlan Reduced(Ctx, Result);
  for (uint64_t X = 0; X < 64; ++X)
    for (uint64_t Y = 0; Y < 64; ++Y)
      for (uint64_t Z = 0; Z < 64; ++Z) {
        uint64_t Expected = 12;
        for (unsigned I = 0; I < 15; ++I)
          Expected *= ((X ^ 1) + 2 * I) ^
                      (((Y + I) & ~uint64_t(1)) * ((Z + 2 * I) & ~uint64_t(1)));
        ASSERT_EQ(Reduced.evalU64({X, Y, Z}), Expected & 255)
            << X << ':' << Y << ':' << Z;
      }
}

TEST(SymMBAWidePairs, OddPartialBlocksRetainUnpairedFullWords) {
  std::mt19937_64 Random(0x554e504149524544);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Degree : {15u, 23u, 31u})
      for (bool Neighbor : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Degree << ':' << Neighbor);
        SymContext Ctx;
        const auto F = partialFixture(Ctx, Width, Degree, Neighbor);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        const auto Result =
            detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
        if (!Neighbor) {
          ASSERT_NE(Result, F.Input);
          EXPECT_LT(detail::readingCost(Ctx, Result),
                    detail::readingCost(Ctx, F.Input));
        }
        SymEvalPlan Reduced(Ctx, Result);
        for (unsigned Trial = 0; Trial < 256; ++Trial) {
          std::array<llvm::APInt, 3> Values;
          for (auto &Value : Values) {
            Value = llvm::APInt(Width, 0);
            for (unsigned Bit = 0; Bit < Width; Bit += 64)
              Value |= llvm::APInt(Width, Random()).shl(Bit);
          }
          Values[0].clearLowBits(8);
          Values[0] |= llvm::APInt(Width, Trial);
          ASSERT_EQ(Reduced.eval(Values), F.oracle(Values));
        }
      }
}

TEST(SymMBAWidePairs, ExhaustsOddPartialBlockLowWords) {
  SymContext Ctx;
  const auto F = partialFixture(Ctx, 8, 15);
  detail::WorkBudget Budget(65536);
  size_t Storage = 1 << 24;
  const auto Result =
      detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
  ASSERT_NE(Result, F.Input);
  SymEvalPlan Reduced(Ctx, Result);
  for (uint64_t X = 0; X < 64; ++X)
    for (uint64_t Y = 0; Y < 64; ++Y)
      for (uint64_t Z = 0; Z < 64; ++Z) {
        uint64_t Expected = 12;
        for (unsigned I = 0; I < 15; ++I)
          Expected *= (X + 2 * I) ^
                      (((Y + I) & ~uint64_t(1)) * ((Z + 2 * I) & ~uint64_t(1)));
        ASSERT_EQ(Reduced.evalU64({X, Y, Z}), Expected & 255)
            << X << ':' << Y << ':' << Z;
      }
}

TEST(SymMBAWidePairs, PartialBlockRefusalsKeepOriginalFactors) {
  for (auto Limits : {std::pair<size_t, size_t>{0, 1 << 24},
                      {1, 1 << 24},
                      {8192, 1 << 24},
                      {65536, 0},
                      {65536, 256},
                      {65536, 1 << 24}}) {
    SymContext Ctx;
    const auto F = partialFixture(Ctx, 64, 15);
    detail::WorkBudget Budget(Limits.first);
    size_t Storage = Limits.second;
    const auto Result =
        detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
    EXPECT_LE(Budget.used(), Limits.first);
    EXPECT_LE(Storage, Limits.second);
    SymEvalPlan Reduced(Ctx, Result);
    for (unsigned I = 0; I < 256; ++I) {
      const std::array Values{llvm::APInt(64, I), llvm::APInt(64, 5 * I),
                              llvm::APInt(64, 17 * I)};
      ASSERT_EQ(Reduced.eval(Values), F.oracle(Values));
    }
  }
}

} // namespace
