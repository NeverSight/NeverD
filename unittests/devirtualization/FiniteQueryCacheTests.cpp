//===- FiniteQueryCacheTests.cpp - Exact finite-proof cache keys ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/interpreter/FiniteQueryCache.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymState.h"

using namespace neverd::analysis;
using namespace neverd::analysis::detail;
using namespace neverd::symbolic;

namespace {
FiniteValues prove(SymContext &Ctx, SymRef Predicate,
                   llvm::ArrayRef<SymRef> Values, uint32_t Limit) {
  uint64_t Queries = 0;
  return enumerateFiniteValues(Ctx, Predicate, Values, Limit,
                               SpecializationOptions{}, Queries);
}

void expectHit(const std::optional<FiniteValues> &Hit,
               const FiniteValues &Expected) {
  ASSERT_TRUE(Hit);
  EXPECT_EQ(Hit->Status, Expected.Status);
  EXPECT_EQ(Hit->Tuples, Expected.Tuples);
}

TEST(FiniteQueryCache, AlphaRenamingIgnoresContextAndInputMetadata) {
  FiniteQueryCache Cache(4096);
  SymContext First;
  const auto X = First.mkVar("source", 8);
  const auto P = First.mkUle(X, First.mkConst(8, 3));
  const SymRef Values[] = {X, First.mkAnd(X, First.mkConst(8, 1))};
  const auto Result = prove(First, P, Values, 8);
  ASSERT_EQ(Result.Status, FiniteValueStatus::Complete);
  ASSERT_EQ(Result.Tuples.size(), 4u);
  Cache.store(First, P, Values, 8, Result);

  SymContext Second;
  Second.mkVar("unrelated", 64);
  Second.mkFreshVar(16);
  const auto Y =
      Second.mkInputVar("renamed", 8, {SymInputKind::Register, 32, 1, 2});
  const auto Q = Second.mkUle(Y, Second.mkConst(8, 3));
  const SymRef Renamed[] = {Y, Second.mkAnd(Y, Second.mkConst(8, 1))};
  ASSERT_NE(First.varId(X), Second.varId(Y));
  const auto Nodes = Second.numNodes();
  expectHit(Cache.lookup(Second, Q, Renamed, 8), Result);
  EXPECT_EQ(Second.numNodes(), Nodes);
  const auto Independent = prove(Second, Q, Renamed, 8);
  EXPECT_EQ(Independent.Tuples, Result.Tuples);
}

TEST(FiniteQueryCache, SharedAndIndependentVariablesCannotShareAKey) {
  FiniteQueryCache Cache(4096);
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 1);
  const auto Y = Ctx.mkVar("y", 1);
  const auto P = Ctx.mkTrue();
  const SymRef Shared[] = {X, X};
  const SymRef Independent[] = {X, Y};
  const auto SharedResult = prove(Ctx, P, Shared, 4);
  ASSERT_EQ(SharedResult.Status, FiniteValueStatus::Complete);
  ASSERT_EQ(SharedResult.Tuples.size(), 2u);
  Cache.store(Ctx, P, Shared, 4, SharedResult);
  EXPECT_FALSE(Cache.lookup(Ctx, P, Independent, 4));
  const auto IndependentResult = prove(Ctx, P, Independent, 4);
  ASSERT_EQ(IndependentResult.Tuples.size(), 4u);
  Cache.store(Ctx, P, Independent, 4, IndependentResult);
  expectHit(Cache.lookup(Ctx, P, Shared, 4), SharedResult);
  expectHit(Cache.lookup(Ctx, P, Independent, 4), IndependentResult);
}

TEST(FiniteQueryCache, PredicateAndValuesShareOneVariableRenaming) {
  FiniteQueryCache Cache(4096);
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 8);
  const auto Y = Ctx.mkVar("y", 8);
  const auto P = Ctx.mkEq(X, Ctx.mkConst(8, 19));
  const auto Result = prove(Ctx, P, {X}, 4);
  ASSERT_EQ(Result.Tuples, (std::vector<std::vector<uint64_t>>{{19}}));
  Cache.store(Ctx, P, {X}, 4, Result);
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Y}, 4));
  EXPECT_EQ(prove(Ctx, P, {Y}, 4).Status, FiniteValueStatus::TooManyValues);
}

TEST(FiniteQueryCache, WidthConstantsAndEnumerationLimitArePartOfTheKey) {
  FiniteQueryCache Cache(4096);
  SymContext Ctx;
  const auto Value = Ctx.mkConst(8, 7);
  const auto P = Ctx.mkTrue();
  const auto Result = prove(Ctx, P, {Value}, 4);
  Cache.store(Ctx, P, {Value}, 4, Result);
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Ctx.mkConst(16, 7)}, 4));
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Ctx.mkConst(8, 8)}, 4));
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Value}, 5));
  EXPECT_FALSE(Cache.lookup(Ctx, Ctx.mkFalse(), {Value}, 4));
  expectHit(Cache.lookup(Ctx, P, {Value}, 4), Result);
}

