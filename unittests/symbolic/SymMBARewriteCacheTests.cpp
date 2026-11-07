//===- SymMBARewriteCacheTests.cpp - Exact cross-query reuse --------------===//

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymMBA.h"

#include <stdexcept>

using namespace neverd::symbolic;

namespace {

SymRef obfuscateSum(SymContext &C, SymRef X, SymRef Y) {
  return C.mkAdd(C.mkXor(X, Y),
                 C.mkMul(C.mkConst(C.width(X), 2), C.mkAnd(X, Y)));
}

TEST(SymMBARewriteCache, WarmChildReducesWorkUnderAnOpaqueParent) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8), Z = C.mkVar("z", 8);
  auto Sum = obfuscateSum(C, X, Y);
  MBARewriteCache Cache(C);
  MBAOptions Options;
  Options.VerifySamples = 0;
  auto Warm = simplifyMBADeep(C, Sum, Options, Cache);
  ASSERT_EQ(Warm.Expr, C.mkAdd(X, Y));
  ASSERT_GT(Cache.size(), 0u);
  auto Root = C.mkUDiv(Sum, Z);
  auto Cold = simplifyMBADeep(C, Root, Options);
  auto Hot = simplifyMBADeep(C, Root, Options, Cache);
  EXPECT_EQ(Cold.Expr, Hot.Expr);
  EXPECT_LT(Hot.Work, Cold.Work);
  EXPECT_GT(Cache.reusedRoots(), 0u);
  RecordProperty("uncached_work", Cold.Work);
  RecordProperty("cached_work", Hot.Work);
  for (unsigned A = 0; A != 256; ++A)
    for (unsigned B = 0; B != 256; ++B) {
      uint64_t Inputs[] = {A, B, 3};
      EXPECT_EQ(C.evalU64(Hot.Expr, Inputs), ((A + B) & 255) / 3);
    }
}

TEST(SymMBARewriteCache, ReusingAnExactRootHasOneWorkUnit) {
  SymContext C;
  auto X = C.mkVar("x", 64), Y = C.mkVar("y", 64);
  auto Root = obfuscateSum(C, X, Y);
  MBARewriteCache Cache(C);
  auto Warm = simplifyMBADeep(C, Root, {}, Cache);
  ASSERT_TRUE(Warm.Changed);
  MBAOptions Tight;
  Tight.MaxWork = 1;
  auto Hot = simplifyMBADeep(C, Root, Tight, Cache);
  EXPECT_EQ(Hot.Expr, Warm.Expr);
  EXPECT_EQ(Hot.Work, 1u);
  EXPECT_EQ(Hot.Evidence, MBAEvidence::Derivation);
}

TEST(SymMBARewriteCache, ZeroWorkNeverConsumesAnExistingRewrite) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto Root = obfuscateSum(C, X, Y);
  MBARewriteCache Cache(C);
  ASSERT_TRUE(simplifyMBADeep(C, Root, {}, Cache).Changed);
  MBAOptions Zero;
  Zero.MaxWork = 0;
  auto Result = simplifyMBADeep(C, Root, Zero, Cache);
  EXPECT_EQ(Result.Expr, Root);
  EXPECT_EQ(Result.Work, 0u);
  EXPECT_EQ(Cache.reusedRoots(), 0u);
}

TEST(SymMBARewriteCache, CapacityBoundsEntriesAndDisabledCacheRetainsNothing) {
  SymContext C;
  MBARewriteCache Cache(C, 1), Disabled(C, 0);
  auto X = C.mkVar("x", 32);
  for (unsigned I = 0; I != 5; ++I) {
    auto Y = C.mkVar("y" + std::to_string(I), 32);
    auto Root = obfuscateSum(C, X, Y);
    EXPECT_EQ(simplifyMBADeep(C, Root, {}, Cache).Expr, C.mkAdd(X, Y));
    EXPECT_EQ(simplifyMBADeep(C, Root, {}, Disabled).Expr, C.mkAdd(X, Y));
    EXPECT_LE(Cache.size(), 1u);
    EXPECT_EQ(Disabled.size(), 0u);
  }
}

TEST(SymMBARewriteCache, ForeignContextsFailBeforePublishingAnyFact) {
  SymContext A, B;
  MBARewriteCache Cache(A);
  auto X = B.mkVar("x", 8), Y = B.mkVar("y", 8);
  EXPECT_THROW(simplifyMBADeep(B, obfuscateSum(B, X, Y), {}, Cache),
               std::invalid_argument);
  EXPECT_EQ(Cache.size(), 0u);
}

