//===- SymMBASampleTests.cpp - Deterministic sample checks ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::symbolic;

namespace {

using Assignment = std::vector<llvm::APInt>;

std::vector<Assignment> referenceAssignments(const SymContext &Ctx,
                                             llvm::ArrayRef<uint32_t> Active,
                                             unsigned Samples) {
  llvm::SmallVector<uint32_t, 16> Vars(Active);
  llvm::sort(Vars);
  Vars.erase(std::unique(Vars.begin(), Vars.end()), Vars.end());
  std::mt19937_64 Rng(0x9E3779B97F4A7C15ull);
  std::vector<Assignment> Out;
  for (unsigned S = 0; S < Samples; ++S) {
    Assignment Values;
    for (size_t I = 0; I < Ctx.numVars(); ++I) {
      const unsigned W = Ctx.varInfo(uint32_t(I)).Width;
      Values.emplace_back(W, 0);
    }
    for (uint32_t Id : Vars) {
      const unsigned W = Ctx.varInfo(Id).Width;
      if (S == 0)
        Values[Id] = llvm::APInt(W, 0);
      else if (S == 1)
        Values[Id] = llvm::APInt::getAllOnes(W);
      else if (S == 2)
        Values[Id] = llvm::APInt(W, 1);
      else {
        std::vector<uint64_t> Words((W + 63) / 64);
        for (uint64_t &Word : Words)
          Word = Rng();
        Values[Id] = llvm::APInt(W, Words);
      }
    }
    Out.push_back(std::move(Values));
  }
  return Out;
}

bool referenceAgree(const SymContext &Ctx, SymRef A, SymRef B,
                    unsigned Samples) {
  SymEvalPlan PlanA(Ctx, A), PlanB(Ctx, B);
  llvm::SmallVector<uint32_t, 16> Vars(PlanA.vars());
  Vars.append(PlanB.vars().begin(), PlanB.vars().end());
  // The reference retains APInt throughout. Plan::eval delegates each
  // operation to evalNodeAP, so no operator semantics are duplicated here.
  for (const Assignment &Values : referenceAssignments(Ctx, Vars, Samples))
    if (PlanA.eval(Values) != PlanB.eval(Values))
      return false;
  return true;
}

// Isolate a particular assignment without requiring the tested operation to
// have different values at every corner. The deliberately wrong constant
// ensures that evaluating neither branch cannot accidentally pass the test.
void expectAtSample(SymContext &Ctx, SymRef E, SymRef Selector,
                    unsigned Sample) {
  SymEvalPlan Plan(Ctx, E);
  llvm::SmallVector<uint32_t, 16> Vars(Plan.vars());
  Vars.push_back(Ctx.varId(Selector));
  const auto Values = referenceAssignments(Ctx, Vars, Sample + 1);
  llvm::APInt Expected = Plan.eval(Values.back());
  SymRef Gate =
      Ctx.mkEq(Selector, Ctx.mkConst(Values.back()[Ctx.varId(Selector)]));
  SymRef Zero = Ctx.mkZero(Ctx.width(E));
  SymRef Actual = Ctx.mkIte(Gate, E, Zero);
  SymRef Match = Ctx.mkIte(Gate, Ctx.mkConst(Expected), Zero);
  SymRef Miss = Ctx.mkIte(
      Gate, Ctx.mkConst(Expected ^ llvm::APInt(Expected.getBitWidth(), 1)),
      Zero);
  ASSERT_TRUE(referenceAgree(Ctx, Actual, Match, Sample + 1));
  ASSERT_FALSE(referenceAgree(Ctx, Actual, Miss, Sample + 1));
  EXPECT_TRUE(detail::agreeOnSamples(Ctx, Actual, Match, Sample + 1));
  EXPECT_TRUE(detail::agreeOnSamples(Ctx, Match, Actual, Sample + 1));
  EXPECT_FALSE(detail::agreeOnSamples(Ctx, Actual, Miss, Sample + 1));
  EXPECT_FALSE(detail::agreeOnSamples(Ctx, Miss, Actual, Sample + 1));
}

TEST(SymMBASample, ZeroSamplesDoNotRejectDifferentValues) {
  for (unsigned W : {1u, 8u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", W);
    EXPECT_TRUE(detail::agreeOnSamples(Ctx, X, Ctx.mkNot(X), 0));
    EXPECT_TRUE(detail::agreeOnSamples(Ctx, Ctx.mkZero(W), Ctx.mkOnes(W), 0));
    EXPECT_FALSE(detail::agreeOnSamples(Ctx, Ctx.mkZero(W), Ctx.mkOnes(W), 1));
  }
}

TEST(SymMBASample, VisitsZeroOnesOneThenTheFirstRandomPoint) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 64);
  const uint32_t Active[] = {Ctx.varId(X)};
  const auto Values = referenceAssignments(Ctx, Active, 4);
  for (unsigned Sample = 0; Sample < Values.size(); ++Sample) {
    SCOPED_TRACE(Sample);
    for (unsigned Earlier = 0; Earlier < Sample; ++Earlier)
      ASSERT_NE(Values[Earlier][0], Values[Sample][0]);
    SymRef Hit = Ctx.mkEq(X, Ctx.mkConst(Values[Sample][0]));
    EXPECT_TRUE(detail::agreeOnSamples(Ctx, Hit, Ctx.mkFalse(), Sample));
    EXPECT_FALSE(detail::agreeOnSamples(Ctx, Hit, Ctx.mkFalse(), Sample + 1));
  }
}