TEST(FiniteQueryCache, ExtractAuxAndOrderedOperandsRemainDistinct) {
  FiniteQueryCache Cache(4096);
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 8);
  const auto Y = Ctx.mkVar("y", 8);
  const auto P =
      Ctx.mkAnd(Ctx.mkEq(X, Ctx.mkConst(8, 6)), Ctx.mkEq(Y, Ctx.mkConst(8, 3)));
  const auto Low = Ctx.mkExtract(X, 0, 1);
  Cache.store(Ctx, P, {Low}, 4, prove(Ctx, P, {Low}, 4));
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Ctx.mkExtract(X, 1, 1)}, 4));
  const auto Quotient = Ctx.mkUDiv(X, Y);
  const auto Result = prove(Ctx, P, {Quotient}, 4);
  ASSERT_EQ(Result.Tuples, (std::vector<std::vector<uint64_t>>{{2}}));
  Cache.store(Ctx, P, {Quotient}, 4, Result);
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Ctx.mkUDiv(Y, X)}, 4));
  const auto Sum = Ctx.mkAdd(X, Y);
  Cache.store(Ctx, P, {Sum}, 4, prove(Ctx, P, {Sum}, 4));
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Ctx.mkXor(X, Y)}, 4));
  Cache.store(Ctx, P, {X, Y}, 4, prove(Ctx, P, {X, Y}, 4));
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Y, X}, 4));
}

TEST(FiniteQueryCache, WideConstantsUseTheirBitsInsteadOfPoolIndices) {
  FiniteQueryCache Cache(4096);
  llvm::APInt Constant(128, 23);
  Constant.setBit(91);
  SymContext First;
  const auto X = First.mkVar("wide", 128);
  const auto P = First.mkEq(X, First.mkConst(Constant));
  const auto Value = First.mkExtract(X, 0, 8);
  const auto Result = prove(First, P, {Value}, 4);
  ASSERT_EQ(Result.Status, FiniteValueStatus::Complete);
  Cache.store(First, P, {Value}, 4, Result);

  SymContext Second;
  Second.mkConst(llvm::APInt::getOneBitSet(128, 100));
  const auto Y = Second.mkFreshVar(128, "other");
  const auto Q = Second.mkEq(Y, Second.mkConst(Constant));
  const auto Other = Second.mkExtract(Y, 0, 8);
  expectHit(Cache.lookup(Second, Q, {Other}, 4), Result);
  Constant.flipBit(90);
  EXPECT_FALSE(Cache.lookup(Second, Second.mkEq(Y, Second.mkConst(Constant)),
                            {Other}, 4));
}

TEST(FiniteQueryCache, CompleteUnsatisfiableAndTooManyResultsAreReusable) {
  FiniteQueryCache Cache(4096);
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 8);
  const auto None = prove(Ctx, Ctx.mkFalse(), {X}, 2);
  ASSERT_EQ(None.Status, FiniteValueStatus::Complete);
  ASSERT_TRUE(None.Tuples.empty());
  Cache.store(Ctx, Ctx.mkFalse(), {X}, 2, None);
  expectHit(Cache.lookup(Ctx, Ctx.mkFalse(), {X}, 2), None);
  const auto Many = prove(Ctx, Ctx.mkTrue(), {X}, 2);
  ASSERT_EQ(Many.Status, FiniteValueStatus::TooManyValues);
  ASSERT_TRUE(Many.Tuples.empty());
  Cache.store(Ctx, Ctx.mkTrue(), {X}, 2, Many);
  expectHit(Cache.lookup(Ctx, Ctx.mkTrue(), {X}, 2), Many);
  EXPECT_FALSE(Cache.lookup(Ctx, Ctx.mkTrue(), {X}, 3));
}

