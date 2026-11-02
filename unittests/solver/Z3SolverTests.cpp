//===- Z3SolverTests.cpp - Optional backend session contracts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/solver/Z3Solver.h"

#include "llvm/ADT/SmallVector.h"

using namespace neverd::solver;
using namespace neverd::symbolic;

TEST(Z3Solver, DisabledBackendDoesNotSubstituteAnotherProver) {
  if (Z3Solver::available())
    GTEST_SKIP() << "This contract applies to builds without Z3";
  SymContext Ctx;
  const SymRef X = Ctx.mkVar("x", 8);
  Z3Solver Solver(Ctx);
  EXPECT_FALSE(Solver.ok());
  EXPECT_FALSE(Solver.assertTrue(Ctx.mkTrue()));
  EXPECT_EQ(Solver.check(), SatResult::Unknown);
  EXPECT_FALSE(Solver.reasonUnknown().empty());
  EXPECT_TRUE(Solver.model().empty());
  EXPECT_TRUE(Solver.dumpSMT2().empty());
  EXPECT_EQ(z3CheckEqual(Ctx, X, X), EquivResult::Unknown);
}

TEST(Z3Solver, MalformedQueriesRemainInvalidInEitherBuild) {
  SymContext Ctx;
  const SymRef X = Ctx.mkVar("x", 8);
  const SymRef Wide = Ctx.mkVar("x", 16);
  EXPECT_EQ(z3CheckSat(Ctx, SymRef()), SatResult::Invalid);
  EXPECT_EQ(z3CheckEqual(Ctx, X, Wide), EquivResult::Invalid);
  EXPECT_EQ(z3CheckEqual(Ctx, SymRef(), SymRef()), EquivResult::Invalid);
  Z3Solver Solver(Ctx);
  EXPECT_EQ(Solver.checkDistinct(X, Wide), SatResult::Invalid);
  EXPECT_EQ(Solver.check(), SatResult::Invalid);
  EXPECT_TRUE(Solver.model().empty());
}

TEST(Z3Solver, DistinctQueriesReuseTheSessionWithoutAddingAssertions) {
  if (!Z3Solver::available())
    GTEST_SKIP() << "Z3 backend disabled";
  SymContext Ctx;
  const SymRef X = Ctx.mkVar("x", 17);
  const SymRef Y = Ctx.mkVar("y", 17);
  Z3Solver Solver(Ctx);
  ASSERT_TRUE(Solver.assertEqual(X, Ctx.mkConst(17, 91)));
  EXPECT_EQ(Solver.checkDistinct(X, Ctx.mkConst(17, 91)), SatResult::Unsat);
  EXPECT_EQ(Solver.checkDistinct(X, Y), SatResult::Sat);
  ASSERT_TRUE(Solver.model().value(Ctx, X));
  ASSERT_TRUE(Solver.model().value(Ctx, Y));
  EXPECT_EQ(Solver.model().value(Ctx, X)->getZExtValue(), 91u);
  EXPECT_NE(Solver.model().value(Ctx, Y)->getZExtValue(), 91u);
  EXPECT_EQ(Solver.checkDistinct(X, X), SatResult::Unsat);
  EXPECT_TRUE(Solver.model().empty());
  EXPECT_EQ(Solver.check(), SatResult::Sat);
}

TEST(Z3Solver, AssumptionsHaveTemporaryModelsAndFailedCores) {
  if (!Z3Solver::available())
    GTEST_SKIP() << "Z3 backend disabled";
  SymContext Ctx;
  const SymRef X = Ctx.mkVar("x", 8);
  const SymRef One = Ctx.mkEq(X, Ctx.mkOne(8));
  const SymRef Two = Ctx.mkEq(X, Ctx.mkConst(8, 2));
  Z3Solver Solver(Ctx);
  EXPECT_EQ(Solver.check({One}), SatResult::Sat);
  ASSERT_TRUE(Solver.model().value(Ctx, X));
  EXPECT_EQ(Solver.model().value(Ctx, X)->getZExtValue(), 1u);
  EXPECT_EQ(Solver.check({Two}), SatResult::Sat);
  ASSERT_TRUE(Solver.model().value(Ctx, X));
  EXPECT_EQ(Solver.model().value(Ctx, X)->getZExtValue(), 2u);
  EXPECT_EQ(Solver.check({One, Two}), SatResult::Unsat);
  EXPECT_EQ(Solver.failedAssumptions().size(), 2u);
  EXPECT_TRUE(Solver.model().empty());
  EXPECT_EQ(Solver.check(), SatResult::Sat);
  EXPECT_TRUE(Solver.failedAssumptions().empty());
}

