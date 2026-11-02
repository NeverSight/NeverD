//===- SymMBAResiduePowerTests.cpp - Bounded modular power decisions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check high-degree low-word products against independently multiplied
/// complete words, including even inputs and exhausted proof resources.
///
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/solver/Z3Solver.h"
#include "neverd/symbolic/SymMBA.h"

#include <algorithm>
#include <array>
#include <random>
#include <utility>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

struct PowerFixture {
  SymRef Input;
  unsigned Width, Observed, Count;
  unsigned Perturb = 0;

  llvm::APInt oracle(const std::array<llvm::APInt, 3> &Values) const {
    auto Result = llvm::APInt(Width, 3).shl(Width - Observed) * Values[2];
    for (unsigned I = 0; I < Count; ++I)
      Result *= (Values[0] + llvm::APInt(Width, I).shl((Observed + 2) / 3) +
                 llvm::APInt(Width, I == 0 ? Perturb : 0)) ^
                Values[1].shl((Observed + 2) / 3);
    return Result;
  }
};

PowerFixture fixture(SymContext &Ctx, unsigned Width, unsigned Observed,
                     unsigned Count, unsigned Perturb = 0) {
  const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  const auto Z = Ctx.mkVar("z", Width);
  const unsigned Split = (Observed + 2) / 3;
  const auto High = Ctx.mkShl(Y, Ctx.mkConst(Width, Split));
  llvm::SmallVector<SymRef, 128> Factors{
      Ctx.mkConst(llvm::APInt(Width, 3).shl(Width - Observed)), Z};
  for (unsigned I = 0; I < Count; ++I)
    Factors.push_back(Ctx.mkXor(
        Ctx.mkAdd(X, Ctx.mkConst(llvm::APInt(Width, I).shl(Split) +
                                 llvm::APInt(Width, I == 0 ? Perturb : 0))),
        High));
  // Z has independent parity. In particular, an odd Z cannot justify
  // treating the repeated X factors as odd before reducing their powers.
  return {Ctx.mkMul(Factors), Width, Observed, Count, Perturb};
}

TEST(SymMBAResiduePowers, LowWordsKeepEvenInputsAndSmallExponents) {
  for (unsigned Observed = 1; Observed <= 8; ++Observed)
    for (unsigned Count :
         {1u, 2u, 3u, 7u, 8u, 16u, 17u, 32u, 33u, 64u, 65u, 96u}) {
      SCOPED_TRACE(::testing::Message() << Observed << ':' << Count);
      SymContext Ctx;
      const auto F = fixture(Ctx, 8, Observed, Count);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
      EXPECT_LE(Budget.used(), 65536u);
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned Trial = 0; Trial < 1024; ++Trial) {
        const std::array Values{
            llvm::APInt(8, Trial), llvm::APInt(8, Trial / 4),
            llvm::APInt(8, std::array{0u, 1u, 2u, 255u}[Trial / 256])};
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values)) << Trial;
      }
    }
}

TEST(SymMBAResiduePowers, LargeGroupsRetainFullWidthWords) {
  std::mt19937_64 Random(0x504f574552574944);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Count : {64u, 65u, 96u, 128u, 192u, 256u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Count);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, 8, Count);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
      // The immediate odd neighbor retains a nonconstant high correction;
      // it is an equivalence control even when the bounded emitter declines.
      if (Count != 65) {
        ASSERT_NE(Result, F.Input) << "work=" << Budget.used();
        EXPECT_LT(detail::readingCost(Ctx, Result),
                  detail::readingCost(Ctx, F.Input));
      }
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned Trial = 0; Trial < 128; ++Trial) {
        std::array<llvm::APInt, 3> Values;
        for (auto &Value : Values) {
          Value = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random()).shl(Bit);
        }
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

TEST(SymMBAResiduePowers, LargeBoundedDecisionsDoNotRequireAffineExpansion) {
  std::mt19937_64 Random(0x504f574552353132);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Observed : {4u, 6u, 8u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Observed);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, Observed, 512);
      ASSERT_GT(Ctx.numOperands(F.Input), 512u);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage, true);
      EXPECT_LE(Budget.used(), 65536u);
      EXPECT_NE(Result, F.Input) << "work=" << Budget.used();
      EXPECT_LT(detail::readingCost(Ctx, Result),
                detail::readingCost(Ctx, F.Input));
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned Trial = 0; Trial < 64; ++Trial) {
        std::array<llvm::APInt, 3> Values;
        for (auto &Value : Values) {
          Value = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random()).shl(Bit);
        }
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

