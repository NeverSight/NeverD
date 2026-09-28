//===- FiniteValuesTests.cpp - Optional finite projection guards ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/interpreter/FiniteValues.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <limits>

using namespace neverd::analysis;
using namespace neverd::analysis::detail;
using namespace neverd::symbolic;

namespace {

TEST(FiniteValues, UnrelatedPredicateLeavesWideProjectionInputUnconstrained) {
  SymContext Ctx;
  const SymRef Value = Ctx.mkVar("projection", 64);
  const SymRef Other = Ctx.mkVar("selector", 8);
  const SymRef Predicate = Ctx.mkEq(Other, Ctx.mkConst(8, 7));
  const size_t Nodes = Ctx.numNodes();
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 9));
  EXPECT_EQ(Ctx.numNodes(), Nodes);
}

TEST(FiniteValues, ExactSymbolAndItsExtractsRemainConstrained) {
  SymContext Ctx;
  const SymRef Value = Ctx.mkVar("projection", 64);
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Value, Ctx.mkConst(64, 7)), Value, 32, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Ctx.mkExtract(Value, 8, 8), Ctx.mkConst(8, 7)), Value, 32,
      100));
  const SymRef Boolean = Ctx.mkVar("condition", 1);
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Boolean, Boolean, 1, 1));
}

TEST(FiniteValues, SymbolIdentityDoesNotDependOnDiagnosticNames) {
  SymContext Ctx;
  const SymRef Value =
      Ctx.mkInputVar("shared_name", 64, {SymInputKind::Register, 0, 8, 0});
  const SymRef Other =
      Ctx.mkInputVar("shared_name", 64, {SymInputKind::Register, 16, 8, 0});
  ASSERT_NE(Value, Other);
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Other, Ctx.mkConst(64, 7)), Value, 32, 100));
  const SymRef Overlapping =
      Ctx.mkInputVar("byte_lane", 8, {SymInputKind::Register, 1, 1, 0});
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Overlapping, Ctx.mkConst(8, 7)), Value, 32, 100));
}

TEST(FiniteValues, ProjectionTypeMustExceedTheRequestedLimit) {
  SymContext Ctx;
  const SymRef Predicate = Ctx.mkTrue();
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Predicate, Ctx.mkVar("five_bits", 5), 32, 1));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate,
                                              Ctx.mkVar("six_bits", 6), 32, 7));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate,
                                               Ctx.mkVar("boolean", 1), 2, 1));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate,
                                              Ctx.mkVar("boolean", 1), 1, 2));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Predicate, Ctx.mkVar("full_word", 64),
      std::numeric_limits<uint32_t>::max(), 33));
}

TEST(FiniteValues, IncompleteDagWalkCannotProveIndependence) {
  SymContext Ctx;
  const SymRef Value = Ctx.mkVar("projection", 64);
  const SymRef Other = Ctx.mkVar("selector", 8);
  const SymRef Predicate = Ctx.mkEq(Other, Ctx.mkConst(8, 7));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 0));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 8));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 9));

  // The shared sum appears below two comparisons. Its node and operands
  // are visited once. Predicate edges and six independent value bits share
  // the same work budget; expanding the shared sum twice would exceed it.
  const SymRef Sum = Ctx.mkAdd(Ctx.mkVar("a", 8), Ctx.mkVar("b", 8));
  const SymRef Shared = Ctx.mkOr(Ctx.mkEq(Sum, Ctx.mkConst(8, 2)),
                                 Ctx.mkEq(Sum, Ctx.mkConst(8, 5)));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Shared, Value, 32, 14));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Shared, Value, 32, 15));
}

TEST(FiniteValues, InvalidInputsAndArithmeticDoNotUseTheShortcut) {
  SymContext Ctx;
  const SymRef Value = Ctx.mkVar("projection", 64);
  const SymRef Predicate = Ctx.mkTrue();
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, {}, Value, 32, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Value, Value, 32, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, {}, 32, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 0, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate,
                                               Ctx.mkConst(64, 7), 32, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Predicate, Ctx.mkAdd(Value, Ctx.mkConst(64, 1)), 32, 100));
}