TEST(SymMBARewriteCache, IdentitiesAndWideCarriesRemainContextLocal) {
  SymContext C;
  auto X = C.mkVar("x", 128), Y = C.mkVar("y", 128);
  auto A = C.mkVar("a", 128), B = C.mkVar("b", 128);
  MBARewriteCache Cache(C);
  ASSERT_TRUE(simplifyMBADeep(C, obfuscateSum(C, X, Y), {}, Cache).Changed);
  auto Root = C.mkLShr(obfuscateSum(C, A, B), C.mkConst(128, 64));
  auto Result = simplifyMBADeep(C, Root, {}, Cache);
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Root, Result.Expr)),
            neverd::solver::SatResult::Unsat);
  auto Other = C.mkLShr(C.mkAdd(X, Y), C.mkConst(128, 64));
  EXPECT_EQ(Proof.check(C.mkNe(Result.Expr, Other)),
            neverd::solver::SatResult::Sat);
}

} // namespace

TEST(SymMBARewriteCache, CompletedOpaqueParentsRetainTheirFinalRewrite) {
  SymContext C;
  const auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  const auto Z = C.mkVar("z", 8);
  const auto Sum = obfuscateSum(C, X, Y);
  for (const auto Root : {C.mkZExt(Sum, 16), C.mkUDiv(Sum, Z),
                          C.mkExtract(Sum, 2, 4), C.mkConcat({Sum, Z})}) {
    MBARewriteCache Cache(C);
    const auto Warm = simplifyMBADeep(C, Root, {}, Cache);
    ASSERT_TRUE(Warm.Changed);
    ASSERT_EQ(Warm.Evidence, MBAEvidence::Derivation);
    const auto Before = Cache.reusedRoots();
    const auto Hot = simplifyMBADeep(C, Root, {}, Cache);
    EXPECT_EQ(Hot.Expr, Warm.Expr);
    EXPECT_EQ(Hot.Work, 1u);
    EXPECT_EQ(Cache.reusedRoots(), Before + 1);
    neverd::solver::BitVectorSolver Proof(C);
    EXPECT_EQ(Proof.check(C.mkNe(Root, Hot.Expr)),
              neverd::solver::SatResult::Unsat);
  }
}

TEST(SymMBARewriteCache, ACompletedChildCutsOnlyItsOwnTraversal) {
  SymContext C;
  const auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  const auto Sum = obfuscateSum(C, X, Y);
  MBARewriteCache Cache(C);
  ASSERT_EQ(simplifyMBADeep(C, Sum, {}, Cache).Expr, C.mkAdd(X, Y));
  // The XOR is both inside the completed child and independently visible
  // through the denominator. Cutting Sum must retain that second use.
  const auto Root = C.mkUDiv(Sum, C.mkXor(X, Y));
  MBAOptions Tight;
  Tight.MaxWork = 1;
  const auto Hot = simplifyMBADeep(C, Root, Tight, Cache);
  EXPECT_EQ(Hot.Expr, C.mkUDiv(C.mkAdd(X, Y), C.mkXor(X, Y)));
  EXPECT_EQ(Hot.Evidence, MBAEvidence::Derivation);
  EXPECT_EQ(Hot.Work, 1u);
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Root, Hot.Expr)),
            neverd::solver::SatResult::Unsat);
}

TEST(SymMBARewriteCache, FinishedUnchangedChildrenDoNotRepeatTheirWalk) {
  SymContext C;
  const auto X = C.mkVar("x", 32), Y = C.mkVar("y", 32);
  const auto Z = C.mkVar("z", 32);
  auto Value = X;
  for (unsigned I = 0; I < 16; ++I)
    Value = C.mkXor(C.mkAdd(Value, Y), Z);
  MBAOptions Options;
  Options.MaxWork = 65536;
  MBARewriteCache Cache(C);
  const auto Warm = simplifyMBADeep(C, Value, Options, Cache);
  ASSERT_EQ(Warm.Expr, Value);
  ASSERT_NE(Warm.Outcome, MBAOutcome::BudgetExhausted);
  const auto Root = C.mkUDiv(Value, C.mkXor(X, Y));
  const auto Cold = simplifyMBADeep(C, Root, Options);
  const auto Hot = simplifyMBADeep(C, Root, Options, Cache);
  EXPECT_EQ(Hot.Expr, Cold.Expr);
  EXPECT_LT(Hot.Work * 4, Cold.Work);
  RecordProperty("cold_work", Cold.Work);
  RecordProperty("completed_child_work", Hot.Work);
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Root, Hot.Expr)),
            neverd::solver::SatResult::Unsat);
}

