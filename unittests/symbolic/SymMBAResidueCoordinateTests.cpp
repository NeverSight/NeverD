//===- SymMBAResidueCoordinateTests.cpp - Coordinate tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check coordinate substitutions, isolated polynomial bases, complementary
/// cofactors, and complete-width results retained at resource boundaries.
///
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/solver/Z3Solver.h"

#include "llvm/ADT/SmallVector.h"

#include <array>
#include <utility>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

enum class CoordinateKind { Linear, Equal, ComplementHigh, ComplementLow };

struct CoordinateFixture {
  SymRef Input;
  unsigned Width;
  CoordinateKind Kind;
  bool Neighbor;
  bool Swap;

  llvm::APInt oracle(const std::array<llvm::APInt, 3> &V) const {
    const bool A = V[0][0], B = V[1][0], C = V[0][1], D = V[1][1];
    const bool E = V[2][0], F = V[2][1];
    const bool H = C ^ D ^ E ^ F ^ (C & D) ^ (C & E) ^ (D & F) ^ (C & E & F);
    const bool Delta = (C & D) ^ (E & F) ^ (C & D & F);
    const bool L = (C & D) ^ (E & F) ^ (C & E);
    bool First = false, Second = (E ^ F) & (C | D);
    switch (Kind) {
    case CoordinateKind::Linear:
      // Input uses the expanded ANF. The oracle uses the independently
      // factored coordinate a XOR b; the neighbor changes one monomial.
      First = ((A ^ B) & (H ^ (B & Delta))) ^ (Neighbor && C && D && E && F);
      Second = A & (H ^ (B & Delta));
      break;
    case CoordinateKind::Equal:
      First = L && !A;
      break;
    case CoordinateKind::ComplementHigh:
      First = L || A;
      break;
    case CoordinateKind::ComplementLow:
      First = !L || A;
      break;
    }
    if (Swap)
      std::swap(First, Second);
    const unsigned Word =
        unsigned(First) + 2 * unsigned(Second) + 4 * unsigned(First != Second);
    return llvm::APInt(Width, Word).shl(Width - 5);
  }
};

CoordinateFixture coordinateFixture(SymContext &Ctx, unsigned Width,
                                    CoordinateKind Kind, bool Neighbor = false,
                                    bool Swap = false) {
  const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  const auto Z = Ctx.mkVar("z", Width);
  auto K = [&](uint64_t Value) { return Ctx.mkConst(Width, Value); };
  const auto A = X, B = Y, C = Ctx.mkLShr(X, K(1));
  const auto D = Ctx.mkLShr(Y, K(1)), E = Z, F = Ctx.mkLShr(Z, K(1));
  const auto H = Ctx.mkXor({C, D, E, F, Ctx.mkAnd(C, D), Ctx.mkAnd(C, E),
                            Ctx.mkAnd(D, F), Ctx.mkAnd({C, E, F})});
  const auto Delta =
      Ctx.mkXor({Ctx.mkAnd(C, D), Ctx.mkAnd(E, F), Ctx.mkAnd({C, D, F})});
  const auto L = Ctx.mkXor({Ctx.mkAnd(C, D), Ctx.mkAnd(E, F), Ctx.mkAnd(C, E)});
  SymRef First;
  auto Second = Ctx.mkAnd(Ctx.mkXor(E, F), Ctx.mkOr(C, D));
  switch (Kind) {
  case CoordinateKind::Linear:
    First = Ctx.mkXor({Ctx.mkAnd(A, H), Ctx.mkAnd(B, Ctx.mkXor(H, Delta)),
                       Ctx.mkAnd({A, B, Delta})});
    Second = Ctx.mkXor(Ctx.mkAnd(A, H), Ctx.mkAnd({A, B, Delta}));
    if (Neighbor)
      First = Ctx.mkXor(First, Ctx.mkAnd({C, D, E, F}));
    break;
  case CoordinateKind::Equal:
    First = Ctx.mkXor(L, Ctx.mkAnd(A, L));
    break;
  case CoordinateKind::ComplementHigh:
    First = Ctx.mkXor(L, Ctx.mkAnd(A, Ctx.mkNot(L)));
    break;
  case CoordinateKind::ComplementLow:
    First = Ctx.mkXor(Ctx.mkNot(L), Ctx.mkAnd(A, L));
    break;
  }
  if (Swap)
    std::swap(First, Second);
  auto Bit = [&](SymRef Value) { return Ctx.mkAnd(Value, K(1)); };
  const auto Word = Ctx.mkAdd({Bit(First), Ctx.mkMul(K(2), Bit(Second)),
                               Ctx.mkMul(K(4), Bit(Ctx.mkXor(First, Second))),
                               Ctx.mkMul(K(32), Ctx.mkAdd({X, Y, Z}))});
  const auto Scale = Ctx.mkConst(llvm::APInt(Width, 1).shl(Width - 5));
  return {Ctx.mkMul(Scale, Word), Width, Kind, Neighbor, Swap};
}