TEST(FiniteValues, ProjectionIndependenceDoesNotEstablishReachability) {
  SymContext Ctx;
  const SymRef Value = Ctx.mkVar("projection", 64);
  const SymRef Predicate = Ctx.mkFalse();
  ASSERT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 7));
  uint64_t Queries = 0;
  const auto Reachable =
      enumerateFiniteValues(Ctx, Predicate, {}, 32, {}, Queries);
  EXPECT_EQ(Reachable.Status, FiniteValueStatus::Complete);
  EXPECT_TRUE(Reachable.Tuples.empty());
  EXPECT_EQ(Queries, 0u);
}

TEST(FiniteValues, ConstantBitwiseMasksPreserveOnlyUnforcedSourceBits) {
  SymContext Ctx;
  const SymRef Word = Ctx.mkVar("word", 64);
  const SymRef Predicate = Ctx.mkTrue();
  constexpr uint64_t SixBits = 0x8000800080008081;
  constexpr uint64_t FiveBits = SixBits & ~uint64_t{1};
  for (uint64_t Mask : {FiveBits, SixBits}) {
    const bool Exceeds = Mask == SixBits;
    for (SymRef Value : {Ctx.mkAnd(Word, Ctx.mkConst(64, Mask)),
                         Ctx.mkOr(Word, Ctx.mkConst(64, ~Mask)),
                         Ctx.mkXor(Ctx.mkAnd(Word, Ctx.mkConst(64, Mask)),
                                   Ctx.mkConst(64, 0x987654321abcdef0)),
                         Ctx.mkNot(Ctx.mkAnd(Word, Ctx.mkConst(64, Mask)))})
      EXPECT_EQ(
          hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 32, 1000),
          Exceeds);
  }
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Predicate, Ctx.mkAnd(Word, Ctx.mkConst(64, SixBits)), 32, 20));
}

TEST(FiniteValues, ExtractAndConcatCountIndependentSourceBitIdentities) {
  SymContext Ctx;
  const SymRef Word = Ctx.mkVar("word", 16);
  const SymRef Other = Ctx.mkVar("other", 3);
  const SymRef Predicate = Ctx.mkTrue();
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Predicate, Ctx.mkExtract(Word, 7, 6), 32, 100));
  const SymRef Split = Ctx.mkConcat(Ctx.mkExtract(Word, 0, 3), Other);
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Split, 32, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Other, Ctx.mkConst(3, 0)), Split, 8, 100));
  // Overlapping views expose six distinct bits, not eight. Repeating a view
  // or complementing it never manufactures another independent source bit.
  const SymRef Overlap =
      Ctx.mkConcat(Ctx.mkExtract(Word, 0, 4), Ctx.mkExtract(Word, 2, 4));
  EXPECT_TRUE(
      hasUnconstrainedProjectionInput(Ctx, Predicate, Overlap, 32, 100));
  EXPECT_FALSE(
      hasUnconstrainedProjectionInput(Ctx, Predicate, Overlap, 64, 100));
  const SymRef Repeated = Ctx.mkConcat(Ctx.mkExtract(Word, 0, 4),
                                       Ctx.mkNot(Ctx.mkExtract(Word, 0, 4)));
  EXPECT_FALSE(
      hasUnconstrainedProjectionInput(Ctx, Predicate, Repeated, 16, 100));
  EXPECT_TRUE(
      hasUnconstrainedProjectionInput(Ctx, Predicate, Repeated, 15, 100));
}

TEST(FiniteValues, SignExtensionDoesNotMultiplyTheSignBitDomain) {
  SymContext Ctx;
  const SymRef Predicate = Ctx.mkTrue();
  const SymRef Boolean = Ctx.mkVar("sign", 1);
  const SymRef Signed = Ctx.mkSExt(Boolean, 64);
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Signed, 1, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Signed, 2, 100));
  const SymRef Narrow = Ctx.mkVar("narrow", 3);
  for (SymRef Extended : {Ctx.mkZExt(Narrow, 64), Ctx.mkSExt(Narrow, 64)}) {
    EXPECT_TRUE(
        hasUnconstrainedProjectionInput(Ctx, Predicate, Extended, 7, 100));
    EXPECT_FALSE(
        hasUnconstrainedProjectionInput(Ctx, Predicate, Extended, 8, 100));
  }
}

