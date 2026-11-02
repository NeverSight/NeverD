//===- SymMBAPrefixResidueTests.cpp - Low-prefix product proof oracles ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check full-word product proofs through independently perturbed high bits.
/// Separate arithmetic oracles, nonzero controls and resource boundaries
/// protect the distinction between a zero prefix and a zero complete word.
///
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"

#include <algorithm>
#include <array>
#include <bit>
#include <random>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

llvm::APInt word(unsigned Width, std::mt19937_64 &Random) {
  llvm::APInt Value(Width, Random());
  for (unsigned I = 64; I < Width; I += 64)
    Value |= llvm::APInt(Width, Random()).shl(I);
  return Value;
}

struct Fixture {
  SymRef Input;
  llvm::APInt Scale;
  std::vector<std::pair<llvm::APInt, llvm::APInt>> Factors;
  unsigned Bits;
  unsigned Kind;

  llvm::APInt oracle(const std::array<llvm::APInt, 5> &V) const {
    const unsigned Width = Scale.getBitWidth();
    auto Mask = llvm::APInt::getLowBitsSet(Width, Bits);
    auto Base = V[0] + 3 * V[1] + 5 * V[2];
    auto High = V[3].shl(Bits);
    auto Other = V[4].shl(Bits);
    auto Result = Scale;
    for (const auto &[A, B] : Factors) {
      auto F = A * (Kind == 3 ? Base & Mask : Base) + B;
      if (Kind == 0 || Kind == 3)
        F ^= High;
      else if (Kind == 1)
        F |= High;
      else if (Kind == 2)
        F &= High | Mask;
      else if (Kind == 4)
        F += High | Other;
      else if (Kind == 5)
        F ^= High | Mask;
      else if (Kind == 6 || Kind == 7)
        F &= Mask;
      else if (Kind == 8)
        F = F.trunc(Bits).sextOrTrunc(Width);
      else if (Kind == 9)
        F += (V[3] * V[4]).shl(Bits);
      Result *= F;
    }
    return Result;
  }
};

Fixture fixture(SymContext &Ctx, unsigned Width, unsigned Degree, unsigned Kind,
                unsigned Control = 0, unsigned PrefixBits = 0) {
  std::mt19937_64 Random(0x505245464958 + Width + 17 * Degree + 31 * Kind);
  auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  auto Z = Ctx.mkVar("z", Width), U = Ctx.mkVar("u", Width);
  auto V = Ctx.mkVar("v", Width);
  const unsigned Bits =
      PrefixBits ? PrefixBits
                 : std::min(Width, unsigned(std::bit_width(Degree - 1)));
  unsigned Zeros = 0;
  for (unsigned B = 1; B <= Bits; ++B)
    Zeros += Degree >> B;
  auto Base = Ctx.mkAdd({X, Ctx.mkMul(Ctx.mkConst(Width, 3), Y),
                         Ctx.mkMul(Ctx.mkConst(Width, 5), Z)});
  const auto Mask = Ctx.mkConst(llvm::APInt::getLowBitsSet(Width, Bits));
  auto High = Ctx.mkShl(U, Ctx.mkConst(Width, Bits));
  auto Other = Ctx.mkShl(V, Ctx.mkConst(Width, Bits));
  Fixture Out{{},
              llvm::APInt(Width, 1).shl(Width > Zeros ? Width - Zeros : 0),
              {},
              Bits,
              Kind};
  if (Control == 2)
    ++Out.Scale;
  llvm::SmallVector<SymRef, 64> Factors{Ctx.mkConst(Out.Scale)};
  for (unsigned I = 0; I < Degree; ++I) {
    auto A = word(Width, Random) | 1;
    auto B = A * (Control == 1 && I + 1 == Degree ? I - 1 : I);
    if (Kind == 5)
      --B; // Complement then gives -A*(Base+I), not an unrelated offset.
    auto F = Ctx.mkAdd(
        Ctx.mkMul(Ctx.mkConst(A), Kind == 3 ? Ctx.mkAnd(Base, Mask) : Base),
        Ctx.mkConst(B));
    if (Kind == 0 || Kind == 3)
      F = Ctx.mkXor(F, High);
    else if (Kind == 1)
      F = Ctx.mkOr(F, High);
    else if (Kind == 2)
      F = Ctx.mkAnd(F, Ctx.mkOr(High, Mask));
    else if (Kind == 4)
      F = Ctx.mkAdd(F, Ctx.mkOr(High, Other));
    else if (Kind == 5)
      F = Ctx.mkXor(F, Ctx.mkOr(High, Mask));
    else if (Kind == 6)
      F = Ctx.mkAnd(F, Mask);
    else if (Kind == 7)
      F = Ctx.mkZExt(Ctx.mkExtract(F, 0, Bits), Width);
    else if (Kind == 8)
      F = Ctx.mkSExt(Ctx.mkExtract(F, 0, Bits), Width);
    else if (Kind == 9)
      F = Ctx.mkAdd(F, Ctx.mkShl(Ctx.mkMul(U, V), Ctx.mkConst(Width, Bits)));
    Factors.push_back(F);
    Out.Factors.emplace_back(std::move(A), std::move(B));
  }
  Out.Input = Ctx.mkMul(Factors);
  return Out;
}

TEST(SymMBAPrefixResidue, ProvesPerturbedHighDegreeProductsAcrossWidths) {
  std::mt19937_64 Random(0x5052454649584f52);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Degree : {4u, 8u, 16u, 32u, 64u, 128u, 256u})
      for (unsigned Kind = 0; Kind < 9; ++Kind) {
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
          std::array<llvm::APInt, 5> V;
          for (auto &Value : V)
            Value = word(Width, Random);
          auto Want = F.oracle(V);
          ASSERT_TRUE(Want.isZero()) << Width << ':' << Degree << ':' << Kind;
          EXPECT_EQ(Ctx.eval(F.Input, V), Want);
        }
      }
}

TEST(SymMBAPrefixResidue, RetainsNonzeroOffsetsAndCoefficientNeighbors) {
  std::mt19937_64 Random(0x4e45494748424f52);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Kind = 0; Kind < 9; ++Kind)
      for (unsigned Control : {1u, 2u}) {
        // A complete set of explicitly truncated factors contains a literal
        // zero for every input regardless of the outer coefficient.
        if (Control == 2 && Kind >= 6)
          continue;
        SymContext Ctx;
        auto F = fixture(Ctx, Width, 4, Kind, Control);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        auto Result =
            detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
        bool Nonzero = false;
        for (unsigned Trial = 0; Trial < 256; ++Trial) {
          std::array<llvm::APInt, 5> V;
          for (auto &Value : V)
            Value = word(Width, Random);
          const auto Want = F.oracle(V);
          Nonzero |= !Want.isZero();
          EXPECT_EQ(Ctx.eval(F.Input, V), Want);
          EXPECT_EQ(Ctx.eval(Result, V), Want);
        }
        EXPECT_TRUE(Nonzero) << Width << ':' << Kind << ':' << Control;
      }
}

TEST(SymMBAPrefixResidue, ProvesCoarsePrefixesWithoutAssumingTheHigherBits) {
  std::mt19937_64 Random(0x434f415253455052);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Degree : {4u, 16u, 64u, 256u})
      for (unsigned Bits : {1u, 2u, 3u}) {
        if (Bits >= std::bit_width(Degree - 1))
          continue;
        for (unsigned Kind = 0; Kind < 10; ++Kind) {
          SymContext Ctx;
          auto F = fixture(Ctx, Width, Degree, Kind, 0, Bits);
          detail::WorkBudget Budget(65536);
          size_t Storage = 1 << 24;
          auto Output =
              detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
          ASSERT_TRUE(Ctx.isConstZero(Output))
              << Width << ':' << Degree << ':' << Bits << ':' << Kind;
          EXPECT_LT(Budget.used(), 65536u);
          for (unsigned Trial = 0; Trial < 8; ++Trial) {
            std::array<llvm::APInt, 5> V;
            for (auto &Value : V)
              Value = word(Width, Random);
            const auto Want = F.oracle(V);
            ASSERT_TRUE(Want.isZero());
            EXPECT_EQ(Ctx.eval(F.Input, V), Want);
          }
        }
      }
}

TEST(SymMBAPrefixResidue, RereadsEvenZeroPrefixesAndKeepsNonzeroNeighbors) {
  std::mt19937_64 Random(0x4556454e50524546);
  const std::array<unsigned, 4> Units{2, 3, 10, 7};
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Control = 0; Control < 3; ++Control) {
      SymContext Ctx;
      auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      auto U = Ctx.mkVar("u", Width);
      auto Base = Ctx.mkAdd(X, Ctx.mkMul(Ctx.mkConst(Width, 3), Y));
      auto Scale = llvm::APInt(Width, 1).shl(Width - 4);
      if (Control == 2)
        ++Scale;
      llvm::SmallVector<SymRef, 8> Factors{Ctx.mkConst(Scale)};
      for (unsigned I = 0; I < 4; ++I) {
        auto Offset = Control == 1 && I == 3 ? 2 : I;
        auto F = Ctx.mkMul(Ctx.mkConst(Width, Units[I]),
                           Ctx.mkAdd(Base, Ctx.mkConst(Width, Offset)));
        Factors.push_back(
            Ctx.mkXor(F, Ctx.mkShl(U, Ctx.mkConst(Width, 1 + (I % 2 == 0)))));
      }
      auto Input = Ctx.mkMul(Factors);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Output =
          detail::foldProjectedProductResidues(Ctx, Input, Budget, Storage);
      if (!Control)
        ASSERT_TRUE(Ctx.isConstZero(Output)) << Width;
      bool Nonzero = false;
      for (unsigned Trial = 0; Trial < 256; ++Trial) {
        std::array<llvm::APInt, 3> V;
        for (auto &Value : V)
          Value = word(Width, Random);
        auto B = V[0] + 3 * V[1];
        auto Want = Scale;
        for (unsigned I = 0; I < 4; ++I) {
          const auto Offset = Control == 1 && I == 3 ? 2 : I;
          Want *= (Units[I] * (B + Offset)) ^ V[2].shl(1 + (I % 2 == 0));
        }
        Nonzero |= !Want.isZero();
        EXPECT_EQ(Ctx.eval(Input, V), Want);
        EXPECT_EQ(Ctx.eval(Output, V), Want);
      }
      EXPECT_EQ(Nonzero, Control != 0) << Width << ':' << Control;
      if (Control || Width != 64)
        continue;
      const size_t Work = Budget.used(), Bytes = (1 << 24) - Storage;
      for (unsigned Short = 0; Short < 3; ++Short) {
        detail::WorkBudget Exact(Work - (Short == 1));
        size_t Remaining = Bytes - (Short == 2);
        auto Repeated =
            detail::foldProjectedProductResidues(Ctx, Input, Exact, Remaining);
        EXPECT_EQ(Repeated, Short ? Input : Output);
      }
    }
}