std::array<llvm::APInt, 3> coordinateValues(unsigned Width, unsigned Trial) {
  std::array<llvm::APInt, 3> Values;
  for (unsigned I = 0; I < Values.size(); ++I) {
    auto Value = llvm::APInt(Width, 0);
    for (unsigned Bit = 0; Bit < Width; Bit += 64)
      Value |= llvm::APInt(Width,
                           0x9e3779b97f4a7c15ULL * (1 + I + (Trial >> 6) + Bit))
                   .shl(Bit);
    const unsigned Shift = 2 * I;
    const unsigned Mask = 3;
    Values[I] = (Value & ~llvm::APInt(Width, Mask)) |
                llvm::APInt(Width, (Trial >> Shift) & Mask);
  }
  return Values;
}

TEST(SymMBAResidueCoordinates, CoordinateBasisDoesNotLeakToLaterOutputBits) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (bool Neighbor : {false, true})
      for (bool Swap : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Neighbor << ':' << Swap);
        SymContext Ctx;
        const auto F = coordinateFixture(Ctx, Width, CoordinateKind::Linear,
                                         Neighbor, Swap);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        const auto Result =
            detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
        ASSERT_NE(Result, F.Input);
        EXPECT_LT(detail::readingCost(Ctx, Result),
                  detail::readingCost(Ctx, F.Input));
        EXPECT_LT(Budget.used(), 65536u);
        SymEvalPlan Reduced(Ctx, Result), Original(Ctx, F.Input);
        // Every assignment of the six relevant input bits, with four distinct
        // high-word fillings. Adjacent output bits use different bases even
        // when their polynomial keys would coincide after the substitution.
        for (unsigned Trial = 0; Trial < 256; ++Trial) {
          const auto Values = coordinateValues(Width, Trial);
          const auto Expected = F.oracle(Values);
          EXPECT_EQ(Original.eval(Values), Expected) << Trial;
          EXPECT_EQ(Reduced.eval(Values), Expected) << Trial;
        }
        auto Positive = F;
        Positive.Neighbor = false;
        auto Negative = F;
        Negative.Neighbor = true;
        const auto Witness = coordinateValues(Width, 63);
        EXPECT_NE(Positive.oracle(Witness), Negative.oracle(Witness));
      }
}

TEST(SymMBAResidueCoordinates, EqualAndComplementCofactorsUseCompleteWords) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (auto Kind : {CoordinateKind::Equal, CoordinateKind::ComplementHigh,
                      CoordinateKind::ComplementLow}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << unsigned(Kind));
      SymContext Ctx;
      const auto F = coordinateFixture(Ctx, Width, Kind);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
      ASSERT_NE(Result, F.Input);
      SymEvalPlan Reduced(Ctx, Result), Original(Ctx, F.Input);
      for (unsigned Trial = 0; Trial < 256; ++Trial) {
        const auto Values = coordinateValues(Width, Trial);
        EXPECT_EQ(Original.eval(Values), F.oracle(Values)) << Trial;
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values)) << Trial;
      }
    }
}