TEST(FiniteValues, PredicateUseOfAnySourceBitExcludesTheWholeVariable) {
  SymContext Ctx;
  const SymRef Word = Ctx.mkVar("word", 64);
  const SymRef Low = Ctx.mkExtract(Word, 0, 8);
  const SymRef Predicate =
      Ctx.mkEq(Ctx.mkExtract(Word, 63, 1), Ctx.mkConst(1, 0));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Low, 32, 100));
  const SymRef Independent = Ctx.mkVar("independent", 6);
  // A constrained or unsupported part does not invalidate distinct, proven
  // independent bits elsewhere in the same output.
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Predicate, Ctx.mkConcat(Low, Independent), 32, 100));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Predicate,
      Ctx.mkConcat(Ctx.mkAdd(Low, Ctx.mkConst(8, 1)), Independent), 32, 100));
}

TEST(FiniteValues, TwoVaryingBitwiseOperandsDoNotProveIndependentBits) {
  SymContext Ctx;
  const SymRef A = Ctx.mkVar("a", 8);
  const SymRef B = Ctx.mkVar("b", 8);
  for (SymRef Value : {Ctx.mkAnd(A, B), Ctx.mkOr(A, B), Ctx.mkXor(A, B)})
    EXPECT_FALSE(
        hasUnconstrainedProjectionInput(Ctx, Ctx.mkTrue(), Value, 32, 100));
}

TEST(FiniteValues, SharedBudgetIncludesEveryRequiredOutputBitProof) {
  SymContext Ctx;
  const SymRef Word = Ctx.mkVar("word", 64);
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Ctx.mkTrue(), Word, 32, 6));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Ctx.mkTrue(), Word, 32, 7));
  // A wide source is safe to inspect through a small view, but a whole wide
  // output is outside this recovery-only shortcut's <=64-bit contract.
  const SymRef Wide = Ctx.mkVar("wide", 4096);
  EXPECT_FALSE(
      hasUnconstrainedProjectionInput(Ctx, Ctx.mkTrue(), Wide, 32, 100));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkTrue(), Ctx.mkExtract(Wide, 2048, 8), 32, 100));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkTrue(), SymRef(Ctx.numNodes() + 1), 32, 100));
}

TEST(FiniteValues, BitProjectionLowerBoundAgreesWithExhaustiveEnumeration) {
  SymContext Ctx;
  const SymRef A = Ctx.mkVar("a", 6);
  const SymRef B = Ctx.mkVar("b", 3);
  const SymRef Selector = Ctx.mkVar("selector", 2);
  const SymRef Predicate = Ctx.mkEq(Selector, Ctx.mkConst(2, 1));
  const SymRef Values[] = {
      Ctx.mkAnd(A, Ctx.mkConst(6, 0x3a)),
      Ctx.mkOr(A, Ctx.mkConst(6, 0x12)),
      Ctx.mkNot(A),
      Ctx.mkSExt(B, 8),
      Ctx.mkConcat(Ctx.mkExtract(A, 0, 3), B),
      Ctx.mkConcat(Ctx.mkExtract(A, 0, 3), Ctx.mkExtract(A, 2, 3))};
  for (SymRef Value : Values)
    for (uint32_t Limit : {1u, 3u, 7u, 15u, 31u, 63u}) {
      uint64_t Queries = 0;
      const auto Domain =
          enumerateFiniteValues(Ctx, Predicate, {Value}, Limit, {}, Queries);
      ASSERT_TRUE(Domain.Status == FiniteValueStatus::Complete ||
                  Domain.Status == FiniteValueStatus::TooManyValues);
      EXPECT_EQ(
          hasUnconstrainedProjectionInput(Ctx, Predicate, Value, Limit, 1000),
          Domain.Status == FiniteValueStatus::TooManyValues);
    }
}

TEST(FiniteValues, RelatedColumnsExcludeEveryReachableSourceVariable) {
  SymContext Ctx;
  const SymRef Word = Ctx.mkVar("word", 64);
  const SymRef Other = Ctx.mkVar("other", 64);
  const SymRef Value = Ctx.mkAnd(Word, Ctx.mkConst(64, 3));
  const SymRef Independent = Ctx.mkAnd(Other, Ctx.mkConst(64, 15));
  const SymRef Predicate = Ctx.mkTrue();
  ASSERT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 3, 100));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 3, 100,
                                              {Independent}));
  for (SymRef Related :
       {Value, Ctx.mkAnd(Word, Ctx.mkConst(64, 1)),
        Ctx.mkAdd(Word, Ctx.mkConst(64, 1)),
        Ctx.mkConcat(Ctx.mkExtract(Word, 0, 1), Ctx.mkExtract(Other, 0, 1))}) {
    const size_t Nodes = Ctx.numNodes();
    EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 3, 100,
                                                 {Related}));
    EXPECT_EQ(Ctx.numNodes(), Nodes);
  }
  // This intentionally excludes the whole variable even for disjoint views.
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Value, 3, 100,
                                               {Ctx.mkExtract(Word, 63, 1)}));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(
      Ctx, Ctx.mkEq(Ctx.mkExtract(Word, 63, 1), Ctx.mkConst(1, 0)), Value, 3,
      100, {Independent}));
}