TEST(FiniteQueryCache, IncompleteResultsAndMalformedTuplesAreNotRecorded) {
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 1);
  const auto P = Ctx.mkTrue();
  for (auto Status : {FiniteValueStatus::Unknown, FiniteValueStatus::Invalid,
                      FiniteValueStatus::QueryBudgetExceeded}) {
    FiniteQueryCache Cache(4096);
    Cache.store(Ctx, P, {X}, 2, {Status, {{0}}});
    EXPECT_FALSE(Cache.lookup(Ctx, P, {X}, 2));
  }
  for (const FiniteValues &Result :
       {FiniteValues{FiniteValueStatus::TooManyValues, {{0}}},
        FiniteValues{FiniteValueStatus::Complete, {{0, 1}}},
        FiniteValues{FiniteValueStatus::Complete, {{2}}},
        FiniteValues{FiniteValueStatus::Complete, {{0}, {1}, {0}}}}) {
    FiniteQueryCache Cache(4096);
    Cache.store(Ctx, P, {X}, 2, Result);
    EXPECT_FALSE(Cache.lookup(Ctx, P, {X}, 2));
  }
}

TEST(FiniteQueryCache, KeyAndResultStorageBudgetsOnlyCauseMisses) {
  SymContext Ctx;
  const auto P = Ctx.mkTrue();
  const auto Seven = Ctx.mkConst(8, 7);
  const auto Result = prove(Ctx, P, {Seven}, 2);
  for (uint64_t Budget : {0, 15, 27}) {
    FiniteQueryCache Cache(Budget);
    Cache.store(Ctx, P, {Seven}, 2, Result);
    EXPECT_FALSE(Cache.lookup(Ctx, P, {Seven}, 2));
  }
  FiniteQueryCache Exact(28);
  Exact.store(Ctx, P, {Seven}, 2, Result);
  expectHit(Exact.lookup(Ctx, P, {Seven}, 2), Result);
  const auto Eight = Ctx.mkConst(8, 8);
  Exact.store(Ctx, P, {Eight}, 2, prove(Ctx, P, {Eight}, 2));
  EXPECT_FALSE(Exact.lookup(Ctx, P, {Eight}, 2));
  expectHit(Exact.lookup(Ctx, P, {Seven}, 2), Result);

  const auto X = Ctx.mkVar("bounded", 5);
  const auto Domain = prove(Ctx, P, {X}, 32);
  ASSERT_EQ(Domain.Status, FiniteValueStatus::Complete);
  ASSERT_EQ(Domain.Tuples.size(), 32u);
  FiniteQueryCache Small(64);
  Small.store(Ctx, P, {X}, 32, Domain);
  EXPECT_FALSE(Small.lookup(Ctx, P, {X}, 32));
  EXPECT_EQ(Domain.Tuples.size(), 32u);
}

TEST(FiniteQueryCache, OversizedDagAndInvalidQueriesCannotHit) {
  SymContext Ctx;
  const auto X = Ctx.mkVar("x", 8);
  auto Deep = X;
  for (unsigned I = 1; I < 32; ++I)
    Deep = Ctx.mkUDiv(X, Ctx.mkAdd(Deep, Ctx.mkConst(8, I)));
  const auto P = Ctx.mkFalse();
  const auto Result = prove(Ctx, P, {Deep}, 2);
  FiniteQueryCache Cache(64);
  Cache.store(Ctx, P, {Deep}, 2, Result);
  EXPECT_FALSE(Cache.lookup(Ctx, P, {Deep}, 2));
  EXPECT_FALSE(Cache.lookup(Ctx, {}, {X}, 2));
  EXPECT_FALSE(Cache.lookup(Ctx, X, {X}, 2));
  EXPECT_FALSE(Cache.lookup(Ctx, P, {SymRef(0xfffffffe)}, 2));
  EXPECT_FALSE(Cache.lookup(Ctx, P, {X}, 0));
}

TEST(FiniteQueryCache, ByteOrderIsRepresentedByTheExpressionDag) {
  FiniteQueryCache Cache(4096);
  SymContext Little;
  SymState LittleState(Little, llvm::endianness::little);
  const auto Word = LittleState.read(SymSpace::Register, 24, 2);
  const auto Byte = LittleState.read(SymSpace::Register, 24, 1);
  const auto P = Little.mkEq(Word, Little.mkConst(16, 0x0102));
  const auto Result = prove(Little, P, {Byte}, 4);
  ASSERT_EQ(Result.Tuples, (std::vector<std::vector<uint64_t>>{{2}}));
  Cache.store(Little, P, {Byte}, 4, Result);

  SymContext Big;
  SymState BigState(Big, llvm::endianness::big);
  const auto OtherWord = BigState.read(SymSpace::Register, 24, 2);
  const auto OtherByte = BigState.read(SymSpace::Register, 24, 1);
  const auto Q = Big.mkEq(OtherWord, Big.mkConst(16, 0x0102));
  EXPECT_FALSE(Cache.lookup(Big, Q, {OtherByte}, 4));
  EXPECT_EQ(prove(Big, Q, {OtherByte}, 4).Tuples,
            (std::vector<std::vector<uint64_t>>{{1}}));
}
} // namespace
