//===- SymMBAAffineResidueTests.cpp - Affine residue proof oracles --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check scaled factors and nonzero residue synthesis against independent
/// modular arithmetic, exhaustive small words and full-width SMT proofs.
///
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/solver/Z3Solver.h"

#include <array>
#include <bit>
#include <random>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

llvm::APInt randomWord(unsigned Width, std::mt19937_64 &Random) {
  llvm::APInt Result(Width, Random());
  for (unsigned Bit = 64; Bit < Width; Bit += 64)
    Result |= llvm::APInt(Width, Random()).shl(Bit);
  return Result;
}

struct Fixture {
  SymRef Input;
  llvm::APInt Scale;
  std::vector<std::pair<llvm::APInt, llvm::APInt>> Factors;
  unsigned Kind;

  llvm::APInt oracle(const std::array<llvm::APInt, 3> &V) const {
    auto Base = V[0];
    if (Kind == 1)
      Base += 3 * V[1] + 5 * V[2];
    else if (Kind == 2)
      Base = (V[0] + V[1]) ^ V[2];
    else if (Kind == 3)
      Base *= V[1];
    else if (Kind == 4)
      Base += 3 * (V[1] + 5 * V[2]);
    auto Result = Scale;
    for (const auto &[A, B] : Factors)
      Result *= A * Base + B;
    return Result;
  }
};

Fixture fixture(SymContext &Ctx, unsigned Width, unsigned Degree, unsigned Kind,
                unsigned Control = 0) {
  std::mt19937_64 Random(0x414646494e45 + 17 * Width + Degree + Kind);
  auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  auto Z = Ctx.mkVar("z", Width);
  auto Base = X;
  if (Kind == 2)
    Base = Ctx.mkXor(Ctx.mkAdd(X, Y), Z);
  else if (Kind == 3)
    Base = Ctx.mkMul(X, Y);
  else if (Kind == 4)
    Base = Ctx.mkAdd(
        X, Ctx.mkMul(Ctx.mkConst(Width, 3),
                     Ctx.mkAdd(Y, Ctx.mkMul(Ctx.mkConst(Width, 5), Z))));
  const unsigned Fixed = Control == 2 ? 1 : 0;
  const unsigned Zeros = Degree - std::popcount(Degree) + Fixed;
  Fixture Out{{},
              llvm::APInt(Width, 1).shl(Width > Zeros ? Width - Zeros : 0),
              {},
              Kind};
  llvm::SmallVector<SymRef, 64> Factors{Ctx.mkConst(Out.Scale)};
  for (unsigned I = 0; I < Degree; ++I) {
    auto A = randomWord(Width, Random) | 1;
    if (!I && Fixed)
      A <<= Fixed;
    const unsigned Offset =
        Control ? (Control == 1 && I + 1 == Degree ? I - 1 : I) : 11 + 5 * I;
    auto B = A * Offset;
    if (!I && Control == 2)
      ++B;
    auto Term = Ctx.mkMul(Ctx.mkConst(A), Base);
    if (Kind == 1)
      Term = Ctx.mkAdd({Ctx.mkMul(Ctx.mkConst(A), X),
                        Ctx.mkMul(Ctx.mkConst(3 * A), Y),
                        Ctx.mkMul(Ctx.mkConst(5 * A), Z)});
    Factors.push_back(Ctx.mkAdd(Term, Ctx.mkConst(B)));
    Out.Factors.emplace_back(std::move(A), std::move(B));
  }
  Out.Input = Ctx.mkMul(Factors);
  return Out;
}