TEST(SymMBAResidueCoordinates,
     ExactAndShortResourcesRetainOnlyCompleteResults) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext FullCtx;
    const auto F = coordinateFixture(FullCtx, Width, CoordinateKind::Linear);
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
      SCOPED_TRACE(::testing::Message()
                   << Limits.first << ':' << Limits.second);
      SymContext Ctx;
      const auto Fixture =
          coordinateFixture(Ctx, Width, CoordinateKind::Linear);
      detail::WorkBudget Budget(Limits.first);
      size_t Storage = Limits.second;
      const auto Result =
          detail::foldResidueDecisions(Ctx, Fixture.Input, Budget, Storage);
      EXPECT_LE(Budget.used(), Limits.first);
      EXPECT_LE(Storage, Limits.second);
      if (Limits.first <= 1 || !Limits.second)
        EXPECT_EQ(Result, Fixture.Input);
      if (Limits.first == Work && Limits.second == Bytes)
        EXPECT_EQ(Result.index(), Full.index());
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned Trial = 0; Trial < 64; ++Trial) {
        const auto Values = coordinateValues(Width, Trial);
        EXPECT_EQ(Reduced.eval(Values), Fixture.oracle(Values)) << Trial;
      }
    }
  }
}

TEST(SymMBAResidueCoordinates, NativeWidthsHaveIndependentFullWidthProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (auto [Kind, Neighbor] :
         {std::pair{CoordinateKind::Linear, false},
          std::pair{CoordinateKind::Linear, true},
          std::pair{CoordinateKind::Equal, false},
          std::pair{CoordinateKind::ComplementHigh, false},
          std::pair{CoordinateKind::ComplementLow, false}})
      for (bool Swap : {false, true}) {
        SCOPED_TRACE(::testing::Message() << Width << ':' << unsigned(Kind)
                                          << ':' << Neighbor << ':' << Swap);
        SymContext Ctx;
        const auto F = coordinateFixture(Ctx, Width, Kind, Neighbor, Swap);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        const auto Result =
            detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
        ASSERT_NE(Result, F.Input);
        solver::Z3SolverOptions Options;
        Options.TimeoutMs = 10000;
        solver::Z3Solver Verifier(Ctx, Options);
        EXPECT_EQ(Verifier.checkDistinct(F.Input, Result),
                  solver::SatResult::Unsat)
            << Verifier.reasonUnknown();
      }
}

TEST(SymMBAResidueCoordinates, CoordinateEmissionShrinksDenseCarryPolynomials) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto ScaleValue = llvm::APInt(Width, 3).shl(Width - 7);
    const auto Mask = llvm::APInt::getAllOnes(Width).shl(2);
    llvm::SmallVector<SymRef, 128> Factors{Ctx.mkConst(ScaleValue)};
    for (unsigned I = 0; I < 96; ++I)
      Factors.push_back(Ctx.mkXor(
          Ctx.mkAdd(X, Ctx.mkConst(Width, 2 * I)),
          Ctx.mkAnd(Ctx.mkAdd(Y, Ctx.mkConst(Width, I)), Ctx.mkConst(Mask))));
    const auto Input = Ctx.mkMul(Factors);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 24;
    const auto Result =
        detail::foldResidueDecisions(Ctx, Input, Budget, Storage);
    // This fixture is carry corpus case 104 without its unrelated additive
    // tail. The old frequency-only emission costs 178 nodes. At 257
    // bits the same budget may retain the original expression; only complete
    // word semantics, not a successful optimization, is asserted there.
    if (Width <= 128) {
      ASSERT_NE(Result, Input);
      EXPECT_LT(detail::readingCost(Ctx, Result), 166u);
    }
    SymEvalPlan Reduced(Ctx, Result);
    bool HasNonzeroWitness = false;
    for (unsigned Trial = 0; Trial < 256; ++Trial) {
      const auto Filled = coordinateValues(Width, Trial);
      const auto LowMask = llvm::APInt(Width, 127);
      const std::array<llvm::APInt, 2> Values{
          (Filled[0] & ~LowMask) | llvm::APInt(Width, Trial & 127),
          (Filled[1] & ~LowMask) |
              llvm::APInt(Width, (Trial * 73 + (Trial >> 7)) & 127)};
      auto Expected = ScaleValue;
      for (unsigned I = 0; I < 96; ++I)
        Expected *= (Values[0] + llvm::APInt(Width, 2 * I)) ^
                    ((Values[1] + llvm::APInt(Width, I)) & Mask);
      HasNonzeroWitness |= !Expected.isZero();
      EXPECT_EQ(Reduced.eval(Values), Expected) << Trial;
    }
    EXPECT_TRUE(HasNonzeroWitness);
  }
}

} // namespace