TEST(SymMBAPrefixResidue, KeepsNativeNonzeroAffineRecoveryWithinBudget) {
  std::mt19937_64 Random(0x414646494e454e5a);
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SymContext Ctx;
    auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
    auto Scale = llvm::APInt(Width, 1).shl(Width - 8);
    std::vector<llvm::APInt> Units;
    llvm::SmallVector<SymRef, 16> Factors{Ctx.mkConst(Scale)};
    for (unsigned I = 0; I < 8; ++I) {
      auto A = (word(Width, Random) | 1).shl(I == 0);
      auto B = I ? A * I : llvm::APInt(Width, 1);
      Units.push_back(A);
      Factors.push_back(
          Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(A), X), Ctx.mkConst(B)));
    }
    auto Input = Ctx.mkAdd({Ctx.mkMul(Factors), Ctx.mkXor(X, Y),
                            Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkAnd(X, Y)),
                            Ctx.mkOr(Z, W), Ctx.mkAnd(Z, W)});
    MBAOptions Opts;
    Opts.MaxWork = 65536;
    Opts.VerifySamples = 0;
    auto Output = simplifyMBADeep(Ctx, Input, Opts);
    EXPECT_LT(Output.Work, Opts.MaxWork) << Width;
    bool Nonzero = false;
    for (unsigned Trial = 0; Trial < 128; ++Trial) {
      std::array<llvm::APInt, 4> V;
      for (auto &Value : V)
        Value = word(Width, Random);
      auto Product = Scale;
      for (unsigned I = 0; I < 8; ++I)
        Product *= Units[I] * V[0] + (I ? Units[I] * I : llvm::APInt(Width, 1));
      Nonzero |= !Product.isZero();
      auto Want = Product + V[0] + V[1] + V[2] + V[3];
      EXPECT_EQ(Ctx.eval(Input, V), Want);
      EXPECT_EQ(Ctx.eval(Output.Expr, V), Want);
    }
    EXPECT_TRUE(Nonzero) << Width;
  }
}

TEST(SymMBAPrefixResidue, ExhaustsSmallWordSlopesMasksAndPerturbations) {
  for (unsigned Slope : {1u, 3u, 5u, 7u})
    for (unsigned Mask : {3u, 7u, 11u, 15u})
      for (unsigned Layout = 0; Layout < 4; ++Layout) {
        SymContext Ctx;
        auto X = Ctx.mkVar("x", 4), U = Ctx.mkVar("u", 4);
        llvm::SmallVector<SymRef, 8> Factors{Ctx.mkConst(4, 2)};
        std::array<unsigned, 4> Offsets{};
        for (unsigned I = 0; I < 4; ++I) {
          Offsets[I] = Layout == 1 && I == 3 ? 2 : I;
          auto F = Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(4, Slope), X),
                             Ctx.mkConst(4, Slope * Offsets[I]));
          if (Layout == 2)
            F = Ctx.mkAnd(F, Ctx.mkOr(Ctx.mkConst(4, Mask), U));
          else
            F = Ctx.mkXor(F, Ctx.mkShl(U, Ctx.mkConst(4, Layout == 3 ? 1 : 2)));
          Factors.push_back(F);
        }
        auto Input = Ctx.mkMul(Factors);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        auto Output = detail::foldProductResidues(Ctx, Input, Budget, Storage);
        for (uint64_t A = 0; A < 16; ++A)
          for (uint64_t B = 0; B < 16; ++B) {
            uint64_t Want = 2;
            for (auto Offset : Offsets) {
              auto F = (Slope * (A + Offset)) & 15;
              F = Layout == 2 ? F & (Mask | B)
                              : F ^ ((B << (Layout == 3 ? 1 : 2)) & 15);
              Want = Want * F & 15;
            }
            EXPECT_EQ(Ctx.evalU64(Input, {A, B}), Want);
            EXPECT_EQ(Ctx.evalU64(Output, {A, B}), Want);
          }
      }
}

TEST(SymMBAPrefixResidue, DoesNotPromoteZeroPrefixesToZeroWords) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SymContext Ctx;
    auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    auto High = ~llvm::APInt::getLowBitsSet(Width, 2);
    auto Odd = Ctx.mkOr(Y, Ctx.mkConst(Width, 1));
    auto Input = Ctx.mkMul(
        {Ctx.mkAnd(Ctx.mkAdd(X, Ctx.mkConst(Width, 1)), Ctx.mkConst(High)), Odd,
         Odd, Odd});
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 20;
    auto Output = detail::foldProductResidues(Ctx, Input, Budget, Storage);
    EXPECT_EQ(Output, Input);
    const std::array<llvm::APInt, 2> Values{llvm::APInt(Width, 3),
                                            llvm::APInt(Width, 0)};
    EXPECT_EQ(Ctx.eval(Output, Values), llvm::APInt(Width, 4));
  }
}

TEST(SymMBAPrefixResidue, CompleteAffineBasesKeepPolynomialRecoveryBudget) {
  std::mt19937_64 Random(0x434f4d504c455445);
  const std::array<std::array<unsigned, 4>, 3> Units{
      {{1, 1, 1, 1}, {141, 75, 249, 107}, {119, 201, 135, 135}}};
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (const auto &Row : Units) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Row[0]);
      SymContext Ctx;
      auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
      const auto Scale = llvm::APInt(Width, 33).shl(Width - 8);
      auto Base = Ctx.mkXor(Ctx.mkAdd(X, Y), Z);
      llvm::SmallVector<SymRef, 8> Factors{Ctx.mkConst(Scale)};
      for (unsigned I = 0; I < Row.size(); ++I)
        Factors.push_back(Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(Width, Row[I]), Base),
                                    Ctx.mkConst(Width, Row[I] * I)));
      auto Input = Ctx.mkAdd({Ctx.mkMul(Factors), Ctx.mkXor(X, Y),
                              Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkAnd(X, Y)),
                              Ctx.mkOr(Z, W), Ctx.mkAnd(Z, W)});
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      auto Result = simplifyMBADeep(Ctx, Input, Options);
      ASSERT_LT(Result.Work, Options.MaxWork);
      EXPECT_LT(detail::readingScore(Ctx, Result.Expr),
                detail::readingScore(Ctx, Input));
      if (Width == 8)
        EXPECT_LE(Result.SizeAfter, Row[0] == 1 ? 39u : 38u);
      for (unsigned Trial = 0; Trial < 256; ++Trial) {
        const std::array<llvm::APInt, 4> Values{
            word(Width, Random), word(Width, Random), word(Width, Random),
            word(Width, Random)};
        auto Expected = Scale;
        const auto B = (Values[0] + Values[1]) ^ Values[2];
        for (unsigned I = 0; I < Row.size(); ++I)
          Expected *= Row[I] * (B + llvm::APInt(Width, I));
        for (const auto &Value : Values)
          Expected += Value;
        EXPECT_EQ(Ctx.eval(Input, Values), Expected);
        EXPECT_EQ(Ctx.eval(Result.Expr, Values), Expected);
      }
    }
}

TEST(SymMBAPrefixResidue, VariableMonomialsKeepTheirAffineFastPath) {
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    llvm::SmallVector<SymRef, 256> Factors;
    for (unsigned I = 0; I < 256; ++I) {
      auto Unit = llvm::APInt(Width, 2 * I + 1);
      Factors.push_back(
          Ctx.mkAdd(Ctx.mkMul({Ctx.mkConst(Unit), X, Y}),
                    Ctx.mkConst(Unit * llvm::APInt(Width, 11 + 5 * I))));
    }
    auto Input = Ctx.mkMul(Factors);
    MBAOptions Options;
    Options.MaxWork = 18000;
    Options.VerifySamples = 0;
    auto Result = simplifyMBADeep(Ctx, Input, Options);
    EXPECT_TRUE(Ctx.isConstZero(Result.Expr));
    EXPECT_LT(Result.Work, Options.MaxWork);
    for (unsigned I = 0; I < 256; ++I) {
      const std::array<llvm::APInt, 2> Values{llvm::APInt(Width, I),
                                              llvm::APInt(Width, 2 * I + 1)};
      auto Product = llvm::APInt(Width, 1);
      for (unsigned J = 0; J < 256; ++J)
        Product *= llvm::APInt(Width, 2 * J + 1) *
                   (Values[0] * Values[1] + llvm::APInt(Width, 11 + 5 * J));
      EXPECT_TRUE(Product.isZero());
      EXPECT_EQ(Ctx.eval(Input, Values), Product);
      EXPECT_EQ(Ctx.eval(Result.Expr, Values), Product);
    }
  }
}

TEST(SymMBAPrefixResidue, RequiresCompleteWorkAndStorage) {
  SymContext Ctx;
  auto F = fixture(Ctx, 257, 8, 2);
  detail::WorkBudget Full(65536);
  size_t Storage = 1 << 24;
  ASSERT_TRUE(Ctx.isConstZero(
      detail::foldProjectedProductResidues(Ctx, F.Input, Full, Storage)));
  const size_t Bytes = (1 << 24) - Storage;
  ASSERT_GT(Full.used(), 1u);
  ASSERT_GT(Bytes, 1u);
  for (size_t Work : {size_t(0), size_t(1), Full.used() - 1}) {
    detail::WorkBudget Budget(Work);
    size_t Remaining = 1 << 24;
    EXPECT_EQ(
        detail::foldProjectedProductResidues(Ctx, F.Input, Budget, Remaining),
        F.Input);
    EXPECT_LE(Budget.used(), Work);
  }
  for (size_t Limit : {size_t(0), size_t(1), Bytes - 1}) {
    detail::WorkBudget Budget(65536);
    size_t Remaining = Limit;
    EXPECT_EQ(
        detail::foldProjectedProductResidues(Ctx, F.Input, Budget, Remaining),
        F.Input);
  }
  detail::WorkBudget Exact(Full.used());
  Storage = Bytes;
  EXPECT_TRUE(Ctx.isConstZero(
      detail::foldProjectedProductResidues(Ctx, F.Input, Exact, Storage)));
}

