//===- SymMBAAffineFastPathTests.cpp - Bounded affine relation search ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"

#include <array>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::symbolic;

namespace {

SymRef affine(SymContext &Ctx, SymRef X, int64_t Scale, int64_t Offset) {
  unsigned Width = Ctx.width(X);
  auto literal = [&](int64_t V) {
    return Ctx.mkConst(llvm::APInt(64, V, true).sextOrTrunc(Width));
  };
  return Ctx.mkAdd(Ctx.mkMul(literal(Scale), X), literal(Offset));
}

TEST(SymMBAAffineFastPath, SameParitySharedInputsFitBoundedWork) {
  for (unsigned Parity : {0u, 1u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", 64);
    llvm::SmallVector<SymRef, 64> Terms;
    for (unsigned I = 0; I < 64; ++I) {
      SymRef Mask = Ctx.mkVar("mask" + std::to_string(I), 64);
      Terms.push_back(
          Ctx.mkAnd(affine(Ctx, X, 4 * I + 2, 2 * I + Parity), Mask));
    }
    SymRef E = Ctx.mkAdd(Terms);
    // All offsets have the same parity, which excludes affine complements.
    // The search should finish within this budget even with 64 shared bases.
    detail::WorkBudget Budget(5800);
    auto A = detail::abstractToMBA(Ctx, E, false, &Budget);
    ASSERT_TRUE(A);
    EXPECT_FALSE(Budget.exhausted());
    EXPECT_EQ(Ctx.substitute(A->Body, A->Hidden), E);
  }
}

TEST(SymMBAAffineFastPath, KeepsDependentAndNestedComplementSources) {
  for (unsigned Width : {3u, 8u, 32u, 64u, 128u, 256u}) {
    for (int64_t Scale : {2, 3, -6, -7}) {
      for (bool Products : {false, true}) {
        SymContext Ctx;
        SymRef X = Ctx.mkVar("x", Width);
        SymRef F = affine(Ctx, X, Scale, 1);
        SymRef G = Ctx.mkSub(Ctx.mkNeg(F), Ctx.mkOne(Width));
        // F and G's boundary summaries can both have odd offsets: G's
        // complete forced F term must retain the direct complement search.
        detail::WorkBudget Dependent(MBAOptions::UnlimitedWork);
        auto A =
            detail::abstractToMBA(Ctx, Ctx.mkAnd(F, G), Products, &Dependent);
        ASSERT_TRUE(A);
        EXPECT_EQ(A->Body, Ctx.mkZero(Width));

        SymRef Nested = affine(Ctx, Ctx.mkAdd(X, Ctx.mkOne(Width)), Scale, 0);
        SymRef Opposite = affine(Ctx, X, -Scale, -Scale - 1);
        detail::WorkBudget Other(MBAOptions::UnlimitedWork);
        auto B = detail::abstractToMBA(Ctx, Ctx.mkOr(Nested, Opposite),
                                       Products, &Other);
        ASSERT_TRUE(B);
        EXPECT_EQ(B->Body, Ctx.mkOnes(Width));
      }
    }
  }
}

TEST(SymMBAAffineFastPath, SameParityNeighboursPreserveActualRestoration) {
  constexpr unsigned Width = 3;
  for (int64_t Scale : {2, 3, -2, -3}) {
    for (int64_t Offset : {0, 1, 2, 3}) {
      SymContext Ctx;
      SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      SymRef F = affine(Ctx, X, Scale, Offset);
      SymRef G = affine(Ctx, X, -Scale, Offset + 2);
      SymRef E = Ctx.mkAdd({Ctx.mkAnd(F, G), Ctx.mkOr(F, Y), X});
      detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
      auto A = detail::abstractToMBA(Ctx, E, false, &Budget);
      ASSERT_TRUE(A);
      SymRef Restored = Ctx.substitute(A->Body, A->Hidden);
      std::vector<uint64_t> Values(Ctx.numVars(), 0);
      for (unsigned XV = 0; XV < 8; ++XV)
        for (unsigned YV = 0; YV < 8; ++YV) {
          Values[Ctx.varId(X)] = XV;
          Values[Ctx.varId(Y)] = YV;
          EXPECT_EQ(Ctx.evalU64(Restored, Values), Ctx.evalU64(E, Values));
        }
    }
  }
}

TEST(SymMBAAffineFastPath, OddCoefficientRecoveryPreservesEveryWord) {
  std::mt19937_64 Random(0x4f4444494e564552);
  for (unsigned Width : {1u, 2u, 3u, 8u, 16u, 32u, 64u, 65u, 128u, 257u}) {
    auto Word = [&] {
      llvm::APInt Value(Width, Random(), false, true);
      for (unsigned Bit = 64; Bit < Width; Bit += 64)
        Value |= llvm::APInt(Width, Random()).shl(Bit);
      return Value;
    };
    for (unsigned Trial = 0; Trial < 16; ++Trial) {
      SCOPED_TRACE(::testing::Message() << Width << " / " << Trial);
      auto Coefficient = Word() | 1;
      if (Trial < 2)
        Coefficient =
            Trial ? llvm::APInt::getAllOnes(Width) : llvm::APInt(Width, 1);
      const auto Offset = Word();
      SymContext Ctx;
      const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      const auto F = Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(Coefficient), X),
                               Ctx.mkConst(Offset));
      const auto Root = Ctx.mkAdd(Ctx.mkXor(F, Y), X);
      detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
      const auto Abstract = detail::abstractToMBA(Ctx, Root, false, &Budget);
      ASSERT_TRUE(Abstract);
      SymEvalPlan Restored(Ctx,
                           Ctx.substitute(Abstract->Body, Abstract->Hidden));
      for (unsigned Sample = 0; Sample < 64; ++Sample) {
        const auto XV = Word(), YV = Word();
        const auto Expected = ((Coefficient * XV + Offset) ^ YV) + XV;
        EXPECT_EQ(Restored.evalWith([&](uint32_t Id) -> const llvm::APInt * {
          return Id == Ctx.varId(X) ? &XV : Id == Ctx.varId(Y) ? &YV : nullptr;
        }),
                  Expected);
      }
    }
  }
}