TEST(FiniteValues, RelatedRootsShareTheTraversalBudgetAndMustBeValid) {
  SymContext Ctx;
  const SymRef Word = Ctx.mkVar("word", 64);
  const SymRef Other = Ctx.mkVar("other", 64);
  const SymRef Predicate = Ctx.mkTrue();
  EXPECT_FALSE(
      hasUnconstrainedProjectionInput(Ctx, Predicate, Word, 32, 7, {Other}));
  EXPECT_TRUE(
      hasUnconstrainedProjectionInput(Ctx, Predicate, Word, 32, 8, {Other}));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Word, 32, 8,
                                               {Other, Other}));
  EXPECT_TRUE(hasUnconstrainedProjectionInput(Ctx, Predicate, Word, 32, 9,
                                              {Other, Other}));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Word, 32, 100,
                                               {SymRef{}}));
  EXPECT_FALSE(hasUnconstrainedProjectionInput(Ctx, Predicate, Word, 32, 100,
                                               {SymRef(Ctx.numNodes() + 1)}));
}

TEST(FiniteValues, CompleteMaskedColumnsNeedIndependentCartesianFactors) {
  SymContext Ctx;
  const SymRef A = Ctx.mkVar("a", 2);
  const SymRef B = Ctx.mkVar("b", 2);
  const SymRef Selector = Ctx.mkVar("selector", 1);
  const SymRef Predicate = Ctx.mkEq(Selector, Ctx.mkConst(1, 1));
  const SymRef Candidate = Ctx.mkZExt(A, 8);
  uint64_t Queries = 0;
  const auto CandidateDomain =
      enumerateFiniteValues(Ctx, Predicate, {Candidate}, 16, {}, Queries);
  ASSERT_EQ(CandidateDomain.Status, FiniteValueStatus::Complete);
  ASSERT_EQ(CandidateDomain.Tuples.size(), 4u);
  for (bool Correlated : {false, true}) {
    const SymRef Related = Correlated ? Ctx.mkAnd(A, Ctx.mkConst(2, 1)) : B;
    const auto Kept =
        enumerateFiniteValues(Ctx, Predicate, {Related}, 16, {}, Queries);
    const auto Joint = enumerateFiniteValues(
        Ctx, Predicate, {Candidate, Related}, 16, {}, Queries);
    ASSERT_EQ(Kept.Status, FiniteValueStatus::Complete);
    ASSERT_EQ(Joint.Status, FiniteValueStatus::Complete);
    const bool Independent = hasUnconstrainedProjectionInput(
        Ctx, Predicate, Candidate, 3, 100, {Related});
    EXPECT_EQ(Independent, !Correlated);
    if (Independent) {
      EXPECT_EQ(Joint.Tuples.size(),
                CandidateDomain.Tuples.size() * Kept.Tuples.size());
      for (const auto &Column : CandidateDomain.Tuples)
        for (const auto &Rest : Kept.Tuples)
          EXPECT_NE(std::find(Joint.Tuples.begin(), Joint.Tuples.end(),
                              std::vector<uint64_t>{Column[0], Rest[0]}),
                    Joint.Tuples.end());
    } else {
      // A full four-value column can still correlate with another column.
      // Dropping it as TOP would add four impossible joint tuples here.
      EXPECT_LT(Joint.Tuples.size(),
                CandidateDomain.Tuples.size() * Kept.Tuples.size());
    }
  }
  const SymRef Partial = Ctx.mkAnd(Candidate, Ctx.mkConst(8, 1));
  EXPECT_FALSE(
      hasUnconstrainedProjectionInput(Ctx, Predicate, Partial, 3, 100, {B}));
}

} // namespace