TEST(SymMBAPrefixResidue, SynthesizesNonzeroProjectedWordsAndMixedWidths) {
  std::mt19937_64 Random(0x50524f4a454354);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned SourceWidth : {4u, Width, 257u})
      for (auto Op : {SymOp::And, SymOp::Or, SymOp::Xor}) {
        SymContext Ctx;
        auto X = Ctx.mkVar("x", SourceWidth), Y = Ctx.mkVar("y", SourceWidth);
        auto U = Ctx.mkVar("u", Width), V = Ctx.mkVar("v", Width);
        auto Base = Ctx.mkAdd(
            Ctx.mkZExtOrTrunc(X, Width),
            Ctx.mkMul(Ctx.mkConst(Width, 3), Ctx.mkZExtOrTrunc(Y, Width)));
        auto Scale = llvm::APInt(Width, 1).shl(Width - 3);
        llvm::SmallVector<SymRef, 8> Factors{Ctx.mkConst(Scale)};
        const unsigned Units[] = {3, 5, 7, 11};
        for (unsigned I = 0; I < 4; ++I) {
          auto A = Ctx.mkShl(Ctx.mkMul(Ctx.mkConst(Width, 2 * I + 1), U),
                             Ctx.mkConst(Width, 2));
          auto B = Ctx.mkShl(V, Ctx.mkConst(Width, 2));
          auto High = Op == SymOp::And  ? Ctx.mkAnd(A, B)
                      : Op == SymOp::Or ? Ctx.mkOr(A, B)
                                        : Ctx.mkXor(A, B);
          Factors.push_back(Ctx.mkAdd(
              {Ctx.mkMul(Ctx.mkConst(Width, Units[I]), Base),
               Ctx.mkConst(Width, Units[I] * std::min(I, 2u)), High}));
        }
        // A source optimizer can absorb the outer scale into the first
        // additive factor. Keep that exact association in this fixture.
        auto First = Factors[1];
        llvm::SmallVector<SymRef, 8> Scaled;
        llvm::SmallVector<SymRef, 8> FirstTerms(Ctx.operands(First));
        for (auto Term : FirstTerms)
          Scaled.push_back(Ctx.mkMul(Ctx.mkConst(Scale), Term));
        Factors[1] = Ctx.mkAdd(Scaled);
        Factors.erase(Factors.begin());
        auto Input = Ctx.mkMul(Factors);
        MBAOptions Opts;
        Opts.MaxWork = 65536;
        Opts.VerifySamples = 0;
        auto Result = simplifyMBADeep(Ctx, Input, Opts);
        ASSERT_LT(Result.Work, Opts.MaxWork)
            << Width << ':' << SourceWidth << ':' << unsigned(Op);
        EXPECT_LT(detail::readingScore(Ctx, Result.Expr),
                  detail::readingScore(Ctx, Input));
        EXPECT_EQ(Ctx.width(Result.Expr), Width);
        bool Nonzero = false;
        for (unsigned Trial = 0; Trial < 128; ++Trial) {
          const std::array<llvm::APInt, 4> Values{
              word(SourceWidth, Random), word(SourceWidth, Random),
              word(Width, Random), word(Width, Random)};
          auto Want = Scale;
          auto Plain =
              Values[0].zextOrTrunc(Width) + 3 * Values[1].zextOrTrunc(Width);
          for (unsigned I = 0; I < 4; ++I) {
            auto A = ((2 * I + 1) * Values[2]).shl(2);
            auto B = Values[3].shl(2);
            auto High = Op == SymOp::And  ? A & B
                        : Op == SymOp::Or ? A | B
                                          : A ^ B;
            Want *= Units[I] * Plain + Units[I] * std::min(I, 2u) + High;
          }
          Nonzero |= !Want.isZero();
          EXPECT_EQ(Ctx.eval(Input, Values), Want);
          EXPECT_EQ(Ctx.eval(Result.Expr, Values), Want);
        }
        EXPECT_TRUE(Nonzero);
        if (Width == 32 && SourceWidth == Width && Op == SymOp::Or) {
          detail::WorkBudget Full(65536);
          size_t Storage = 1 << 24;
          auto Complete =
              detail::foldAffineProductResidues(Ctx, Input, Full, Storage);
          ASSERT_TRUE(Complete && *Complete != Input);
          const size_t Bytes = (1 << 24) - Storage;
          ASSERT_GT(Full.used(), 1u);
          ASSERT_GT(Bytes, 1u);
          for (bool ShortWork : {false, true}) {
            detail::WorkBudget Short(Full.used() - ShortWork);
            Storage = Bytes - !ShortWork;
            auto Refused =
                detail::foldAffineProductResidues(Ctx, Input, Short, Storage);
            ASSERT_TRUE(Refused);
            EXPECT_EQ(*Refused, Input);
          }
          detail::WorkBudget Exact(Full.used());
          Storage = Bytes;
          auto Repeated =
              detail::foldAffineProductResidues(Ctx, Input, Exact, Storage);
          ASSERT_TRUE(Repeated);
          EXPECT_EQ(*Repeated, *Complete);
        }
        if (Width <= 64 && SourceWidth == Width &&
            solver::proofBackendAvailable(solver::ProofBackend::Z3)) {
          solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
          EXPECT_EQ(Verifier(Ctx, Input, Result.Expr),
                    SynthVerification::Equivalent);
        }
      }
}

TEST(SymMBAPrefixResidue, HasIndependentFullWidthSMTProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Kind : {0u, 2u, 5u, 8u})
      for (unsigned Prefix : {0u, 1u}) {
        SymContext Ctx;
        auto F = fixture(Ctx, Width, 4, Kind, 0, Prefix);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        auto Output =
            detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
        ASSERT_TRUE(Ctx.isConstZero(Output)) << Width << ':' << Kind;
        solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
        EXPECT_EQ(Verifier(Ctx, F.Input, Output), SynthVerification::Equivalent)
            << Width << ':' << Kind;
      }
}

struct DistributedFixture {
  SymRef Input;
  unsigned Degree;
  unsigned Kind;
  unsigned Control;
  llvm::APInt Scale;

  llvm::APInt oracle(const std::array<llvm::APInt, 5> &V) const {
    const unsigned Width = Scale.getBitWidth();
    const auto One = llvm::APInt(Width, 1);
    const auto Low = llvm::APInt(Width, 3);
    auto Product = Scale;
    for (unsigned I = 0; I < Degree; ++I) {
      auto U = V[2] + 2 * I, W = V[3] + 2 * I;
      llvm::APInt High(Width, 0);
      switch (Kind) {
      case 0:
        High = (U & ~One) * (W & ~One);
        break;
      case 1:
        High = (U | One) * (W | One) - One;
        break;
      case 2:
        High = (U | Low) * (W | Low) + Low;
        break;
      case 3:
        High = (U & ~One) * (W & ~One) * (V[4] & ~One);
        break;
      case 4:
        High = ~((U | One) * (W | One));
        break;
      case 5:
        High = ((U & ~One) ^ (W & ~One)) * (V[4] & ~One);
        break;
      case 6:
        High = ((U & ~One) | (W & ~One)) * (V[4] & ~One);
        break;
      default:
        High = ((U & ~Low) + (W & ~Low)) * (V[4] | One);
        break;
      }
      if (Control == 1 && I + 1 == Degree)
        High += One;
      auto Base = V[0] + 3 * V[1] + I;
      Product *= Kind & 1 ? Base + High : Base ^ High;
    }
    return Product;
  }
};

DistributedFixture distributedFixture(SymContext &Ctx, unsigned Width,
                                      unsigned Degree, unsigned Kind,
                                      unsigned Control = 0) {
  auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  auto U = Ctx.mkVar("u", Width), V = Ctx.mkVar("v", Width);
  auto W = Ctx.mkVar("w", Width);
  auto C = [&](uint64_t Value) { return Ctx.mkConst(Width, Value); };
  auto One = C(1), Low = C(3);
  auto Even = Ctx.mkConst(~llvm::APInt(Width, 1));
  auto Four = Ctx.mkConst(~llvm::APInt(Width, 3));
  const unsigned Bits = Kind == 1 || Kind == 4 ? 1 : Kind == 3 ? 3 : 2;
  unsigned Zeros = 0;
  for (unsigned B = 1; B <= Bits; ++B)
    Zeros += Degree >> B;
  auto Scale = llvm::APInt(Width, 1).shl(Width > Zeros ? Width - Zeros : 0);
  if (Control == 2)
    ++Scale;
  llvm::SmallVector<SymRef, 64> Factors{Ctx.mkConst(Scale)};
  for (unsigned I = 0; I < Degree; ++I) {
    auto A = Ctx.mkAdd(U, C(2 * I)), B = Ctx.mkAdd(V, C(2 * I));
    auto AE = Ctx.mkAnd(A, Even), BE = Ctx.mkAnd(B, Even);
    SymRef High;
    switch (Kind) {
    case 0:
      High = Ctx.mkMul(AE, BE);
      break;
    case 1:
      High = Ctx.mkSub(Ctx.mkMul(Ctx.mkOr(A, One), Ctx.mkOr(B, One)), One);
      break;
    case 2:
      High = Ctx.mkAdd(Ctx.mkMul(Ctx.mkOr(A, Low), Ctx.mkOr(B, Low)), Low);
      break;
    case 3:
      High = Ctx.mkMul({AE, BE, Ctx.mkAnd(W, Even)});
      break;
    case 4:
      High = Ctx.mkNot(Ctx.mkMul(Ctx.mkOr(A, One), Ctx.mkOr(B, One)));
      break;
    case 5:
      High = Ctx.mkMul(Ctx.mkXor(AE, BE), Ctx.mkAnd(W, Even));
      break;
    case 6:
      High = Ctx.mkMul(Ctx.mkOr(AE, BE), Ctx.mkAnd(W, Even));
      break;
    default:
      High = Ctx.mkMul(Ctx.mkAdd(Ctx.mkAnd(A, Four), Ctx.mkAnd(B, Four)),
                       Ctx.mkOr(W, One));
      break;
    }
    if (Control == 1 && I + 1 == Degree)
      High = Ctx.mkAdd(High, One);
    auto Base = Ctx.mkAdd({X, Ctx.mkMul(C(3), Y), C(I)});
    Factors.push_back(Kind & 1 ? Ctx.mkAdd(Base, High) : Ctx.mkXor(Base, High));
  }
  return {Ctx.mkMul(Factors), Degree, Kind, Control, Scale};
}

TEST(SymMBAPrefixResidue,
     CombinesPartialKnownBitsAcrossArithmeticAndBitwiseNodes) {
  std::mt19937_64 Random(0x5041525449414c);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Kind = 0; Kind < 8; ++Kind)
      for (unsigned Degree : {4u, 16u, 64u}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Kind << ':' << Degree);
        SymContext Ctx;
        auto F = distributedFixture(Ctx, Width, Degree, Kind);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        auto Result =
            detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
        ASSERT_TRUE(Ctx.isConstZero(Result));
        for (unsigned Trial = 0; Trial < 32; ++Trial) {
          const std::array<llvm::APInt, 5> Values{
              word(Width, Random), word(Width, Random), word(Width, Random),
              word(Width, Random), word(Width, Random)};
          auto Expected = F.oracle(Values);
          EXPECT_TRUE(Expected.isZero());
          EXPECT_EQ(Ctx.eval(F.Input, Values), Expected);
        }
      }
}