TEST(SymMBAAffineFastPath, GateBudgetRefusalRemainsAtomic) {
  for (unsigned Width : {8u, 64u, 256u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    SymRef F = affine(Ctx, X, 3, 1);
    SymRef G = Ctx.mkSub(Ctx.mkNeg(F), Ctx.mkOne(Width));
    SymRef E = Ctx.mkAdd({Ctx.mkAnd(F, G), Ctx.mkAnd(F, Y), X});
    detail::WorkBudget Zero(0), Unlimited(MBAOptions::UnlimitedWork);
    auto Original = detail::abstractToMBA(Ctx, E, false, &Zero);
    auto Full = detail::abstractToMBA(Ctx, E, false, &Unlimited);
    ASSERT_TRUE(Original);
    ASSERT_TRUE(Full);
    ASSERT_NE(Original->Body, Full->Body);
    for (size_t Limit = 0; Limit < Unlimited.used(); ++Limit) {
      detail::WorkBudget Tight(Limit);
      auto A = detail::abstractToMBA(Ctx, E, false, &Tight);
      ASSERT_TRUE(A);
      EXPECT_TRUE(Tight.exhausted());
      EXPECT_EQ(A->Body, Original->Body);
      EXPECT_EQ(A->Hidden, Original->Hidden);
    }
    for (size_t Bytes = 0; Bytes < 8192; Bytes += 113) {
      detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
      auto A = detail::abstractToMBA(Ctx, E, false, &Budget, Bytes);
      ASSERT_TRUE(A);
      EXPECT_TRUE(A->Body == Original->Body || A->Body == Full->Body);
      EXPECT_EQ(A->Hidden, Original->Hidden);
    }
  }
}

TEST(SymMBAAffineFastPath, DirectAffineLeavesKeepOpaqueAndNestedBoundaries) {
  std::mt19937_64 Random(0x4449524543544146);
  for (unsigned Width : {3u, 8u, 64u, 257u}) {
    auto Word = [&] {
      llvm::APInt Value(Width, Random(), false, true);
      for (unsigned Bit = 64; Bit < Width; Bit += 64)
        Value |= llvm::APInt(Width, Random()).shl(Bit);
      return Value;
    };
    for (unsigned Kind = 0; Kind < 4; ++Kind)
      for (bool Scale : {false, true})
        for (bool Products : {false, true}) {
          SCOPED_TRACE(::testing::Message() << Width << " / " << Kind << " / "
                                            << Scale << " / " << Products);
          SymContext Ctx;
          const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
          const auto Z = Ctx.mkVar("z", Width);
          const std::array Bases{X, Ctx.mkAnd(X, Y), Ctx.mkMul(X, Y),
                                 Ctx.mkAdd(X, Y)};
          const auto Base = Bases[Kind];
          const auto Literal = Ctx.mkConst(Width, Scale ? 5 : 17);
          const auto Affine =
              Scale ? Ctx.mkMul(Literal, Base) : Ctx.mkAdd(Literal, Base);
          const auto Root = Ctx.mkAdd(Ctx.mkXor(Affine, Z), Base);
          detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
          const auto A = detail::abstractToMBA(Ctx, Root, Products, &Budget);
          ASSERT_TRUE(A);
          SymEvalPlan Restored(Ctx, Ctx.substitute(A->Body, A->Hidden));
          for (unsigned Trial = 0; Trial < (Width == 3 ? 512u : 64u); ++Trial) {
            const auto XV = Width == 3 ? llvm::APInt(Width, Trial & 7) : Word();
            const auto YV =
                Width == 3 ? llvm::APInt(Width, (Trial >> 3) & 7) : Word();
            const auto ZV =
                Width == 3 ? llvm::APInt(Width, Trial >> 6) : Word();
            const auto B = Kind == 0   ? XV
                           : Kind == 1 ? XV & YV
                           : Kind == 2 ? XV * YV
                                       : XV + YV;
            const auto F = Scale ? 5 * B : B + 17;
            EXPECT_EQ(
                Restored.evalWith([&](uint32_t Id) -> const llvm::APInt * {
                  return Id == Ctx.varId(X)   ? &XV
                         : Id == Ctx.varId(Y) ? &YV
                         : Id == Ctx.varId(Z) ? &ZV
                                              : nullptr;
                }),
                (F ^ ZV) + B);
          }
        }
  }
}

TEST(SymMBAAffineFastPath, WideFrontiersRetainIndependentProductInputs) {
  std::mt19937_64 Random(0x46524f4e54494552);
  for (unsigned Width : {3u, 8u, 64u, 65u, 128u, 257u}) {
    for (unsigned Count : {33u, 65u}) {
      SymContext Ctx;
      const auto Y = Ctx.mkVar("y", Width);
      llvm::SmallVector<SymRef, 8> Variables, Terms;
      for (unsigned I = 0; I < Count; ++I) {
        const auto X = Ctx.mkVar("x" + std::to_string(I), Width);
        Variables.push_back(X);
        Terms.push_back(Ctx.mkMul(
            Ctx.mkConst(llvm::APInt(Width, 2 * I + 1, false, true)), X));
      }
      Terms.push_back(Ctx.mkConst(llvm::APInt(Width, 17, false, true)));
      const auto Root = Ctx.mkAdd({Ctx.mkXor(Ctx.mkAdd(Terms), Y),
                                   Ctx.mkAdd(Variables), Ctx.mkMul(Variables)});
      for (auto [Limit, Bytes] :
           {std::pair{size_t(0), size_t(1 << 24)},
            std::pair{size_t(4096), size_t(1 << 24)},
            std::pair{MBAOptions::UnlimitedWork, size_t(0)},
            std::pair{MBAOptions::UnlimitedWork, size_t(2047)},
            std::pair{MBAOptions::UnlimitedWork, size_t(2048)},
            std::pair{MBAOptions::UnlimitedWork, size_t(4096)},
            std::pair{MBAOptions::UnlimitedWork, size_t(1 << 24)}}) {
        SCOPED_TRACE(::testing::Message() << Width << " / " << Count << " / "
                                          << Limit << " / " << Bytes);
        detail::WorkBudget Budget(Limit);
        const auto A = detail::abstractToMBA(Ctx, Root, true, &Budget, Bytes);
        ASSERT_TRUE(A);
        SymEvalPlan Restored(Ctx, Ctx.substitute(A->Body, A->Hidden));
        std::vector<llvm::APInt> Values(Ctx.numVars(), llvm::APInt(Width, 0));
        for (unsigned Sample = 0; Sample < 32; ++Sample) {
          for (auto &Value : Values) {
            Value = llvm::APInt(Width, Random(), false, true);
            for (unsigned Bit = 64; Bit < Width; Bit += 64)
              Value |= llvm::APInt(Width, Random()).shl(Bit);
            if (Sample % 2)
              Value |= 1;
          }
          llvm::APInt Sum(Width, 0), Weighted(Width, 17, false, true),
              Product(Width, 1);
          for (unsigned I = 0; I < Count; ++I) {
            const auto &Value = Values[Ctx.varId(Variables[I])];
            Sum += Value;
            Weighted += llvm::APInt(Width, 2 * I + 1, false, true) * Value;
            Product *= Value;
          }
          const auto Expected =
              (Weighted ^ Values[Ctx.varId(Y)]) + Sum + Product;
          EXPECT_EQ(Restored.evalWith([&](uint32_t Id) { return &Values[Id]; }),
                    Expected);
        }
      }
    }
  }
}

TEST(SymMBAAffineFastPath, SharedArithmeticAndComplementsKeepEveryInput) {
  for (unsigned Width : {3u, 8u, 65u, 257u}) {
    for (bool Products : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Width << " / " << Products);
      SymContext Ctx;
      const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      const auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
      const auto Sum = Ctx.mkAdd(X, Y);
      const auto Product = Ctx.mkMul(Sum, Ctx.mkAnd(Y, Z));
      const auto Complement = Ctx.mkNot(Product);
      auto DisguisedAdd = [&](SymRef A, SymRef B) {
        return Ctx.mkAdd(Ctx.mkXor(A, B),
                         Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkAnd(A, B)));
      };
      const auto Root =
          Ctx.mkAdd({DisguisedAdd(Product, W), DisguisedAdd(Complement, Y),
                     DisguisedAdd(Sum, Z)});
      detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
      const auto A = detail::abstractToMBA(Ctx, Root, Products, &Budget);
      ASSERT_TRUE(A);
      SymEvalPlan Restored(Ctx, Ctx.substitute(A->Body, A->Hidden));
      std::mt19937_64 Random(0x534841524544524f);
      const unsigned Trials = Width == 3 ? 4096 : 128;
      for (unsigned Trial = 0; Trial < Trials; ++Trial) {
        std::array<llvm::APInt, 4> V;
        for (unsigned I = 0; I < V.size(); ++I) {
          V[I] = llvm::APInt(Width,
                             Width == 3 ? (Trial >> (3 * I)) & 7 : Random());
          for (unsigned Bit = 64; Bit < Width; Bit += 64)
            V[I] |= llvm::APInt(Width, Random()).shl(Bit);
        }
        const auto Expected = V[0] + 2 * V[1] + V[2] + V[3] - 1;
        EXPECT_EQ(Restored.evalWith([&](uint32_t Id) -> const llvm::APInt * {
          return Id == Ctx.varId(X)   ? &V[0]
                 : Id == Ctx.varId(Y) ? &V[1]
                 : Id == Ctx.varId(Z) ? &V[2]
                 : Id == Ctx.varId(W) ? &V[3]
                                      : nullptr;
        }),
                  Expected);
      }
    }
  }
}

