//===- SymMBAAffineResidualTests.cpp - Small affine Boolean residuals ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"

#include <limits>
#include <string>
#include <vector>

using namespace neverd::symbolic;

namespace {

SymRef scaled(SymContext &Ctx, const llvm::APInt &Scale, SymRef E) {
  return Ctx.mkMul(Ctx.mkConst(Scale), E);
}

SymRef expandedResidual(SymContext &Ctx, SymRef X, SymRef Y,
                        const llvm::APInt &Scale) {
  const llvm::APInt One(Scale.getBitWidth(), 1);
  return Ctx.mkAdd({scaled(Ctx, Scale, X), scaled(Ctx, Scale - One, Y),
                    scaled(Ctx, -Scale - Scale, Ctx.mkAnd(X, Y))});
}

void expectAllPairsEqual(SymContext &Ctx, SymRef A, SymRef B, SymRef X,
                         SymRef Y) {
  std::vector<uint64_t> Assignment(Ctx.numVars(), 0);
  const uint64_t End = uint64_t(1) << Ctx.width(X);
  for (uint64_t XV = 0; XV < End; ++XV)
    for (uint64_t YV = 0; YV < End; ++YV) {
      Assignment[Ctx.varId(X)] = XV;
      Assignment[Ctx.varId(Y)] = YV;
      ASSERT_EQ(Ctx.evalU64(A, Assignment), Ctx.evalU64(B, Assignment));
    }
}

TEST(SymMBAAffineResidual, AlignsHalfValueSetsBeyondAdjacentDifferences) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8), Y = Ctx.mkVar("y", 8);
  SymRef E = expandedResidual(Ctx, X, Y, llvm::APInt(8, 1));
  SymRef Expected = Ctx.mkSub(Ctx.mkXor(X, Y), Y);
  auto Result = simplifyMBA(Ctx, E);
  EXPECT_EQ(Result.Expr, Expected) << Ctx.toString(Result.Expr);
  EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
  EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
}

TEST(SymMBAAffineResidual, HandlesOddEvenAndWideModularCoefficients) {
  for (unsigned Width : {1u, 2u, 3u, 8u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    for (int64_t Coefficient : {-6, -3, 2, 3, 6}) {
      SCOPED_TRACE(Coefficient);
      SymContext Ctx;
      SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      llvm::APInt Scale = llvm::APInt(64, Coefficient, true).sextOrTrunc(Width);
      if (Width > 64)
        Scale ^= llvm::APInt(Width, 1).shl(Width - 17);
      SymRef E = expandedResidual(Ctx, X, Y, Scale);
      SymRef Expected = Ctx.mkSub(scaled(Ctx, Scale, Ctx.mkXor(X, Y)), Y);
      MBAOptions Options;
      Options.VerifySamples = 0;
      auto Result = simplifyMBA(Ctx, E, Options);
      EXPECT_LE(Result.SizeAfter, Ctx.readabilityCost(Expected))
          << Ctx.toString(Result.Expr);
      detail::WorkBudget Proof(MBAOptions::UnlimitedWork);
      EXPECT_TRUE(
          detail::proveLinearIdentity(Ctx, Expected, Result.Expr, 2, Proof));
      if (Width <= 3)
        expectAllPairsEqual(Ctx, E, Result.Expr, X, Y);
    }
  }
}

TEST(SymMBAAffineResidual, RecoversThreeInputSelectorsInBothOrientations) {
  for (unsigned Width : {8u, 64u, 128u}) {
    for (int64_t Coefficient : {-4, -1, 1, 6}) {
      SymContext Ctx;
      SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      SymRef Z = Ctx.mkVar("z", Width);
      llvm::APInt Scale = llvm::APInt(64, Coefficient, true).sextOrTrunc(Width);
      for (bool Complement : {false, true}) {
        SymRef Pair = Ctx.mkAnd(Y, Z);
        SymRef E = Ctx.mkSub(expandedResidual(Ctx, X, Pair, Scale),
                             Ctx.mkSub(X, Pair));
        SymRef Expected = Ctx.mkSub(scaled(Ctx, Scale, Ctx.mkXor(X, Pair)), X);
        if (Complement) {
          E = Ctx.mkSub(Ctx.mkNeg(E), Ctx.mkOne(Width));
          Expected = Ctx.mkNot(Expected);
        }
        MBAOptions Options;
        Options.VerifySamples = 0;
        auto Result = simplifyMBA(Ctx, E, Options);
        EXPECT_LE(Result.SizeAfter, Ctx.readabilityCost(Expected))
            << Ctx.toString(Result.Expr);
        detail::WorkBudget Proof(MBAOptions::UnlimitedWork);
        EXPECT_TRUE(detail::proveLinearIdentity(Ctx, E, Result.Expr, 3, Proof));
      }
    }
  }
}

TEST(SymMBAAffineResidual, RestoresOpaqueArithmeticInputsAfterProof) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 64), Y = Ctx.mkVar("y", 64);
  SymRef Z = Ctx.mkVar("z", 64);
  SymRef A = Ctx.mkUDiv(X, Y), B = Ctx.mkAdd(Y, Z);
  SymRef E = expandedResidual(Ctx, A, B, llvm::APInt(64, 1));
  SymRef Expected = Ctx.mkSub(Ctx.mkXor(A, B), B);
  auto Result = simplifyMBA(Ctx, E);
  EXPECT_EQ(Result.Expr, Expected) << Ctx.toString(Result.Expr);
  EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
}