TEST(Z3Solver, DumpIncludesTheLastQueryAndDiscardsStaleAssumptions) {
  if (!Z3Solver::available())
    GTEST_SKIP() << "Z3 backend disabled";
  SymContext Ctx;
  const SymRef X = Ctx.mkVar("x", 8);
  Z3Solver Solver(Ctx);
  ASSERT_TRUE(Solver.assertTrue(X));
  const std::string Before = Solver.dumpSMT2();
  EXPECT_NE(Before.find("check-sat"), std::string::npos);
  ASSERT_EQ(Solver.checkDistinct(X, Ctx.mkConst(8, 57)), SatResult::Sat);
  const std::string Query = Solver.dumpSMT2();
  EXPECT_NE(Query, Before);
  EXPECT_EQ(Solver.check(), SatResult::Sat);
  EXPECT_EQ(Solver.dumpSMT2(), Before);
}

TEST(Z3Solver, ResourceExhaustionCannotBecomeAProof) {
  if (!Z3Solver::available())
    GTEST_SKIP() << "Z3 backend disabled";
  SymContext Ctx;
  const SymRef X = Ctx.mkVar("x", 64);
  const SymRef Y = Ctx.mkVar("y", 64);
  const SymRef Product = Ctx.mkMul(X, Y);
  Z3SolverOptions Options;
  Options.ResourceLimit = 1;
  Z3Solver Solver(Ctx, Options);
  EXPECT_EQ(Solver.checkDistinct(Product, Ctx.mkConst(64, 187)),
            SatResult::Unknown);
  EXPECT_TRUE(Solver.ok());
  EXPECT_FALSE(Solver.reasonUnknown().empty());
  EXPECT_TRUE(Solver.model().empty());
  EXPECT_EQ(z3CheckEqual(Ctx, Product, X, nullptr, Options),
            EquivResult::Unknown);
}

TEST(Z3Solver, ModelExtractionCanBeDisabledAndOldOutputIsCleared) {
  if (!Z3Solver::available())
    GTEST_SKIP() << "Z3 backend disabled";
  SymContext Ctx;
  const SymRef X = Ctx.mkVar("x", 129);
  Z3SolverOptions Options;
  Options.BuildModel = false;
  BitVectorModel Model;
  Model.set(Ctx.varId(X), llvm::APInt(129, 7));
  EXPECT_EQ(z3CheckSat(Ctx, X, &Model, Options), SatResult::Sat);
  EXPECT_TRUE(Model.empty());
  Model.set(Ctx.varId(X), llvm::APInt(129, 7));
  EXPECT_EQ(z3CheckEqual(Ctx, X, X, &Model), EquivResult::Equal);
  EXPECT_TRUE(Model.empty());
}

