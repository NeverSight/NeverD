//===- SymMBAWitnessTests.cpp - Nonzero product witness regressions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check that concrete nonzero witnesses avoid futile zero searches without
/// granting rewrites, losing high bits, or expanding cumulative budgets.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"

#include <array>
#include <random>
#include <utility>

using namespace neverd::symbolic;

namespace {

struct Fixture {
  SymRef Input;
  unsigned Width, HighBits, Degree;
  bool Neighbor;
  bool Complement;

  llvm::APInt oracle(const std::array<llvm::APInt, 4> &V) const {
    auto Product = llvm::APInt(Width, 3).shl(Width - 6);
    const auto LeftMask = ~llvm::APInt(Width, 1);
    const auto RightMask = ~llvm::APInt::getLowBitsSet(Width, HighBits - 1);
    for (unsigned I = 0; I < Degree; ++I) {
      const auto Base = (Complement ? ~V[0] : V[0] ^ llvm::APInt(Width, 1)) +
                        2 * I + unsigned(Neighbor && I == 0);
      const auto High = ((V[1] + I) & LeftMask) * ((V[2] + 2 * I) & RightMask);
      Product *= Base ^ High;
    }
    return Product + V[0] + V[1] + V[2] + V[3];
  }
};

Fixture fixture(SymContext &Ctx, unsigned Width, unsigned HighBits,
                bool Neighbor = false, unsigned Degree = 96,
                bool Complement = false) {
  const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  const auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
  auto C = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
  const auto LeftMask = Ctx.mkConst(~llvm::APInt(Width, 1));
  const auto RightMask =
      Ctx.mkConst(~llvm::APInt::getLowBitsSet(Width, HighBits - 1));
  llvm::SmallVector<SymRef, 128> Factors{
      Ctx.mkConst(llvm::APInt(Width, 3).shl(Width - 6))};
  for (unsigned Step = 0; Step < Degree; ++Step) {
    const auto I = Complement ? Degree - 1 - Step : Step;
    const auto Base = Ctx.mkAdd(Complement ? Ctx.mkNot(X) : Ctx.mkXor(X, C(1)),
                                C(2 * I + unsigned(Neighbor && I == 0)));
    const auto High = Ctx.mkMul(Ctx.mkAnd(Ctx.mkAdd(Y, C(I)), LeftMask),
                                Ctx.mkAnd(Ctx.mkAdd(Z, C(2 * I)), RightMask));
    Factors.push_back(Ctx.mkXor(Base, High));
  }
  const auto Input = Ctx.mkAdd({Ctx.mkMul(Factors), Ctx.mkXor(X, Y),
                                Ctx.mkMul(C(2), Ctx.mkAnd(X, Y)),
                                Ctx.mkOr(Z, W), Ctx.mkAnd(Z, W)});
  return {Input, Width, HighBits, Degree, Neighbor, Complement};
}

TEST(SymMBAWitness, CompletedPrefixesRetainBudgetForLargeNonzeroProducts) {
  std::mt19937_64 Random(0x5052454649585749);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned High : {2u, 3u})
      for (bool Complement : {false, true})
        for (bool Neighbor : {false, true}) {
          SCOPED_TRACE(::testing::Message() << Width << ':' << High << ':'
                                            << Complement << ':' << Neighbor);
          SymContext Ctx;
          const auto F = fixture(Ctx, Width, High, Neighbor, 256, Complement);
          MBAOptions Options;
          Options.MaxWork = 65536;
          Options.VerifySamples = 0;
          const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
          if (Width <= 64 && !Neighbor) {
            ASSERT_LT(Result.Work, Options.MaxWork);
            EXPECT_LE(Result.SizeAfter, 20u);
          }
          EXPECT_LE(Result.Work, Options.MaxWork);
          EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
          EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
          SymEvalPlan Reduced(Ctx, Result.Expr);
          for (unsigned Trial = 0; Trial < 128; ++Trial) {
            std::array<llvm::APInt, 4> Values;
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

TEST(SymMBAWitness, ComplementedProductsFinishWithinTheExistingAllowance) {
  std::mt19937_64 Random(0x5749544e455353);
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (const auto [Degree, HighBits] :
         {std::pair{96u, 2u}, std::pair{96u, 3u}, std::pair{128u, 3u}}) {
      SCOPED_TRACE(::testing::Message()
                   << Width << ':' << Degree << ':' << HighBits);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, HighBits, false, Degree);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
      ASSERT_TRUE(Result.Changed);
      EXPECT_LT(Result.Work, Options.MaxWork);
      EXPECT_LE(Result.SizeAfter, 20u);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned I = 0; I < 4096; ++I) {
        std::array<llvm::APInt, 4> Values;
        for (auto &V : Values)
          V = llvm::APInt(Width, Random());
        if (I < 256)
          Values = {llvm::APInt(Width, I), llvm::APInt(Width, 3 * I),
                    llvm::APInt(Width, 7 * I), llvm::APInt(Width, 11 * I)};
        ASSERT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

TEST(SymMBAWitness, ExhaustsObservedCarryInputsIndependently) {
  for (unsigned HighBits : {2u, 3u}) {
    SymContext Ctx;
    const auto F = fixture(Ctx, 8, HighBits);
    MBAOptions Options;
    Options.MaxWork = 65536;
    Options.VerifySamples = 0;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    ASSERT_LT(Result.SizeAfter, Result.SizeBefore);
    SymEvalPlan Reduced(Ctx, Result.Expr);
    for (uint64_t X = 0; X < 64; ++X)
      for (uint64_t Y = 0; Y < 64; ++Y)
        for (uint64_t Z = 0; Z < 64; ++Z) {
          uint64_t Expected = 12;
          for (unsigned I = 0; I < 96; ++I) {
            const auto High = ((Y + I) & ~uint64_t(1)) *
                              ((Z + 2 * I) & (~uint64_t(0) << (HighBits - 1)));
            Expected *= ((X ^ 1) + 2 * I) ^ High;
          }
          const uint64_t W = X ^ Y ^ Z;
          Expected = (Expected + X + Y + Z + W) & 255;
          ASSERT_EQ(Reduced.evalU64({X, Y, Z, W}), Expected)
              << HighBits << ':' << X << ':' << Y << ':' << Z;
        }
  }
}

TEST(SymMBAWitness, NearMissesWideInputsAndRefusalsKeepOriginalSemantics) {
  std::mt19937_64 Random(0x5749544e4553534e);
  for (unsigned Width : {8u, 64u, 65u, 257u})
    for (bool Neighbor : {false, true})
      for (size_t Allowance :
           {size_t(0), size_t(1), size_t(4096), size_t(65536)}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Neighbor << ':' << Allowance);
        SymContext Ctx;
        const auto F = fixture(Ctx, Width, 2, Neighbor);
        MBAOptions Options;
        Options.MaxWork = Allowance;
        Options.VerifySamples = 0;
        const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
        EXPECT_LE(Result.Work, Allowance);
        EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
        SymEvalPlan Reduced(Ctx, Result.Expr);
        for (unsigned I = 0; I < 256; ++I) {
          std::array<llvm::APInt, 4> Values;
          for (auto &V : Values) {
            V = llvm::APInt(Width, Random());
            for (unsigned Bit = 64; Bit < Width; Bit += 64)
              V |= llvm::APInt(Width, Random()).shl(Bit);
          }
          if (I < 2)
            Values.fill(llvm::APInt(Width, I));
          ASSERT_EQ(Reduced.eval(Values), F.oracle(Values));
        }
      }
}

TEST(SymMBAWitness, EvenFactorsStillSupplyConcreteNonzeroWitnesses) {
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Degree : {48u, 65u, 96u})
      for (unsigned High : {2u, 3u}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Degree << ':' << High);
        SymContext Ctx;
        const auto F = fixture(Ctx, Width, High, true, Degree);
        MBAOptions Options;
        Options.MaxWork = 65536;
        Options.VerifySamples = 0;
        const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
        ASSERT_LT(Result.Work, Options.MaxWork);
        ASSERT_LT(Result.SizeAfter, Result.SizeBefore / 2);
        EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
        SymEvalPlan Reduced(Ctx, Result.Expr);
        for (unsigned X = 0; X < 256; ++X)
          for (unsigned Y : {0u, 1u, 2u, 127u}) {
            const std::array Values{
                llvm::APInt(Width, X), llvm::APInt(Width, Y),
                llvm::APInt(Width, X ^ Y), llvm::APInt(Width, 7 * X + Y)};
            ASSERT_EQ(Reduced.eval(Values), F.oracle(Values));
          }
        const std::array Zero{llvm::APInt(Width, 0), llvm::APInt(Width, 0),
                              llvm::APInt(Width, 0), llvm::APInt(Width, 0)};
        EXPECT_FALSE(F.oracle(Zero).isZero());
      }
}

} // namespace