TEST(SymMBAAffineResidual, KeepsHiddenRestorationExactAtEveryBudget) {
  for (bool Deep : {false, true}) {
    for (size_t Limit : {size_t(0), size_t(32), size_t(128), size_t(256),
                         size_t(512), size_t(2048), size_t(1) << 22}) {
      SymContext Ctx;
      SymRef X = Ctx.mkVar("x", 3), Y = Ctx.mkVar("y", 3);
      SymRef Z = Ctx.mkVar("z", 3);
      SymRef E = expandedResidual(Ctx, Ctx.mkUDiv(X, Y), Ctx.mkAdd(Y, Z),
                                  llvm::APInt(3, 1));
      MBAOptions Options;
      Options.MaxWork = Limit;
      Options.VerifySamples = 0;
      auto Result = Deep ? simplifyMBADeep(Ctx, E, Options)
                         : simplifyMBA(Ctx, E, Options);
      EXPECT_LE(Result.Work, Limit);
      EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
      SymEvalPlan Before(Ctx, E), After(Ctx, Result.Expr);
      std::vector<uint64_t> Assignment(Ctx.numVars(), 0);
      for (unsigned K = 0; K < 512; ++K) {
        Assignment[Ctx.varId(X)] = K & 7;
        Assignment[Ctx.varId(Y)] = (K >> 3) & 7;
        Assignment[Ctx.varId(Z)] = K >> 6;
        EXPECT_EQ(Before.evalU64(Assignment), After.evalU64(Assignment));
      }
    }
  }
}

TEST(SymMBAAffineResidual, PreservesExistingProofWhenOptionalWorkRunsOut) {
  for (size_t Limit : {size_t(64), size_t(128), size_t(256), size_t(512)}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", 8), Y = Ctx.mkVar("y", 8);
    SymRef E = Ctx.mkAdd(Ctx.mkXor(X, Y),
                         scaled(Ctx, llvm::APInt(8, 2), Ctx.mkAnd(X, Y)));
    MBAOptions Options;
    Options.MaxWork = Limit;
    Options.VerifySamples = 0;
    auto Result = simplifyMBA(Ctx, E, Options);
    EXPECT_EQ(Result.Expr, Ctx.mkAdd(X, Y));
    EXPECT_LE(Result.Work, Limit);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
  }
}

TEST(SymMBAAffineResidual, BoundsWideWordScratchWithoutLosingExistingProofs) {
  for (unsigned Width : {129u, 4096u}) {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    SymRef E = expandedResidual(Ctx, X, Y, llvm::APInt(Width, 1));
    SymRef Expected = Ctx.mkSub(Ctx.mkXor(X, Y), Y);
    MBAOptions Tight;
    Tight.MaxTableBytes = 256;
    Tight.VerifySamples = 0;
    auto Limited = simplifyMBA(Ctx, E, Tight);
    EXPECT_EQ(Limited.Expr, E);
    auto Full = simplifyMBA(Ctx, E);
    EXPECT_EQ(Full.Expr, Expected);
    EXPECT_LT(Full.SizeAfter, Limited.SizeAfter);

    SymRef Carry = Ctx.mkAdd(
        Ctx.mkXor(X, Y), scaled(Ctx, llvm::APInt(Width, 2), Ctx.mkAnd(X, Y)));
    auto Existing = simplifyMBA(Ctx, Carry, Tight);
    EXPECT_EQ(Existing.Expr, Ctx.mkAdd(X, Y));
    EXPECT_EQ(Existing.Evidence, MBAEvidence::Derivation);
  }
}

