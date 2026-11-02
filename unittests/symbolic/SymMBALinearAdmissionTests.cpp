//===- SymMBALinearAdmissionTests.cpp - Dense corner admission ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks exact rejection certificates before dense linear MBA measurement,
/// including consumer boundaries, full-width semantics and resource refusal.
///
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"

#include <array>
#include <bit>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace neverd::symbolic;

namespace {

enum class LinearFixture { TwoGroups, OneGroup, Conjunction, Negative };

struct AdmissionFixture {
  std::array<SymRef, 12> Variables;
  SymRef Input;
};

AdmissionFixture makeAdmission(SymContext &Ctx, unsigned Width,
                               LinearFixture Kind) {
  AdmissionFixture F;
  for (unsigned I = 0; I < F.Variables.size(); ++I)
    F.Variables[I] = Ctx.mkVar("v" + std::to_string(I), Width);
  if (Kind == LinearFixture::OneGroup)
    F.Input = Ctx.mkXor(F.Variables);
  else if (Kind == LinearFixture::Conjunction)
    F.Input = Ctx.mkAnd(F.Variables);
  else if (Kind == LinearFixture::Negative) {
    std::vector<SymRef> Terms{Ctx.mkAnd(F.Variables[2], F.Variables[3])};
    for (unsigned I = 0; I < F.Variables.size(); ++I)
      if (I != 3)
        Terms.push_back(Ctx.mkMul(Ctx.mkConst(llvm::APInt::getAllOnes(Width)),
                                  F.Variables[I]));
    F.Input = Ctx.mkAdd(Terms);
  } else {
    // Interleaving makes the first two coordinates independent. Odd-sized
    // groups also expose the same two nonzero weights at opposite corners.
    std::vector<SymRef> A, B;
    for (unsigned I = 0; I < F.Variables.size(); ++I)
      ((I < 10 && I % 2 == 0) ? A : B).push_back(F.Variables[I]);
    F.Input = Ctx.mkAdd(Ctx.mkXor(A), Ctx.mkXor(B));
  }
  return F;
}

llvm::APInt admissionOracle(llvm::ArrayRef<llvm::APInt> Values,
                            LinearFixture Kind) {
  const unsigned Width = Values.front().getBitWidth();
  llvm::APInt A(Width, 0), B(Width, 0);
  if (Kind == LinearFixture::Negative) {
    A = Values[2] & Values[3];
    for (unsigned I = 0; I < Values.size(); ++I)
      if (I != 3)
        A -= Values[I];
    return A;
  }
  if (Kind == LinearFixture::Conjunction)
    A.setAllBits();
  for (unsigned I = 0; I < Values.size(); ++I) {
    if (Kind == LinearFixture::Conjunction)
      A &= Values[I];
    else if (Kind == LinearFixture::OneGroup || (I < 10 && I % 2 == 0))
      A ^= Values[I];
    else
      B ^= Values[I];
  }
  return A + B;
}

void checkAdmissionSemantics(SymContext &Ctx, const AdmissionFixture &F,
                             SymRef Result, LinearFixture Kind) {
  const unsigned Width = Ctx.width(F.Input);
  SymEvalPlan Before(Ctx, F.Input), After(Ctx, Result);
  std::vector<llvm::APInt> Assignment(Ctx.numVars(), llvm::APInt(Width, 0));
  std::array<llvm::APInt, 12> Values;
  auto check = [&] {
    for (unsigned I = 0; I < Values.size(); ++I)
      Assignment[Ctx.varId(F.Variables[I])] = Values[I];
    const llvm::APInt Expected = admissionOracle(Values, Kind);
    EXPECT_EQ(Before.eval(Assignment), Expected);
    EXPECT_EQ(After.eval(Assignment), Expected);
  };
  // Every Boolean corner is exact, including the unseen all-ones conjunction
  // witness. Wide mixed assignments below exercise carries between bits.
  for (unsigned Pattern = 0; Pattern < (1u << Values.size()); ++Pattern) {
    for (unsigned I = 0; I < Values.size(); ++I)
      Values[I] = (Pattern & (1u << I)) ? llvm::APInt::getAllOnes(Width)
                                        : llvm::APInt(Width, 0);
    check();
  }
  std::mt19937_64 Random(0x1c0a7e);
  for (unsigned Sample = 0; Sample < 64; ++Sample) {
    for (llvm::APInt &Value : Values) {
      Value = llvm::APInt(Width, 0);
      for (unsigned Bit = 0; Bit < Width; Bit += 64)
        Value |= llvm::APInt(Width, Random()).shl(Bit);
    }
    check();
  }
}

MBAOptions admissionOptions() {
  MBAOptions Opts;
  Opts.MaxWork = 4095;
  Opts.VerifySamples = 0;
  return Opts;
}

TEST(SymMBALinearAdmission, RejectsOnlyMoreThanTheGroupLimit) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    for (LinearFixture Kind :
         {LinearFixture::TwoGroups, LinearFixture::OneGroup,
          LinearFixture::Conjunction}) {
      SCOPED_TRACE(static_cast<unsigned>(Kind));
      SymContext Ctx;
      const AdmissionFixture F = makeAdmission(Ctx, Width, Kind);
      const MBAResult Result = simplifyMBA(Ctx, F.Input, admissionOptions());
      EXPECT_LE(Result.Work, admissionOptions().MaxWork);
      EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
      if (Kind == LinearFixture::TwoGroups)
        EXPECT_NE(Result.Outcome, MBAOutcome::BudgetExhausted);
      else
        EXPECT_EQ(Result.Outcome, MBAOutcome::BudgetExhausted);
      checkAdmissionSemantics(Ctx, F, Result.Expr, Kind);
    }
  }
}