TEST(SymMBAPrefixResidue,
     PartialKnownBitsPreserveNonzeroNeighborsAndResourceBounds) {
  for (unsigned Width : {8u, 32u, 65u, 257u})
    for (unsigned Kind = 0; Kind < 8; ++Kind) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Kind);
      for (unsigned Control : {1u, 2u}) {
        SymContext Ctx;
        auto F = distributedFixture(Ctx, Width, 4, Kind, Control);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        auto Result =
            detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
        EXPECT_FALSE(Ctx.isConstZero(Result));
        bool Witness = false;
        for (unsigned X = 0; X < 16; ++X) {
          const std::array<llvm::APInt, 5> Values{
              llvm::APInt(Width, X), llvm::APInt(Width, 1),
              llvm::APInt(Width, 2), llvm::APInt(Width, 4),
              llvm::APInt(Width, 6)};
          auto Expected = F.oracle(Values);
          Witness |= !Expected.isZero();
          EXPECT_EQ(Ctx.eval(F.Input, Values), Expected);
          EXPECT_EQ(Ctx.eval(Result, Values), Expected);
        }
        EXPECT_TRUE(Witness);
      }
      SymContext Ctx;
      auto F = distributedFixture(Ctx, Width, 4, Kind);
      detail::WorkBudget Full(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldProductResidues(Ctx, F.Input, Full, Storage);
      ASSERT_TRUE(Ctx.isConstZero(Result));
      const size_t Bytes = (1 << 24) - Storage;
      for (bool ShortWork : {false, true}) {
        detail::WorkBudget Short(Full.used() - ShortWork);
        Storage = Bytes - !ShortWork;
        EXPECT_EQ(detail::foldProductResidues(Ctx, F.Input, Short, Storage),
                  F.Input);
      }
      detail::WorkBudget Exact(Full.used());
      Storage = Bytes;
      EXPECT_EQ(detail::foldProductResidues(Ctx, F.Input, Exact, Storage),
                Result);
    }
}

TEST(SymMBAPrefixResidue,
     PartialKnownBitProductsHaveIndependentFullWidthProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Kind : {0u, 1u, 2u, 5u, 6u, 7u}) {
      SymContext Ctx;
      auto F = distributedFixture(Ctx, Width, 4, Kind);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
      ASSERT_TRUE(Ctx.isConstZero(Result));
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
      EXPECT_EQ(Verifier(Ctx, F.Input, Result), SynthVerification::Equivalent)
          << Width << ':' << Kind;
    }
}

struct NonzeroResidueFixture {
  SymRef Input;
  llvm::APInt Scale;
  unsigned Kind;

  llvm::APInt oracle(const std::array<llvm::APInt, 5> &V) const {
    const unsigned Width = Scale.getBitWidth();
    const auto One = llvm::APInt(Width, 1);
    const auto Low = llvm::APInt(Width, 3);
    const uint64_t Units[] = {17, 51, 101, 121};
    auto Product = Scale;
    for (unsigned I = 0; I < 4; ++I) {
      auto U = V[2] + 2 * I, W = V[3] + 4 * I;
      auto High = llvm::APInt(Width, 0);
      if (Kind == 0)
        High = (U | Low) * (W | Low) + Low;
      else if (Kind == 1)
        High = ((U | Low) + (W | Low) + 2) * (V[4] | One);
      else if (Kind == 2)
        High = ((U & ~One) * (V[3] | One) + (V[4] & ~One)) * (W & ~Low);
      else
        High = ((U & ~One) * (W & ~One) * (V[4] | One)) |
               ((U | Low) * (W | Low) - One);
      const unsigned Offset = Kind < 2 ? std::min(I, 2u) : I;
      Product *= Units[I] * (V[0] + 3 * V[1] + Offset) + High;
    }
    return Product;
  }
};

NonzeroResidueFixture nonzeroResidueFixture(SymContext &Ctx, unsigned Width,
                                            unsigned Kind) {
  auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  auto U = Ctx.mkVar("u", Width), V = Ctx.mkVar("v", Width);
  auto W = Ctx.mkVar("w", Width);
  auto C = [&](uint64_t Value) { return Ctx.mkConst(Width, Value); };
  const auto One = C(1), Low = C(3);
  const auto Even = Ctx.mkConst(~llvm::APInt(Width, 1));
  const auto Four = Ctx.mkConst(~llvm::APInt(Width, 3));
  const uint64_t Units[] = {17, 51, 101, 121};
  const unsigned Needed = Kind < 2 ? 3 : 4;
  auto Scale = llvm::APInt(Width, 1).shl(Width - Needed);
  llvm::SmallVector<SymRef, 8> Factors{Ctx.mkConst(Scale)};
  for (unsigned I = 0; I < 4; ++I) {
    auto A = Ctx.mkAdd(U, C(2 * I)), B = Ctx.mkAdd(V, C(4 * I));
    SymRef High;
    if (Kind == 0)
      High = Ctx.mkAdd(Ctx.mkMul(Ctx.mkOr(A, Low), Ctx.mkOr(B, Low)), Low);
    else if (Kind == 1)
      High = Ctx.mkMul(Ctx.mkAdd({Ctx.mkOr(A, Low), Ctx.mkOr(B, Low), C(2)}),
                       Ctx.mkOr(W, One));
    else if (Kind == 2)
      High =
          Ctx.mkMul(Ctx.mkAdd(Ctx.mkMul(Ctx.mkAnd(A, Even), Ctx.mkOr(V, One)),
                              Ctx.mkAnd(W, Even)),
                    Ctx.mkAnd(B, Four));
    else
      High = Ctx.mkOr(
          Ctx.mkMul({Ctx.mkAnd(A, Even), Ctx.mkAnd(B, Even), Ctx.mkOr(W, One)}),
          Ctx.mkSub(Ctx.mkMul(Ctx.mkOr(A, Low), Ctx.mkOr(B, Low)), One));
    const unsigned Offset = Kind < 2 ? std::min(I, 2u) : I;
    Factors.push_back(Ctx.mkAdd(
        Ctx.mkMul(C(Units[I]), Ctx.mkAdd({X, Ctx.mkMul(C(3), Y), C(Offset)})),
        High));
  }
  return {Ctx.mkMul(Factors), Scale, Kind};
}

TEST(SymMBAPrefixResidue, RetainsTheUnobservedFactorInNonzeroProducts) {
  std::mt19937_64 Random(0x4e4f4e5a45524f);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Kind = 0; Kind < 4; ++Kind)
      for (unsigned OddScale : {1u, 3u}) {
        SCOPED_TRACE(::testing::Message() << Width << ':' << Kind);
        SymContext Ctx;
        auto F = nonzeroResidueFixture(Ctx, Width, Kind);
        F.Input = Ctx.mkMul(Ctx.mkConst(Width, OddScale), F.Input);
        F.Scale *= OddScale;
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        auto Result =
            detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
        ASSERT_LT(Ctx.readabilityCost(Result), Ctx.readabilityCost(F.Input));
        ASSERT_FALSE(Ctx.isConstZero(Result));
        SymEvalPlan Original(Ctx, F.Input), Reduced(Ctx, Result);
        bool Witness = false;
        for (unsigned Trial = 0; Trial < 48; ++Trial) {
          std::array<llvm::APInt, 5> Values{
              word(Width, Random), word(Width, Random), word(Width, Random),
              word(Width, Random), word(Width, Random)};
          if (Trial < 16)
            Values = {llvm::APInt(Width, Trial), llvm::APInt(Width, 1),
                      llvm::APInt(Width, 2), llvm::APInt(Width, 4),
                      llvm::APInt(Width, 6)};
          auto Expected = F.oracle(Values);
          Witness |= !Expected.isZero();
          EXPECT_EQ(Original.eval(Values), Expected);
          EXPECT_EQ(Reduced.eval(Values), Expected);
        }
        EXPECT_TRUE(Witness);
      }
}

TEST(SymMBAPrefixResidue, NonzeroResidueTablesRespectExactResourceBoundaries) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Kind = 0; Kind < 4; ++Kind) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Kind);
      SymContext Ctx;
      auto F = nonzeroResidueFixture(Ctx, Width, Kind);
      detail::WorkBudget Full(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldProductResidues(Ctx, F.Input, Full, Storage);
      ASSERT_LT(Ctx.readabilityCost(Result), Ctx.readabilityCost(F.Input));
      const size_t Bytes = (1 << 24) - Storage;
      ASSERT_GT(Full.used(), 0u);
      ASSERT_GT(Bytes, 0u);
      for (bool ShortWork : {false, true}) {
        detail::WorkBudget Short(Full.used() - ShortWork);
        Storage = Bytes - !ShortWork;
        auto Partial =
            detail::foldProductResidues(Ctx, F.Input, Short, Storage);
        EXPECT_LE(Short.used(), Full.used() - ShortWork);
        EXPECT_LE(Storage, Bytes - !ShortWork);
        EXPECT_LE(Ctx.readabilityCost(Partial), Ctx.readabilityCost(F.Input));
        SymEvalPlan Reduced(Ctx, Partial);
        for (unsigned X = 0; X < 32; ++X) {
          const std::array<llvm::APInt, 5> Values{
              llvm::APInt(Width, X), llvm::APInt(Width, 3),
              llvm::APInt(Width, X * 17), llvm::APInt(Width, X * 51),
              llvm::APInt(Width, X * 101)};
          EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
        }
      }
      detail::WorkBudget Exact(Full.used());
      Storage = Bytes;
      EXPECT_EQ(detail::foldProductResidues(Ctx, F.Input, Exact, Storage),
                Result);
    }
}

TEST(SymMBAPrefixResidue, NonzeroResidueTablesExhaustSmallInputDomains) {
  for (unsigned Kind = 0; Kind < 4; ++Kind) {
    SCOPED_TRACE(Kind);
    SymContext Ctx;
    auto F = nonzeroResidueFixture(Ctx, 8, Kind);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 24;
    auto Result = detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
    ASSERT_LT(Ctx.readabilityCost(Result), Ctx.readabilityCost(F.Input));
    SymEvalPlan Original(Ctx, F.Input), Reduced(Ctx, Result);
    bool Witness = false;
    for (unsigned X = 0; X < 16; ++X)
      for (unsigned U = 0; U < 16; ++U)
        for (unsigned V = 0; V < 16; ++V) {
          const std::array<llvm::APInt, 5> Values{
              llvm::APInt(8, X), llvm::APInt(8, 3), llvm::APInt(8, U),
              llvm::APInt(8, V), llvm::APInt(8, 5)};
          auto Expected = F.oracle(Values);
          Witness |= !Expected.isZero();
          EXPECT_EQ(Original.eval(Values), Expected)
              << X << ':' << U << ':' << V;
          EXPECT_EQ(Reduced.eval(Values), Expected)
              << X << ':' << U << ':' << V;
        }
    EXPECT_TRUE(Witness);
  }
}

TEST(SymMBAPrefixResidue, NonzeroResidueTablesHaveIndependentFullWidthProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Kind = 0; Kind < 4; ++Kind) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Kind);
      SymContext Ctx;
      auto F = nonzeroResidueFixture(Ctx, Width, Kind);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
      ASSERT_LT(Ctx.readabilityCost(Result), Ctx.readabilityCost(F.Input));
      ASSERT_FALSE(Ctx.isConstZero(Result));
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
      EXPECT_EQ(Verifier(Ctx, F.Input, Result), SynthVerification::Equivalent);
    }
}

struct MultipleResidualFixture {
  SymRef Input;
  llvm::APInt Scale;
  unsigned Degree;
  unsigned Kind;