TEST(SymMBAResiduePowers, LargeGroupsReachTheSharedSimplifier) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Observed : {4u, 6u, 8u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Observed);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, Observed, 512);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
      EXPECT_TRUE(Result.Changed) << "work=" << Result.Work;
      EXPECT_LT(Result.Work, Options.MaxWork);
      EXPECT_LT(Result.SizeAfter, F.Count);
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned I = 0; I < 256; ++I) {
        const std::array Values{llvm::APInt(Width, I),
                                llvm::APInt(Width, I * 17),
                                llvm::APInt(Width, I / 4)};
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

TEST(SymMBAResiduePowers, PeriodNeighborsReachTheSharedSimplifier) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Observed : {4u, 6u, 8u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Observed);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, Observed, 513);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
      EXPECT_TRUE(Result.Changed) << "work=" << Result.Work;
      EXPECT_LT(Result.Work, Options.MaxWork);
      EXPECT_LT(Result.SizeAfter, F.Count);
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned I = 0; I < 256; ++I) {
        const std::array Values{llvm::APInt(Width, I),
                                llvm::APInt(Width, I * 17),
                                llvm::APInt(Width, I / 4)};
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

TEST(SymMBAResiduePowers, ProjectedFactorsRetainLowDisturbances) {
  for (unsigned Width : {8u, 32u, 65u, 257u})
    for (unsigned Perturb : {0u, 1u, 128u, 256u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Perturb);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, 8, 512, Perturb);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage, true);
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned I = 0; I < 256; ++I)
        for (unsigned J : {0u, 1u, 7u, 31u}) {
          const std::array Values{llvm::APInt(Width, I), llvm::APInt(Width, J),
                                  llvm::APInt(Width, I * 53)};
          EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
        }
      const std::array Witness{llvm::APInt(Width, 1), llvm::APInt(Width, 0),
                               llvm::APInt(Width, 1)};
      const auto Unperturbed = llvm::APInt(Width, 3).shl(Width - 8);
      EXPECT_EQ(F.oracle(Witness),
                Unperturbed * llvm::APInt(Width, 1 + Perturb));
      if (Perturb == 1 || Perturb == 128)
        EXPECT_NE(F.oracle(Witness), Unperturbed);
      else
        EXPECT_EQ(F.oracle(Witness), Unperturbed);
    }
}

TEST(SymMBAResiduePowers, DominantPrefixesRetainDisturbedAndIndependentWords) {
  std::mt19937_64 Random(0x524553494455414c);
  for (unsigned Width : {8u, 32u, 65u, 257u})
    for (unsigned Observed : {4u, 6u, 8u})
      for (unsigned Count : {511u, 512u, 513u, 514u}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Observed << ':' << Count);
        SymContext Ctx;
        const auto F = fixture(Ctx, Width, Observed, Count, 1);
        const auto Z = Ctx.mkVar("z", Width);
        // This multiplier can be even or zero independently of the selected
        // X parity. It also moves the total count off a unit-period boundary.
        const auto Other = Ctx.mkAdd(Z, Ctx.mkConst(Width, 3));
        const auto Input = Ctx.mkMul(F.Input, Other);
        MBAOptions Options;
        Options.MaxWork = 65536;
        Options.VerifySamples = 0;
        const auto Result = simplifyMBADeep(Ctx, Input, Options);
        EXPECT_TRUE(Result.Changed) << "work=" << Result.Work;
        EXPECT_LT(Result.Work, Options.MaxWork);
        EXPECT_LT(Result.SizeAfter, Result.SizeBefore / 2);
        SymEvalPlan Reduced(Ctx, Result.Expr);
        for (unsigned Trial = 0; Trial < 256; ++Trial) {
          std::array<llvm::APInt, 3> Values;
          for (auto &Value : Values) {
            Value = llvm::APInt(Width, 0);
            for (unsigned Bit = 0; Bit < Width; Bit += 64)
              Value |= llvm::APInt(Width, Random()).shl(Bit);
          }
          // Exhaust every byte of X alongside independently varied complete
          // words, including both branches and residual zero/even values.
          Values[0].clearLowBits(8);
          Values[0] |= llvm::APInt(Width, Trial);
          if (Trial < 4)
            Values[2] = std::array{llvm::APInt(Width, 0), llvm::APInt(Width, 1),
                                   llvm::APInt(Width, 2),
                                   -llvm::APInt(Width, 3)}[Trial];
          EXPECT_EQ(Reduced.eval(Values),
                    F.oracle(Values) * (Values[2] + llvm::APInt(Width, 3)));
        }
      }
}