TEST(SymMBAAffineFastPath, TransferredTermsCombineWithSharedAffineTails) {
  std::mt19937_64 Random(0x5452414e53464552);
  for (unsigned Width : {3u, 8u, 64u, 65u, 128u, 257u}) {
    SymContext Ctx;
    const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
    const auto Three = Ctx.mkConst(llvm::APInt(Width, 3));
    const auto Five = Ctx.mkConst(llvm::APInt(Width, 5));
    const auto Shared = Ctx.mkAdd(X, Y);
    const auto F = Ctx.mkSub(Ctx.mkMul(Three, Shared), Ctx.mkMul(Three, X));
    const auto G = Ctx.mkAdd(Ctx.mkMul(Five, Shared), X);
    const auto Root = Ctx.mkAdd({Ctx.mkXor(F, Z), Ctx.mkAnd(G, W), X});
    detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
    const auto A = detail::abstractToMBA(Ctx, Root, false, &Budget);
    ASSERT_TRUE(A);
    SymEvalPlan Restored(Ctx, Ctx.substitute(A->Body, A->Hidden));
    std::vector<llvm::APInt> Values(Ctx.numVars(), llvm::APInt(Width, 0));
    for (unsigned Sample = 0; Sample < 128; ++Sample) {
      for (auto &Value : Values) {
        Value = llvm::APInt(Width, Random(), false, true);
        for (unsigned Bit = 64; Bit < Width; Bit += 64)
          Value |= llvm::APInt(Width, Random()).shl(Bit);
      }
      const auto &XV = Values[Ctx.varId(X)], &YV = Values[Ctx.varId(Y)];
      const auto &ZV = Values[Ctx.varId(Z)], &WV = Values[Ctx.varId(W)];
      const auto Expected = ((3 * YV) ^ ZV) + ((6 * XV + 5 * YV) & WV) + XV;
      EXPECT_EQ(Restored.evalWith([&](uint32_t Id) { return &Values[Id]; }),
                Expected);
    }
  }
}