  llvm::APInt oracle(const std::array<llvm::APInt, 3> &V) const {
    const auto Mask = ~llvm::APInt(Scale.getBitWidth(), 3);
    auto Product = Scale;
    for (unsigned I = 0; I < Degree; ++I) {
      auto High = (V[1] + I) & Mask;
      if (Kind == 1)
        High = ((V[1] + I) * (V[2] + 2 * I)) & Mask;
      else if (Kind == 2)
        High = (V[1] + I).shl(2);
      auto Factor = V[0] + 2 * I;
      Product *= Kind == 3 ? Factor + High : Factor ^ High;
    }
    return Product;
  }
};

MultipleResidualFixture multipleResidualFixture(SymContext &Ctx, unsigned Width,
                                                unsigned Degree, unsigned Kind,
                                                unsigned Needed = 3) {
  const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  const auto Z = Ctx.mkVar("z", Width);
  auto C = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
  const auto Mask = Ctx.mkConst(~llvm::APInt(Width, 3));
  auto Scale = llvm::APInt(Width, 3).shl(Width - Needed);
  llvm::SmallVector<SymRef, 16> Factors{Ctx.mkConst(Scale)};
  for (unsigned I = 0; I < Degree; ++I) {
    auto U = Ctx.mkAdd(Y, C(I));
    auto High = Ctx.mkAnd(U, Mask);
    if (Kind == 1)
      High = Ctx.mkAnd(Ctx.mkMul(U, Ctx.mkAdd(Z, C(2 * I))), Mask);
    else if (Kind == 2)
      High = Ctx.mkShl(U, C(2));
    auto Base = Ctx.mkAdd(X, C(2 * I));
    Factors.push_back(Kind == 3 ? Ctx.mkAdd(Base, High)
                                : Ctx.mkXor(Base, High));
  }
  return {Ctx.mkMul(Factors), Scale, Degree, Kind};
}

TEST(SymMBAPrefixResidue, SynthesizesMultipleUnobservedFactors) {
  std::mt19937_64 Random(0x4d554c5449524553);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Degree : {4u, 8u, 16u})
      for (unsigned Kind = 0; Kind < 4; ++Kind) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Degree << ':' << Kind);
        SymContext Ctx;
        auto F = multipleResidualFixture(Ctx, Width, Degree, Kind);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        auto Result =
            detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
        ASSERT_LT(Ctx.readabilityCost(Result), Ctx.readabilityCost(F.Input));
        EXPECT_FALSE(Budget.exhausted());
        SymEvalPlan Original(Ctx, F.Input), Reduced(Ctx, Result);
        bool Nonzero = false;
        for (unsigned Trial = 0; Trial < 64; ++Trial) {
          std::array<llvm::APInt, 3> Values{
              word(Width, Random), word(Width, Random), word(Width, Random)};
          if (Trial < 16)
            Values = {llvm::APInt(Width, Trial), llvm::APInt(Width, 3 * Trial),
                      llvm::APInt(Width, 7 * Trial)};
          auto Want = F.oracle(Values);
          Nonzero |= !Want.isZero();
          EXPECT_EQ(Original.eval(Values), Want);
          EXPECT_EQ(Reduced.eval(Values), Want);
        }
        EXPECT_TRUE(Nonzero);
      }
}

TEST(SymMBAPrefixResidue, MultipleResidualsPreserveObservableCrossTerms) {
  for (unsigned Needed : {3u, 4u, 5u})
    for (unsigned Kind = 0; Kind < 4; ++Kind) {
      SCOPED_TRACE(::testing::Message() << Needed << ':' << Kind);
      SymContext Ctx;
      auto F = multipleResidualFixture(Ctx, 8, 4, Kind, Needed);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
      EXPECT_LE(Ctx.readabilityCost(Result), Ctx.readabilityCost(F.Input));
      if (Needed == 3)
        EXPECT_NE(Result, F.Input);
      SymEvalPlan Original(Ctx, F.Input), Reduced(Ctx, Result);
      for (unsigned X = 0; X < 32; ++X)
        for (unsigned Y = 0; Y < 32; ++Y)
          for (unsigned Z = 0; Z < 16; ++Z) {
            std::array<llvm::APInt, 3> Values{
                llvm::APInt(8, X), llvm::APInt(8, Y), llvm::APInt(8, Z)};
            const auto Want = F.oracle(Values);
            ASSERT_EQ(Original.eval(Values), Want);
            ASSERT_EQ(Reduced.eval(Values), Want) << X << ':' << Y << ':' << Z;
          }
    }
}

TEST(SymMBAPrefixResidue, MultipleResidualsRespectCumulativeResources) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SymContext Ctx;
    auto F = multipleResidualFixture(Ctx, Width, 8, 1);
    detail::WorkBudget Full(65536);
    size_t Storage = 1 << 24;
    auto Result = detail::foldProductResidues(Ctx, F.Input, Full, Storage);
    ASSERT_NE(Result, F.Input);
    const size_t Bytes = (1 << 24) - Storage;
    ASSERT_GT(Full.used(), 0u);
    ASSERT_GT(Bytes, 0u);
    for (const auto [Work, Space] : {std::pair<size_t, size_t>{0, Bytes},
                                     {Full.used(), 0},
                                     {Full.used() - 1, Bytes},
                                     {Full.used(), Bytes - 1},
                                     {Full.used(), Bytes}}) {
      detail::WorkBudget Budget(Work);
      Storage = Space;
      auto Partial = detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
      EXPECT_LE(Budget.used(), Work);
      EXPECT_LE(Storage, Space);
      if (Work == Full.used() && Space == Bytes)
        EXPECT_EQ(Partial, Result);
      SymEvalPlan Reduced(Ctx, Partial);
      for (unsigned X = 0; X < 32; ++X) {
        const std::array<llvm::APInt, 3> Values{llvm::APInt(Width, X),
                                                llvm::APInt(Width, X * 17),
                                                llvm::APInt(Width, X * 51)};
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
  }
}

TEST(SymMBAPrefixResidue, MultipleResidualsHaveIndependentFullWidthProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Kind = 0; Kind < 4; ++Kind) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Kind);
      SymContext Ctx;
      auto F = multipleResidualFixture(Ctx, Width, 4, Kind);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
      ASSERT_NE(Result, F.Input);
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
      EXPECT_EQ(Verifier(Ctx, F.Input, Result), SynthVerification::Equivalent);
    }
}

TEST(SymMBAPrefixResidue, ResidualAttemptsRespectContextAndStorage) {
  for (unsigned Needed : {3u, 4u}) {
    SymContext Ctx;
    auto F = multipleResidualFixture(Ctx, 64, 4, 0, Needed);
    detail::WorkBudget Budget(65536);
    Budget.MemoOwner = &Ctx;
    Budget.MemoCapacity = 16;
    size_t Storage = 1 << 24;
    auto First =
        detail::foldResidualProductResidues(Ctx, F.Input, Budget, Storage);
    const auto Work = Budget.used();
    ASSERT_GT(Work, 1u);
    Storage = 1 << 24;
    auto Reused =
        detail::foldResidualProductResidues(Ctx, F.Input, Budget, Storage);
    EXPECT_EQ(Reused, First);
    EXPECT_EQ(Budget.used() - Work, 1u);
    const auto BeforeLarger = Budget.used();
    Storage = (1 << 24) + 1;
    auto Larger =
        detail::foldResidualProductResidues(Ctx, F.Input, Budget, Storage);
    EXPECT_EQ(Larger, First);
    EXPECT_EQ(Budget.used() - BeforeLarger, 1u);

    SymContext Other;
    auto G = multipleResidualFixture(Other, 32, 4, 1, Needed);
    const auto BeforeOther = Budget.used();
    Storage = 1 << 24;
    auto Result =
        detail::foldResidualProductResidues(Other, G.Input, Budget, Storage);
    EXPECT_GT(Budget.used() - BeforeOther, 1u);
    SymEvalPlan Plan(Other, Result);
    for (unsigned X = 0; X < 16; ++X) {
      const std::array<llvm::APInt, 3> Values{
          llvm::APInt(32, X), llvm::APInt(32, 3 * X), llvm::APInt(32, 7 * X)};
      EXPECT_EQ(Plan.eval(Values), G.oracle(Values));
    }
  }
  SymContext Ctx;
  auto F = multipleResidualFixture(Ctx, 64, 8, 1);
  detail::WorkBudget Budget(65536);
  Budget.MemoOwner = &Ctx;
  Budget.MemoCapacity = 16;
  size_t Storage = 512;
  EXPECT_EQ(detail::foldResidualProductResidues(Ctx, F.Input, Budget, Storage),
            F.Input);
  Storage = 1 << 24;
  EXPECT_NE(detail::foldResidualProductResidues(Ctx, F.Input, Budget, Storage),
            F.Input);
}

TEST(SymMBAPrefixResidue, DecisionWordsPreserveCastsShiftsAndOpaqueInputs) {
  std::mt19937_64 Random(0x4445434953494f4e);
  unsigned Rewritten = 0;
  for (unsigned Width : {8u, 16u, 64u, 257u})
    for (unsigned Kind = 0; Kind < 4; ++Kind) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Kind);
      SymContext Ctx;
      const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      auto C = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
      SymRef A, B;
      if (Kind == 0) {
        A = Ctx.mkXor(X, Y);
        B = A;
      } else if (Kind == 1) {
        A = Ctx.mkXor(Ctx.mkLShr(X, C(2)),
                      Ctx.mkLShr(C(0xa9), Ctx.mkAnd(Y, C(31))));
        B = Ctx.mkAdd(A, C(2));
      } else if (Kind == 2) {
        A = Ctx.mkAdd(Ctx.mkZExt(Ctx.mkExtract(X, 2, 3), Width),
                      Ctx.mkSExt(Ctx.mkExtract(Y, 1, 3), Width));
        B = A;
      } else {
        A = Ctx.mkXor(Ctx.mkUDiv(X, Ctx.mkOr(Y, C(1))), X);
        B = Ctx.mkAdd(A, C(2));
      }
      const auto Scale = llvm::APInt(Width, 3).shl(Width - 4);
      llvm::SmallVector<SymRef, 16> Factors{Ctx.mkConst(Scale)};
      for (unsigned I = 0; I < 4; ++I) {
        Factors.push_back(A);
        Factors.push_back(B);
      }
      const auto Input = Ctx.mkMul(Factors);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, Input, Budget, Storage);
      Rewritten += Result != Input;
      EXPECT_LE(Ctx.readabilityCost(Result), Ctx.readabilityCost(Input));
      SymEvalPlan Original(Ctx, Input), Reduced(Ctx, Result);
      for (unsigned Trial = 0; Trial < 1024; ++Trial) {
        auto U = word(Width, Random), V = word(Width, Random);
        if (Trial < 512) {
          U = llvm::APInt(Width, Trial / 32);
          V = llvm::APInt(Width, Trial % 32);
        }
        llvm::APInt F(Width, 0);
        if (Kind == 0)
          F = U ^ V;
        else if (Kind == 1) {
          const auto Shift = (V & llvm::APInt(Width, 31)).getZExtValue();
          F = U.lshr(2) ^
              (Shift >= Width ? llvm::APInt(Width, 0)
                              : llvm::APInt(Width, 0xa9).lshr(Shift));
        } else if (Kind == 2)
          F = U.lshr(2).trunc(3).zext(Width) + V.lshr(1).trunc(3).sext(Width);
        else
          F = U.udiv(V | llvm::APInt(Width, 1)) ^ U;
        auto Want = Scale;
        for (unsigned I = 0; I < 4; ++I)
          Want *= F * (Kind == 1 || Kind == 3 ? F + 2 : F);
        const std::array<llvm::APInt, 2> Values{U, V};
        EXPECT_EQ(Original.eval(Values), Want);
        EXPECT_EQ(Reduced.eval(Values), Want);
      }
    }
  EXPECT_GT(Rewritten, 0u);
}