TEST(SymMBAAffineResidual, RejectsNearMissesAndKeepsEveryBudgetExact) {
  for (int64_t Perturbation : {-1, 0, 1}) {
    for (size_t Limit : {size_t(0), size_t(1), size_t(64), size_t(512),
                         size_t(2048), size_t(1) << 22}) {
      SymContext Ctx;
      SymRef X = Ctx.mkVar("x", 4), Y = Ctx.mkVar("y", 4);
      SymRef E =
          Ctx.mkAdd(expandedResidual(Ctx, X, Y, llvm::APInt(4, 6)),
                    scaled(Ctx, llvm::APInt(64, Perturbation, true).trunc(4),
                           Ctx.mkAnd(X, Y)));
      MBAOptions Options;
      Options.MaxWork = Limit;
      Options.VerifySamples = 0;
      auto Result = simplifyMBA(Ctx, E, Options);
      EXPECT_LE(Result.Work, Limit);
      EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
      expectAllPairsEqual(Ctx, E, Result.Expr, X, Y);
      if (Perturbation)
        EXPECT_NE(
            Result.Expr,
            Ctx.mkSub(scaled(Ctx, llvm::APInt(4, 6), Ctx.mkXor(X, Y)), Y));
    }
  }
}

TEST(SymMBAAffineResidual, StopsAfterProvingAMinimalUnaryVariable) {
  for (unsigned Width : {2u, 3u, 8u, 64u, 128u, 4096u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    for (SymRef E : {Ctx.mkAdd(Ctx.mkMul(X, Y), Ctx.mkMul(X, Ctx.mkNot(Y))),
                     Ctx.mkNeg(X), Ctx.mkNot(X)}) {
      MBAOptions Options;
      Options.VerifySamples = 0;
      detail::WorkBudget ArithmeticBudget(Options.MaxWork);
      detail::SolveReport ArithmeticReport;
      SymRef Arithmetic = detail::solveArithmetic(
          Ctx, E, Options, ArithmeticBudget, ArithmeticReport);
      ASSERT_EQ(Ctx.readabilityCost(Arithmetic), 2u);
      detail::WorkBudget RegionBudget(Options.MaxWork);
      detail::SolveReport RegionReport;
      SymRef Result =
          detail::solveOneRegion(Ctx, E, Options, RegionBudget, RegionReport);
      EXPECT_EQ(Result, Arithmetic);
      // An exact minimal answer makes every later measurement redundant.
      EXPECT_EQ(RegionBudget.used(), ArithmeticBudget.used());
      EXPECT_EQ(RegionReport.Evidence, ArithmeticReport.Evidence);
      if (Width <= 3)
        expectAllPairsEqual(Ctx, E, Result, X, Y);
    }
  }
}

TEST(SymMBAAffineResidual, PreservesMinimalUnaryAnswersAcrossBudgets) {
  for (unsigned Width : {1u, 2u, 3u}) {
    for (size_t Limit : {size_t(0), size_t(1), size_t(16), size_t(64),
                         size_t(256), size_t(4096)}) {
      for (bool Deep : {false, true}) {
        SymContext Ctx;
        SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
        SymRef E = Ctx.mkAdd(Ctx.mkMul(X, Y), Ctx.mkMul(X, Ctx.mkNot(Y)));
        MBAOptions Options;
        Options.MaxWork = Limit;
        Options.VerifySamples = 0;
        auto Result = Deep ? simplifyMBADeep(Ctx, E, Options)
                           : simplifyMBA(Ctx, E, Options);
        EXPECT_LE(Result.Work, Limit);
        EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
        expectAllPairsEqual(Ctx, E, Result.Expr, X, Y);
      }
    }
  }
}

TEST(SymMBAAffineResidual, StopsAfterProvingAMinimalAffineVariable) {
  for (unsigned Width : {2u, 3u, 8u, 64u, 128u, 257u, 4096u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    for (SymRef E : {Ctx.mkAdd(Ctx.mkConst(Width, 2), Ctx.mkNot(X)),
                     Ctx.mkAdd(Ctx.mkConst(Width, 2), X),
                     Ctx.mkSub(Ctx.mkConst(Width, 2), X)}) {
      MBAOptions Options;
      Options.VerifySamples = 0;
      detail::WorkBudget ArithmeticBudget(Options.MaxWork);
      detail::SolveReport ArithmeticReport;
      SymRef Arithmetic = detail::solveArithmetic(
          Ctx, E, Options, ArithmeticBudget, ArithmeticReport);
      ASSERT_EQ(Ctx.readabilityCost(Arithmetic), 3u);
      detail::WorkBudget RegionBudget(Options.MaxWork);
      detail::SolveReport RegionReport;
      SymRef Result =
          detail::solveOneRegion(Ctx, E, Options, RegionBudget, RegionReport);
      EXPECT_EQ(Result, Arithmetic);
      EXPECT_EQ(RegionBudget.used(), ArithmeticBudget.used());
      EXPECT_EQ(RegionReport.Evidence, ArithmeticReport.Evidence);
      if (Width <= 3)
        expectAllPairsEqual(Ctx, E, Result, X, Y);
    }
    // The complement is cheaper than a subtraction with an all-ones offset.
    SymRef Complement = Ctx.mkSub(Ctx.mkOnes(Width), X);
    EXPECT_EQ(simplifyMBADeep(Ctx, Complement).Expr, Ctx.mkNot(X));
  }
}

TEST(SymMBAAffineResidual, StopsAtDistinctFreeVariableProducts) {
  for (unsigned Width : {1u, 2u, 8u, 64u, 256u, 4096u}) {
    for (unsigned Count : {2u, 3u, 4u, 6u}) {
      SymContext Ctx;
      llvm::SmallVector<SymRef, 8> Factors;
      for (unsigned I = 0; I < Count; ++I)
        Factors.push_back(Ctx.mkVar("v" + std::to_string(I), Width));
      SymRef E = Ctx.mkMul(Factors);
      for (size_t Limit : {size_t(0), size_t(1), size_t(128)}) {
        MBAOptions Options;
        Options.MaxWork = Limit;
        Options.MaxAtoms = Count;
        Options.VerifySamples = 0;
        detail::WorkBudget Budget(Limit);
        detail::SolveReport Report;
        SymRef Result = detail::solveOneRegion(Ctx, E, Options, Budget, Report);
        EXPECT_EQ(Result, E);
        EXPECT_EQ(Ctx.readabilityCost(Result), 2 * Count - 1);
        EXPECT_EQ(Budget.used(), 0u);
        EXPECT_FALSE(Budget.exhausted());
        EXPECT_EQ(Report.Outcome, MBAOutcome::AlreadyShortest);
      }
    }
  }
}

TEST(SymMBAAffineResidual, DoesNotAssumeEveryProductIsMinimal) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8), Y = Ctx.mkVar("y", 8);
  SymRef Z = Ctx.mkVar("z", 8);
  for (SymRef E : {Ctx.mkMul(X, X), Ctx.mkMul({Ctx.mkConst(8, 3), X, Y}),
                   Ctx.mkMul(Ctx.mkUDiv(X, Y), Z)}) {
    MBAOptions Options;
    Options.MaxWork = 128;
    Options.VerifySamples = 0;
    detail::WorkBudget Budget(Options.MaxWork);
    detail::SolveReport Report;
    detail::solveOneRegion(Ctx, E, Options, Budget, Report);
    EXPECT_GT(Budget.used(), 0u);
    EXPECT_LE(Budget.used(), Options.MaxWork);
  }
  // The cheap recognition stays within the existing per-region input ceiling.
  MBAOptions Options;
  Options.MaxAtoms = 2;
  Options.MaxWork = 128;
  detail::WorkBudget Budget(Options.MaxWork);
  detail::SolveReport Report;
  detail::solveOneRegion(Ctx, Ctx.mkMul({X, Y, Z}), Options, Budget, Report);
  EXPECT_TRUE(Report.TooWide);
  EXPECT_LE(Budget.used(), Options.MaxWork);
}