TEST(SymMBASample, MatchesAPAcrossNarrowAndWideWidths) {
  for (unsigned Unused : {0u, 17u})
    for (unsigned W : {1u, 8u, 32u, 64u, 65u, 128u, 257u}) {
      SCOPED_TRACE(::testing::Message() << W << ":" << Unused);
      SymContext Ctx;
      for (unsigned I = 0; I < Unused; ++I)
        Ctx.mkVar("unused_" + std::to_string(I), 257);
      SymRef Selector = Ctx.mkVar("selector", 64);
      SymRef X = Ctx.mkVar("x", W), Y = Ctx.mkVar("y", W);
      SymRef E = Ctx.mkXor(Ctx.mkMul(X, Y), Ctx.mkAdd(X, Y));
      for (unsigned Sample : {0u, 1u, 2u, 3u, 6u})
        expectAtSample(Ctx, E, Selector, Sample);
      SymRef Other = Ctx.mkOr(X, Y);
      for (unsigned Samples : {0u, 1u, 2u, 3u, 4u, 7u})
        EXPECT_EQ(detail::agreeOnSamples(Ctx, E, Other, Samples),
                  referenceAgree(Ctx, E, Other, Samples));
    }
}

TEST(SymMBASample, UnusedWideVariablesDoNotConsumeRandomWords) {
  SymContext Ctx;
  SymRef UnusedBefore = Ctx.mkVar("unused_before", 257);
  SymRef X = Ctx.mkVar("x", 64);
  SymRef UnusedBetween = Ctx.mkVar("unused_between", 65);
  SymRef Y = Ctx.mkVar("y", 32);
  SymRef UnusedAfter = Ctx.mkVar("unused_after", 128);
  const uint32_t Active[] = {Ctx.varId(X), Ctx.varId(Y)};
  const auto Values = referenceAssignments(Ctx, Active, 4);
  for (SymRef Unused : {UnusedBefore, UnusedAfter})
    ASSERT_TRUE(Values.back()[Ctx.varId(Unused)].isZero());
  ASSERT_EQ(Ctx.width(UnusedBetween), 65u);
  SymRef Hit = Ctx.mkAnd(Ctx.mkEq(X, Ctx.mkConst(Values.back()[Ctx.varId(X)])),
                         Ctx.mkEq(Y, Ctx.mkConst(Values.back()[Ctx.varId(Y)])));
  ASSERT_TRUE(SymEvalPlan(Ctx, Hit).fitsU64());
  EXPECT_TRUE(detail::agreeOnSamples(Ctx, Hit, Ctx.mkFalse(), 3));
  EXPECT_FALSE(detail::agreeOnSamples(Ctx, Hit, Ctx.mkFalse(), 4));
  expectAtSample(Ctx, Ctx.mkXor(X, Ctx.mkZExt(Y, 64)), X, 6);
}