TEST(SymMBAPrefixResidue, SharedParityDecisionsKeepBothInputParities) {
  std::mt19937_64 Random(0x4f44444252414e43);
  for (unsigned Width : {8u, 64u, 257u})
    for (unsigned Needed : {4u, 6u})
      for (bool Complement : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Needed << ':' << Complement);
        SymContext Ctx;
        auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
        auto Z = Ctx.mkVar("z", Width);
        auto C = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
        const auto Scale = llvm::APInt(Width, 3).shl(Width - Needed);
        llvm::SmallVector<SymRef, 16> Factors{Ctx.mkConst(Scale)};
        for (unsigned I = 0; I < 16; ++I) {
          auto U = Ctx.mkAnd(Ctx.mkAdd(Y, C(I)),
                             Ctx.mkConst(~llvm::APInt(Width, 1)));
          auto V = Ctx.mkAnd(Ctx.mkAdd(Z, C(2 * I)),
                             Ctx.mkConst(~llvm::APInt(Width, 3)));
          auto Base = Ctx.mkAdd(Complement ? Ctx.mkNot(X) : X, C(2 * I));
          Factors.push_back(Ctx.mkXor(Base, Ctx.mkMul(U, V)));
        }
        const auto Input = Ctx.mkMul(Factors);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        const auto Result =
            detail::foldResidueDecisions(Ctx, Input, Budget, Storage, true);
        if (Needed == 4)
          ASSERT_LT(Ctx.readabilityCost(Result), Ctx.readabilityCost(Input));
        EXPECT_FALSE(Budget.exhausted());
        SymEvalPlan Original(Ctx, Input), Reduced(Ctx, Result);
        bool Zero = false, Nonzero = false;
        for (unsigned Trial = 0; Trial < 1024; ++Trial) {
          std::array<llvm::APInt, 3> Values{
              word(Width, Random), word(Width, Random), word(Width, Random)};
          if (Trial < 512)
            Values = {llvm::APInt(Width, Trial / 64),
                      llvm::APInt(Width, Trial / 8 % 8),
                      llvm::APInt(Width, Trial % 8)};
          auto Want = Scale;
          for (unsigned I = 0; I < 16; ++I) {
            const auto U = (Values[1] + I) & ~llvm::APInt(Width, 1);
            const auto V = (Values[2] + 2 * I) & ~llvm::APInt(Width, 3);
            const auto Base = (Complement ? ~Values[0] : Values[0]) + 2 * I;
            Want *= Base ^ (U * V);
          }
          Zero |= Want.isZero();
          Nonzero |= !Want.isZero();
          EXPECT_EQ(Original.eval(Values), Want);
          EXPECT_EQ(Reduced.eval(Values), Want);
        }
        EXPECT_TRUE(Zero);
        EXPECT_TRUE(Nonzero);
      }
}

TEST(SymMBAPrefixResidue, ZeroDecisionWordsKeepTheirOriginalWidth) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SymContext Ctx;
    auto X = Ctx.mkVar("x", Width);
    llvm::SmallVector<SymRef, 8> Factors{
        Ctx.mkConst(llvm::APInt(Width, 1).shl(Width - 3))};
    for (unsigned I = 0; I < 4; ++I)
      Factors.push_back(Ctx.mkAdd(X, Ctx.mkConst(Width, I)));
    auto Input = Ctx.mkMul(Factors);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 24;
    auto Result = detail::foldResidueDecisions(Ctx, Input, Budget, Storage);
    ASSERT_TRUE(Ctx.isConstZero(Result));
    EXPECT_EQ(Ctx.width(Result), Width);
    for (unsigned Value = 0; Value < 256; ++Value)
      EXPECT_TRUE(Ctx.eval(Input, {llvm::APInt(Width, Value)}).isZero());
  }
}

TEST(SymMBAPrefixResidue, GeneratedReadingsStillSimplifyOriginalOpaqueInputs) {
  std::mt19937_64 Random(0x4f50415155455744);
  for (unsigned Width : {8u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto Z = Ctx.mkVar("z", Width);
    auto C = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
    const auto Sum = Ctx.mkAdd(X, Y);
    const auto Numerator =
        Ctx.mkAdd(Ctx.mkXor(X, Y), Ctx.mkMul(C(2), Ctx.mkAnd(X, Y)));
    const auto Opaque = Ctx.mkUDiv(Numerator, C(7));
    const auto Scale = llvm::APInt(Width, 3).shl(Width - 3);
    llvm::SmallVector<SymRef, 16> Factors{Ctx.mkConst(Scale)};
    for (unsigned I = 0; I < 8; ++I) {
      auto High =
          Ctx.mkAnd(Ctx.mkAdd(Z, C(I)), Ctx.mkConst(~llvm::APInt(Width, 3)));
      Factors.push_back(Ctx.mkXor(Ctx.mkAdd(Opaque, C(2 * I)), High));
    }
    const auto Input = Ctx.mkMul(Factors);
    MBAOptions Opts;
    Opts.MaxWork = 65536;
    Opts.VerifySamples = 0;
    const auto Result = simplifyMBADeep(Ctx, Input, Opts);
    ASSERT_TRUE(Result.Changed);
    EXPECT_LT(Result.Work, Opts.MaxWork);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    bool SawDivision = false;
    std::vector<SymRef> Pending{Result.Expr};
    std::vector<bool> Seen(Ctx.numNodes());
    while (!Pending.empty()) {
      const auto R = Pending.back();
      Pending.pop_back();
      if (Seen[R.index()])
        continue;
      Seen[R.index()] = true;
      if (Ctx.op(R) == SymOp::UDiv) {
        SawDivision = true;
        EXPECT_EQ(Ctx.operand(R, 0), Sum);
      }
      Pending.insert(Pending.end(), Ctx.operands(R).begin(),
                     Ctx.operands(R).end());
    }
    EXPECT_TRUE(SawDivision);
    SymEvalPlan Reduced(Ctx, Result.Expr);
    for (unsigned Trial = 0; Trial < 1024; ++Trial) {
      std::array<llvm::APInt, 3> Values{
          word(Width, Random), word(Width, Random), word(Width, Random)};
      if (Trial < 512)
        Values = {llvm::APInt(Width, Trial / 64),
                  llvm::APInt(Width, Trial / 8 % 8),
                  llvm::APInt(Width, Trial % 8)};
      const auto Base = (Values[0] + Values[1]).udiv(llvm::APInt(Width, 7));
      auto Want = Scale;
      for (unsigned I = 0; I < 8; ++I)
        Want *= (Base + 2 * I) ^ ((Values[2] + I) & ~llvm::APInt(Width, 3));
      EXPECT_EQ(Reduced.eval(Values), Want);
    }
  }
}

TEST(SymMBAPrefixResidue, MixedResidualProductsLeaveBudgetForTheirLinearTail) {
  std::mt19937_64 Random(0x4d49584544544149);
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Degree : {4u, 24u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Degree);
      SymContext Ctx;
      const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      const auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
      auto C = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
      const auto Scale = llvm::APInt(Width, 3).shl(Width - 6);
      llvm::SmallVector<SymRef, 32> Factors{Ctx.mkConst(Scale)};
      for (unsigned I = 0; I < Degree; ++I) {
        auto U =
            Ctx.mkAnd(Ctx.mkAdd(Y, C(I)), Ctx.mkConst(~llvm::APInt(Width, 1)));
        auto V = Ctx.mkAnd(Ctx.mkAdd(Z, C(2 * I)),
                           Ctx.mkConst(~llvm::APInt(Width, 3)));
        Factors.push_back(Ctx.mkXor(Ctx.mkAdd(X, C(2 * I)), Ctx.mkMul(U, V)));
      }
      const auto Input = Ctx.mkAdd({Ctx.mkMul(Factors), Ctx.mkXor(X, Y),
                                    Ctx.mkMul(C(2), Ctx.mkAnd(X, Y)),
                                    Ctx.mkOr(Z, W), Ctx.mkAnd(Z, W)});
      MBAOptions Opts;
      Opts.MaxWork = 65536;
      Opts.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, Input, Opts);
      EXPECT_LT(Result.Work, Opts.MaxWork);
      EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned Trial = 0; Trial < 1024; ++Trial) {
        std::array<llvm::APInt, 4> Values;
        for (auto &V : Values)
          V = word(Width, Random);
        if (Trial < 512)
          Values = {llvm::APInt(Width, Trial / 64),
                    llvm::APInt(Width, Trial / 8 % 8),
                    llvm::APInt(Width, Trial % 8), llvm::APInt(Width, Trial)};
        auto Want = Scale;
        for (unsigned I = 0; I < Degree; ++I)
          Want *= (Values[0] + 2 * I) ^
                  (((Values[1] + I) & ~llvm::APInt(Width, 1)) *
                   ((Values[2] + 2 * I) & ~llvm::APInt(Width, 3)));
        Want += Values[0] + Values[1] + Values[2] + Values[3];
        EXPECT_EQ(Reduced.eval(Values), Want);
      }
    }
}

struct CarryProductFixture {
  SymRef Product;
  SymRef Input;
  llvm::APInt Scale;
  unsigned Degree;
  bool Mixed;
  bool Neighbor;
  unsigned HighBits;

  llvm::APInt oracle(const std::array<llvm::APInt, 4> &V) const {
    const auto Width = Scale.getBitWidth();
    auto Result = Scale;
    for (unsigned I = 0; I < Degree; ++I) {
      auto High = (V[1] + I) & ~llvm::APInt::getLowBitsSet(Width, HighBits);
      if (Mixed)
        High =
            ((V[1] + I) & ~llvm::APInt(Width, 1)) *
            ((V[2] + 2 * I) & ~llvm::APInt::getLowBitsSet(Width, HighBits - 1));
      auto Base = V[0] + 2 * I;
      if (Neighbor && I + 1 == Degree)
        ++Base;
      Result *= Base ^ High;
    }
    return Result + V[0] + V[1] + V[2] + V[3];
  }
};