TEST(SymMBAAffineResidue, RemovesIndependentUnitsAcrossWidthsAndBases) {
  std::mt19937_64 Random(0x41464650524f4f46);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Degree : {4u, 8u, 16u, 32u, 64u, 128u, 256u})
      for (unsigned Kind = 0; Kind < 5; ++Kind) {
        SymContext Ctx;
        auto F = fixture(Ctx, Width, Degree, Kind);
        MBAOptions Opts;
        Opts.MaxWork = 65536;
        Opts.VerifySamples = 0;
        auto Result = simplifyMBADeep(Ctx, F.Input, Opts);
        ASSERT_TRUE(Ctx.isConstZero(Result.Expr))
            << Width << ':' << Degree << ':' << Kind << ':' << Result.Work;
        EXPECT_LT(Result.Work, Opts.MaxWork);
        EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
        for (unsigned Trial = 0; Trial < 16; ++Trial) {
          std::array<llvm::APInt, 3> Values;
          for (auto &V : Values)
            V = randomWord(Width, Random);
          auto Want = F.oracle(Values);
          EXPECT_TRUE(Want.isZero());
          EXPECT_EQ(Ctx.eval(F.Input, Values), Want);
        }
      }
}

TEST(SymMBAAffineResidue, SynthesizesNonzeroNeighborsWithoutExhaustingWork) {
  std::mt19937_64 Random(0x4e4f4e5a45524f);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Control : {1u, 2u}) {
      SymContext Ctx;
      auto F = fixture(Ctx, Width, 8, 1, Control);
      MBAOptions Opts;
      Opts.MaxWork = 65536;
      Opts.VerifySamples = 0;
      auto Result = simplifyMBADeep(Ctx, F.Input, Opts);
      ASSERT_LT(Result.Work, Opts.MaxWork) << Width << ':' << Control;
      ASSERT_LT(Result.SizeAfter, Result.SizeBefore);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      bool Nonzero = false;
      for (unsigned I = 0; I < 512; ++I) {
        std::array<llvm::APInt, 3> V;
        for (auto &Word : V)
          Word = randomWord(Width, Random);
        auto Want = F.oracle(V);
        Nonzero |= !Want.isZero();
        EXPECT_EQ(Ctx.eval(F.Input, V), Want);
        EXPECT_EQ(Ctx.eval(Result.Expr, V), Want);
      }
      EXPECT_TRUE(Nonzero);
    }
}

TEST(SymMBAAffineResidue, ExhaustsSmallWordsAndIndependentFactorSlopes) {
  for (unsigned Slopes = 0; Slopes < 256; ++Slopes)
    for (unsigned Layout = 0; Layout < 4; ++Layout)
      for (unsigned Scale : {1u, 2u, 4u, 8u}) {
        SymContext Ctx;
        auto X = Ctx.mkVar("x", 4);
        llvm::SmallVector<SymRef, 8> Factors{Ctx.mkConst(4, Scale)};
        std::array<unsigned, 4> A{}, B{};
        for (unsigned I = 0; I < 4; ++I) {
          A[I] = 1 + 2 * ((Slopes >> (2 * I)) & 3);
          const unsigned Offset = Layout == 1 && I == 3 ? 2 : I;
          if (Layout == 2 && !I)
            A[I] *= 2;
          B[I] = A[I] * Offset + (Layout == 2 && !I);
          if (Layout == 3 && I == 3)
            ++A[I];
          Factors.push_back(Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(4, A[I]), X),
                                      Ctx.mkConst(4, B[I])));
        }
        auto Input = Ctx.mkMul(Factors);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 20;
        auto Output = detail::foldProductResidues(Ctx, Input, Budget, Storage);
        for (uint64_t V = 0; V < 16; ++V) {
          uint64_t Want = Scale;
          for (unsigned I = 0; I < 4; ++I)
            Want = Want * (A[I] * V + B[I]) & 15;
          EXPECT_EQ(Ctx.evalU64(Output, {V}), Want);
        }
        if (!Layout && Scale >= 2)
          EXPECT_TRUE(Ctx.isConstZero(Output)) << Slopes << ':' << Scale;
      }
}