TEST(SymMBALinearAdmission, CoversHiddenSubtractionNodesAtFullWidth) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const AdmissionFixture F =
        makeAdmission(Ctx, Width, LinearFixture::Negative);
    SymEvalPlan Plan(Ctx, F.Input);
    EXPECT_GT(Plan.numSteps(), Ctx.readabilityCost(F.Input));
    EXPECT_LE(Plan.numSteps(), 2 * Ctx.readabilityCost(F.Input));
    const MBAResult Result = simplifyMBA(Ctx, F.Input, admissionOptions());
    EXPECT_NE(Result.Outcome, MBAOutcome::BudgetExhausted);
    checkAdmissionSemantics(Ctx, F, Result.Expr, LinearFixture::Negative);
  }
}

TEST(SymMBALinearAdmission, PreservesTheConjunctionConsumerAtItsExactLimit) {
  SymContext Ctx;
  std::array<SymRef, 4> Atoms;
  for (unsigned I = 0; I < Atoms.size(); ++I)
    Atoms[I] = Ctx.mkVar("c" + std::to_string(I), 257);
  std::vector<llvm::APInt> Weights;
  for (unsigned Pattern = 0; Pattern < 16; ++Pattern)
    Weights.emplace_back(257, std::popcount(Pattern));
  detail::SolverLimits Limits;
  Limits.MaxAtoms = 4;
  Limits.MaxSynthesisAtoms = 0;
  Limits.SynthesisWork = 64;
  llvm::SmallVector<SymRef, 8> Forms;
  detail::linearCandidates(Ctx, Weights, Atoms, 128, Limits, Forms);
  EXPECT_TRUE(llvm::is_contained(Forms, Ctx.mkAdd(Atoms)));
  Limits.SynthesisWork = 63;
  Forms.clear();
  detail::linearCandidates(Ctx, Weights, Atoms, 128, Limits, Forms);
  EXPECT_TRUE(Forms.empty());
}