TEST(SymMBAResiduePowers, ConstantResiduesRemainProductFactors) {
  for (unsigned Width : {16u, 32u, 65u, 257u})
    for (unsigned Residue : {0u, 1u, 2u, 3u, 255u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Residue);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, 8, 512);
      const auto X = Ctx.mkVar("x", Width), Z = Ctx.mkVar("z", Width);
      const auto Carrier =
          Ctx.mkAdd(Ctx.mkConst(Width, Residue),
                    Ctx.mkMul(Ctx.mkConst(llvm::APInt(Width, 1).shl(8)), X));
      llvm::SmallVector<SymRef, 16> Factors(Ctx.operands(F.Input));
      for (auto &Factor : Factors)
        if (Factor == Z)
          Factor = Carrier;
      const auto Input = Ctx.mkMul(Factors);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, Input, Options);
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned I = 0; I < 256; ++I) {
        std::array Values{llvm::APInt(Width, I), llvm::APInt(Width, I * 17),
                          llvm::APInt(Width, 0)};
        Values[2] = llvm::APInt(Width, Residue) + Values[0].shl(8);
        EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

TEST(SymMBAResiduePowers, JointResidualZerosReduceGroupPrecision) {
  for (unsigned Width : {8u, 32u, 65u, 257u})
    for (unsigned Offset : {2u, 3u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Offset);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, 8, 511);
      const auto Input =
          Ctx.mkMul(F.Input, Ctx.mkAdd(Ctx.mkVar("z", Width),
                                       Ctx.mkConst(Width, Offset)));
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, Input, Options);
      // Neither residual is always even. Their product has a guaranteed zero
      // bit only for the odd offset; a nearby even offset is a negative
      // control.
      if (Offset == 3) {
        EXPECT_TRUE(Result.Changed);
        EXPECT_LT(Result.Work, Options.MaxWork);
        EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
      }
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned X = 0; X < 256; ++X)
        for (unsigned Z : {0u, 1u, 2u, 3u, 254u, 255u}) {
          const std::array Values{llvm::APInt(Width, X),
                                  llvm::APInt(Width, X * 17),
                                  llvm::APInt(Width, Z)};
          EXPECT_EQ(Reduced.eval(Values),
                    F.oracle(Values) *
                        (Values[2] + llvm::APInt(Width, Offset)));
        }
    }
}

TEST(SymMBAResiduePowers, ProjectedDomainsKeepEveryVisibleInputBit) {
  for (unsigned Width : {16u, 32u, 65u, 257u})
    for (unsigned Shift : {7u, 8u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Shift);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, 8, 513);
      const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      const auto U = Ctx.mkVar("u", Width), V = Ctx.mkVar("v", Width);
      const auto First = Ctx.mkXor(X, Ctx.mkShl(Y, Ctx.mkConst(Width, 3)));
      const auto High = Ctx.mkConst(llvm::APInt(Width, 1).shl(Shift));
      const auto Changed =
          Ctx.mkXor(Ctx.mkAdd(First, Ctx.mkMul(High, U)), Ctx.mkMul(High, V));
      llvm::SmallVector<SymRef, 16> Factors(Ctx.operands(F.Input));
      const auto It = std::find(Factors.begin(), Factors.end(), First);
      ASSERT_NE(It, Factors.end());
      *It = Changed;
      const auto Input = Ctx.mkMul(Factors);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, Input, Options);
      if (Shift == 8) {
        EXPECT_TRUE(Result.Changed);
        EXPECT_LT(Result.Work, Options.MaxWork);
        EXPECT_LT(Result.SizeAfter, F.Count);
      }
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned Trial = 0; Trial < 256; ++Trial) {
        const std::array Values{
            llvm::APInt(Width, Trial), llvm::APInt(Width, Trial * 17),
            llvm::APInt(Width, Trial / 4), llvm::APInt(Width, Trial * 53),
            llvm::APInt(Width, Trial / 8)};
        auto Expected = llvm::APInt(Width, 3).shl(Width - 8) * Values[2];
        for (unsigned I = 0; I < F.Count; ++I) {
          auto Factor =
              (Values[0] + llvm::APInt(Width, I).shl(3)) ^ Values[1].shl(3);
          if (I == 0)
            Factor = (Factor + Values[3].shl(Shift)) ^ Values[4].shl(Shift);
          Expected *= Factor;
        }
        EXPECT_EQ(Reduced.eval(Values), Expected);
      }
    }
}