TEST(SymMBAAffineResidue, ChecksExtraMissingAndPerturbedCoordinates) {
  std::mt19937_64 Random(0x554e494f4e);
  for (unsigned Width : {8u, 32u, 65u, 257u})
    for (unsigned Variant = 0; Variant < 3; ++Variant) {
      SymContext Ctx;
      auto F = fixture(Ctx, Width, 8, 1, 1);
      auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
      auto Base = Ctx.mkAdd({X, Ctx.mkMul(Ctx.mkConst(Width, 3), Y),
                             Ctx.mkMul(Ctx.mkConst(Width, 5), Z)});
      llvm::SmallVector<SymRef, 16> Factors{Ctx.mkConst(F.Scale)};
      for (unsigned I = 0; I < F.Factors.size(); ++I) {
        const auto &[A, B] = F.Factors[I];
        auto Term = Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(A), Base), Ctx.mkConst(B));
        if (I == 3) {
          auto Extra = Variant == 0   ? W
                       : Variant == 1 ? Ctx.mkMul(Ctx.mkConst(-5 * A), Z)
                                      : Ctx.mkMul(Ctx.mkConst(Width, 16), Y);
          Term = Ctx.mkAdd(Term, Extra);
        }
        Factors.push_back(Term);
      }
      auto Input = Ctx.mkMul(Factors);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Output = detail::foldProductResidues(Ctx, Input, Budget, Storage);
      bool Nonzero = false;
      for (unsigned I = 0; I < 1024; ++I) {
        std::array<llvm::APInt, 4> V;
        for (auto &Word : V)
          Word = randomWord(Width, Random);
        auto B = V[0] + 3 * V[1] + 5 * V[2];
        auto Want = F.Scale;
        for (unsigned J = 0; J < F.Factors.size(); ++J) {
          auto T = F.Factors[J].first * B + F.Factors[J].second;
          if (J == 3)
            T += Variant == 0   ? V[3]
                 : Variant == 1 ? -5 * F.Factors[J].first * V[2]
                                : 16 * V[1];
          Want *= T;
        }
        Nonzero |= !Want.isZero();
        EXPECT_EQ(Ctx.eval(Input, V), Want);
        EXPECT_EQ(Ctx.eval(Output, V), Want);
      }
      EXPECT_TRUE(Nonzero);
    }
}

TEST(SymMBAAffineResidue, RequiresCompleteWorkAndStorageForEveryResult) {
  for (unsigned Control : {0u, 1u, 2u}) {
    SymContext Ctx;
    auto F = fixture(Ctx, 257, 8, 1, Control);
    detail::WorkBudget Full(65536);
    size_t Storage = 1 << 24;
    const auto Output =
        detail::foldProductResidues(Ctx, F.Input, Full, Storage);
    ASSERT_NE(Output, F.Input);
    const size_t Bytes = (1 << 24) - Storage;
    ASSERT_GT(Full.used(), 1u);
    ASSERT_GT(Bytes, 1u);
    for (size_t Work : {size_t(0), size_t(1), Full.used() - 1}) {
      detail::WorkBudget Budget(Work);
      size_t Remaining = 1 << 24;
      EXPECT_EQ(detail::foldProductResidues(Ctx, F.Input, Budget, Remaining),
                F.Input);
      EXPECT_LE(Budget.used(), Work);
    }
    for (size_t Limit : {size_t(0), size_t(1), Bytes - 1}) {
      detail::WorkBudget Budget(65536);
      size_t Remaining = Limit;
      EXPECT_EQ(detail::foldProductResidues(Ctx, F.Input, Budget, Remaining),
                F.Input);
    }
    detail::WorkBudget Exact(Full.used());
    Storage = Bytes;
    EXPECT_EQ(detail::foldProductResidues(Ctx, F.Input, Exact, Storage),
              Output);
  }
}

TEST(SymMBAAffineResidue, HasIndependentFullWidthSMTProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Control : {0u, 1u, 2u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Control);
      SymContext Ctx;
      auto F = fixture(Ctx, Width, Control ? 8 : 4, 1, Control);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Output = detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
      ASSERT_NE(Output, F.Input) << Width << ":" << Control;
      // Full-width products with independent coefficients can need more
      // wall-clock time on shared runners. Keep the original proof query and
      // require unsat; this timeout does not change simplification budgets.
      constexpr unsigned ProofTimeoutMs = 30000;
      solver::Z3SolverOptions Options;
      Options.TimeoutMs = ProofTimeoutMs;
      solver::Z3Solver Verifier(Ctx, Options);
      EXPECT_EQ(Verifier.checkDistinct(F.Input, Output),
                solver::SatResult::Unsat)
          << Verifier.reasonUnknown();
    }
}

} // namespace