TEST(SymMBASample, CancelsOnlyIdenticalCompleteAddends) {
  for (unsigned W : {8u, 16u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", W), Y = Ctx.mkVar("y", W);
    SymRef Z = Ctx.mkVar("z", W);
    llvm::SmallVector<SymRef, 256> Factors;
    for (unsigned I = 0; I < 256; ++I)
      Factors.push_back(Ctx.mkXor(Ctx.mkAdd(X, Ctx.mkConst(W, 2 * I)),
                                  Ctx.mkAnd(Z, Ctx.mkConst(W, 252))));
    SymRef Product = Ctx.mkMul(Factors);
    SymRef Tail = Ctx.mkAdd(Ctx.mkXor(X, Y),
                            Ctx.mkMul(Ctx.mkConst(W, 2), Ctx.mkAnd(X, Y)));
    SymRef A = Ctx.mkAdd(Product, Tail);
    SymRef B = Ctx.mkAdd({Product, X, Y});
    SymRef Scaled = Ctx.mkMul(Ctx.mkConst(W, 3), Product);
    for (unsigned Samples : {0u, 1u, 3u, 4u, 17u}) {
      EXPECT_TRUE(referenceAgree(Ctx, A, B, Samples));
      EXPECT_TRUE(detail::agreeOnSamples(Ctx, A, B, Samples));
      EXPECT_TRUE(detail::agreeOnSamples(Ctx, B, A, Samples));
      for (SymRef Other :
           {Ctx.mkAdd({Product, X, Y, Z}), Ctx.mkAdd({Scaled, X, Y}),
            Ctx.mkAdd({Product, X, Ctx.mkNot(Y)})}) {
        EXPECT_EQ(detail::agreeOnSamples(Ctx, A, Other, Samples),
                  referenceAgree(Ctx, A, Other, Samples));
        EXPECT_EQ(detail::agreeOnSamples(Ctx, Other, A, Samples),
                  referenceAgree(Ctx, Other, A, Samples));
      }
    }
  }
}

TEST(SymMBASample, CanceledInputsKeepTheirOriginalRandomWords) {
  for (unsigned CanceledWidth : {8u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(CanceledWidth);
    SymContext Ctx;
    Ctx.mkVar("unused_before", 257);
    SymRef Canceled = Ctx.mkVar("canceled", CanceledWidth);
    Ctx.mkVar("unused_between", 128);
    SymRef X = Ctx.mkVar("x", 64), Y = Ctx.mkVar("y", 32);
    SymRef Common = CanceledWidth < 64 ? Ctx.mkZExt(Canceled, 64)
                                       : Ctx.mkExtract(Canceled, 0, 64);
    const uint32_t Active[] = {Ctx.varId(Canceled), Ctx.varId(X), Ctx.varId(Y)};
    const auto Values = referenceAssignments(Ctx, Active, 7);
    SymRef Base = Ctx.mkAdd(Common, Ctx.mkConst(64, 5));
    for (unsigned Sample : {0u, 1u, 2u, 3u, 6u}) {
      SCOPED_TRACE(Sample);
      SymRef Hit =
          Ctx.mkAnd(Ctx.mkEq(X, Ctx.mkConst(Values[Sample][Ctx.varId(X)])),
                    Ctx.mkEq(Y, Ctx.mkConst(Values[Sample][Ctx.varId(Y)])));
      SymRef Other = Ctx.mkAdd(Base, Ctx.mkZExt(Hit, 64));
      ASSERT_TRUE(referenceAgree(Ctx, Base, Other, Sample));
      ASSERT_FALSE(referenceAgree(Ctx, Base, Other, Sample + 1));
      EXPECT_TRUE(detail::agreeOnSamples(Ctx, Base, Other, Sample));
      EXPECT_TRUE(detail::agreeOnSamples(Ctx, Other, Base, Sample));
      EXPECT_FALSE(detail::agreeOnSamples(Ctx, Base, Other, Sample + 1));
      EXPECT_FALSE(detail::agreeOnSamples(Ctx, Other, Base, Sample + 1));
    }
  }
}

TEST(SymMBASample, ChecksChangedFactorsBeforeLongCommonProducts) {
  for (unsigned W : {8u, 16u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", W), Y = Ctx.mkVar("y", W);
    SymRef Z = Ctx.mkVar("z", W);
    llvm::SmallVector<SymRef, 128> Factors;
    for (unsigned I = 0; I < 128; ++I)
      Factors.push_back(
          Ctx.mkOr(Ctx.mkXor(Ctx.mkAdd(X, Ctx.mkConst(W, 2 * I)), Z),
                   Ctx.mkConst(W, 1)));
    SymRef Common = Ctx.mkMul(Factors);
    SymRef Tail = Ctx.mkAdd(Ctx.mkXor(X, Y),
                            Ctx.mkMul(Ctx.mkConst(W, 2), Ctx.mkAnd(X, Y)));
    SymRef A = Ctx.mkMul(Common, Tail);
    SymRef B = Ctx.mkMul(Common, Ctx.mkAdd(X, Y));
    SymRef Miss = Ctx.mkMul(Common, Ctx.mkAdd({X, Y, Ctx.mkConst(W, 1)}));
    for (unsigned AddMode : {0u, 1u, 2u}) {
      SymRef L = A, R = B, Wrong = Miss;
      if (AddMode == 1) {
        L = Ctx.mkAdd(A, Z);
        R = Ctx.mkAdd(B, Z);
        Wrong = Ctx.mkAdd(Miss, Z);
      } else if (AddMode == 2) {
        L = Ctx.mkAdd({A, Ctx.mkXor(Y, Z),
                       Ctx.mkMul(Ctx.mkConst(W, 2), Ctx.mkAnd(Y, Z))});
        R = Ctx.mkAdd({B, Y, Z});
        Wrong = Ctx.mkAdd({Miss, Y, Z});
      }
      for (unsigned Samples : {0u, 1u, 3u, 4u, 17u}) {
        EXPECT_TRUE(referenceAgree(Ctx, L, R, Samples));
        EXPECT_TRUE(detail::agreeOnSamples(Ctx, L, R, Samples));
        EXPECT_TRUE(detail::agreeOnSamples(Ctx, R, L, Samples));
        EXPECT_EQ(detail::agreeOnSamples(Ctx, L, Wrong, Samples),
                  referenceAgree(Ctx, L, Wrong, Samples));
        EXPECT_EQ(detail::agreeOnSamples(Ctx, Wrong, L, Samples),
                  referenceAgree(Ctx, Wrong, L, Samples));
      }
    }
    SymRef Expanded = Ctx.mkAdd({Ctx.mkMul(Common, X), Common, Z});
    SymRef Factored =
        Ctx.mkAdd(Ctx.mkMul(Common, Ctx.mkAdd(X, Ctx.mkConst(W, 1))), Z);
    EXPECT_TRUE(referenceAgree(Ctx, Expanded, Factored, 17));
    EXPECT_TRUE(detail::agreeOnSamples(Ctx, Expanded, Factored, 17));
    EXPECT_TRUE(detail::agreeOnSamples(Ctx, Factored, Expanded, 17));
  }
}

TEST(SymMBASample, CommonFactorsRetainModularZerosAndMultiplicities) {
  for (unsigned W : {8u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    SymRef X = Ctx.mkVar("x", W), Y = Ctx.mkVar("y", W);
    SymRef Common =
        Ctx.mkMul(Ctx.mkConst(llvm::APInt::getOneBitSet(W, W - 1)), Y);
    SymRef A = Ctx.mkMul(Common, Ctx.mkAdd(X, Ctx.mkConst(W, 2)));
    SymRef B = Ctx.mkMul(Common, X);
    SymRef Miss = Ctx.mkMul(Common, Ctx.mkAdd(X, Ctx.mkConst(W, 1)));
    for (unsigned Samples : {0u, 1u, 2u, 3u, 4u, 17u}) {
      EXPECT_TRUE(referenceAgree(Ctx, A, B, Samples));
      EXPECT_TRUE(detail::agreeOnSamples(Ctx, A, B, Samples));
      EXPECT_TRUE(detail::agreeOnSamples(Ctx, B, A, Samples));
      EXPECT_EQ(detail::agreeOnSamples(Ctx, A, Miss, Samples),
                referenceAgree(Ctx, A, Miss, Samples));
      SymRef WrongSum = Ctx.mkAdd({B, Y, Ctx.mkConst(W, 1)});
      EXPECT_EQ(detail::agreeOnSamples(Ctx, Ctx.mkAdd(A, Y), WrongSum, Samples),
                referenceAgree(Ctx, Ctx.mkAdd(A, Y), WrongSum, Samples));
      SymRef Repeated = Ctx.mkMul({X, X, Y});
      SymRef Other = Ctx.mkMul({X, Y, Y});
      EXPECT_EQ(detail::agreeOnSamples(Ctx, Repeated, Other, Samples),
                referenceAgree(Ctx, Repeated, Other, Samples));
      EXPECT_EQ(detail::agreeOnSamples(Ctx, Other, Repeated, Samples),
                referenceAgree(Ctx, Other, Repeated, Samples));
    }
  }
}

TEST(SymMBASample, FactoredInputsKeepRandomWordsAndWideIntermediates) {
  for (unsigned W : {8u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    Ctx.mkVar("unused_before", 257);
    SymRef Canceled = Ctx.mkVar("canceled", W);
    Ctx.mkVar("unused_between", 128);
    SymRef CommonInput = Ctx.mkVar("common", 64);
    SymRef X = Ctx.mkVar("x", 64), Y = Ctx.mkVar("y", 32);
    SymRef Common =
        Ctx.mkOr(Ctx.mkExtract(Ctx.mkUDiv(Ctx.mkZExt(CommonInput, 257),
                                          Ctx.mkConst(257, 3)),
                               0, 64),
                 Ctx.mkConst(64, 1));
    Common = Ctx.mkMul(Common, Ctx.mkOr(Ctx.mkZExt(Y, 64), Ctx.mkConst(64, 1)));
    ASSERT_FALSE(SymEvalPlan(Ctx, Common).fitsU64());
    SymRef CanceledTerm = Ctx.mkZExtOrTrunc(Canceled, 64);
    const uint32_t Active[] = {Ctx.varId(Canceled), Ctx.varId(CommonInput),
                               Ctx.varId(X), Ctx.varId(Y)};
    const auto Values = referenceAssignments(Ctx, Active, 7);
    SymRef Base = Ctx.mkAdd(Ctx.mkMul(Common, Ctx.mkAdd(X, Ctx.mkConst(64, 7))),
                            CanceledTerm);
    for (unsigned Sample : {0u, 1u, 2u, 3u, 6u}) {
      SCOPED_TRACE(Sample);
      SymRef Hit =
          Ctx.mkAnd(Ctx.mkEq(X, Ctx.mkConst(Values[Sample][Ctx.varId(X)])),
                    Ctx.mkEq(Y, Ctx.mkConst(Values[Sample][Ctx.varId(Y)])));
      SymRef Other = Ctx.mkAdd(
          Ctx.mkMul(Common,
                    Ctx.mkAdd({X, Ctx.mkConst(64, 7), Ctx.mkZExt(Hit, 64)})),
          CanceledTerm);
      ASSERT_TRUE(referenceAgree(Ctx, Base, Other, Sample));
      ASSERT_FALSE(referenceAgree(Ctx, Base, Other, Sample + 1));
      EXPECT_TRUE(detail::agreeOnSamples(Ctx, Base, Other, Sample));
      EXPECT_TRUE(detail::agreeOnSamples(Ctx, Other, Base, Sample));
      EXPECT_FALSE(detail::agreeOnSamples(Ctx, Base, Other, Sample + 1));
      EXPECT_FALSE(detail::agreeOnSamples(Ctx, Other, Base, Sample + 1));
    }
  }
}

TEST(SymMBASample, ResidualPlansKeepInternalWideArithmetic) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 8), Y = Ctx.mkVar("y", 8);
  SymRef Product = Ctx.mkMul(X, Y);
  SymRef Wide = Ctx.mkUDiv(Ctx.mkZExt(X, 257), Ctx.mkConst(257, 3));
  SymRef Low = Ctx.mkExtract(Wide, 0, 8);
  SymRef A = Ctx.mkAdd(Product, Low);
  SymRef B = Ctx.mkAdd(Product, Ctx.mkUDiv(X, Ctx.mkConst(8, 3)));
  SymRef Miss = Ctx.mkAdd(Product, X);
  ASSERT_FALSE(SymEvalPlan(Ctx, Low).fitsU64());
  for (unsigned Samples : {0u, 1u, 2u, 3u, 4u, 17u}) {
    EXPECT_TRUE(referenceAgree(Ctx, A, B, Samples));
    EXPECT_TRUE(detail::agreeOnSamples(Ctx, A, B, Samples));
    EXPECT_TRUE(detail::agreeOnSamples(Ctx, B, A, Samples));
    EXPECT_EQ(detail::agreeOnSamples(Ctx, A, Miss, Samples),
              referenceAgree(Ctx, A, Miss, Samples));
    EXPECT_EQ(detail::agreeOnSamples(Ctx, Miss, A, Samples),
              referenceAgree(Ctx, Miss, A, Samples));
  }
}

TEST(SymMBASample, ChecksInternalWidthsAndBothPlanOrientations) {
  for (unsigned W : {65u, 128u, 257u}) {
    SCOPED_TRACE(W);
    SymContext NarrowCtx;
    SymRef Selector = NarrowCtx.mkVar("selector", 64);
    SymRef X = NarrowCtx.mkVar("x", 8);
    SymRef Wide =
        NarrowCtx.mkUDiv(NarrowCtx.mkZExt(X, W), NarrowCtx.mkConst(W, 3));
    SymRef Low = NarrowCtx.mkExtract(Wide, 0, 8);
    ASSERT_FALSE(SymEvalPlan(NarrowCtx, Low).fitsU64());
    for (unsigned Sample : {0u, 1u, 2u, 3u})
      expectAtSample(NarrowCtx, Low, Selector, Sample);
  }
  SymContext Ctx;
  SymRef Selector = Ctx.mkVar("selector", 64);
  SymRef X = Ctx.mkVar("x", 8), Count = Ctx.mkVar("count", 32);
  SymRef Wide = Ctx.mkVar("wide", 257), WideY = Ctx.mkVar("wide_y", 257);
  SymRef Narrow = Ctx.mkVar("narrow", 1);
  const std::vector<SymRef> Exprs = {
      Ctx.mkExtract(Ctx.mkUDiv(Wide, WideY), 129, 8),
      Ctx.mkUlt(Wide, WideY),
      Ctx.mkShl(X, Wide),
      Ctx.mkZExt(X, 65),
      Ctx.mkSExt(X, 128),
      Ctx.mkConcat(Wide, Narrow),
      Ctx.mkLShr(X, Count)};
  for (size_t I = 0; I < Exprs.size(); ++I) {
    SCOPED_TRACE(I);
    EXPECT_EQ(SymEvalPlan(Ctx, Exprs[I]).fitsU64(), I + 1 == Exprs.size());
    for (unsigned Sample : {0u, 1u, 2u, 3u})
      expectAtSample(Ctx, Exprs[I], Selector, Sample);
  }
}

TEST(SymMBASample, MatchesAPOperatorSemanticsAtCornersAndRandomValues) {
  for (unsigned W : {8u, 64u, 128u}) {
    SCOPED_TRACE(W);
    SymContext Ctx;
    SymRef Selector = Ctx.mkVar("selector", 64);
    SymRef X = Ctx.mkVar("x", W), Y = Ctx.mkVar("y", W);
    SymRef Small = Ctx.mkVar("small", W / 2);
    SymRef SmallY = Ctx.mkVar("small_y", W / 2);
    SymRef Cond = Ctx.mkVar("cond", 1);
    const std::vector<std::pair<SymOp, SymRef>> Exprs = {
        {SymOp::Const, Ctx.mkConst(llvm::APInt::getSignedMinValue(W))},
        {SymOp::Var, X},
        {SymOp::Add, Ctx.mkAdd(X, Y)},
        {SymOp::Mul, Ctx.mkMul(X, Y)},
        {SymOp::And, Ctx.mkAnd(X, Y)},
        {SymOp::Or, Ctx.mkOr(X, Y)},
        {SymOp::Xor, Ctx.mkXor(X, Y)},
        {SymOp::Not, Ctx.mkNot(X)},
        {SymOp::Shl, Ctx.mkShl(X, Y)},
        {SymOp::LShr, Ctx.mkLShr(X, Y)},
        {SymOp::AShr, Ctx.mkAShr(X, Y)},
        {SymOp::UDiv, Ctx.mkUDiv(X, Y)},
        {SymOp::SDiv, Ctx.mkSDiv(X, Y)},
        {SymOp::URem, Ctx.mkURem(X, Y)},
        {SymOp::SRem, Ctx.mkSRem(X, Y)},
        {SymOp::Rol, Ctx.mkRol(X, Y)},
        {SymOp::Ror, Ctx.mkRor(X, Y)},
        {SymOp::Extract, Ctx.mkExtract(X, 1, W / 2)},
        {SymOp::Concat, Ctx.mkConcat(Small, SmallY)},
        {SymOp::ZExt, Ctx.mkZExt(Small, W)},
        {SymOp::SExt, Ctx.mkSExt(Small, W)},
        {SymOp::Ite, Ctx.mkIte(Cond, X, Y)},
        {SymOp::Eq, Ctx.mkEq(X, Y)},
        {SymOp::Ult, Ctx.mkUlt(X, Y)},
        {SymOp::Ule, Ctx.mkUle(X, Y)},
        {SymOp::Slt, Ctx.mkSlt(X, Y)},
        {SymOp::Sle, Ctx.mkSle(X, Y)}};
    for (const auto &[Op, E] : Exprs) {
      SCOPED_TRACE(unsigned(Op));
      ASSERT_EQ(Ctx.op(E), Op);
      for (unsigned Sample : {0u, 1u, 2u, 3u})
        expectAtSample(Ctx, E, Selector, Sample);
    }
    // The shared evaluator also defines division by zero and signed overflow.
    SymRef Min = Ctx.mkOr(X, Ctx.mkConst(llvm::APInt::getSignedMinValue(W)));
    SymRef NegativeOne = Ctx.mkNot(Y);
    expectAtSample(Ctx, Ctx.mkSDiv(Min, NegativeOne), Selector, 0);
    expectAtSample(Ctx, Ctx.mkSRem(Min, NegativeOne), Selector, 0);
    expectAtSample(Ctx, Ctx.mkUDiv(X, Y), Selector, 0);
    expectAtSample(Ctx, Ctx.mkURem(X, Y), Selector, 0);
  }
}

} // namespace