TEST(SymMBAResiduePowers, ExactAndShortResourcesKeepCompleteProofs) {
  for (unsigned Count : {64u, 512u}) {
    SCOPED_TRACE(Count);
    SymContext FullCtx;
    const auto F = fixture(FullCtx, 64, 8, Count);
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
      SymContext Ctx;
      const auto Input = fixture(Ctx, 64, 8, Count);
      detail::WorkBudget Budget(Limits.first);
      size_t Storage = Limits.second;
      const auto Result =
          detail::foldResidueDecisions(Ctx, Input.Input, Budget, Storage);
      EXPECT_LE(Budget.used(), Limits.first);
      EXPECT_LE(Storage, Limits.second);
      if (Limits.first == Work && Limits.second == Bytes)
        EXPECT_EQ(Result.index(), Full.index());
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned I = 0; I < 256; ++I) {
        const std::array Values{llvm::APInt(64, I), llvm::APInt(64, I * 17),
                                llvm::APInt(64, I * 51)};
        EXPECT_EQ(Reduced.eval(Values), Input.oracle(Values));
      }
    }
  }
}

TEST(SymMBAResiduePowers, ParitySubgroupsKeepIndependentMultipliers) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext ExactCtx;
    const auto ExactInput = fixture(ExactCtx, Width, 6, 64);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 24;
    const auto Exact = detail::foldResidueDecisions(ExactCtx, ExactInput.Input,
                                                    Budget, Storage, true);
    ASSERT_NE(Exact, ExactInput.Input);
    SymContext Ctx;
    const auto F = fixture(Ctx, Width, 6, 64);
    MBAOptions Options;
    Options.MaxWork = 65536;
    Options.VerifySamples = 0;
    const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
    ASSERT_TRUE(Result.Changed);
    EXPECT_LT(Result.Work, Options.MaxWork);
    EXPECT_LT(Result.SizeAfter, F.Count);
    SymEvalPlan Reduced(Ctx, Result.Expr);
    for (unsigned I = 0; I < 256; ++I) {
      const std::array Values{llvm::APInt(Width, I), llvm::APInt(Width, I * 17),
                              llvm::APInt(Width, I / 4)};
      EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
    }
  }
}

TEST(SymMBAResiduePowers, RetainedGroupsRespectExactAndShortResources) {
  auto MakeInput = [](SymContext &Ctx) {
    const auto F = fixture(Ctx, 65, 6, 513, 1);
    return Ctx.mkMul(F.Input,
                     Ctx.mkAdd(Ctx.mkVar("z", 65), Ctx.mkConst(65, 3)));
  };
  SymContext FullCtx;
  const auto Input = MakeInput(FullCtx);
  detail::WorkBudget FullBudget(65536);
  size_t FullStorage = 1 << 24;
  const auto Full = detail::foldResidueDecisions(FullCtx, Input, FullBudget,
                                                 FullStorage, true);
  ASSERT_NE(Full, Input);
  const size_t Work = FullBudget.used(), Bytes = (1 << 24) - FullStorage;
  ASSERT_GT(Work, 1u);
  ASSERT_GT(Bytes, 1u);
  for (auto Limits : {std::pair<size_t, size_t>{0, Bytes},
                      {1, Bytes},
                      {Work, 0},
                      {Work - 1, Bytes},
                      {Work, Bytes - 1},
                      {Work, Bytes}}) {
    SymContext Ctx;
    const auto Original = MakeInput(Ctx);
    const auto F = fixture(Ctx, 65, 6, 513, 1);
    detail::WorkBudget Budget(Limits.first);
    size_t Storage = Limits.second;
    const auto Result =
        detail::foldResidueDecisions(Ctx, Original, Budget, Storage, true);
    EXPECT_LE(Budget.used(), Limits.first);
    EXPECT_LE(Storage, Limits.second);
    if (Limits.first == Work && Limits.second == Bytes)
      EXPECT_EQ(Result.index(), Full.index());
    SymEvalPlan Reduced(Ctx, Result);
    for (unsigned I = 0; I < 256; ++I) {
      const std::array Values{llvm::APInt(65, I), llvm::APInt(65, I * 17),
                              llvm::APInt(65, I * 51)};
      EXPECT_EQ(Reduced.eval(Values),
                F.oracle(Values) * (Values[2] + llvm::APInt(65, 3)));
    }
  }
}

