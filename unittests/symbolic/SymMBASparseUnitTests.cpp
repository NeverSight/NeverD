//===- SymMBASparseUnitTests.cpp - Short-prefix product proofs ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check reordered and short-prefix products against independent arithmetic,
/// nearby parity violations, cumulative refusals and full-width SMT proofs.
///
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/solver/Z3Solver.h"
#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

#include "llvm/ADT/SmallString.h"

#include <array>
#include <random>
#include <string>
#include <utility>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

struct SparseProduct {
  unsigned Width;
  bool Complement = false;
  bool Neighbor = false;
  bool Linear = false;
  unsigned Degree = 23;
  unsigned Observed = 7;

  std::string text(bool Tail = true) const {
    llvm::SmallString<80> Literal;
    (llvm::APInt(Width, 5).shl(Width - Observed)).toString(Literal, 10, false);
    std::string Result = "(" + std::string(Literal);
    for (unsigned I = 0; I < Degree; ++I) {
      const auto Offset = std::to_string(2 * I + (Neighbor && I + 1 == Degree));
      const auto Index = std::to_string(I);
      const std::string High = Linear ? "(y&4)"
                                      : "(((y+" + Index + ")&-2)*((z+" +
                                            std::to_string(2 * I) + ")&-2))";
      Result += "*(((" +
                std::string(Linear       ? "x&7"
                            : Complement ? "x^1"
                                         : "x") +
                ")+" + Offset + ")^" + High + ")";
    }
    Result += ")";
    if (Tail)
      Result += "+(x^y)+2*(x&y)+(z|w)+(z&w)";
    return Result;
  }

  llvm::APInt oracle(const std::array<llvm::APInt, 4> &V,
                     bool Tail = true) const {
    auto Product = llvm::APInt(Width, 5).shl(Width - Observed);
    const auto Even = ~llvm::APInt(Width, 1);
    for (unsigned I = 0; I < Degree; ++I) {
      const auto Base = Linear ? V[0] & llvm::APInt(Width, 7) : V[0];
      const auto Low = (Base ^ llvm::APInt(Width, Complement)) + 2 * I +
                       (Neighbor && I + 1 == Degree);
      const auto High = Linear ? V[1] & llvm::APInt(Width, 4)
                               : ((V[1] + I) & Even) * ((V[2] + 2 * I) & Even);
      Product *= Low ^ High;
    }
    return Tail ? Product + V[0] + V[1] + V[2] + V[3] : Product;
  }
};

TEST(SymMBASparseUnits, PermutedProductsCompleteAndReduceTheCofactor) {
  // The oracle uses natural order; the original DAG uses a fixed permutation.
  constexpr std::array<unsigned, 64> Order{
      18, 61, 44, 34, 0,  36, 19, 11, 15, 21, 40, 55, 35, 42, 5,  52,
      31, 10, 63, 17, 50, 47, 22, 60, 20, 14, 49, 53, 43, 28, 23, 39,
      1,  57, 9,  37, 16, 26, 45, 4,  29, 8,  41, 46, 54, 59, 51, 32,
      13, 62, 3,  2,  38, 7,  33, 56, 24, 58, 48, 25, 6,  27, 12, 30};
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const uint64_t Scale = uint64_t(3) << (Width - 8);
    std::string Text = std::to_string(Scale);
    for (auto I : Order)
      Text += "*((x+" + std::to_string(2 * I) + ")^((y+" + std::to_string(I) +
              ")&-8))";
    const auto Parsed = parseSymExpr(C, Text, Width);
    ASSERT_TRUE(Parsed);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 24;
    bool Complete = false;
    const auto Result = detail::foldResidueDecisions(
        C, Parsed.Root, Budget, Storage, false, 65536, nullptr, &Complete);
    ASSERT_TRUE(Complete);
    ASSERT_NE(Result, Parsed.Root);
    EXPECT_LT(detail::readingCost(C, Result),
              detail::readingCost(C, Parsed.Root) / 2);
    EXPECT_LT(Budget.used(), 65536u);
    SymEvalPlan Reduced(C, Result);
    // Exhaust every observed byte independently; also populate the unobserved
    // high bits so a truncation of the full-width result cannot go unnoticed.
    const uint64_t Mask = llvm::APInt::getAllOnes(Width).getZExtValue();
    for (unsigned X = 0; X < 256; ++X)
      for (unsigned Y = 0; Y < 256; ++Y) {
        const uint64_t VX = uint64_t(X) | (uint64_t(X ^ Y) << 40);
        const uint64_t VY = uint64_t(Y) | (uint64_t(3 * X + Y) << 32);
        uint64_t Expected = Scale;
        for (unsigned I = 0; I < 64; ++I)
          Expected *= (VX + 2 * I) ^ ((VY + I) & ~uint64_t(7));
        ASSERT_EQ(Reduced.evalU64({VX, VY}), Expected & Mask);
      }
  }
}

