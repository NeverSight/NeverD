//===- SymMBAKnownFactTests.cpp - Symbolic regressions ----------------===//

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymMBA.h"

#include <algorithm>
#include <set>

using namespace neverd::symbolic;

static SymRef simplifyWindow(SymContext &C, SymRef Root,
                             MBAOptions Options = {}) {
  return simplifyMBADeep(C, Root, Options).Expr;
}

TEST(SymMBAKnownFacts, KnownMaskBitsReuseExistingFacts) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto Source = C.mkZExt(C.mkUDiv(X, Y), 64);
  auto Flag = C.mkConst(64, 256);
  auto Root = C.mkOr(Source, Flag);
  EXPECT_EQ(simplifyWindow(C, Root), C.mkAdd(Source, Flag));
  auto Signed = C.mkSExt(C.mkUDiv(X, Y), 64);
  auto Other = C.mkOr(Signed, Flag);
  auto Result = simplifyWindow(C, Other);
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Other, Result)),
            neverd::solver::SatResult::Unsat);
  EXPECT_EQ(Proof.check(C.mkNe(Other, C.mkAdd(Signed, Flag))),
            neverd::solver::SatResult::Sat);
}

TEST(SymMBAKnownFacts, KnownPredicatesReuseExistingFacts) {
  SymContext C;
  auto X = C.mkVar("x", 64), Y = C.mkVar("y", 64);
  auto Base =
      C.mkAdd(C.mkAnd(X, C.mkConst(64, 7)), C.mkAnd(Y, C.mkConst(64, 15)));
  auto Count = C.mkConst(64, 4);
  auto Roundtrip = C.mkEq(Base, C.mkAShr(C.mkShl(Base, Count), Count));
  auto Ordering = C.mkUle(Base, C.mkMul(Base, C.mkConst(64, 5)));
  for (auto Root : {Roundtrip, Ordering}) {
    EXPECT_EQ(simplifyWindow(C, Root), C.mkConst(1, 1));
    neverd::solver::BitVectorSolver Proof(C);
    EXPECT_EQ(Proof.check(C.mkNot(Root)), neverd::solver::SatResult::Unsat);
  }
  auto Unbounded = C.mkEq(X, C.mkAShr(C.mkShl(X, Count), Count));
  auto Reduced = simplifyWindow(C, Unbounded);
  EXPECT_NE(Reduced, C.mkConst(1, 1));
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Unbounded, Reduced)),
            neverd::solver::SatResult::Unsat);
}

TEST(SymMBAKnownFacts, PartialBudgetsPreserveIndependentEvaluations) {
  SymContext C;
  auto X = C.mkVar("x", 4), Y = C.mkVar("y", 4);
  auto Source = C.mkZExt(C.mkUDiv(X, Y), 12);
  auto Root = C.mkOr(Source, C.mkConst(12, 64));
  for (unsigned Work : {0u, 1u, 3u, 16u, 64u, 10000u}) {
    MBAOptions Options;
    Options.MaxWork = Work;
    auto Result = simplifyMBADeep(C, Root, Options);
    EXPECT_LE(Result.Work, Work);
    if (!Work)
      EXPECT_EQ(Result.Expr, Root);
    for (unsigned A = 0; A != 16; ++A)
      for (unsigned B = 0; B != 16; ++B) {
        uint64_t Inputs[] = {A, B};
        EXPECT_EQ(C.evalU64(Result.Expr, Inputs), (B ? A / B : 15) | 64u);
      }
  }
}

TEST(SymMBAKnownFacts, MissingFactStorageKeepsTheOriginalSemantics) {
  SymContext C;
  auto X = C.mkVar("x", 64);
  auto Root = C.mkOr(C.mkZExt(C.mkExtract(X, 0, 8), 64), C.mkConst(64, 256));
  MBAOptions Options;
  Options.MaxTableBytes = 0;
  auto Result = simplifyMBADeep(C, Root, Options);
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Root, Result.Expr)),
            neverd::solver::SatResult::Unsat);
}

TEST(SymMBAKnownFacts, ExtractConsumesTheBoundOfATotalUnsignedRemainder) {
  SymContext C;
  const auto Dividend = C.mkFreshVar(64, "dividend_");
  const auto Key = C.mkFreshVar(32, "divisor_input_");
  const auto Divisor = C.mkAdd(C.mkConst(64, 0x1e139d83b), C.mkZExt(Key, 64));
  const auto Flag = C.mkExtract(C.mkURem(Dividend, Divisor), 47, 1);
  const auto Result = simplifyMBADeep(C, Flag);
  EXPECT_EQ(Result.Expr, C.mkZero(1));
  EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
  const auto MayBeZero =
      C.mkExtract(C.mkURem(Dividend, C.mkZExt(Key, 64)), 47, 1);
  EXPECT_NE(simplifyWindow(C, MayBeZero), C.mkZero(1));
}

TEST(SymMBAKnownFacts, ConstantsBesideSeveralTermsStillFold) {
  SymContext C;
  const auto P = C.mkZExt(C.mkVar("p", 1), 8), Q = C.mkZExt(C.mkVar("q", 1), 8);
  const auto X = C.mkVar("x", 8);
  // Bit 0 and bit 5 never meet, whatever else the AND reads.
  const auto Nothing =
      C.mkAnd({C.mkConst(8, 0xAA), X, P, C.mkMul(C.mkConst(8, 32), Q)});
  ASSERT_EQ(C.op(Nothing), SymOp::And);
  EXPECT_EQ(simplifyMBADeep(C, Nothing).Expr, C.mkZero(8));
  // Every bit these terms can set is already set.
  const auto Doubled = C.mkMul(C.mkConst(8, 2), P);
  const auto Scaled = C.mkMul(C.mkConst(8, 4), Q);
  const auto Everything = C.mkOr({C.mkConst(8, 0xFE), Doubled, Scaled});
  ASSERT_EQ(C.op(Everything), SymOp::Or);
  EXPECT_EQ(simplifyMBADeep(C, Everything).Expr, C.mkConst(8, 0xFE));
  // A term that can set another bit stays.
  const auto Kept = C.mkOr({C.mkConst(8, 0xFE), P, Scaled});
  const auto Result = simplifyMBADeep(C, Kept).Expr;
  EXPECT_FALSE(C.isConst(Result));
  for (uint64_t PV : {0u, 1u})
    for (uint64_t QV : {0u, 1u})
      for (uint64_t XV : {0u, 0x5Au}) {
        const uint64_t Values[] = {PV, QV, XV};
        EXPECT_EQ(C.evalU64(Result, Values), C.evalU64(Kept, Values));
      }
}