TEST(SymMBAResiduePowers, ResidualGroupsHaveIndependentSolverProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Count : {512u, 513u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Count);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, 4, Count, 1);
      const auto Input = Ctx.mkMul(
          F.Input, Ctx.mkAdd(Ctx.mkVar("z", Width), Ctx.mkConst(Width, 3)));
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, Input, Options);
      ASSERT_TRUE(Result.Changed);
      EXPECT_LT(Result.Work, Options.MaxWork);
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 10000);
      EXPECT_EQ(Verifier(Ctx, Input, Result.Expr),
                SynthVerification::Equivalent);
    }
}

TEST(SymMBAResiduePowers, NativeWidthsHaveIndependentSolverProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Count : {2u, 7u, 16u, 17u, 512u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Count);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, Count < 8 ? 8 : 4, Count);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage);
      // A complete period must exercise an actual rewrite in each native
      // width; unchanged neighbor controls alone would make this proof vacuous.
      if (Count == 16 || Count == 512)
        ASSERT_NE(Result, F.Input);
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 10000);
      EXPECT_EQ(Verifier(Ctx, F.Input, Result), SynthVerification::Equivalent);
    }
}

TEST(SymMBAResiduePowers, ParityGroupsRetainMultipleIndependentWords) {
  std::mt19937_64 Random(0x5041524954595245);
  for (unsigned Width : {8u, 32u, 65u, 257u})
    for (unsigned Count : {16u, 17u, 32u, 33u, 64u, 65u}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Count);
      SymContext Ctx;
      const auto F = fixture(Ctx, Width, 4, Count, 1);
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      const auto Result = simplifyMBADeep(Ctx, F.Input, Options);
      ASSERT_TRUE(Result.Changed);
      EXPECT_LT(Result.Work, Options.MaxWork);
      EXPECT_LT(Result.SizeAfter, Result.SizeBefore / 2);
      SymEvalPlan Reduced(Ctx, Result.Expr);
      for (unsigned Trial = 0; Trial < 256; ++Trial) {
        std::array<llvm::APInt, 3> Values;
        for (auto &Value : Values) {
          Value = llvm::APInt(Width, 0);
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Value |= llvm::APInt(Width, Random()).shl(Bit);
        }
        Values[0].clearLowBits(8);
        Values[0] |= llvm::APInt(Width, Trial);
        if (Trial < 4)
          Values[2] = llvm::APInt(Width, Trial);
        ASSERT_EQ(Reduced.eval(Values), F.oracle(Values));
      }
    }
}

TEST(SymMBAResiduePowers, ParityMajorityDoesNotReplaceTheZeroBranchProof) {
  for (unsigned Count : {3u, 4u, 5u, 8u})
    for (unsigned Perturb : {0u, 1u, 2u}) {
      SCOPED_TRACE(::testing::Message() << Count << ':' << Perturb);
      SymContext Ctx;
      const auto F = fixture(Ctx, 8, 4, Count, Perturb);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      const auto Result =
          detail::foldResidueDecisions(Ctx, F.Input, Budget, Storage, true);
      SymEvalPlan Reduced(Ctx, Result);
      for (unsigned X = 0; X < 256; ++X)
        for (unsigned Y = 0; Y < 16; ++Y)
          for (unsigned Z : {0u, 1u, 2u, 255u}) {
            const std::array Values{llvm::APInt(8, X), llvm::APInt(8, Y),
                                    llvm::APInt(8, Z)};
            ASSERT_EQ(Reduced.eval(Values), F.oracle(Values));
          }
    }
}

} // namespace