TEST(SymMBARewriteCache, CompletedChildrenOfARewrittenRootRetainTheirWork) {
  SymContext C;
  const auto X = C.mkVar("x", 32), Y = C.mkVar("y", 32);
  const auto Z = C.mkVar("z", 32);
  const auto A = C.mkVar("a", 32), B = C.mkVar("b", 32);
  auto Value = X;
  for (unsigned I = 0; I < 12; ++I)
    Value = C.mkXor(C.mkAdd(Value, Y), Z);
  MBAOptions Options;
  Options.MaxWork = 1 << 20;
  MBARewriteCache Cache(C);
  const auto Warm =
      simplifyMBADeep(C, C.mkAdd(Value, obfuscateSum(C, A, B)), Options, Cache);
  ASSERT_TRUE(Warm.Changed);
  const auto Root = C.mkUDiv(Value, C.mkXor(A, B));
  const auto Cold = simplifyMBADeep(C, Root, Options);
  const auto Hot = simplifyMBADeep(C, Root, Options, Cache);
  EXPECT_EQ(Hot.Expr, Cold.Expr);
  EXPECT_EQ(Hot.Outcome, Cold.Outcome);
  EXPECT_EQ(Hot.NumAtoms, Cold.NumAtoms);
  EXPECT_LT(Hot.Work * 4, Cold.Work);
}

TEST(SymMBARewriteCache, ARefusedSiblingRetainsOnlyTheCompletedChildWork) {
  SymContext C;
  const auto X = C.mkVar("x", 32), Y = C.mkVar("y", 32);
  const auto Z = C.mkVar("z", 32);
  const auto A = C.mkVar("a", 32), B = C.mkVar("b", 32);
  const auto D = C.mkVar("d", 32);
  auto Value = X;
  for (unsigned I = 0; I < 12; ++I)
    Value = C.mkXor(C.mkAdd(Value, Y), Z);
  auto Sibling = A;
  for (unsigned I = 0; I < 12; ++I)
    Sibling = C.mkXor(C.mkAdd(Sibling, B), D);
  MBAOptions Options;
  Options.MaxWork = 30000;
  MBARewriteCache Cache(C);
  const auto Warm =
      simplifyMBADeep(C, C.mkConcat({Value, Sibling}), Options, Cache);
  ASSERT_EQ(Warm.Outcome, MBAOutcome::BudgetExhausted);
  const auto Root = C.mkUDiv(Value, C.mkXor(A, B));
  const auto Cold = simplifyMBADeep(C, Root, Options);
  const auto Hot = simplifyMBADeep(C, Root, Options, Cache);
  EXPECT_EQ(Hot.Expr, Cold.Expr);
  EXPECT_EQ(Hot.Outcome, Cold.Outcome);
  EXPECT_LT(Hot.Work * 4, Cold.Work);
}

TEST(SymMBARewriteCache, AFullCacheRetainsNewExactDerivations) {
  SymContext C;
  const auto X = C.mkVar("x", 32);
  MBARewriteCache Cache(C, 2);
  for (unsigned I = 0; I < 8; ++I) {
    const auto Y = C.mkVar("y" + std::to_string(I), 32);
    const auto Root = obfuscateSum(C, X, Y);
    const auto Warm = simplifyMBADeep(C, Root, {}, Cache);
    ASSERT_EQ(Warm.Expr, C.mkAdd(X, Y));
    MBAOptions Tight;
    Tight.MaxWork = 1;
    const auto Hot = simplifyMBADeep(C, Root, Tight, Cache);
    EXPECT_EQ(Hot.Expr, Warm.Expr);
    EXPECT_EQ(Hot.Work, 1u);
    EXPECT_LE(Cache.size(), 2u);
  }
}