TEST(SymMBALinearAdmission, KeepsTinyResidualSynthesisAvailable) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    const SymRef X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    const SymRef Z = Ctx.mkVar("z", Width);
    auto scale = [&](int V, SymRef E) {
      return Ctx.mkMul(Ctx.mkConst(llvm::APInt(64, V, true).sextOrTrunc(Width)),
                       E);
    };
    const SymRef Input = Ctx.mkAdd(
        {scale(5, Ctx.mkAdd({X, Y, Z})),
         scale(-8,
               Ctx.mkAdd({Ctx.mkAnd(X, Y), Ctx.mkAnd(X, Z), Ctx.mkAnd(Y, Z)})),
         scale(16, Ctx.mkAnd({X, Y, Z}))});
    const SymRef Majority =
        Ctx.mkOr(Ctx.mkAnd(X, Y), Ctx.mkAnd(Z, Ctx.mkOr(X, Y)));
    const SymRef Compact =
        Ctx.mkAdd(scale(2, Majority), scale(5, Ctx.mkXor({X, Y, Z})));
    MBAOptions Opts;
    Opts.VerifySamples = 0;
    const MBAResult Result = simplifyMBA(Ctx, Input, Opts);
    EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
    EXPECT_LE(Result.SizeAfter, Ctx.readabilityCost(Compact));
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    SymEvalPlan Plan(Ctx, Result.Expr);
    std::mt19937_64 Random(0x710f);
    std::vector<llvm::APInt> Values(Ctx.numVars(), llvm::APInt(Width, 0));
    for (unsigned Sample = 0; Sample < 128; ++Sample) {
      for (SymRef Variable : {X, Y, Z}) {
        llvm::APInt Value(Width, 0);
        for (unsigned Bit = 0; Bit < Width; Bit += 64)
          Value |= llvm::APInt(Width, Random()).shl(Bit);
        Values[Ctx.varId(Variable)] = Value;
      }
      const llvm::APInt &A = Values[Ctx.varId(X)], &B = Values[Ctx.varId(Y)],
                        &C = Values[Ctx.varId(Z)];
      const llvm::APInt Expected =
          llvm::APInt(Width, 2) * ((A & B) | (A & C) | (B & C)) +
          llvm::APInt(Width, 5) * (A ^ B ^ C);
      EXPECT_EQ(Plan.eval(Values), Expected);
    }
  }
}

TEST(SymMBALinearAdmission, HonorsExactAndShortWorkAndTableAdmission) {
  const MBAOptions Opts = admissionOptions();
  auto run = [&](size_t Work, size_t Bytes, size_t &Used, bool &Exhausted,
                 bool &TooWide) {
    SymContext Ctx;
    const AdmissionFixture F =
        makeAdmission(Ctx, 257, LinearFixture::TwoGroups);
    MBAOptions Local = Opts;
    Local.MaxTableBytes = Bytes;
    detail::WorkBudget Budget(Work);
    detail::SolveReport Report;
    const SymRef Result =
        detail::solveOneRegion(Ctx, F.Input, Local, Budget, Report);
    Used = Budget.used();
    Exhausted = Budget.exhausted() || Report.BudgetExhausted;
    TooWide = Report.TooWide;
    checkAdmissionSemantics(Ctx, F, Result, LinearFixture::TwoGroups);
  };
  size_t FullWork = 0;
  bool Exhausted = false, TooWide = false;
  run(Opts.MaxWork, Opts.MaxTableBytes, FullWork, Exhausted, TooWide);
  ASSERT_FALSE(Exhausted);
  ASSERT_GT(FullWork, 0u);
  size_t Used = 0;
  run(FullWork, Opts.MaxTableBytes, Used, Exhausted, TooWide);
  EXPECT_FALSE(Exhausted);
  EXPECT_EQ(Used, FullWork);
  run(FullWork - 1, Opts.MaxTableBytes, Used, Exhausted, TooWide);
  EXPECT_TRUE(Exhausted);
  EXPECT_EQ(Used, FullWork - 1);
  // This is the public corner-table admission boundary; it is deliberately
  // independent of the private certificate's temporary-allocation estimate.
  const size_t ExactBytes = (size_t(1) << 12) * sizeof(llvm::APInt);
  run(Opts.MaxWork, ExactBytes, Used, Exhausted, TooWide);
  EXPECT_FALSE(TooWide);
  EXPECT_FALSE(Exhausted);
  run(Opts.MaxWork, ExactBytes - 1, Used, Exhausted, TooWide);
  EXPECT_TRUE(TooWide);
  EXPECT_LE(Used, Opts.MaxWork);
}

} // namespace