CarryProductFixture carryProductFixture(SymContext &Ctx, unsigned Width,
                                        unsigned Degree, unsigned Needed,
                                        bool Mixed, bool Neighbor = false,
                                        unsigned HighBits = 0) {
  if (!HighBits)
    HighBits = Mixed ? 3 : 2;
  auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
  auto C = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
  auto Scale = llvm::APInt(Width, 3).shl(Width - Needed);
  llvm::SmallVector<SymRef, 32> Factors{Ctx.mkConst(Scale)};
  for (unsigned I = 0; I < Degree; ++I) {
    auto U = Ctx.mkAdd(Y, C(I));
    auto High =
        Ctx.mkAnd(U, Ctx.mkConst(~llvm::APInt::getLowBitsSet(Width, HighBits)));
    if (Mixed)
      High = Ctx.mkMul(Ctx.mkAnd(U, Ctx.mkConst(~llvm::APInt(Width, 1))),
                       Ctx.mkAnd(Ctx.mkAdd(Z, C(2 * I)),
                                 Ctx.mkConst(~llvm::APInt::getLowBitsSet(
                                     Width, HighBits - 1))));
    auto Base = Ctx.mkAdd(X, C(2 * I + (Neighbor && I + 1 == Degree)));
    Factors.push_back(Ctx.mkXor(Base, High));
  }
  auto Product = Ctx.mkMul(Factors);
  auto Input =
      Ctx.mkAdd({Product, Ctx.mkXor(X, Y), Ctx.mkMul(C(2), Ctx.mkAnd(X, Y)),
                 Ctx.mkOr(Z, W), Ctx.mkAnd(Z, W)});
  return {Product, Input, Scale, Degree, Mixed, Neighbor, HighBits};
}

TEST(SymMBAPrefixResidue, GroupedCarriesRecoverHigherPrecisionProducts) {
  std::mt19937_64 Random(0x4341525259505246);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (auto Case : {std::array<unsigned, 3>{32, 7, 1},
                      {64, 6, 1},
                      {64, 7, 0},
                      {128, 6, 0}}) {
      if (Width > 64 && Case[0] > 32)
        continue;
      SCOPED_TRACE(::testing::Message() << Width << ':' << Case[0] << ':'
                                        << Case[1] << ':' << Case[2]);
      SymContext Ctx;
      auto F = carryProductFixture(Ctx, Width, Case[0], Case[1], Case[2]);
      MBAOptions Opts;
      Opts.MaxWork = 65536;
      Opts.VerifySamples = 0;
      auto Result = simplifyMBADeep(Ctx, F.Input, Opts);
      ASSERT_LT(Result.SizeAfter, Result.SizeBefore / 2);
      EXPECT_LT(Result.Work, Opts.MaxWork);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      SymEvalPlan Reduced(Ctx, Result.Expr);
      bool Zero = false, Nonzero = false;
      for (unsigned Trial = 0; Trial < 1024; ++Trial) {
        std::array<llvm::APInt, 4> Values;
        for (auto &V : Values)
          V = word(Width, Random);
        if (Trial < 512)
          Values = {llvm::APInt(Width, Trial / 64),
                    llvm::APInt(Width, Trial / 8 % 8),
                    llvm::APInt(Width, Trial % 8), llvm::APInt(Width, Trial)};
        auto Want = F.oracle(Values);
        EXPECT_EQ(Reduced.eval(Values), Want);
        auto Product = Want - Values[0] - Values[1] - Values[2] - Values[3];
        Zero |= Product.isZero();
        Nonzero |= !Product.isZero();
      }
      EXPECT_TRUE(Zero);
      EXPECT_TRUE(Nonzero);
    }
}

SymRef groupedQuadraticInput(SymContext &Ctx, unsigned Width, unsigned Needed) {
  auto C = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
  auto X = Ctx.mkAnd(Ctx.mkVar("x", Width), C(1));
  auto Y = Ctx.mkAnd(Ctx.mkVar("y", Width), C(1));
  auto Z = Ctx.mkAnd(Ctx.mkVar("z", Width), C(1));
  auto Sum = [&](SymRef A, SymRef B) {
    return Ctx.mkAdd(Ctx.mkXor(A, B), Ctx.mkMul(C(2), Ctx.mkAnd(A, B)));
  };
  // Deliberately retain a redundant zero so publishing the exact quadratic
  // word is profitable. This test calls the residue proof without a preceding
  // general MBA simplification, which could erase this proof's input.
  auto Zero = Ctx.mkSub(Sum(X, Y), Ctx.mkAdd(X, Y));
  const auto Scale = llvm::APInt(Width, 3).shl(Width - Needed);
  llvm::SmallVector<SymRef, 8> Factors{Ctx.mkConst(Scale)};
  for (auto H : {Sum(X, Y), Sum(Y, Z), Sum(Z, X), Sum(X, Y)})
    Factors.push_back(Ctx.mkAdd(C(1), Ctx.mkMul(C(4), Ctx.mkAdd(H, Zero))));
  return Ctx.mkMul(Factors);
}

TEST(SymMBAPrefixResidue, GroupedQuadraticResiduesKeepTheDivisionBit) {
  for (unsigned Width : {8u, 64u, 257u})
    for (unsigned Needed : {5u, 6u, 7u, 8u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Needed);
      SymContext Ctx;
      const auto Scale = llvm::APInt(Width, 3).shl(Width - Needed);
      auto Input = groupedQuadraticInput(Ctx, Width, Needed);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldResidueDecisions(Ctx, Input, Budget, Storage);
      if (Needed <= 6)
        EXPECT_LT(Ctx.readabilityCost(Result), Ctx.readabilityCost(Input));
      EXPECT_LE(Ctx.readabilityCost(Result), Ctx.readabilityCost(Input));
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned Pattern = 0; Pattern < 512; ++Pattern) {
        std::array<llvm::APInt, 3> V{llvm::APInt(Width, Pattern / 64),
                                     llvm::APInt(Width, Pattern / 8 % 8),
                                     llvm::APInt(Width, Pattern % 8)};
        auto Want = Scale;
        auto X = V[0] & 1, Y = V[1] & 1, Z = V[2] & 1;
        for (auto H : {X + Y, Y + Z, Z + X, X + Y})
          Want *= 1 + 4 * H;
        EXPECT_EQ(Reduced.eval(V), Want);
      }
    }
}

TEST(SymMBAPrefixResidue, GroupedCarriesRespectParityAndPartialBudgets) {
  std::mt19937_64 Random(0x4341525259424e44);
  for (unsigned Width : {8u, 64u, 257u})
    for (bool Neighbor : {false, true})
      for (auto Limits : {std::array<size_t, 2>{0, 1 << 24},
                          {1, 1 << 24},
                          {8192, 1 << 24},
                          {65536, 0},
                          {65536, 1024},
                          {65536, 1 << 24}}) {
        SCOPED_TRACE(::testing::Message() << Width << ':' << Neighbor << ':'
                                          << Limits[0] << ':' << Limits[1]);
        SymContext Ctx;
        auto F = carryProductFixture(Ctx, Width, 32, 7, true, Neighbor);
        MBAOptions Opts;
        Opts.MaxWork = Limits[0];
        Opts.MaxTableBytes = Limits[1];
        Opts.VerifySamples = 0;
        auto Result = simplifyMBADeep(Ctx, F.Input, Opts);
        EXPECT_LE(Result.Work, Limits[0]);
        EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
        SymEvalPlan Reduced(Ctx, Result.Expr);
        for (unsigned Trial = 0; Trial < 256; ++Trial) {
          std::array<llvm::APInt, 4> Values;
          for (auto &V : Values)
            V = word(Width, Random);
          EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
        }
      }
}

TEST(SymMBAPrefixResidue, GroupedQuadraticResiduesHaveFullWidthProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Needed : {5u, 6u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Needed);
      SymContext Ctx;
      auto Input = groupedQuadraticInput(Ctx, Width, Needed);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldResidueDecisions(Ctx, Input, Budget, Storage);
      ASSERT_LT(Ctx.readabilityCost(Result), Ctx.readabilityCost(Input));
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 10000);
      EXPECT_EQ(Verifier(Ctx, Input, Result), SynthVerification::Equivalent);
    }
}

TEST(SymMBAPrefixResidue, SharedBooleanCofactorsShortenCarryFrontiers) {
  std::mt19937_64 Random(0x434f464143544f52);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (auto Case : {std::array<unsigned, 5>{8, 5, 1, 2, 40},
                      {16, 6, 1, 2, 60},
                      {32, 7, 0, 2, 300},
                      {48, 6, 0, 2, 200},
                      {96, 7, 0, 3, 400}}) {
      if (Width > 64 && Case[0] != 8)
        continue;
      SCOPED_TRACE(::testing::Message() << Width << ':' << Case[0]);
      SymContext Ctx;
      auto F = carryProductFixture(Ctx, Width, Case[0], Case[1], Case[2], false,
                                   Case[3]);
      MBAOptions Opts;
      Opts.MaxWork = 65536;
      Opts.VerifySamples = 0;
      auto Result = simplifyMBADeep(Ctx, F.Input, Opts);
      ASSERT_LE(Result.SizeAfter, Case[4]);
      EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
      EXPECT_LE(Result.Work, Opts.MaxWork);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned Trial = 0; Trial < 1024; ++Trial) {
        std::array<llvm::APInt, 4> Values;
        for (auto &V : Values)
          V = word(Width, Random);
        if (Trial < 512)
          Values = {llvm::APInt(Width, Trial / 64),
                    llvm::APInt(Width, Trial / 8 % 8),
                    llvm::APInt(Width, Trial % 8), llvm::APInt(Width, Trial)};
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
      // Exhaust the two observed input words for the linear degree-32 case.
      // The high input bits remain arbitrary in the independent random rows.
      if (Width == 8 && Case[0] == 32)
        for (unsigned X = 0; X < 128; ++X)
          for (unsigned Y = 0; Y < 128; ++Y) {
            std::array<llvm::APInt, 4> Values{
                llvm::APInt(Width, X), llvm::APInt(Width, Y),
                llvm::APInt(Width, 17), llvm::APInt(Width, 29)};
            ASSERT_EQ(Reduced.eval(Values), F.oracle(Values));
          }
    }
}

TEST(SymMBAPrefixResidue, SharedBooleanCofactorsHaveFullWidthProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    auto F = carryProductFixture(Ctx, Width, 8, 5, true, false, 2);
    MBAOptions Opts;
    Opts.MaxWork = 65536;
    Opts.VerifySamples = 0;
    // Prove the nonlinear rewrite separately from the independent additive
    // tail. The complete composed expression is covered by the oracle test.
    auto Result = simplifyMBADeep(Ctx, F.Product, Opts);
    ASSERT_LE(Result.SizeAfter, 40u);
    solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 10000);
    EXPECT_EQ(Verifier(Ctx, F.Product, Result.Expr),
              SynthVerification::Equivalent);
  }
}