TEST(SymMBARewriteCache, RecoveredWordChoicesStayInTheEvictionOrder) {
  SymContext C;
  const auto P = C.mkVar("p", 1), X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  const auto Choice =
      C.mkXor(X, C.mkAnd(C.mkNeg(C.mkZExt(P, 8)), C.mkXor(X, Y)));
  MBARewriteCache Cache(C, 1);
  MBAOptions Options;
  Options.VerifySamples = 0;
  const auto First = simplifyMBADeep(C, Choice, Options, Cache);
  ASSERT_NE(First.Expr, Choice);
  ASSERT_EQ(Cache.size(), 1u);
  // The next entry must find the first in the order that evicts it.
  const auto Second = simplifyMBADeep(C, obfuscateSum(C, X, Y), Options, Cache);
  EXPECT_EQ(Second.Expr, C.mkAdd(X, Y));
  EXPECT_EQ(Cache.size(), 1u);
  EXPECT_EQ(Cache.evictedEntries(), 1u);
}

TEST(SymMBARewriteCache, AFullCacheRetainsNewCompletedSubtrees) {
  SymContext C;
  const auto X = C.mkVar("x", 32), Y = C.mkVar("y", 32);
  const auto Z = C.mkVar("z", 32);
  MBARewriteCache Cache(C, 4);
  for (unsigned I = 0; I < 8; ++I)
    simplifyMBADeep(
        C, obfuscateSum(C, X, C.mkVar("other" + std::to_string(I), 32)), {},
        Cache);
  auto Value = X;
  for (unsigned I = 0; I < 12; ++I)
    Value = C.mkXor(C.mkAdd(Value, Y), Z);
  MBAOptions Options;
  Options.MaxWork = 65536;
  const auto Warm = simplifyMBADeep(C, Value, Options, Cache);
  ASSERT_EQ(Warm.Expr, Value);
  ASSERT_NE(Warm.Outcome, MBAOutcome::BudgetExhausted);
  const auto Root = C.mkUDiv(Value, C.mkXor(X, Y));
  const auto Cold = simplifyMBADeep(C, Root, Options);
  const auto Hot = simplifyMBADeep(C, Root, Options, Cache);
  EXPECT_EQ(Hot.Expr, Cold.Expr);
  EXPECT_LT(Hot.Work * 4, Cold.Work);
  EXPECT_LE(Cache.size(), 4u);
}

TEST(SymMBARewriteCache, UnchangedAttemptsRespectPolicyAndWorkBoundaries) {
  SymContext C;
  const auto X = C.mkVar("x", 32), Y = C.mkVar("y", 32);
  const auto Z = C.mkVar("z", 32);
  auto Value = X;
  for (unsigned I = 0; I < 12; ++I)
    Value = C.mkXor(C.mkAdd(Value, Y), Z);
  MBAOptions Options;
  Options.MaxWork = 65536;
  MBARewriteCache Cache(C), Disabled(C, 0);
  const auto Warm = simplifyMBADeep(C, Value, Options, Cache);
  ASSERT_NE(Warm.Outcome, MBAOutcome::BudgetExhausted);
  const auto Repeated = simplifyMBADeep(C, Value, Options, Cache);
  EXPECT_EQ(Repeated.Expr, Warm.Expr);
  EXPECT_EQ(Repeated.Work, 1u);
  auto Changed = Options;
  Changed.MaxAtoms += 1;
  const auto Retried = simplifyMBADeep(C, Value, Changed, Cache);
  EXPECT_GT(Retried.Work, 1u);
  EXPECT_EQ(Retried.Expr, Value);
  auto Zero = Options;
  Zero.MaxWork = 0;
  EXPECT_EQ(simplifyMBADeep(C, Value, Zero, Cache).Work, 0u);
  MBARewriteCache Incomplete(C);
  auto Tight = Options;
  Tight.MaxWork = 1;
  EXPECT_EQ(simplifyMBADeep(C, Value, Tight, Incomplete).Outcome,
            MBAOutcome::BudgetExhausted);
  EXPECT_EQ(simplifyMBADeep(C, Value, Tight, Incomplete).Outcome,
            MBAOutcome::BudgetExhausted);
  EXPECT_EQ(Incomplete.reusedCompletions(), 0u);
  const auto Cold = simplifyMBADeep(C, Value, Options, Disabled);
  EXPECT_GT(Cold.Work, 1u);
  EXPECT_EQ(Disabled.size(), 0u);
  EXPECT_LE(Cache.size(),
            ::neverd::limits::symbolic::kSymMBAMBARewriteCacheCapacity);
}