TEST(SymMBASparseUnits, ShortPrefixPowersKeepCorrelatedBitsAndExponents) {
  for (unsigned Width : {8u, 64u})
    for (unsigned Degree = 0; Degree <= 65; ++Degree)
      for (unsigned Mask : {7u, 15u}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Degree << ':' << Mask);
        SymContext C;
        const uint64_t Scale = uint64_t(1) << (Width - 8);
        std::string Text = std::to_string(Scale);
        for (unsigned I = 0; I < Degree; ++I)
          Text += "*((((x^(x>>1))&" + std::to_string(Mask) + ")|1))";
        const auto Parsed = parseSymExpr(C, Text, Width);
        ASSERT_TRUE(Parsed);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        const auto Result =
            detail::foldResidueDecisions(C, Parsed.Root, Budget, Storage);
        SymEvalPlan Reduced(C, Result);
        const auto WordMask = llvm::APInt::getAllOnes(Width).getZExtValue();
        for (uint64_t X = 0; X < 256; ++X) {
          uint64_t Expected = Scale;
          for (unsigned I = 0; I < Degree; ++I)
            Expected *= ((X ^ (X >> 1)) & Mask) | 1;
          ASSERT_EQ(Reduced.evalU64({X}), Expected & WordMask);
        }
      }
}

TEST(SymMBASparseUnits, RefusalsAndNearbyParitiesKeepEveryWordBit) {
  std::mt19937_64 Random(0x535041525345554e);
  for (unsigned Width : {8u, 32u, 64u, 65u, 128u, 257u})
    for (bool Neighbor : {false, true})
      for (const auto [Work, Bytes] : {std::pair<size_t, size_t>{0, 1 << 24},
                                       {1, 1 << 24},
                                       {8192, 1 << 24},
                                       {65536, 0},
                                       {65536, 256},
                                       {65536, 1 << 24}}) {
        SCOPED_TRACE(::testing::Message() << Width << ':' << Neighbor << ':'
                                          << Work << ':' << Bytes);
        SymContext C;
        const SparseProduct F{Width, true, Neighbor};
        const auto Parsed = parseSymExpr(C, F.text(false), Width);
        ASSERT_TRUE(Parsed);
        detail::WorkBudget Budget(Work + 37);
        ASSERT_TRUE(Budget.consume(37));
        auto Storage = Bytes;
        const auto Result = detail::foldResidueDecisions(C, Parsed.Root, Budget,
                                                         Storage, true, Work);
        EXPECT_LE(Budget.used(), Work + 37);
        EXPECT_LE(Storage, Bytes);
        SymEvalPlan Plan(C, Result);
        for (unsigned Trial = 0; Trial < 128; ++Trial) {
          std::array<llvm::APInt, 4> Values;
          for (auto &V : Values) {
            V = llvm::APInt(Width, Random());
            for (unsigned Bit = 64; Bit < Width; Bit += 64)
              V |= llvm::APInt(Width, Random()).shl(Bit);
          }
          ASSERT_EQ(Plan.eval(Values), F.oracle(Values, false));
        }
      }
}

TEST(SymMBASparseUnits, CompletedCofactorsHaveIndependentFullWidthProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const SparseProduct F{Width, false, false, true};
    const auto Parsed = parseSymExpr(C, F.text(false), Width);
    ASSERT_TRUE(Parsed);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 24;
    const auto Result =
        detail::foldResidueDecisions(C, Parsed.Root, Budget, Storage);
    ASSERT_NE(Result, Parsed.Root);
    solver::Z3SolverOptions Options;
    Options.TimeoutMs = 30000;
    solver::Z3Solver Proof(C, Options);
    ASSERT_TRUE(Proof.assertDistinct(Parsed.Root, Result));
    EXPECT_EQ(Proof.check(), solver::SatResult::Unsat) << Proof.reasonUnknown();
  }
}

