//===- SymMBASelectorTests.cpp - Linear weight selector candidates --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <string>

using namespace neverd::symbolic;

namespace {

void simplifiesTo(llvm::StringRef Input, llvm::StringRef Expected,
                  uint32_t Width) {
  SCOPED_TRACE(Width);
  SCOPED_TRACE(Input.str());
  SymContext Ctx;
  SymParseResult Original = parseSymExpr(Ctx, Input, Width);
  SymParseResult Want = parseSymExpr(Ctx, Expected, Width);
  ASSERT_TRUE(Original.ok()) << Original.Error;
  ASSERT_TRUE(Want.ok()) << Want.Error;
  MBAOptions Opts;
  Opts.VerifySamples = 0;
  MBAResult R = simplifyMBA(Ctx, Original.Root, Opts);
  EXPECT_EQ(R.Expr, Want.Root) << Ctx.toString(R.Expr);
  EXPECT_LE(R.SizeAfter, R.SizeBefore);
  EXPECT_LE(R.Work, Opts.MaxWork);
  if (R.Changed)
    EXPECT_EQ(R.Evidence, MBAEvidence::Derivation);
}

TEST(SymMBASelector, KeepsIndependentWeightedAndFactoredVariableSums) {
  for (unsigned Count : {4u, 6u, 8u}) {
    std::string Weighted;
    std::string Sum;
    for (unsigned I = 0; I < Count; ++I) {
      if (I != 0) {
        Weighted += " + ";
        Sum += " + ";
      }
      const std::string Variable = "x" + std::to_string(I);
      Weighted += std::to_string(I % 3 + 2) + " * " + Variable;
      Sum += Variable;
    }
    const std::string Factored = "2 * (" + Sum + ")";
    std::string Grouped;
    for (unsigned Coefficient = 2; Coefficient != 5; ++Coefficient) {
      if (!Grouped.empty())
        Grouped += " + ";
      Grouped += std::to_string(Coefficient) + " * (";
      for (unsigned I = Coefficient - 2; I < Count; I += 3) {
        if (I != Coefficient - 2)
          Grouped += " + ";
        Grouped += "x" + std::to_string(I);
      }
      Grouped += ")";
    }
    for (uint32_t Width : {3u, 8u, 32u, 64u, 129u, 256u}) {
      simplifiesTo(Weighted, Grouped, Width);
      simplifiesTo(Factored, Factored, Width);
    }
  }
}

TEST(SymMBASelector, KeepsSignedAndWideIndependentCoefficients) {
  constexpr llvm::StringLiteral Signed(
      "-2 * x0 + 3 * x1 - 4 * x2 + 2 * x3 - 3 * x4 + 4 * x5");
  for (uint32_t Width : {3u, 8u, 32u, 64u, 129u, 256u}) {
    if (Width == 3)
      simplifiesTo(Signed, "-2*x0+3*x1+2*x3-3*x4+4*(x2+x5)", Width);
    else
      simplifiesTo(Signed, Signed, Width);
  }

  constexpr llvm::StringLiteral Wide(
      "0x100000000000000000000000000000002 * x0 + 3 * x1 - 2 * x2");
  for (uint32_t Width : {129u, 256u})
    simplifiesTo(Wide, Wide, Width);
}

TEST(SymMBASelector, StillRecoversOverlappingSelectors) {
  // Each expression has an interaction between inputs.  In particular, its
  // zero-weight minterm must not be added to the cumulative selector support.
  for (uint32_t Width : {3u, 8u, 32u, 64u, 129u, 256u}) {
    simplifiesTo("2 * x + (~x & y)", "x + (x | y)", Width);
    simplifiesTo("3 * x + (~x & y)", "2 * x + (x | y)", Width);
    simplifiesTo("-2 * x - (~x & y)", "-x - (x | y)", Width);
  }
}

TEST(SymMBASelector, RetainsEstablishedCandidatesForIndependentWeights) {
  // Skipping cumulative selectors must leave the older grouped and
  // conjunction candidates available to recover an ordinary variable sum.
  for (uint32_t Width : {3u, 32u, 64u, 256u}) {
    simplifiesTo("(x ^ y) + 2 * (x & y)", "x + y", Width);
    simplifiesTo("2 * (x | y) - (x ^ y)", "x + y", Width);
    simplifiesTo("(x | y) - (x & y)", "x ^ y", Width);
  }
}

} // namespace