TEST(Z3Solver, ScaledProductsKeepFullWordsAndIndependentInputModels) {
  if (!Z3Solver::available())
    GTEST_SKIP() << "Z3 backend disabled";
  for (unsigned Width : {1u, 8u, 16u, 32u, 64u, 65u, 129u, 257u})
    for (unsigned Shift : {0u, 1u, Width / 2, Width - 1})
      for (bool Negative : {false, true}) {
        if (Shift >= Width)
          continue;
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Shift << ':' << Negative);
        auto Scale = llvm::APInt(Width, 3).shl(Shift);
        if (Negative)
          Scale = -Scale;
        SymContext Ctx;
        const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
        const auto Mask = ~llvm::APInt(Width, 1);
        const auto A = Ctx.mkAdd(X, Y);
        const auto B = Ctx.mkXor(X, Ctx.mkConst(Mask));
        const auto Product = Ctx.mkMul({Ctx.mkConst(Scale), A, B, A, B, B});
        for (unsigned Trial = 0; Trial < 4; ++Trial) {
          const auto XV = Trial == 0 ? llvm::APInt(Width, 0)
                                     : llvm::APInt::getAllOnes(Width) - Trial;
          const auto YV = llvm::APInt(Width, 1).shl(Width - 1) + Trial;
          const auto AV = XV + YV, BV = XV ^ Mask;
          const auto Expected = Scale * AV * BV * AV * BV * BV;
          Z3Solver Solver(Ctx);
          ASSERT_TRUE(Solver.assertEqual(X, Ctx.mkConst(XV)));
          ASSERT_TRUE(Solver.assertEqual(Y, Ctx.mkConst(YV)));
          // These wide concrete operator checks expose the complete query to
          // preprocessing. Incremental session behavior has its own tests.
          ASSERT_TRUE(Solver.assertDistinct(Product, Ctx.mkConst(Expected)));
          ASSERT_EQ(Solver.check(), SatResult::Unsat) << Solver.reasonUnknown();
          // A mismatch in the highest output bit remains observable. The
          // model must also retain all high input bits beyond the quotient.
          const auto Different =
              Expected ^ llvm::APInt(Width, 1).shl(Width - 1);
          Z3Solver ModelSolver(Ctx);
          ASSERT_TRUE(ModelSolver.assertEqual(X, Ctx.mkConst(XV)));
          ASSERT_TRUE(ModelSolver.assertEqual(Y, Ctx.mkConst(YV)));
          ASSERT_TRUE(
              ModelSolver.assertDistinct(Product, Ctx.mkConst(Different)));
          ASSERT_EQ(ModelSolver.check(), SatResult::Sat)
              << ModelSolver.reasonUnknown();
          ASSERT_TRUE(ModelSolver.model().value(Ctx, X));
          ASSERT_TRUE(ModelSolver.model().value(Ctx, Y));
          EXPECT_EQ(*ModelSolver.model().value(Ctx, X), XV);
          EXPECT_EQ(*ModelSolver.model().value(Ctx, Y), YV);
        }
      }
}

TEST(Z3Solver, RepeatedProductsKeepAllFactorsAndCompleteInputModels) {
  if (!Z3Solver::available())
    GTEST_SKIP() << "Z3 backend disabled";
  for (unsigned Width : {1u, 8u, 32u, 64u, 65u, 257u})
    for (unsigned Count : {7u, 8u, 9u, 63u, 64u, 65u, 128u, 129u})
      for (unsigned Shift : {0u, Width - 1}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Count << ':' << Shift);
        SymContext Ctx;
        const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
        const auto Other = Ctx.mkAdd(X, Y);
        const auto Scale = llvm::APInt(Width, 3).shl(Shift);
        llvm::SmallVector<SymRef, 16> Factors{Ctx.mkConst(Scale)};
        Factors.append(Count, X);
        Factors.append(Count + 1, Other);
        const auto Product = Ctx.mkMul(Factors);
        for (unsigned Trial = 0; Trial < 2; ++Trial) {
          const auto XV = llvm::APInt::getAllOnes(Width) - Trial;
          const auto YV = !Trial && Width > 1
                              ? llvm::APInt(Width, 1).shl(Width - 1)
                              : llvm::APInt(Width, Trial);
          auto Expected = Scale;
          // The oracle deliberately multiplies each complete original word.
          for (unsigned I = 0; I < Count; ++I)
            Expected *= XV;
          for (unsigned I = 0; I <= Count; ++I)
            Expected *= XV + YV;
          Z3Solver Solver(Ctx);
          ASSERT_TRUE(Solver.assertEqual(X, Ctx.mkConst(XV)));
          ASSERT_TRUE(Solver.assertEqual(Y, Ctx.mkConst(YV)));
          ASSERT_TRUE(Solver.assertDistinct(Product, Ctx.mkConst(Expected)));
          ASSERT_EQ(Solver.check(), SatResult::Unsat) << Solver.reasonUnknown();
          Z3Solver ModelSolver(Ctx);
          const auto Different =
              Expected ^ llvm::APInt(Width, 1).shl(Width - 1);
          ASSERT_TRUE(ModelSolver.assertEqual(X, Ctx.mkConst(XV)));
          ASSERT_TRUE(ModelSolver.assertEqual(Y, Ctx.mkConst(YV)));
          ASSERT_TRUE(
              ModelSolver.assertDistinct(Product, Ctx.mkConst(Different)));
          ASSERT_EQ(ModelSolver.check(), SatResult::Sat)
              << ModelSolver.reasonUnknown();
          ASSERT_TRUE(ModelSolver.model().value(Ctx, X));
          ASSERT_TRUE(ModelSolver.model().value(Ctx, Y));
          EXPECT_EQ(*ModelSolver.model().value(Ctx, X), XV);
          EXPECT_EQ(*ModelSolver.model().value(Ctx, Y), YV);
        }
      }
}