TEST(SymMBAAffineResidual, EnforcesArityWorkAndActualCostLimits) {
  for (unsigned Count : {1u, 2u, 3u, 4u}) {
    SymContext Ctx;
    llvm::SmallVector<SymRef, 4> Atoms;
    for (unsigned I = 0; I < Count; ++I)
      Atoms.push_back(Ctx.mkVar("v" + std::to_string(I), 8));
    std::vector<llvm::APInt> Weights;
    for (size_t K = 0; K < (size_t(1) << Count); ++K)
      Weights.emplace_back(8, K & 1 ? (K & 2 ? 255 : 1) : 0);
    auto Limits = detail::resolveLimits(MBAOptions{});
    Limits.MaxOptimalSynthesisAtoms = 4;
    for (size_t MaxCost : {size_t(0), std::numeric_limits<size_t>::max()})
      for (size_t BudgetLimit :
           {size_t(0), size_t(4), size_t(256), size_t(4096)}) {
        detail::WorkBudget Budget(BudgetLimit);
        llvm::SmallVector<SymRef, 8> Forms;
        detail::affineResidualCandidates(Ctx, Weights, Atoms, MaxCost, Limits,
                                         Budget, Forms);
        if (MaxCost == 0 || Count == 1 || Count == 4)
          EXPECT_TRUE(Forms.empty());
        EXPECT_LE(Budget.used(), BudgetLimit);
        if (Count == 1 || Count == 4)
          EXPECT_EQ(Budget.used(), 0u);
        else if (MaxCost != 0 && BudgetLimit == 0)
          EXPECT_TRUE(Budget.exhausted());
      }
  }
}

} // namespace