TEST(SymMBASelector, ComplementaryBooleanWordMasksKeepWideDataOpaque) {
  for (unsigned Width : {3u, 8u, 64u, 129u}) {
    for (unsigned Form = 0; Form != 3; ++Form) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Form);
      SymContext C;
      auto Q = C.mkVar("control", 1);
      auto A = C.mkVar("left", Width), B = C.mkVar("right", Width);
      auto Mask = [&](SymRef Bit) {
        if (Form == 0)
          return C.mkNeg(C.mkZExt(Bit, Width));
        if (Form == 1)
          return C.mkSExt(Bit, Width);
        return C.mkAdd(C.mkConst(llvm::APInt::getAllOnes(Width)),
                       C.mkZExt(Bit, Width));
      };
      const auto Root =
          C.mkOr(C.mkAnd(A, Mask(Q)), C.mkAnd(B, Mask(C.mkNot(Q))));
      const auto Want = Form == 2 ? C.mkIte(Q, B, A) : C.mkIte(Q, A, B);
      MBAOptions Options;
      Options.VerifySamples = 0;
      Options.MaxWork = 65536;
      auto Result = simplifyMBADeep(C, Root, Options);
      EXPECT_EQ(Result.Expr, Want) << C.toString(Result.Expr);
      EXPECT_LE(Result.Work, Options.MaxWork);
      neverd::solver::BitVectorSolver Proof(C);
      EXPECT_EQ(Proof.check(C.mkNe(Root, Result.Expr)),
                neverd::solver::SatResult::Unsat);
      for (unsigned Bit = 0; Bit != 2; ++Bit)
        for (unsigned X = 0; X != 8; ++X)
          for (unsigned Y = 0; Y != 8; ++Y) {
            const llvm::APInt Inputs[] = {llvm::APInt(1, Bit),
                                          llvm::APInt(Width, X),
                                          llvm::APInt(Width, Y)};
            EXPECT_EQ(C.eval(Result.Expr, Inputs), C.eval(Root, Inputs));
          }
    }
  }
}

TEST(SymMBASelector, PartialMasksDoNotBecomeBooleanWordSelections) {
  SymContext C;
  auto A = C.mkVar("left", 8), B = C.mkVar("right", 8);
  auto Mask = C.mkVar("mask", 8);
  const auto Root = C.mkOr(C.mkAnd(A, Mask), C.mkAnd(B, C.mkNot(Mask)));
  auto Result = simplifyMBADeep(C, Root).Expr;
  for (unsigned X : {0u, 0x55u, 0xAAu, 255u})
    for (unsigned Y : {0u, 0x55u, 0xAAu, 255u})
      for (unsigned M = 0; M != 256; ++M) {
        const llvm::APInt Inputs[] = {llvm::APInt(8, X), llvm::APInt(8, Y),
                                      llvm::APInt(8, M)};
        EXPECT_EQ(C.eval(Result, Inputs), C.eval(Root, Inputs));
      }
}

TEST(SymMBASelector, LargePredicatesDoNotHideACompleteWordChoice) {
  SymContext C;
  llvm::SmallVector<SymRef, 64> Terms;
  for (unsigned I = 0; I != 64; ++I)
    Terms.push_back(C.mkFreshVar(64, "predicate_input_"));
  const auto Q = C.mkEq(C.mkAdd(Terms), C.mkZero(64));
  const auto A = C.mkVar("left", 64), B = C.mkVar("right", 64);
  const auto Mask = C.mkNeg(C.mkZExt(Q, 64));
  const auto Root = C.mkOr(C.mkAnd(A, Mask), C.mkAnd(B, C.mkNot(Mask)));
  MBAOptions Options;
  Options.MaxWork = 64;
  Options.VerifySamples = 0;
  const auto Result = simplifyMBADeep(C, Root, Options);
  EXPECT_EQ(Result.Expr, C.mkIte(Q, A, B)) << C.toString(Result.Expr);
  EXPECT_LE(Result.Work, Options.MaxWork);
  EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
}