TEST(SymMBAAffineFastPath, OpaqueInteriorsDoNotMintUnusedInputs) {
  std::mt19937_64 Random(0x4f5041515545494e);
  for (unsigned Width : {3u, 8u, 64u, 65u, 128u, 257u}) {
    SymContext Ctx;
    const auto X = Ctx.mkVar("mba$" + std::to_string(Width) + ".0", Width);
    const auto Y = Ctx.mkVar("y", Width), Z = Ctx.mkVar("z", Width);
    const auto Divisor = Ctx.mkVar("d", Width);
    SymRef Interior = X;
    for (unsigned I = 0; I < 64; ++I)
      Interior = Ctx.mkXor(Ctx.mkAdd(Interior, Y),
                           Ctx.mkConst(llvm::APInt(Width, I % 7 + 1)));
    const auto Opaque = Ctx.mkURem(Interior, Divisor);
    const auto Root = Ctx.mkAdd(
        {Ctx.mkXor(Opaque, Z),
         Ctx.mkMul(Ctx.mkConst(llvm::APInt(Width, 2)), Ctx.mkAnd(Opaque, Z)),
         X});
    const auto OriginalVariables = Ctx.numVars();
    detail::WorkBudget Budget(MBAOptions::UnlimitedWork);
    const auto A = detail::abstractToMBA(Ctx, Root, false, &Budget);
    ASSERT_TRUE(A);
    EXPECT_EQ(A->Hidden.size(), 1u);
    EXPECT_EQ(Ctx.numVars(), OriginalVariables + 1);
    EXPECT_FALSE(A->Hidden.contains(X.index()));
    SymEvalPlan Restored(Ctx, Ctx.substitute(A->Body, A->Hidden));
    std::vector<llvm::APInt> Values(Ctx.numVars(), llvm::APInt(Width, 0));
    for (unsigned Sample = 0; Sample < 64; ++Sample) {
      for (auto &Value : Values) {
        Value = llvm::APInt(Width, Random(), false, true);
        for (unsigned Bit = 64; Bit < Width; Bit += 64)
          Value |= llvm::APInt(Width, Random()).shl(Bit);
      }
      const auto &XV = Values[Ctx.varId(X)], &YV = Values[Ctx.varId(Y)];
      const auto &DV = Values[Ctx.varId(Divisor)];
      auto Inner = XV;
      for (unsigned I = 0; I < 64; ++I)
        Inner = (Inner + YV) ^ llvm::APInt(Width, I % 7 + 1);
      const auto Remainder = DV.isZero() ? Inner : Inner.urem(DV);
      const auto Expected = Remainder + Values[Ctx.varId(Z)] + XV;
      EXPECT_EQ(Restored.evalWith([&](uint32_t Id) { return &Values[Id]; }),
                Expected);
    }
  }
}

} // namespace