TEST(SymMBASparseUnits, RecursiveSquaresPreserveCompleteMixedFactors) {
  std::mt19937_64 Random(0x5351554152454437);
  for (unsigned Width : {8u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Degree : {25u, 31u, 49u, 63u})
      for (bool Complement : {false, true})
        for (bool Neighbor : {false, true}) {
          SCOPED_TRACE(::testing::Message() << Width << ':' << Degree << ':'
                                            << Complement << ':' << Neighbor);
          SymContext C;
          SparseProduct F{Width, Complement, Neighbor};
          F.Degree = Degree;
          const auto Parsed = parseSymExpr(C, F.text(false), Width);
          ASSERT_TRUE(Parsed);
          detail::WorkBudget Budget(65536);
          size_t Storage = 1 << 24;
          const auto Result =
              detail::foldResidueDecisions(C, Parsed.Root, Budget, Storage);
          EXPECT_LE(Budget.used(), 65536u);
          // Native widths must improve within the default allowance. Wider
          // words also pay for limb operations and may retain their source;
          // the independent full-word oracle checks both outcomes.
          if (!Neighbor && Degree >= 49 && Width <= 64) {
            ASSERT_NE(Result, Parsed.Root);
            EXPECT_LT(detail::readingCost(C, Result),
                      3 * detail::readingCost(C, Parsed.Root) / 4);
          }
          SymEvalPlan Reduced(C, Result);
          for (unsigned Trial = 0; Trial < 128; ++Trial) {
            std::array<llvm::APInt, 4> Values;
            for (auto &V : Values) {
              V = llvm::APInt(Width, 0);
              for (unsigned Bit = 0; Bit < Width; Bit += 64)
                V |= llvm::APInt(Width, Random()).shl(Bit);
            }
            ASSERT_EQ(Reduced.eval(Values), F.oracle(Values, false));
          }
        }
}

TEST(SymMBASparseUnits, SinglePairedRepresentativeNeedsNoNewPrefixProbe) {
  for (unsigned Width : {16u, 32u, 64u}) {
    SymContext C;
    SparseProduct F{Width, true};
    F.Degree = 33;
    F.Observed = 8;
    const auto Parsed = parseSymExpr(C, F.text(), Width);
    ASSERT_TRUE(Parsed);
    MBAOptions Options;
    Options.MaxWork = 65536;
    const auto Result = simplifyMBADeep(C, Parsed.Root, Options);
    EXPECT_LT(Result.Work, Options.MaxWork);
    EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
    SymEvalPlan Reduced(C, Result.Expr);
    for (unsigned I = 0; I < 256; ++I) {
      const std::array Values{llvm::APInt(Width, I), llvm::APInt(Width, 3 * I),
                              llvm::APInt(Width, 7 * I),
                              llvm::APInt(Width, 11 * I)};
      EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
    }
  }
}

TEST(SymMBASparseUnits, ExhaustsSevenBitInputsOfRetainedRecursiveSquares) {
  SymContext C;
  SparseProduct F{8, true};
  F.Degree = 49;
  const auto Parsed = parseSymExpr(C, F.text(false), 8);
  ASSERT_TRUE(Parsed);
  detail::WorkBudget Budget(65536);
  size_t Storage = 1 << 24;
  const auto Result =
      detail::foldResidueDecisions(C, Parsed.Root, Budget, Storage);
  ASSERT_NE(Result, Parsed.Root);
  SymEvalPlan Reduced(C, Result);
  for (uint64_t X = 0; X < 128; ++X)
    for (uint64_t Y = 0; Y < 128; ++Y)
      for (uint64_t Z = 0; Z < 128; ++Z) {
        const auto VX = X | ((Y & 1) << 7);
        const auto VY = Y | ((Z & 1) << 7);
        const auto VZ = Z | ((X & 1) << 7);
        uint64_t Expected = 10;
        for (unsigned I = 0; I < 49; ++I)
          Expected *= ((VX ^ 1) + 2 * I) ^ (((VY + I) & ~uint64_t(1)) *
                                            ((VZ + 2 * I) & ~uint64_t(1)));
        ASSERT_EQ(Reduced.evalU64({VX, VY, VZ}), Expected & 255);
      }
}

} // namespace