TEST(SymMBAPrefixResidue, CompletedDecisionsRequireFreeInputsAndResources) {
  for (bool Opaque : {false, true}) {
    SymContext Ctx;
    auto X = Ctx.mkVar("x", 64), Y = Ctx.mkVar("y", 64);
    auto One = Ctx.mkConst(64, 1);
    auto Base = Opaque ? Ctx.mkUDiv(X, Ctx.mkOr(Y, One)) : X;
    llvm::SmallVector<SymRef, 32> Factors(16, Ctx.mkOr(Base, One));
    auto Scale = llvm::APInt(64, 3).shl(58);
    Factors.push_back(Ctx.mkConst(Scale));
    auto Input = Ctx.mkMul(Factors);
    for (size_t Allowance : {size_t(0), size_t(1), size_t(65536)}) {
      detail::WorkBudget Budget(Allowance);
      size_t Storage = 1 << 24;
      bool Complete = true;
      auto Result = detail::foldResidueDecisions(
          Ctx, Input, Budget, Storage, false, Allowance, nullptr, &Complete);
      EXPECT_EQ(Complete, !Opaque && Allowance == 65536);
      EXPECT_LE(Budget.used(), Allowance);
      if (Allowance == 65536) {
        ASSERT_TRUE(Ctx.isConst(Result));
        EXPECT_EQ(Ctx.constValue(Result), Scale);
      } else {
        EXPECT_EQ(Result, Input);
      }
    }
  }
}

TEST(SymMBAPrefixResidue, ImprovedBitReadingsKeepShorterAffineCoordinates) {
  std::mt19937_64 Random(0x414646494e454249);
  const std::array<uint64_t, 4> Coefficients{4211296093, 3120383843, 3718174401,
                                             2842018661};
  const std::array<unsigned, 4> Offsets{0, 1, 2, 2};
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
    auto C = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
    auto Base = Ctx.mkAdd({X, Ctx.mkMul(C(3), Y), Ctx.mkMul(C(5), Z)});
    auto Scale = llvm::APInt(Width, 1).shl(Width - 3);
    llvm::SmallVector<SymRef, 8> Factors{Ctx.mkConst(Scale)};
    for (unsigned I = 0; I < Coefficients.size(); ++I)
      Factors.push_back(Ctx.mkAnd(Ctx.mkAdd(Ctx.mkMul(C(Coefficients[I]), Base),
                                            C(Coefficients[I] * Offsets[I])),
                                  C(3)));
    auto Input = Ctx.mkAdd({Ctx.mkMul(Factors), Ctx.mkXor(X, Y),
                            Ctx.mkMul(C(2), Ctx.mkAnd(X, Y)), Ctx.mkOr(Z, W),
                            Ctx.mkAnd(Z, W)});
    MBAOptions Opts;
    Opts.MaxWork = 65536;
    Opts.VerifySamples = 0;
    auto Result = simplifyMBADeep(Ctx, Input, Opts);
    ASSERT_LE(Result.SizeAfter, 45u);
    EXPECT_LT(Result.Work, Opts.MaxWork);
    SymEvalPlan Reduced(Ctx, Result.Expr);
    for (unsigned Trial = 0; Trial < 1024; ++Trial) {
      std::array<llvm::APInt, 4> V;
      for (auto &Value : V)
        Value = word(Width, Random);
      auto B = V[0] + 3 * V[1] + 5 * V[2];
      auto Want = Scale;
      for (unsigned I = 0; I < Coefficients.size(); ++I) {
        auto A = llvm::APInt(Width, Coefficients[I]);
        Want *= (A * B + A * Offsets[I]) & llvm::APInt(Width, 3);
      }
      EXPECT_EQ(Reduced.eval(V), Want + V[0] + V[1] + V[2] + V[3]);
    }
  }
}

TEST(SymMBAPrefixResidue, PairedProductsRecoverLongMixedCarryFrontiers) {
  std::mt19937_64 Random(0x5041495250524f44);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (auto Case : {std::array<unsigned, 3>{64, 7, 2},
                      {96, 6, 2},
                      {128, 6, 2},
                      {128, 7, 2},
                      {128, 7, 3},
                      {128, 8, 2},
                      {128, 8, 3}}) {
      if (Width > 64 && Case[0] != 64)
        continue;
      SCOPED_TRACE(::testing::Message() << Width << ':' << Case[0] << ':'
                                        << Case[1] << ':' << Case[2]);
      SymContext Ctx;
      auto F = carryProductFixture(Ctx, Width, Case[0], Case[1], true, false,
                                   Case[2]);
      MBAOptions Opts;
      Opts.MaxWork = 65536;
      Opts.VerifySamples = 0;
      auto Result = simplifyMBADeep(Ctx, F.Input, Opts);
      ASSERT_LE(Result.SizeAfter, 13u);
      EXPECT_LT(Result.Work, Opts.MaxWork);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned Trial = 0; Trial < 1024; ++Trial) {
        std::array<llvm::APInt, 4> Values;
        for (auto &V : Values)
          V = word(Width, Random);
        if (Trial < 512)
          Values = {llvm::APInt(Width, Trial / 64),
                    llvm::APInt(Width, Trial / 8 % 8),
                    llvm::APInt(Width, Trial % 8), llvm::APInt(Width, Trial)};
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

struct PairedFlipFixture {
  SymRef Input;
  unsigned Width;
  bool Neighbor;

  llvm::APInt oracle(const std::array<llvm::APInt, 3> &V) const {
    auto High =
        (V[1] & ~llvm::APInt(Width, 1)) * (V[2] & ~llvm::APInt(Width, 1));
    auto A = V[0] ^ High;
    auto B = (V[0] + 32 + unsigned(Neighbor)) ^
             (((V[1] + 16) & ~llvm::APInt(Width, 1)) *
              (V[2] & ~llvm::APInt(Width, 1)));
    auto Result = llvm::APInt(Width, 3).shl(Width - 6);
    for (unsigned I = 0; I < 31; ++I)
      Result *= A;
    return Result * B;
  }
};

PairedFlipFixture pairedFlipFixture(SymContext &Ctx, unsigned Width,
                                    bool Neighbor = false) {
  auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  auto Z = Ctx.mkVar("z", Width);
  auto C = [&](uint64_t V) { return Ctx.mkConst(Width, V); };
  auto Mask = Ctx.mkConst(~llvm::APInt(Width, 1));
  auto High = Ctx.mkMul(Ctx.mkAnd(Y, Mask), Ctx.mkAnd(Z, Mask));
  auto A = Ctx.mkXor(X, High);
  auto B = Ctx.mkXor(
      Ctx.mkAdd(X, C(32 + unsigned(Neighbor))),
      Ctx.mkMul(Ctx.mkAnd(Ctx.mkAdd(Y, C(16)), Mask), Ctx.mkAnd(Z, Mask)));
  llvm::SmallVector<SymRef, 32> Factors(31, A);
  Factors.push_back(B);
  Factors.push_back(Ctx.mkConst(llvm::APInt(Width, 3).shl(Width - 6)));
  return {Ctx.mkMul(Factors), Width, Neighbor};
}

TEST(SymMBAPrefixResidue, PairedProductsRetainVariableHighestBitCorrections) {
  std::mt19937_64 Random(0x50414952464c4950);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (bool Neighbor : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Neighbor);
      SymContext Ctx;
      auto F = pairedFlipFixture(Ctx, Width, Neighbor);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage,
                                                 false, 65536);
      if (!Neighbor) {
        ASSERT_LT(Ctx.readabilityCost(Result), Ctx.readabilityCost(F.Input));
        EXPECT_LT(Budget.used(), 65536u);
      }
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned Trial = 0; Trial < 1024; ++Trial) {
        std::array<llvm::APInt, 3> Values;
        for (auto &V : Values)
          V = word(Width, Random);
        if (Trial < 512)
          Values = {llvm::APInt(Width, Trial / 64),
                    llvm::APInt(Width, Trial / 8 % 8),
                    llvm::APInt(Width, Trial % 8)};
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
      if (Width == 8 && !Neighbor)
        for (unsigned X = 0; X < 64; ++X)
          for (unsigned Y = 0; Y < 32; ++Y)
            for (unsigned Z = 0; Z < 32; ++Z) {
              std::array<llvm::APInt, 3> V{llvm::APInt(Width, X),
                                           llvm::APInt(Width, Y),
                                           llvm::APInt(Width, Z)};
              ASSERT_EQ(Reduced.eval(V), F.oracle(V));
            }
    }
}

TEST(SymMBAPrefixResidue, PairedProductsHaveIndependentFullWidthProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    auto F = pairedFlipFixture(Ctx, Width);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 24;
    auto Result = detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage,
                                               false, 65536);
    ASSERT_LT(Ctx.readabilityCost(Result), Ctx.readabilityCost(F.Input));
    solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 10000);
    EXPECT_EQ(Verifier(Ctx, F.Input, Result), SynthVerification::Equivalent);
  }
}

TEST(SymMBAPrefixResidue, PairedProductsKeepCumulativeResourceBounds) {
  auto Run = [](size_t Work, size_t Storage, bool &Complete) {
    SymContext Ctx;
    auto F = pairedFlipFixture(Ctx, 64);
    detail::WorkBudget Budget(Work);
    const auto Available = Storage;
    auto Result = detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage,
                                               false, Work, nullptr, &Complete);
    EXPECT_LE(Budget.used(), Work);
    EXPECT_LE(Storage, Available);
    SymEvalPlan Reduced(Ctx, Result);
    std::mt19937_64 Random(0x5041495242554447);
    for (unsigned I = 0; I < 128; ++I) {
      std::array<llvm::APInt, 3> Values{word(64, Random), word(64, Random),
                                        word(64, Random)};
      EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
    }
    return Budget.used();
  };
  bool Complete = false;
  const auto Used = Run(65536, 1 << 24, Complete);
  ASSERT_TRUE(Complete);
  ASSERT_GT(Used, 1u);
  Run(Used, 1 << 24, Complete);
  EXPECT_TRUE(Complete);
  Run(Used - 1, 1 << 24, Complete);
  EXPECT_FALSE(Complete);
  for (size_t Work : {size_t(0), size_t(1), size_t(32)}) {
    Run(Work, 1 << 24, Complete);
    EXPECT_FALSE(Complete);
  }
  for (size_t Storage : {size_t(0), size_t(1), size_t(256)}) {
    Run(65536, Storage, Complete);
    EXPECT_FALSE(Complete);
  }
}

TEST(SymMBAPrefixResidue, WidePairsRecoverNonlinearTwoBitCorrections) {
  std::mt19937_64 Random(0x5749444550414952);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned HighBits : {2u, 3u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << HighBits);
      SymContext Ctx;
      auto F = carryProductFixture(Ctx, Width, 128, 8, false, false, HighBits);
      MBAOptions Opts;
      Opts.MaxWork = 65536;
      Opts.VerifySamples = 0;
      auto Result = simplifyMBADeep(Ctx, F.Input, Opts);
      if (Width <= 64) {
        ASSERT_LE(Result.SizeAfter, 19u);
        EXPECT_LT(Result.Work, Opts.MaxWork);
        EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      }
      EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned Trial = 0; Trial < 1024; ++Trial) {
        std::array<llvm::APInt, 4> Values;
        for (auto &V : Values)
          V = word(Width, Random);
        if (Trial < 512)
          Values = {llvm::APInt(Width, Trial / 16),
                    llvm::APInt(Width, Trial % 16), llvm::APInt(Width, 17),
                    llvm::APInt(Width, 29)};
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

} // namespace
