//===- FrameOffsetsTests.cpp - Entry-relative address proofs --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/core/FrameEntryConstraints.h"
#include "../../lib/analysis/core/FrameOffsets.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymState.h"

#include <memory>
#include <optional>
#include <tuple>

using namespace neverd::analysis;
using namespace neverd::analysis::detail;
using namespace neverd::symbolic;

namespace {

TEST(FrameOffsets, DisjointRelativeRangesKeepEqualRootsAndBoundaryBytes) {
  struct Case {
    uint64_t Frame;
    int64_t FrameBegin;
    int64_t FrameEnd;
    uint64_t Object;
    int64_t ObjectBegin;
    int64_t ObjectEnd;
    bool Allowed;
  };
  const Case Cases[] = {
      {0, 0, 8, 0, 16, 24, true},
      {0, 0, 8, 0, 0, 8, false},
      {0, -1, 8, 16, 0, 8, false},
      {32, -32, 8, 40, 0, 32, true},
      {32, -32, 8, 39, 0, 32, false},
      {32, -32, 8, 0, -1, 8, false},
      {2, 0, 8, 1, -1, 0, true},
      {UINT64_MAX - 7, 0, 8, 0, 0, 8, true},
      {UINT64_MAX - 6, 0, 8, 0, 0, 8, false},
      {0, 0, 8, UINT64_MAX, 0, 1, true},
      {UINT64_C(1) << 63, INT64_MIN, INT64_MIN + 1, 1, 0, 1, true},
      {UINT64_MAX, INT64_MIN, INT64_MIN + 1, UINT64_MAX, 0, 1, true},
      {0, INT64_MAX - 1, INT64_MAX, 1, INT64_MIN, INT64_MIN + 1, false},
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Frame);
    SCOPED_TRACE(C.Object);
    SCOPED_TRACE(C.FrameBegin);
    SCOPED_TRACE(C.ObjectBegin);
    SymContext Ctx;
    const auto Frame = Ctx.mkConst(64, C.Frame);
    const auto Object = Ctx.mkConst(64, C.Object);
    const auto Domain = Ctx.mkAnd(
        {nonwrappingFramePredicate(Ctx, Frame, C.FrameBegin, C.FrameEnd),
         nonwrappingFramePredicate(Ctx, Object, C.ObjectBegin, C.ObjectEnd),
         disjointFrameRangesPredicate(Ctx, Frame, C.FrameBegin, C.FrameEnd,
                                      Object, C.ObjectBegin, C.ObjectEnd)});
    const auto Actual = Ctx.asConst(Domain);
    ASSERT_TRUE(Actual);
    EXPECT_EQ(!Actual->isZero(), C.Allowed);
  }
}

SymRef residue(SymContext &Ctx, SymRef Root, uint64_t Mask, uint64_t Value) {
  return Ctx.mkEq(Ctx.mkAnd(Root, Ctx.mkConst(64, Mask)),
                  Ctx.mkConst(64, Value));
}

TEST(FrameOffsets, ConstantCoveredBitsExposeOffsetsWithoutSolverQueries) {
  SymContext C;
  const auto Root = C.mkVar("entry", 64);
  const auto Adjusted = C.mkAdd(Root, C.mkConst(64, uint64_t{0} - 69));
  const auto Mask = C.mkConst(64, 0xa5);
  const auto Hidden =
      C.mkNot(C.mkOr(C.mkConst(64, ~uint64_t{0xa5}), C.mkNot(Adjusted)));
  const auto Address = C.mkAdd(Adjusted, C.mkOr(Mask, C.mkXor(Mask, Hidden)));
  SpecializationOptions Options;
  Options.MaxSolverQueries = 0;
  uint64_t Queries = 0;
  const auto Result =
      proveFrameOffset(C, C.mkTrue(), Address, Root, Options, Queries);
  ASSERT_EQ(Result.Status, FrameOffsetStatus::Exact);
  EXPECT_EQ(Result.Offset, uint64_t{0xa5} - 69);
  EXPECT_EQ(Queries, 0u);

  // An uncovered high bit makes the displacement input-dependent.
  const auto Wider =
      C.mkNot(C.mkOr(C.mkConst(64, ~uint64_t{0x1a5}), C.mkNot(Adjusted)));
  const auto Variable = C.mkAdd(Adjusted, C.mkOr(Mask, C.mkXor(Mask, Wider)));
  EXPECT_EQ(proveFrameOffset(C, C.mkTrue(), Variable, Root, {}, Queries).Status,
            FrameOffsetStatus::NonUnique);
}

TEST(FrameOffsets, EveryAlignmentResidueKeepsArbitraryHighBits) {
  uint64_t Queries = 0;
  for (uint64_t Alignment : {2, 4, 8, 16, 32}) {
    for (uint64_t Low = 0; Low != Alignment; ++Low) {
      SCOPED_TRACE(Alignment);
      SCOPED_TRACE(Low);
      SymContext Ctx;
      const auto Root = Ctx.mkVar("entry", 64);
      for (uint64_t Bias : {uint64_t{0}, uint64_t{37}, uint64_t{0} - 53}) {
        const auto Value =
            Ctx.mkAdd(Ctx.mkAnd(Ctx.mkAdd(Root, Ctx.mkConst(64, Bias)),
                                Ctx.mkConst(64, uint64_t{0} - Alignment)),
                      Ctx.mkConst(64, 11));
        const auto Result =
            proveFrameOffset(Ctx, residue(Ctx, Root, Alignment - 1, Low), Value,
                             Root, {}, Queries);
        ASSERT_EQ(Result.Status, FrameOffsetStatus::Exact);
        EXPECT_EQ(Result.Offset, Bias + 11 - ((Low + Bias) & (Alignment - 1)));
      }
    }
  }
  EXPECT_GT(Queries, 0u);
}

TEST(FrameOffsets, ByteStateAndSplitArithmeticPreserveThePhysicalRoot) {
  SymContext Ctx;
  SymState State(Ctx);
  // The proof executor may obtain the root from independently named bytes.
  const auto Root = State.read(SymSpace::Register, 0, 8);
  const auto Adjusted = Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t{0} - 101));
  const auto Aligned = Ctx.mkConcat(
      Ctx.mkExtract(Adjusted, 8, 56),
      Ctx.mkAnd(Ctx.mkExtract(Adjusted, 0, 8), Ctx.mkConst(8, 0xe0)));
  State.write(SymSpace::Register, 8, Aligned);
  uint64_t Queries = 0;
  for (uint64_t Low = 0; Low != 32; ++Low) {
    const auto Result = proveFrameOffset(Ctx, residue(Ctx, Root, 31, Low),
                                         State.read(SymSpace::Register, 8, 8),
                                         Root, {}, Queries);
    ASSERT_EQ(Result.Status, FrameOffsetStatus::Exact);
    EXPECT_EQ(Result.Offset, uint64_t{0} - 101 - ((Low - 101) & 31));
  }
}

TEST(FrameOffsets, SplitAlignmentProjectsOnlyTheRemovedLowBits) {
  for (unsigned Split : {8U, 16U, 32U})
    for (uint64_t Alignment : {2, 4, 8, 16, 32})
      for (uint64_t Low = 0; Low != Alignment; ++Low)
        for (uint64_t Bias : {uint64_t{0}, uint64_t{173}, uint64_t{0} - 405}) {
          SCOPED_TRACE(Split);
          SCOPED_TRACE(Alignment);
          SCOPED_TRACE(Low);
          SCOPED_TRACE(Bias);
          SymContext Ctx;
          const auto Root = Ctx.mkVar("stack", 64);
          const auto Adjusted = Ctx.mkAdd(Root, Ctx.mkConst(64, Bias));
          const auto Aligned = Ctx.mkConcat(
              Ctx.mkExtract(Adjusted, Split, 64 - Split),
              Ctx.mkAnd(Ctx.mkExtract(Adjusted, 0, Split),
                        Ctx.mkConst(Split, uint64_t{0} - Alignment)));
          const auto Value = Ctx.mkAdd(Aligned, Ctx.mkConst(64, 27));
          SpecializationOptions Options;
          // A low-residue proof fits; bit-blasting a whole 64-bit subtraction
          // under the same ceiling does not. No high root bits are fixed.
          Options.MaxSolverGates = 256;
          uint64_t Queries = 0;
          const auto R =
              proveFrameOffset(Ctx, residue(Ctx, Root, Alignment - 1, Low),
                               Value, Root, Options, Queries);
          ASSERT_EQ(R.Status, FrameOffsetStatus::Exact);
          EXPECT_EQ(R.Offset, Bias + 27 - ((Low + Bias) & (Alignment - 1)));
        }
}

TEST(FrameOffsets, SplitAlignmentDoesNotMergeDifferentSourcesOrBiases) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("stack", 64);
  const auto Other = Ctx.mkVar("other", 64);
  const auto Predicate = residue(Ctx, Root, 15, 8);
  const auto High = Ctx.mkExtract(Ctx.mkAdd(Root, Ctx.mkConst(64, 128)), 8, 56);
  for (const auto LowSource : {Root, Other}) {
    const auto Low =
        Ctx.mkAnd(Ctx.mkExtract(LowSource, 0, 8), Ctx.mkConst(8, 0xf0));
    uint64_t Queries = 0;
    EXPECT_EQ(proveFrameOffset(Ctx, Predicate, Ctx.mkConcat(High, Low), Root,
                               {}, Queries)
                  .Status,
              FrameOffsetStatus::NonUnique);
  }
  // A sparse mask removes an unconstrained bit above the declared residue.
  const auto Sparse = Ctx.mkAnd(Root, Ctx.mkConst(64, ~uint64_t{0x21}));
  uint64_t Queries = 0;
  EXPECT_EQ(proveFrameOffset(Ctx, Predicate, Sparse, Root, {}, Queries).Status,
            FrameOffsetStatus::NonUnique);
  const auto Aligned = Ctx.mkAnd(Root, Ctx.mkConst(64, -16));
  EXPECT_EQ(
      proveFrameOffset(Ctx, Ctx.mkTrue(), Aligned, Root, {}, Queries).Status,
      FrameOffsetStatus::NonUnique);
}

TEST(FrameOffsets, NestedMaskedOffsetsPreserveModularCarriesAndWraparound) {
  for (uint64_t Entry :
       {uint64_t{0}, uint64_t{1}, uint64_t{15}, uint64_t{16}, uint64_t{127},
        uint64_t{255}, uint64_t{256}, uint64_t{0x7fffffffffffffff},
        UINT64_MAX - 255, UINT64_MAX})
    for (uint64_t Bias : {uint64_t{0}, uint64_t{173}, uint64_t{0} - 405}) {
      SCOPED_TRACE(Entry);
      SCOPED_TRACE(Bias);
      SymContext Ctx;
      const auto Root = Ctx.mkVar("stack", 64);
      const auto Adjusted = Ctx.mkAdd(Root, Ctx.mkConst(64, Bias));
      const auto Split = Ctx.mkConcat(
          Ctx.mkExtract(Adjusted, 8, 56),
          Ctx.mkAnd(Ctx.mkExtract(Adjusted, 0, 8), Ctx.mkConst(8, 0xf0)));
      const auto Value = Ctx.mkAdd(
          Ctx.mkAnd(Ctx.mkAdd(Split, Ctx.mkConst(64, 7)), Ctx.mkConst(64, -32)),
          Ctx.mkConst(64, 11));
      uint64_t Queries = 0;
      const auto Result =
          proveFrameOffset(Ctx, Ctx.mkEq(Root, Ctx.mkConst(64, Entry)), Value,
                           Root, {}, Queries);
      ASSERT_EQ(Result.Status, FrameOffsetStatus::Exact);
      const uint64_t First = (Entry + Bias) & ~uint64_t{15};
      const uint64_t Expected = ((First + 7) & ~uint64_t{31}) + 11;
      EXPECT_EQ(Result.Offset, Expected - Entry);
    }
}

TEST(FrameOffsets, RemainderConstructionAndIncompleteQueriesStayBudgeted) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("stack", 64);
  const auto Value = Ctx.mkAdd(
      Ctx.mkAnd(Ctx.mkAdd(Root, Ctx.mkConst(64, 173)), Ctx.mkConst(64, -16)),
      Ctx.mkConst(64, 27));
  const auto Predicate = residue(Ctx, Root, 15, 8);
  SpecializationOptions Options;
  Options.MaxSymbolicNodes = Ctx.numNodes();
  uint64_t Queries = 0;
  EXPECT_EQ(
      proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries).Status,
      FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Queries, 0U);
  Options.MaxSymbolicNodes = 65536;
  Options.MaxSolverQueries = 1;
  EXPECT_EQ(
      proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries).Status,
      FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Queries, 1U);
  Options.MaxSolverQueries = 3;
  EXPECT_EQ(
      proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries).Status,
      FrameOffsetStatus::Exact);
  EXPECT_EQ(Queries, 3U);
}

TEST(FrameOffsets, UnpartitionedAndIndependentValuesDoNotInventOffsets) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("entry", 64);
  const auto Aligned = Ctx.mkAnd(Root, Ctx.mkConst(64, uint64_t{0} - 16));
  uint64_t Queries = 0;
  for (auto Value : {Aligned, Ctx.mkVar("other", 64),
                     Ctx.mkAnd(Root, Ctx.mkVar("mask", 64))}) {
    const auto Result =
        proveFrameOffset(Ctx, Ctx.mkTrue(), Value, Root, {}, Queries);
    EXPECT_EQ(Result.Status, FrameOffsetStatus::NonUnique);
  }
}

TEST(FrameOffsets, PartialModelsAndExhaustedWorkSupplyNoOffset) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("entry", 64);
  const auto Value = Ctx.mkAnd(Root, Ctx.mkConst(64, uint64_t{0} - 8));
  const auto Predicate = residue(Ctx, Root, 7, 3);
  SpecializationOptions Options;
  uint64_t Queries = 0;
  Options.MaxSolverQueries = 1;
  EXPECT_EQ(
      proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries).Status,
      FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Queries, 1u);
  Options.MaxSolverQueries = 3;
  EXPECT_EQ(
      proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries).Status,
      FrameOffsetStatus::Exact);
  EXPECT_EQ(Queries, 3u);
  Options.MaxSymbolicNodes = Ctx.numNodes() - 1;
  EXPECT_EQ(
      proveFrameOffset(Ctx, Predicate, Root, Root, Options, Queries).Status,
      FrameOffsetStatus::BudgetExceeded);
}

TEST(FrameOffsets, ExactIdentityIsSeparateFromReachability) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("entry", 64);
  const auto Value = Ctx.mkAdd(Root, Ctx.mkConst(64, 8));
  uint64_t Queries = 0;
  const auto UnknownPredicate = Ctx.mkVar("condition", 1);
  EXPECT_EQ(
      proveFrameOffset(Ctx, UnknownPredicate, Value, Root, {}, Queries).Status,
      FrameOffsetStatus::Exact);
  EXPECT_EQ(Queries, 0u);
  EXPECT_EQ(
      proveFrameOffset(Ctx, Ctx.mkFalse(), Value, Root, {}, Queries).Status,
      FrameOffsetStatus::Infeasible);
  const auto Contradiction = Ctx.mkAnd(Ctx.mkUlt(Root, Ctx.mkConst(64, 3)),
                                       Ctx.mkUlt(Ctx.mkConst(64, 5), Root));
  EXPECT_EQ(proveFrameOffset(Ctx, Contradiction,
                             Ctx.mkAnd(Root, Ctx.mkConst(64, -16)), Root, {},
                             Queries)
                .Status,
            FrameOffsetStatus::Infeasible);
  EXPECT_GT(Queries, 0u);
}

TEST(FrameOffsets, CacheRetainsCompletePredicateAndRootRelationships) {
  FiniteQueryCache Cache(65536);
  SymContext Ctx;
  const auto Root = Ctx.mkVar("entry", 64);
  const auto Value = Ctx.mkAnd(Root, Ctx.mkConst(64, uint64_t{0} - 16));
  uint64_t Queries = 0;
  const auto Prove = [&](uint64_t Low) {
    return proveFrameOffset(Ctx, residue(Ctx, Root, 15, Low), Value, Root, {},
                            Queries, &Cache);
  };
  ASSERT_EQ(Prove(5).Status, FrameOffsetStatus::Exact);
  const auto Before = Queries;
  EXPECT_EQ(Prove(5).Offset, uint64_t{0} - 5);
  EXPECT_EQ(Queries, Before);
  EXPECT_EQ(Prove(6).Offset, uint64_t{0} - 6);
  EXPECT_GT(Queries, Before);
  EXPECT_EQ(
      proveFrameOffset(Ctx, Ctx.mkTrue(), Value, Root, {}, Queries, &Cache)
          .Status,
      FrameOffsetStatus::NonUnique);
}

TEST(FrameOffsets, CanonicalAccessesShareBytesAndRetainAliasInvalidation) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkVar("entry", 64);
  const auto Pointer =
      Ctx.mkAnd(Ctx.mkAdd(Root, Ctx.mkConst(64, -32)), Ctx.mkConst(64, -16));
  uint64_t Queries = 0;
  const auto Offset = proveFrameOffset(Ctx, residue(Ctx, Root, 15, 9), Pointer,
                                       Root, {}, Queries);
  ASSERT_EQ(Offset.Status, FrameOffsetStatus::Exact);
  const auto Canonical = Ctx.mkAdd(Root, Ctx.mkConst(64, Offset.Offset));
  State.store(Canonical, Root);
  EXPECT_EQ(State.load(Ctx.mkAdd(Root, Ctx.mkConst(64, -41)), 8), Root);
  State.store(Ctx.mkAdd(Canonical, Ctx.mkConst(64, 1)), Ctx.mkConst(8, 7));
  EXPECT_NE(State.load(Canonical, 8), Root);
  State.store(Ctx.mkVar("possible_alias", 64), Ctx.mkConst(8, 2));
  EXPECT_FALSE(Ctx.asConst(State.load(Canonical, 8)));
}

TEST(FrameOffsets, InvalidShapesAreRejectedBeforeSymbolicOperations) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("entry", 64);
  uint64_t Queries = 0;
  EXPECT_EQ(proveFrameOffset(Ctx, {}, Root, Root, {}, Queries).Status,
            FrameOffsetStatus::Invalid);
  EXPECT_EQ(proveFrameOffset(Ctx, Root, Root, Root, {}, Queries).Status,
            FrameOffsetStatus::Invalid);
  EXPECT_EQ(proveFrameOffset(Ctx, Ctx.mkTrue(), {}, Root, {}, Queries).Status,
            FrameOffsetStatus::Invalid);
  EXPECT_EQ(
      proveFrameOffset(Ctx, Ctx.mkTrue(), Ctx.mkConst(32, 0), Root, {}, Queries)
          .Status,
      FrameOffsetStatus::Invalid);
  EXPECT_EQ(Queries, 0u);
}

TEST(FrameOffsets, SymbolicAddendsPreserveAlignmentAndWholeIndex) {
  for (unsigned Width : {8U, 64U})
    for (unsigned Split : {0U, 8U, 32U})
      for (bool IndexFirst : {false, true})
        for (uint64_t Bias : {uint64_t{37}, uint64_t{0} - 101}) {
          SCOPED_TRACE(Width);
          SCOPED_TRACE(Split);
          SCOPED_TRACE(IndexFirst);
          SCOPED_TRACE(Bias);
          SymContext Ctx;
          SymRef Root, Index;
          if (IndexFirst) {
            Index = Ctx.mkVar("index", Width);
            Root = Ctx.mkVar("stack", 64);
          } else {
            Root = Ctx.mkVar("stack", 64);
            Index = Ctx.mkVar("index", Width);
          }
          const auto Adjusted = Ctx.mkAdd(Root, Ctx.mkConst(64, Bias));
          const auto Aligned =
              Split ? Ctx.mkConcat(
                          Ctx.mkExtract(Adjusted, Split, 64 - Split),
                          Ctx.mkAnd(Ctx.mkExtract(Adjusted, 0, Split),
                                    Ctx.mkConst(Split, uint64_t{0} - 16)))
                    : Ctx.mkAnd(Adjusted, Ctx.mkConst(64, uint64_t{0} - 16));
          const auto Value = Ctx.mkAdd(Aligned, Ctx.mkZExtOrTrunc(Index, 64));
          SpecializationOptions Options;
          Options.MaxSolverGates = 512;
          for (uint64_t Low : {0U, 9U, 15U}) {
            const uint64_t Number = Width == 8 ? 253 : UINT64_MAX - 6;
            const auto Predicate =
                Ctx.mkAnd(residue(Ctx, Root, 15, Low),
                          Ctx.mkEq(Index, Ctx.mkConst(Width, Number)));
            uint64_t Queries = 0;
            const auto Good =
                proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries);
            ASSERT_EQ(Good.Status, FrameOffsetStatus::Exact);
            EXPECT_EQ(Good.Offset, Bias + Number - ((Low + Bias) & 15));
            EXPECT_EQ(Queries, 2U);
          }
        }
}

TEST(FrameOffsets, SymbolicAddendsDoNotDiscardConstraintsOrRootDependence) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("stack", 64);
  const auto Other = Ctx.mkVar("other", 64);
  const auto Index = Ctx.mkVar("index", 8);
  const auto Aligned = Ctx.mkAnd(Root, Ctx.mkConst(64, -16));
  const auto Predicate = residue(Ctx, Root, 15, 8);
  for (const auto Addend : {Root, Ctx.mkZExt(Ctx.mkExtract(Root, 0, 8), 64),
                            Ctx.mkZExt(Index, 64)}) {
    uint64_t Queries = 0;
    EXPECT_EQ(proveFrameOffset(Ctx, Predicate, Ctx.mkAdd(Aligned, Addend), Root,
                               {}, Queries)
                  .Status,
              FrameOffsetStatus::NonUnique);
    EXPECT_EQ(Queries, 2U);
  }
  const auto Fixed = Ctx.mkAnd(Predicate, Ctx.mkEq(Index, Ctx.mkConst(8, 17)));
  const auto WrongRoot =
      Ctx.mkAdd(Ctx.mkAnd(Other, Ctx.mkConst(64, -16)), Ctx.mkZExt(Index, 64));
  uint64_t Queries = 0;
  EXPECT_EQ(proveFrameOffset(Ctx, Fixed, WrongRoot, Root, {}, Queries).Status,
            FrameOffsetStatus::NonUnique);
  const auto Guard = Ctx.mkVar("unrelated_guard", 8);
  const auto Contradiction = Ctx.mkAnd(Ctx.mkUlt(Guard, Ctx.mkConst(8, 3)),
                                       Ctx.mkUlt(Ctx.mkConst(8, 5), Guard));
  EXPECT_EQ(proveFrameOffset(Ctx, Ctx.mkAnd(Fixed, Contradiction),
                             Ctx.mkAdd(Aligned, Ctx.mkZExt(Index, 64)), Root,
                             {}, Queries)
                .Status,
            FrameOffsetStatus::Infeasible);
}

TEST(FrameOffsets, SymbolicAddendProofsStillRequireCompleteBudgets) {
  for (unsigned Case = 0; Case != 4; ++Case) {
    SymContext Ctx;
    const auto Root = Ctx.mkVar("stack", 64), Index = Ctx.mkVar("index", 8);
    const auto Value =
        Ctx.mkAdd(Ctx.mkAnd(Root, Ctx.mkConst(64, -16)), Ctx.mkZExt(Index, 64));
    const auto Predicate = Ctx.mkAnd(residue(Ctx, Root, 15, 8),
                                     Ctx.mkEq(Index, Ctx.mkConst(8, 17)));
    SpecializationOptions Options;
    Options.MaxSolverGates = Case == 0 ? 1 : 512;
    Options.MaxSymbolicNodes = Case == 1 ? Ctx.numNodes() : 65536;
    Options.MaxSolverQueries = Case == 2 ? 1 : 2;
    uint64_t Queries = 0;
    const auto Result =
        proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries);
    EXPECT_EQ(Result.Status, Case == 3 ? FrameOffsetStatus::Exact
                                       : FrameOffsetStatus::BudgetExceeded);
    EXPECT_EQ(Queries, Case < 2 ? 0U : Case == 2 ? 1U : 2U);
    if (Case == 3)
      EXPECT_EQ(Result.Offset, 9U);
  }
}

TEST(FrameOffsets, DomainEncodingReuseKeepsFullFrameProofsAndBudgets) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("entry", 64);
  const auto Pointer = Ctx.mkVar("pointer", 64);
  neverd::solver::SolverOptions Settings;
  FiniteDomainEncoding Encoding(Ctx, Settings);
  for (unsigned Offset : {7U, 7U, 19U, 7U}) {
    const auto Predicate =
        Ctx.mkEq(Pointer, Ctx.mkAdd(Root, Ctx.mkConst(64, Offset)));
    for (unsigned Bias = 0; Bias != 8; ++Bias) {
      const auto Value = Ctx.mkAdd(Pointer, Ctx.mkConst(64, Bias));
      uint64_t FreshQueries = 0, CopyQueries = 0;
      const auto Fresh = proveFrameOffset(Ctx, Predicate, Value, Root, Settings,
                                          2, 100000, FreshQueries);
      const auto Copy = proveFrameOffset(Encoding, Predicate, Value, Root, 2,
                                         100000, CopyQueries);
      EXPECT_EQ(Copy.Status, FrameOffsetStatus::Exact);
      EXPECT_EQ(Copy.Status, Fresh.Status);
      EXPECT_EQ(Copy.Offset, Offset + Bias);
      EXPECT_EQ(Copy.Offset, Fresh.Offset);
      EXPECT_EQ(CopyQueries, FreshQueries);
      EXPECT_EQ(CopyQueries, 2U);
      uint64_t LimitedQueries = 0;
      EXPECT_EQ(proveFrameOffset(Encoding, Predicate, Value, Root, 1, 100000,
                                 LimitedQueries)
                    .Status,
                FrameOffsetStatus::BudgetExceeded);
      EXPECT_EQ(LimitedQueries, 1U);
    }
  }
  uint64_t Queries = 0;
  EXPECT_EQ(proveFrameOffset(Encoding, Ctx.mkTrue(), Pointer, Root, 3, 100000,
                             Queries)
                .Status,
            FrameOffsetStatus::NonUnique);
  Queries = 0;
  EXPECT_EQ(proveFrameOffset(Encoding, Ctx.mkFalse(), Pointer, Root, 2, 100000,
                             Queries)
                .Status,
            FrameOffsetStatus::Infeasible);
}

TEST(FrameOffsets, FrameAndJointTargetProjectionsKeepIndependentSearches) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("stack", 64);
  const auto X = Ctx.mkVar("x", 4), Y = Ctx.mkVar("y", 4);
  neverd::solver::SolverOptions Settings;
  Settings.BuildModel = false;
  FiniteDomainEncoding Encoding(Ctx, Settings);
  for (unsigned Low : {8U, 24U, 8U}) {
    const auto Predicate = Ctx.mkAnd(
        Ctx.mkEq(Ctx.mkAnd(Root, Ctx.mkConst(64, 127)), Ctx.mkConst(64, Low)),
        Ctx.mkEq(Ctx.mkAdd(X, Y), Ctx.mkConst(4, Low == 24 ? 3 : 7)));
    for (unsigned Round = 0; Round != 4; ++Round) {
      const auto Address =
          Ctx.mkAdd(Ctx.mkAnd(Ctx.mkAdd(Root, Ctx.mkConst(64, Round + 1)),
                              Ctx.mkConst(64, uint64_t{0} - 128)),
                    Ctx.mkConst(64, 11));
      uint64_t FrameQueries = 0, FreshFrameQueries = 0;
      const auto Frame = proveFrameOffset(Encoding, Predicate, Address, Root, 2,
                                          100000, FrameQueries);
      const auto FreshFrame =
          proveFrameOffset(Ctx, Predicate, Address, Root, Settings, 2, 100000,
                           FreshFrameQueries);
      ASSERT_EQ(Frame.Status, FrameOffsetStatus::Exact);
      EXPECT_EQ(Frame.Status, FreshFrame.Status);
      EXPECT_EQ(Frame.Offset, uint64_t{11} - Low);
      EXPECT_EQ(Frame.Offset, FreshFrame.Offset);
      EXPECT_EQ(FrameQueries, FreshFrameQueries);
      EXPECT_EQ(FrameQueries, 2U);

      const auto Target = Ctx.mkAdd(Ctx.mkZExt(X, 64), Ctx.mkConst(64, 0x7000));
      uint64_t Queries = 0, FreshQueries = 0;
      std::vector<std::vector<uint64_t>> Seen, FreshSeen;
      const auto MaxQueries = Round == 1 ? 16U : 17U;
      const auto MaxValues = Round == 2 ? 15U : 16U;
      const auto Result = enumerateFiniteValues(
          Encoding, Predicate, {Target, Y}, MaxValues, MaxQueries, 100000,
          Queries, [&](llvm::ArrayRef<uint64_t> Tuple) {
            Seen.emplace_back(Tuple.begin(), Tuple.end());
            return true;
          });
      const auto Fresh = enumerateFiniteValues(
          Ctx, Predicate, {Target, Y}, MaxValues, Settings, MaxQueries, 100000,
          FreshQueries, [&](llvm::ArrayRef<uint64_t> Tuple) {
            FreshSeen.emplace_back(Tuple.begin(), Tuple.end());
            return true;
          });
      EXPECT_EQ(Result.Status, Fresh.Status);
      EXPECT_EQ(Result.Tuples, Fresh.Tuples);
      EXPECT_EQ(Queries, FreshQueries);
      EXPECT_EQ(Seen, FreshSeen);
      if (Round == 1 || Round == 2) {
        EXPECT_EQ(Result.Status, Round == 1
                                     ? FiniteValueStatus::QueryBudgetExceeded
                                     : FiniteValueStatus::TooManyValues);
        EXPECT_TRUE(Result.Tuples.empty());
        EXPECT_EQ(Queries, 16U);
      } else {
        ASSERT_EQ(Result.Status, FiniteValueStatus::Complete);
        ASSERT_EQ(Result.Tuples.size(), 16U);
        EXPECT_EQ(Queries, 17U);
        for (unsigned I = 0; I != 16; ++I) {
          EXPECT_EQ(Result.Tuples[I][0], 0x7000 + I);
          EXPECT_EQ(Result.Tuples[I][1], ((Low == 24 ? 3U : 7U) - I) & 15);
        }
      }
    }
  }
}

TEST(FrameOffsets, CachedAddressTranslationsPreserveUnsignedWrap) {
  for (bool Bound : {false, true}) {
    SymContext Ctx;
    const auto Root = Ctx.mkVar("root", 64);
    const auto Low = Ctx.mkAnd(Root, Ctx.mkConst(64, 15));
    const auto Predicate = Ctx.mkEq(Low, Ctx.mkConst(64, 8));
    const auto Base = Ctx.mkAnd(Ctx.mkAdd(Root, Ctx.mkConst(64, 32)),
                                Ctx.mkConst(64, ~uint64_t(15)));
    auto Cache = Bound ? std::make_unique<FiniteQueryCache>(100000, Ctx)
                       : std::make_unique<FiniteQueryCache>(100000);
    FiniteDomainEncoding Encoding(Ctx, {});
    bool First = true;
    for (uint64_t Bias : {uint64_t(0), uint64_t(7), ~uint64_t(0),
                          uint64_t(1) << 63, uint64_t(16)}) {
      uint64_t Queries = 0;
      auto Value = Ctx.mkAdd(Base, Ctx.mkConst(64, Bias));
      const auto Result = proveFrameOffset(Encoding, Predicate, Value, Root,
                                           100, 100000, Queries, Cache.get());
      ASSERT_EQ(Result.Status, FrameOffsetStatus::Exact);
      EXPECT_EQ(Result.Offset, uint64_t(24) + Bias);
      if (First)
        EXPECT_EQ(Queries, 2U);
      else
        EXPECT_EQ(Queries, 0U);
      First = false;
    }
  }
}

TEST(FrameOffsets, CachedNonUniqueAndEmptyDomainsRemainDistinct) {
  for (bool Empty : {false, true}) {
    SymContext Ctx;
    auto Root = Ctx.mkVar("root", 64), Input = Ctx.mkVar("x", 8);
    auto Base = Ctx.mkAnd(Ctx.mkAdd(Root, Ctx.mkConst(64, 32)),
                          Ctx.mkConst(64, ~uint64_t(15)));
    auto Predicate =
        Empty ? Ctx.mkEq(Ctx.mkMul(Input, Input), Ctx.mkConst(8, 2))
              : Ctx.mkEq(Ctx.mkAnd(Root, Ctx.mkConst(64, 16)), Ctx.mkZero(64));
    FiniteQueryCache Cache(100000, Ctx);
    FiniteDomainEncoding Encoding(Ctx, {});
    for (unsigned I = 0; I != 3; ++I) {
      uint64_t Queries = 0;
      auto Result = proveFrameOffset(Encoding, Predicate,
                                     Ctx.mkAdd(Base, Ctx.mkConst(64, I)), Root,
                                     100, 100000, Queries, &Cache);
      ASSERT_EQ(Result.Status, Empty ? FrameOffsetStatus::Infeasible
                                     : FrameOffsetStatus::NonUnique);
      if (I)
        EXPECT_EQ(Queries, 0U);
      else
        EXPECT_EQ(Queries, Empty ? 1U : 2U);
    }
  }
}

TEST(FrameOffsets, CachedTranslationsSeparatePredicatesAndCapacity) {
  for (bool ZeroCapacity : {false, true}) {
    SymContext Ctx;
    auto Root = Ctx.mkVar("root", 64);
    auto Base = Ctx.mkAnd(Ctx.mkAdd(Root, Ctx.mkConst(64, 32)),
                          Ctx.mkConst(64, ~uint64_t(15)));
    FiniteQueryCache Cache(ZeroCapacity ? 0 : 100000, Ctx);
    FiniteDomainEncoding Encoding(Ctx, {});
    for (uint64_t Low : {uint64_t(8), uint64_t(9), uint64_t(8)}) {
      auto Predicate =
          Ctx.mkEq(Ctx.mkAnd(Root, Ctx.mkConst(64, 15)), Ctx.mkConst(64, Low));
      uint64_t Queries = 0;
      auto Result = proveFrameOffset(Encoding, Predicate,
                                     Ctx.mkAdd(Base, Ctx.mkConst(64, 3)), Root,
                                     100, 100000, Queries, &Cache);
      ASSERT_EQ(Result.Status, FrameOffsetStatus::Exact);
      EXPECT_EQ(Result.Offset, 35 - Low);
      if (ZeroCapacity || Low == 9)
        EXPECT_EQ(Queries, 2U);
    }
  }
}

TEST(FrameOffsets, CachedTranslationsRejectIncompleteProofs) {
  SymContext Ctx;
  auto Root = Ctx.mkVar("root", 64);
  auto Predicate =
      Ctx.mkEq(Ctx.mkAnd(Root, Ctx.mkConst(64, 15)), Ctx.mkConst(64, 8));
  auto Base = Ctx.mkAnd(Ctx.mkAdd(Root, Ctx.mkConst(64, 32)),
                        Ctx.mkConst(64, ~uint64_t(15)));
  FiniteQueryCache Cache(100000, Ctx);
  FiniteDomainEncoding Encoding(Ctx, {});
  uint64_t Queries = 0;
  EXPECT_EQ(proveFrameOffset(Encoding, Predicate, Base, Root, 1, 100000,
                             Queries, &Cache)
                .Status,
            FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Queries, 1U);
  Queries = 0;
  auto Value = Ctx.mkAdd(Base, Ctx.mkConst(64, 7));
  EXPECT_EQ(proveFrameOffset(Encoding, Predicate, Value, Root, 2,
                             Ctx.numNodes() - 1, Queries, &Cache)
                .Status,
            FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Queries, 0U);
  Queries = 0;
  auto Result = proveFrameOffset(Encoding, Predicate, Value, Root, 2, 100000,
                                 Queries, &Cache);
  ASSERT_EQ(Result.Status, FrameOffsetStatus::Exact);
  EXPECT_EQ(Result.Offset, 31U);
  EXPECT_EQ(Queries, 2U);
  Queries = 0;
  Result =
      proveFrameOffset(Encoding, Predicate, Ctx.mkAdd(Base, Ctx.mkConst(64, 1)),
                       Root, 0, 100000, Queries, &Cache);
  EXPECT_EQ(Result.Status, FrameOffsetStatus::Exact);
  EXPECT_EQ(Result.Offset, 25U);
  EXPECT_EQ(Queries, 0U);
}

TEST(FrameOffsets, CachedNaryAddressesKeepProofIdentity) {
  SymContext Ctx;
  auto Root = Ctx.mkVar("root", 64), Index = Ctx.mkVar("index", 64);
  auto Aligned = Ctx.mkAnd(Ctx.mkAdd(Root, Ctx.mkConst(64, 32)),
                           Ctx.mkConst(64, ~uint64_t(15)));
  auto Address = Ctx.mkAdd(Aligned, Index);
  auto Predicate = Ctx.mkAnd(
      Ctx.mkEq(Ctx.mkAnd(Root, Ctx.mkConst(64, 15)), Ctx.mkConst(64, 8)),
      Ctx.mkEq(Index, Ctx.mkConst(64, 5)));
  FiniteQueryCache Cache(100000, Ctx);
  FiniteDomainEncoding Encoding(Ctx, {});
  uint64_t Queries = 0;
  auto Result = proveFrameOffset(Encoding, Predicate, Address, Root, 2, 100000,
                                 Queries, &Cache);
  ASSERT_EQ(Result.Status, FrameOffsetStatus::Exact);
  EXPECT_EQ(Result.Offset, 29U);
  EXPECT_EQ(Queries, 2U);
  for (uint64_t Bias : {uint64_t(7), ~uint64_t(0), uint64_t(1) << 63}) {
    Queries = 0;
    auto Shifted = Ctx.mkAdd(Address, Ctx.mkConst(64, Bias));
    Result = proveFrameOffset(Encoding, Predicate, Shifted, Root, 0, 100000,
                              Queries, &Cache);
    ASSERT_EQ(Result.Status, FrameOffsetStatus::Exact);
    EXPECT_EQ(Result.Offset, uint64_t(29) + Bias);
    EXPECT_EQ(Queries, 0U);
  }
}

// Independently authored modular identity: XOR carries plus the doubled AND
// are addition. The unrelated input must cancel before a frame fact is usable.
SymRef mbaFrameAddress(SymContext &Ctx, SymRef Root, SymRef Input,
                       uint64_t Bias) {
  const auto Sum =
      Ctx.mkAdd(Ctx.mkXor(Root, Input),
                Ctx.mkMul(Ctx.mkConst(64, 2), Ctx.mkAnd(Root, Input)));
  return Ctx.mkAdd(Ctx.mkSub(Sum, Input), Ctx.mkConst(64, Bias));
}

TEST(FrameOffsets, ExactNormalizationIsOptInAndKeepsModularRootIdentity) {
  for (bool ReuseEncoding : {false, true})
    for (uint64_t Bias :
         {uint64_t{0}, uint64_t{24}, uint64_t{0} - 40, UINT64_MAX}) {
      SCOPED_TRACE(ReuseEncoding);
      SCOPED_TRACE(Bias);
      SymContext Ctx;
      const auto Root = Ctx.mkVar("frame", 64);
      const auto Input = Ctx.mkVar("data", 64);
      const auto Value = mbaFrameAddress(Ctx, Root, Input, Bias);
      const auto Predicate = Ctx.mkTrue();
      SpecializationOptions Options;
      Options.MaxSolverConflicts = 1;
      uint64_t Queries = 0;
      EXPECT_EQ(proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries)
                    .Status,
                FrameOffsetStatus::BudgetExceeded);
      FrameOffsetNormalization Work{0, 99};
      Queries = 0;
      EXPECT_EQ(proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries,
                                 nullptr, &Work)
                    .Status,
                FrameOffsetStatus::BudgetExceeded);
      EXPECT_EQ(Work.WorkUsed, 0u);
      Work.MaxWork = 65536;
      Queries = 0;
      neverd::solver::SolverOptions Settings;
      Settings.Sat.MaxConflicts = Options.MaxSolverConflicts;
      FiniteDomainEncoding Encoding(Ctx, Settings);
      const auto R = ReuseEncoding
                         ? proveFrameOffset(Encoding, Predicate, Value, Root,
                                            100, 65536, Queries, nullptr, &Work)
                         : proveFrameOffset(Ctx, Predicate, Value, Root,
                                            Options, Queries, nullptr, &Work);
      ASSERT_EQ(R.Status, FrameOffsetStatus::Exact);
      EXPECT_EQ(R.Offset, Bias);
      EXPECT_GT(Queries, 0u); // The failed cold proof is still charged.
      EXPECT_GT(Work.WorkUsed, 0u);
      EXPECT_LE(Work.WorkUsed, Work.MaxWork);
      for (uint64_t X : {uint64_t{0}, uint64_t{1}, uint64_t{0x123456789abcdef0},
                         uint64_t{1} << 63, UINT64_MAX})
        for (uint64_t Y : {uint64_t{0}, uint64_t{7}, UINT64_MAX}) {
          const uint64_t Expected = (X ^ Y) + 2 * (X & Y) - Y + Bias;
          EXPECT_EQ(Expected, X + R.Offset);
          EXPECT_EQ(Ctx.evalU64(Value, {X, Y}), Expected);
        }
    }
}

TEST(FrameOffsets, NormalizationRetryPreservesPredicateAndFiniteDomains) {
  for (bool ReuseEncoding : {false, true})
    for (unsigned Case = 0; Case != 4; ++Case) {
      SCOPED_TRACE(ReuseEncoding);
      SCOPED_TRACE(Case);
      SymContext Ctx;
      const auto Root = Ctx.mkVar("frame", 64);
      const auto Input = Ctx.mkVar("data", 64);
      const auto Byte = Ctx.mkVar("admission", 8);
      const auto ProvedRoot = Case == 3 ? Ctx.mkVar("independent", 64) : Root;
      const auto Aligned =
          Ctx.mkAnd(mbaFrameAddress(Ctx, Root, Input, uint64_t{0} - 73),
                    Ctx.mkConst(64, -16));
      const auto Value = Ctx.mkAdd(Aligned, Ctx.mkConst(64, 11));
      const auto Predicate =
          Case == 0 || Case == 3 ? residue(Ctx, Root, 15, 6)
          : Case == 1 ? Ctx.mkTrue()
                      : Ctx.mkAnd(Ctx.mkUlt(Byte, Ctx.mkConst(8, 3)),
                                  Ctx.mkUlt(Ctx.mkConst(8, 5), Byte));
      // The address has a constant bias. Check the actual unbiased cache key,
      // not Value - Root, which would miss a wrongly cached result.
      const auto CacheValue = Ctx.mkSub(Aligned, ProvedRoot);
      FiniteQueryCache Cache(65536, Ctx);
      neverd::solver::SolverOptions Settings;
      Settings.Blast.MaxGates = Case == 3 ? 1024 : 256;
      FiniteDomainEncoding Encoding(Ctx, Settings);
      uint64_t Queries = 0;
      const auto Prove = [&](FrameOffsetNormalization *Work,
                             uint64_t Limit = 100) {
        return ReuseEncoding
                   ? proveFrameOffset(Encoding, Predicate, Value, ProvedRoot,
                                      Limit, 65536, Queries, &Cache, Work)
                   : proveFrameOffset(Ctx, Predicate, Value, ProvedRoot,
                                      Settings, Limit, 65536, Queries, &Cache,
                                      Work);
      };
      ASSERT_EQ(Prove(nullptr).Status, FrameOffsetStatus::BudgetExceeded);
      EXPECT_EQ(Queries, 0u); // Cold encoding cannot represent the carry sum.
      FrameOffsetNormalization Work{65536};
      const auto Result = Prove(&Work);
      EXPECT_EQ(Result.Status, Case == 0   ? FrameOffsetStatus::Exact
                               : Case == 2 ? FrameOffsetStatus::Infeasible
                                           : FrameOffsetStatus::NonUnique);
      EXPECT_EQ(Queries, Case == 2 ? 1u : 2u);
      EXPECT_GT(Work.WorkUsed, 0u);
      EXPECT_LE(Work.WorkUsed, Work.MaxWork);
      const auto Cached = Cache.lookup(Ctx, Predicate, {CacheValue}, 1);
      ASSERT_TRUE(Cached);
      EXPECT_EQ(Cached->Status, Case == 0 || Case == 2
                                    ? FiniteValueStatus::Complete
                                    : FiniteValueStatus::TooManyValues);
      if (Case == 0)
        EXPECT_EQ(Cached->Tuples,
                  (std::vector<std::vector<uint64_t>>{{uint64_t{0} - 86}}));
      else
        EXPECT_TRUE(Cached->Tuples.empty());
      const auto Before = Queries;
      EXPECT_EQ(Prove(nullptr, Before).Status, Result.Status);
      EXPECT_EQ(Queries, Before);
      if (Case == 0) {
        EXPECT_EQ(Result.Offset, uint64_t{0} - 75);
        for (uint64_t X :
             {uint64_t{6}, uint64_t{0x123456789abcdef6}, UINT64_MAX - 9})
          for (uint64_t Y : {uint64_t{0}, uint64_t{37}, UINT64_MAX}) {
            const auto Expected =
                (((X ^ Y) + 2 * (X & Y) - Y - 73) & ~uint64_t{15}) + 11;
            EXPECT_EQ(Expected, X + Result.Offset);
            EXPECT_EQ(Ctx.evalU64(Value, {X, Y, 0}), Expected);
          }
      }
    }
}

TEST(FrameOffsets, NormalizationRetryKeepsCumulativeQueriesAndNodeLimit) {
  for (bool ReuseEncoding : {false, true}) {
    const auto Run = [&](uint64_t Allowance, uint64_t NodeLimit) {
      SCOPED_TRACE(ReuseEncoding);
      SCOPED_TRACE(Allowance);
      SymContext Ctx;
      const auto Root = Ctx.mkVar("frame", 64);
      const auto Input = Ctx.mkVar("data", 64);
      const auto Value = Ctx.mkAdd(
          Ctx.mkAnd(mbaFrameAddress(Ctx, Root, Input, uint64_t{0} - 73),
                    Ctx.mkConst(64, -16)),
          Ctx.mkConst(64, 11));
      const auto Predicate = residue(Ctx, Root, 15, 6);
      neverd::solver::SolverOptions Settings;
      Settings.Blast.MaxGates = 1024;
      const SpecializationOptions Defaults;
      Settings.Sat.MaxConflicts = Defaults.MaxSolverConflicts;
      Settings.Sat.MaxPropagations = Defaults.MaxSolverPropagations;
      Settings.Sat.MaxWatchVisits = Defaults.MaxSolverWatchVisits;
      FiniteDomainEncoding Encoding(Ctx, Settings);
      uint64_t Queries = 7;
      FrameOffsetNormalization Work{65536};
      const auto Result =
          ReuseEncoding ? proveFrameOffset(Encoding, Predicate, Value, Root,
                                           7 + Allowance, NodeLimit, Queries,
                                           nullptr, &Work)
                        : proveFrameOffset(Ctx, Predicate, Value, Root,
                                           Settings, 7 + Allowance, NodeLimit,
                                           Queries, nullptr, &Work);
      EXPECT_GE(Queries, 7u);
      EXPECT_LE(Queries, 7 + Allowance);
      EXPECT_GT(Work.WorkUsed, 0u);
      EXPECT_LE(Work.WorkUsed, Work.MaxWork);
      return std::tuple{Result, Queries, Ctx.numNodes()};
    };
    const auto [Good, Queries, Nodes] = Run(4, 65536);
    ASSERT_EQ(Good.Status, FrameOffsetStatus::Exact);
    EXPECT_EQ(Good.Offset, uint64_t{0} - 75);
    EXPECT_EQ(Queries, 11u); // Two cold checks plus two normalized checks.
    for (uint64_t Allowance = 0; Allowance != 4; ++Allowance) {
      const auto [Short, Used, Count] = Run(Allowance, 65536);
      EXPECT_EQ(Short.Status, FrameOffsetStatus::BudgetExceeded);
      EXPECT_EQ(Used, 7 + Allowance);
    }
    const auto [Short, Used, Count] = Run(4, Nodes - 1);
    EXPECT_EQ(Short.Status, FrameOffsetStatus::BudgetExceeded);
    EXPECT_GT(Count, Nodes - 1);
  }
}

TEST(FrameOffsets, NormalizationDoesNotEraseNonAffineOrIndependentInputs) {
  for (unsigned Case = 0; Case != 4; ++Case) {
    SCOPED_TRACE(Case);
    SymContext Ctx;
    const auto Root = Ctx.mkVar("frame", 64);
    const auto Input = Ctx.mkVar("data", 64);
    auto Value = mbaFrameAddress(Ctx, Root, Input, uint64_t{0} - 40);
    if (Case == 0)
      Value = Ctx.mkAdd(Value, Ctx.mkAnd(Input, Ctx.mkConst(64, 1)));
    if (Case == 1)
      Value = Ctx.mkAdd(Value, Ctx.mkAnd(Root, Input)); // Wrong carry weight.
    if (Case == 2)
      Value = mbaFrameAddress(Ctx, Input, Root, 24); // Different physical root.
    if (Case == 3)
      Value = Ctx.mkMul(Value, Input);
    SpecializationOptions Options;
    Options.MaxSolverQueries = 0;
    FrameOffsetNormalization Work{16384};
    uint64_t Queries = 0;
    const auto R = proveFrameOffset(Ctx, Ctx.mkTrue(), Value, Root, Options,
                                    Queries, nullptr, &Work);
    EXPECT_EQ(R.Status, FrameOffsetStatus::BudgetExceeded);
    EXPECT_EQ(Queries, 0u);
    EXPECT_GT(Work.WorkUsed, 0u);
    EXPECT_LE(Work.WorkUsed, Work.MaxWork);
  }
}

TEST(FrameOffsets, NormalizationTheoremsNeverPopulateFiniteDomains) {
  for (bool Impossible : {false, true}) {
    SCOPED_TRACE(Impossible);
    SymContext Ctx;
    const auto Root = Ctx.mkVar("frame", 64);
    const auto Input = Ctx.mkVar("data", 64);
    const auto Byte = Ctx.mkVar("byte", 8);
    const auto Value = mbaFrameAddress(Ctx, Root, Input, 0);
    const auto Predicate =
        Impossible ? Ctx.mkEq(Ctx.mkMul(Byte, Byte), Ctx.mkConst(8, 2))
                   : Ctx.mkTrue();
    // No square is 2 modulo 4. The one-gate cold query cannot encode this.
    const auto Difference = Ctx.mkSub(Value, Root);
    FiniteQueryCache Cache(65536, Ctx);
    SpecializationOptions Options;
    Options.MaxSolverGates = 1;
    FrameOffsetNormalization Work{65536};
    uint64_t Queries = 0;
    EXPECT_EQ(
        proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries, &Cache)
            .Status,
        FrameOffsetStatus::BudgetExceeded);
    EXPECT_EQ(Queries, 0u); // Encoding refused before a SAT request.
    for (unsigned Retry = 0; Retry != 2; ++Retry) {
      const auto Before = Queries;
      const auto R = proveFrameOffset(Ctx, Predicate, Value, Root, Options,
                                      Queries, &Cache, &Work);
      ASSERT_EQ(R.Status, FrameOffsetStatus::Exact);
      EXPECT_EQ(R.Offset, 0u);
      EXPECT_EQ(Queries, Before);
      EXPECT_GT(Work.WorkUsed, 0u);
      EXPECT_FALSE(Cache.lookup(Ctx, Predicate, {Difference}, 1));
    }
    if (Impossible) {
      const auto Domain = enumerateFiniteValues(Ctx, Predicate, {Difference}, 1,
                                                {}, 100, 65536, Queries);
      ASSERT_EQ(Domain.Status, FiniteValueStatus::Complete);
      ASSERT_TRUE(Domain.Tuples.empty());
      Cache.store(Ctx, Predicate, {Difference}, 1, Domain);
      const auto Before = Queries;
      EXPECT_EQ(proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries,
                                 &Cache, &Work)
                    .Status,
                FrameOffsetStatus::Infeasible);
      EXPECT_EQ(Queries, Before);
      EXPECT_EQ(Work.WorkUsed, 0u);
    }
  }
}

TEST(FrameOffsets, NormalizationFeasibilityCachesExactTranslatedTheorems) {
  for (bool Bound : {false, true})
    for (bool ReuseEncoding : {false, true})
      for (bool Impossible : {false, true}) {
        SCOPED_TRACE(Bound);
        SCOPED_TRACE(ReuseEncoding);
        SCOPED_TRACE(Impossible);
        SymContext Ctx;
        const auto Root = Ctx.mkVar("frame", 64);
        const auto Input = Ctx.mkVar("data", 64);
        const auto Byte = Ctx.mkVar("byte", 8);
        const auto Base = mbaFrameAddress(Ctx, Root, Input, 0);
        const auto Predicate =
            Impossible ? Ctx.mkEq(Ctx.mkMul(Byte, Byte), Ctx.mkConst(8, 2))
                       : residue(Ctx, Root, 15, 8);
        const auto OtherPredicate = residue(Ctx, Root, 15, 7);
        const auto OtherRoot = Ctx.mkVar("other frame", 64);
        const auto Difference = Ctx.mkSub(Base, Root);
        auto Cache = Bound ? std::make_unique<FiniteQueryCache>(65536, Ctx)
                           : std::make_unique<FiniteQueryCache>(65536);
        uint64_t Queries = 0;
        const auto Feasibility = enumerateFiniteValues(Ctx, Predicate, {}, 1,
                                                       {}, 100, 65536, Queries);
        ASSERT_EQ(Feasibility.Status, FiniteValueStatus::Complete);
        ASSERT_EQ(Feasibility.Tuples.size(), Impossible ? 0u : 1u);
        Cache->store(Ctx, Predicate, {}, 1, Feasibility);
        const auto Before = Queries;
        neverd::solver::SolverOptions Settings;
        Settings.Blast.MaxGates = 1;
        FiniteDomainEncoding Encoding(Ctx, Settings);
        FrameOffsetNormalization Work{65536};
        const auto Prove = [&](SymRef P, SymRef V, SymRef R,
                               uint64_t MaxNodes) {
          return ReuseEncoding
                     ? proveFrameOffset(Encoding, P, V, R, Queries, MaxNodes,
                                        Queries, Cache.get(), &Work)
                     : proveFrameOffset(Ctx, P, V, R, Settings, Queries,
                                        MaxNodes, Queries, Cache.get(), &Work);
        };
        const auto Shifted = Ctx.mkAdd(Base, Ctx.mkConst(64, 37));
        const auto First = Prove(Predicate, Shifted, Root, 65536);
        ASSERT_EQ(First.Status, Impossible ? FrameOffsetStatus::Infeasible
                                           : FrameOffsetStatus::Exact);
        if (!Impossible)
          EXPECT_EQ(First.Offset, 37u);
        EXPECT_GT(Work.WorkUsed, 0u);
        EXPECT_EQ(Queries, Before);
        const auto Domain = Cache->lookup(Ctx, Predicate, {Difference}, 1);
        ASSERT_TRUE(Domain);
        ASSERT_EQ(Domain->Status, FiniteValueStatus::Complete);
        EXPECT_EQ(Domain->Tuples,
                  Impossible ? std::vector<std::vector<uint64_t>>{}
                             : std::vector<std::vector<uint64_t>>{{0}});

        // Warm queries consume neither normalization nor solver work, including
        // modulo-2^64 translations. The root's high bits remain unconstrained.
        Work.MaxWork = 0;
        for (uint64_t Bias : {uint64_t{0}, uint64_t{73}, uint64_t{0} - 91,
                              uint64_t{1} << 63, UINT64_MAX}) {
          const auto R = Prove(
              Predicate, Ctx.mkAdd(Base, Ctx.mkConst(64, Bias)), Root, 65536);
          ASSERT_EQ(R.Status, First.Status);
          if (!Impossible)
            EXPECT_EQ(R.Offset, Bias);
          EXPECT_EQ(Work.WorkUsed, 0u);
          EXPECT_EQ(Queries, Before);
        }
        EXPECT_EQ(Prove(OtherPredicate, Base, Root, 65536).Status,
                  FrameOffsetStatus::BudgetExceeded);
        EXPECT_EQ(Prove(Predicate, Base, OtherRoot, 65536).Status,
                  FrameOffsetStatus::BudgetExceeded);
        EXPECT_EQ(Prove(Predicate, Base, Root, Ctx.numNodes() - 1).Status,
                  FrameOffsetStatus::BudgetExceeded);
      }
}

TEST(FrameOffsets,
     NormalizationFeasibilityRejectsIncompleteAndUnrelatedProofs) {
  enum class Evidence {
    Absent,
    ShortQuery,
    Rejected,
    OtherPredicate,
    OtherLimit
  };
  for (bool Bound : {false, true})
    for (auto Kind :
         {Evidence::Absent, Evidence::ShortQuery, Evidence::Rejected,
          Evidence::OtherPredicate, Evidence::OtherLimit}) {
      SCOPED_TRACE(Bound);
      SCOPED_TRACE(static_cast<unsigned>(Kind));
      SymContext Ctx;
      const auto Root = Ctx.mkVar("frame", 64);
      const auto Input = Ctx.mkVar("data", 64);
      const auto Value = mbaFrameAddress(Ctx, Root, Input, 0);
      const auto Predicate = residue(Ctx, Root, 15, 8);
      const auto ProofPredicate = Kind == Evidence::OtherPredicate
                                      ? residue(Ctx, Root, 15, 7)
                                      : Predicate;
      const uint32_t Limit = Kind == Evidence::OtherLimit ? 2 : 1;
      const auto Difference = Ctx.mkSub(Value, Root);
      auto Cache = Bound ? std::make_unique<FiniteQueryCache>(65536, Ctx)
                         : std::make_unique<FiniteQueryCache>(65536);
      uint64_t Queries = 0;
      if (Kind != Evidence::Absent) {
        const auto Observe = [&](llvm::ArrayRef<uint64_t>) {
          return Kind != Evidence::Rejected;
        };
        const auto Domain = enumerateFiniteValues(
            Ctx, ProofPredicate, {}, Limit, {},
            Kind == Evidence::ShortQuery ? 1 : 100, 65536, Queries, Observe);
        EXPECT_EQ(Domain.Status, Kind == Evidence::ShortQuery
                                     ? FiniteValueStatus::QueryBudgetExceeded
                                 : Kind == Evidence::Rejected
                                     ? FiniteValueStatus::Unknown
                                     : FiniteValueStatus::Complete);
        Cache->store(Ctx, ProofPredicate, {}, Limit, Domain);
      }
      const auto Before = Queries;
      SpecializationOptions Options;
      Options.MaxSolverQueries = Queries;
      FrameOffsetNormalization Work{65536};
      for (unsigned Retry = 0; Retry != 2; ++Retry) {
        const auto R = proveFrameOffset(Ctx, Predicate, Value, Root, Options,
                                        Queries, Cache.get(), &Work);
        ASSERT_EQ(R.Status, FrameOffsetStatus::Exact);
        EXPECT_EQ(R.Offset, 0u);
        EXPECT_GT(Work.WorkUsed, 0u);
        EXPECT_EQ(Queries, Before);
        EXPECT_FALSE(Cache->lookup(Ctx, Predicate, {Difference}, 1));
      }
    }
}

TEST(FrameOffsets, NormalizationFeasibilityRetainsStorageAndWorkLimits) {
  // A bound empty-projection proof needs 19 words; its singleton frame-domain
  // proof needs 21. Rejected storage must not turn a theorem into a warm hit.
  const auto Run = [&](uint64_t Words, uint64_t MaxWork) {
    SymContext Ctx;
    const auto Root = Ctx.mkVar("frame", 64);
    const auto Input = Ctx.mkVar("data", 64);
    const auto Value = mbaFrameAddress(Ctx, Root, Input, 0);
    const auto Predicate = Ctx.mkTrue();
    const auto Difference = Ctx.mkSub(Value, Root);
    FiniteQueryCache Cache(Words, Ctx);
    uint64_t Queries = 0;
    const auto Domain =
        enumerateFiniteValues(Ctx, Predicate, {}, 1, {}, 100, 65536, Queries);
    EXPECT_EQ(Domain.Status, FiniteValueStatus::Complete);
    Cache.store(Ctx, Predicate, {}, 1, Domain);
    const auto Before = Queries;
    SpecializationOptions Options;
    Options.MaxSolverQueries = Queries;
    FrameOffsetNormalization Work{MaxWork};
    const auto R = proveFrameOffset(Ctx, Predicate, Value, Root, Options,
                                    Queries, &Cache, &Work);
    EXPECT_EQ(Queries, Before);
    EXPECT_LE(Work.WorkUsed, MaxWork);
    const bool Retained =
        Cache.lookup(Ctx, Predicate, {Difference}, 1).has_value();
    FrameOffsetNormalization Disabled{0, 99};
    const auto Warm = proveFrameOffset(Ctx, Predicate, Value, Root, Options,
                                       Queries, &Cache, &Disabled);
    EXPECT_EQ(Warm.Status, Retained ? FrameOffsetStatus::Exact
                                    : FrameOffsetStatus::BudgetExceeded);
    EXPECT_EQ(Disabled.WorkUsed, 0u);
    EXPECT_EQ(Queries, Before);
    return std::tuple{R.Status, Work.WorkUsed, Retained};
  };
  const auto [Status, Used, Retained] = Run(21, 65536);
  ASSERT_EQ(Status, FrameOffsetStatus::Exact);
  ASSERT_GT(Used, 0u);
  EXPECT_TRUE(Retained);
  const auto [ExactStatus, ExactWork, ExactRetained] = Run(21, Used);
  EXPECT_EQ(ExactStatus, Status);
  EXPECT_LE(ExactWork, Used);
  EXPECT_TRUE(ExactRetained);
  const auto [ShortStatus, ShortWork, ShortRetained] = Run(21, 1);
  EXPECT_EQ(ShortStatus, FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(ShortWork, 1u);
  EXPECT_FALSE(ShortRetained);
  for (uint64_t Words : {18, 19, 20}) {
    const auto [LimitedStatus, LimitedWork, LimitedRetained] =
        Run(Words, 65536);
    EXPECT_EQ(LimitedStatus, FrameOffsetStatus::Exact);
    EXPECT_GT(LimitedWork, 0u);
    EXPECT_FALSE(LimitedRetained);
  }
}

TEST(FrameOffsets, CompletedAndInvalidProofsSkipNormalization) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("root", 64);
  const auto Value = Ctx.mkAnd(Root, Ctx.mkConst(64, -16));
  const auto Predicate = residue(Ctx, Root, 15, 8);
  FiniteQueryCache Cache(65536, Ctx);
  uint64_t Queries = 0;
  ASSERT_EQ(
      proveFrameOffset(Ctx, Predicate, Value, Root, {}, Queries, &Cache).Status,
      FrameOffsetStatus::Exact);
  FrameOffsetNormalization Work{65536, 99};
  auto O = SpecializationOptions{};
  O.MaxSolverQueries = 0;
  const auto Before = Queries;
  EXPECT_EQ(
      proveFrameOffset(Ctx, Predicate, Value, Root, O, Queries, &Cache, &Work)
          .Offset,
      uint64_t{0} - 8);
  EXPECT_EQ(Queries, Before);
  EXPECT_EQ(Work.WorkUsed, 0u);
  for (auto P : {Ctx.mkFalse(), Root}) {
    const auto R =
        proveFrameOffset(Ctx, P, Value, Root, O, Queries, nullptr, &Work);
    EXPECT_EQ(R.Status, P == Root ? FrameOffsetStatus::Invalid
                                  : FrameOffsetStatus::Infeasible);
    EXPECT_EQ(Work.WorkUsed, 0u);
  }
  EXPECT_EQ(proveFrameOffset(Ctx, Ctx.mkTrue(), Value, Root, {}, Queries,
                             nullptr, &Work)
                .Status,
            FrameOffsetStatus::NonUnique);
  EXPECT_EQ(Work.WorkUsed, 0u);
}

TEST(FrameOffsets, NormalizationAdmissionAndPostNodeLimitsRetainCharges) {
  const auto Run = [&](uint64_t MaxWork, uint64_t MaxNodes, bool WidePrefix) {
    SymContext Ctx;
    Ctx.mkConst(llvm::APInt(WidePrefix ? 256 : 64, 0xabcdef));
    const auto Root = Ctx.mkVar("frame", 64);
    const auto Input = Ctx.mkVar("data", 64);
    const auto Value = mbaFrameAddress(Ctx, Root, Input, uint64_t{0} - 40);
    SpecializationOptions O;
    O.MaxSolverQueries = 0;
    O.MaxSymbolicNodes = MaxNodes;
    FrameOffsetNormalization Work{MaxWork};
    uint64_t Queries = 0;
    const auto R = proveFrameOffset(Ctx, Ctx.mkTrue(), Value, Root, O, Queries,
                                    nullptr, &Work);
    EXPECT_EQ(Queries, 0u);
    EXPECT_LE(Work.WorkUsed, MaxWork);
    return std::tuple{R, Work.WorkUsed, Ctx.numNodes()};
  };
  const auto [Good, Used, Nodes] = Run(65536, 65536, false);
  ASSERT_EQ(Good.Status, FrameOffsetStatus::Exact);
  EXPECT_EQ(std::get<0>(Run(Used, 65536, false)).Status,
            FrameOffsetStatus::Exact);
  const auto [Wide, WideUsed, WideNodes] = Run(65536, 65536, true);
  EXPECT_EQ(Wide.Status, FrameOffsetStatus::Exact);
  EXPECT_EQ(WideUsed, Used + 3); // Four constant limbs instead of one.
  for (uint64_t Cap : {uint64_t{0}, uint64_t{1}, uint64_t{8}, uint64_t{32}}) {
    const auto [R, Charged, N] = Run(Cap, 65536, false);
    EXPECT_EQ(R.Status, FrameOffsetStatus::BudgetExceeded);
    EXPECT_EQ(Charged, Cap);
  }
  const auto [Short, Charged, ShortNodes] = Run(65536, Nodes - 1, false);
  EXPECT_EQ(Short.Status, FrameOffsetStatus::BudgetExceeded);
  EXPECT_GT(Charged, 0u);
  EXPECT_GT(ShortNodes, Nodes - 1);
}

TEST(FrameOffsets, AdmittedNormalizationMeasurementCanStillExhaust) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("frame", 64);
  const auto Input = Ctx.mkVar("data", 64);
  const auto Value = mbaFrameAddress(Ctx, Root, Input, 0);
  const auto Admission = Ctx.mkVar("admission", 8);
  const auto Predicate =
      Ctx.mkEq(Ctx.mkMul(Admission, Admission), Ctx.mkConst(8, 9));
  SpecializationOptions O;
  O.MaxSolverGates = 1;
  uint64_t Queries = 0;
  FiniteQueryCache Cache(65536, Ctx);
  ASSERT_EQ(
      proveFrameOffset(Ctx, Predicate, Value, Root, O, Queries, &Cache).Status,
      FrameOffsetStatus::BudgetExceeded);
  const auto Difference = Ctx.mkSub(Value, Root);
  const auto Nodes = Ctx.numNodes();
  ASSERT_EQ(
      proveFrameOffset(Ctx, Predicate, Value, Root, O, Queries, &Cache).Status,
      FrameOffsetStatus::BudgetExceeded);
  ASSERT_EQ(Ctx.numNodes(), Nodes);
  ASSERT_EQ(Queries, 0u);
  // Encoding the independent satisfiable square predicate fails before a fresh
  // projected-value variable is allocated. Repeated cold refusals therefore
  // leave the prefix fixed; price that documented admission contract.
  uint64_t Prefix = 0;
  for (uint32_t I = 0; I != Ctx.numNodes(); ++I) {
    const SymRef Node(I);
    Prefix += 1 + Ctx.numOperands(Node);
    if (Ctx.op(Node) == SymOp::Const)
      Prefix += (uint64_t{Ctx.width(Node)} + 63) / 64;
  }
  FrameOffsetNormalization Work{Prefix};
  EXPECT_EQ(
      proveFrameOffset(Ctx, Predicate, Value, Root, O, Queries, &Cache, &Work)
          .Status,
      FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Work.WorkUsed, Prefix);
  ASSERT_EQ(Ctx.numNodes(), Nodes);
  // One extra unit now reaches measurement, but cannot finish the carry proof.
  Work.MaxWork = Prefix + 1;
  const auto R =
      proveFrameOffset(Ctx, Predicate, Value, Root, O, Queries, &Cache, &Work);
  EXPECT_EQ(R.Status, FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Work.WorkUsed, Work.MaxWork);
  EXPECT_FALSE(Cache.lookup(Ctx, Predicate, {Difference}, 1));
  Work.MaxWork = UINT64_MAX;
  const auto Complete =
      proveFrameOffset(Ctx, Predicate, Value, Root, O, Queries, &Cache, &Work);
  EXPECT_EQ(Complete.Status, FrameOffsetStatus::Exact);
  EXPECT_EQ(Complete.Offset, 0u);
  EXPECT_LT(Work.WorkUsed, 65536u);
  EXPECT_FALSE(Cache.lookup(Ctx, Predicate, {Difference}, 1));
}

TEST(FrameOffsets, NormalizationChargesOnlyTheUncachedReadabilityPrefix) {
  SymContext Original;
  // History unrelated to the demanded address must still cost admission when
  // cold, but a previous readability query has already paid for its cache.
  for (unsigned I = 1; I <= 4096; ++I)
    Original.mkConst(llvm::APInt(256, I).shl(128));
  const auto Root = Original.mkVar("frame", 64);
  const auto Input = Original.mkVar("data", 64);
  const auto Value = mbaFrameAddress(Original, Root, Input, uint64_t{0} - 24);
  const auto Admission = Original.mkVar("admission", 8);
  const auto Predicate = Original.mkEq(Original.mkMul(Admission, Admission),
                                       Original.mkConst(8, 9));
  Original.mkSub(Value, Root);
  ASSERT_EQ(Original.numCachedReadabilityNodes(), 0u);
  uint64_t Prefix = 0;
  for (uint32_t I = 0; I != Original.numNodes(); ++I) {
    const SymRef R(I);
    Prefix += 1 + Original.numOperands(R);
    if (Original.op(R) == SymOp::Const)
      Prefix += (uint64_t{Original.width(R)} + 63) / 64;
  }
  const auto Run = [&](bool Warm, uint64_t MaxWork, bool ShortNodes = false) {
    auto Ctx = Original;
    if (Warm)
      Ctx.readability(Value);
    // Appended nodes still need admission even after a fully warm prefix.
    Ctx.mkConst(llvm::APInt(256, 0xabcdef));
    SpecializationOptions O;
    O.MaxSolverGates =
        1; // Refuse the predicate before allocating query values.
    if (ShortNodes)
      O.MaxSymbolicNodes = Ctx.numNodes() - 1;
    FrameOffsetNormalization Work{MaxWork};
    uint64_t Queries = 0;
    const auto R = proveFrameOffset(Ctx, Predicate, Value, Root, O, Queries,
                                    nullptr, &Work);
    EXPECT_EQ(Queries, 0u);
    EXPECT_LE(Work.WorkUsed, MaxWork);
    return std::pair{R, Work.WorkUsed};
  };
  const auto [Cold, ColdWork] = Run(false, 65536);
  const auto [Warm, WarmWork] = Run(true, 65536);
  ASSERT_EQ(Cold.Status, FrameOffsetStatus::Exact);
  ASSERT_EQ(Warm.Status, FrameOffsetStatus::Exact);
  EXPECT_EQ(Cold.Offset, uint64_t{0} - 24);
  EXPECT_EQ(Warm.Offset, Cold.Offset);
  EXPECT_EQ(ColdWork - WarmWork, Prefix);
  EXPECT_GT(WarmWork, 5u); // The new four-limb literal plus MBA work.
  EXPECT_EQ(Run(true, WarmWork).first.Status, FrameOffsetStatus::Exact);
  EXPECT_EQ(Run(false, WarmWork).first.Status,
            FrameOffsetStatus::BudgetExceeded);
  const auto [Short, Charged] = Run(true, 4);
  EXPECT_EQ(Short.Status, FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Charged, 4u);
  const auto [NodeLimit, NodeWork] = Run(true, 65536, true);
  EXPECT_EQ(NodeLimit.Status, FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(NodeWork, 0u);
}

TEST(FrameOffsets, NormalizationRetryReusesOriginalTranslatedQuery) {
  for (bool Bound : {false, true})
    for (bool ReuseEncoding : {false, true}) {
      SCOPED_TRACE(Bound);
      SCOPED_TRACE(ReuseEncoding);
      SymContext Ctx;
      const auto Root = Ctx.mkVar("frame", 64);
      const auto Input = Ctx.mkVar("data", 64);
      const auto Base =
          Ctx.mkAnd(mbaFrameAddress(Ctx, Root, Input, uint64_t{0} - 73),
                    Ctx.mkConst(64, -16));
      const auto Predicate = residue(Ctx, Root, 15, 6);
      auto Cache = Bound ? std::make_unique<FiniteQueryCache>(65536, Ctx)
                         : std::make_unique<FiniteQueryCache>(65536);
      neverd::solver::SolverOptions Settings;
      Settings.Blast.MaxGates = 256;
      FiniteDomainEncoding Encoding(Ctx, Settings);
      uint64_t Queries = 0;
      const auto Prove = [&](SymRef P, SymRef Value, uint64_t Limit,
                             uint64_t Nodes, FrameOffsetNormalization &Work) {
        return ReuseEncoding
                   ? proveFrameOffset(Encoding, P, Value, Root, Limit, Nodes,
                                      Queries, Cache.get(), &Work)
                   : proveFrameOffset(Ctx, P, Value, Root, Settings, Limit,
                                      Nodes, Queries, Cache.get(), &Work);
      };
      FrameOffsetNormalization Work{65536};
      const auto Initial = Prove(
          Predicate, Ctx.mkAdd(Base, Ctx.mkConst(64, 11)), 100, 65536, Work);
      ASSERT_EQ(Initial.Status, FrameOffsetStatus::Exact);
      ASSERT_EQ(Queries, 2u);
      ASSERT_GT(Work.WorkUsed, 0u);
      for (uint64_t Bias : {uint64_t{0}, uint64_t{1}, uint64_t{0} - 11,
                            uint64_t{1} << 63, UINT64_MAX}) {
        const auto Address = Ctx.mkAdd(Base, Ctx.mkConst(64, Bias));
        Work.MaxWork = 0;
        Work.WorkUsed = 99;
        const auto Result = Prove(Predicate, Address, Queries, 65536, Work);
        ASSERT_EQ(Result.Status, FrameOffsetStatus::Exact);
        EXPECT_EQ(Result.Offset, uint64_t{0} - 86 + Bias);
        EXPECT_EQ(Queries, 2u);
        EXPECT_EQ(Work.WorkUsed, 0u);
        // Cached proofs still answer only their exact predicate and respect
        // the caller's current context-node ceiling.
        EXPECT_EQ(
            Prove(Predicate, Address, Queries, Ctx.numNodes() - 1, Work).Status,
            FrameOffsetStatus::BudgetExceeded);
        EXPECT_EQ(
            Prove(residue(Ctx, Root, 15, 7), Address, Queries, 65536, Work)
                .Status,
            FrameOffsetStatus::BudgetExceeded);
        EXPECT_EQ(Queries, 2u);
      }
    }
}

TEST(FrameOffsets, IncompleteNormalizationRetryNeverSuppliesCachedEvidence) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("frame", 64);
  const auto Input = Ctx.mkVar("data", 64);
  const auto Base =
      Ctx.mkAnd(mbaFrameAddress(Ctx, Root, Input, uint64_t{0} - 73),
                Ctx.mkConst(64, -16));
  const auto Value = Ctx.mkAdd(Base, Ctx.mkConst(64, 11));
  const auto Predicate = residue(Ctx, Root, 15, 6);
  const auto Key = Ctx.mkSub(Base, Root);
  neverd::solver::SolverOptions Settings;
  Settings.Blast.MaxGates = 256;
  FiniteQueryCache Cache(65536, Ctx);
  uint64_t Queries = 0;
  FrameOffsetNormalization Work{65536};
  // A single SAT request cannot establish uniqueness. Repeating the original
  // query with no new allowance must not acquire a cached singleton.
  EXPECT_EQ(proveFrameOffset(Ctx, Predicate, Value, Root, Settings, 1, 65536,
                             Queries, &Cache, &Work)
                .Status,
            FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Queries, 1u);
  EXPECT_FALSE(Cache.lookup(Ctx, Predicate, {Key}, 1));
  Work.MaxWork = 0;
  EXPECT_EQ(proveFrameOffset(Ctx, Predicate, Value, Root, Settings, 1, 65536,
                             Queries, &Cache, &Work)
                .Status,
            FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Queries, 1u);
  EXPECT_FALSE(Cache.lookup(Ctx, Predicate, {Key}, 1));
}

TEST(FrameOffsets, CachedNormalizationRetainsAdmissionAndZeroWorkLimits) {
  SymContext Ctx;
  const auto Root = Ctx.mkVar("frame", 64);
  const auto Input = Ctx.mkVar("data", 64);
  const auto Value = mbaFrameAddress(Ctx, Root, Input, 24);
  const auto Admission = Ctx.mkVar("admission", 8);
  const auto Predicate =
      Ctx.mkEq(Ctx.mkMul(Admission, Admission), Ctx.mkConst(8, 9));
  SpecializationOptions Options;
  Options.MaxSolverQueries = 0;
  // Refuse the predicate before allocating a fresh finite-value variable.
  Options.MaxSolverGates = 1;
  MBARewriteCache Rewrites(Ctx);
  FrameOffsetNormalization Work{65536, 0, &Rewrites};
  uint64_t Queries = 0;
  const auto Prove = [&] {
    return proveFrameOffset(Ctx, Predicate, Value, Root, Options, Queries,
                            nullptr, &Work);
  };
  const auto Cold = Prove();
  ASSERT_EQ(Cold.Status, FrameOffsetStatus::Exact);
  EXPECT_EQ(Cold.Offset, 24u);
  ASSERT_GT(Rewrites.size(), 0u);
  const auto ColdWork = Work.WorkUsed;
  // Complete the readability prefix so the next proof measures cache use,
  // independently of the preceding proof's newly constructed nodes.
  Ctx.readability(Value);
  Work.MaxWork = 1;
  const auto Hot = Prove();
  ASSERT_EQ(Hot.Status, FrameOffsetStatus::Exact);
  EXPECT_EQ(Hot.Offset, Cold.Offset);
  EXPECT_EQ(Work.WorkUsed, 1u);
  EXPECT_LT(Work.WorkUsed, ColdWork);
  const auto Hits = Rewrites.reusedRoots();
  ASSERT_GT(Hits, 0u);
  Work.MaxWork = 0;
  EXPECT_EQ(Prove().Status, FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Work.WorkUsed, 0u);
  EXPECT_EQ(Rewrites.reusedRoots(), Hits);
  EXPECT_EQ(Queries, 0u);

  // A cached derivation does not skip admission of new context nodes.
  Ctx.mkConst(llvm::APInt(256, 0xabcdef));
  Work.MaxWork = 5; // One node and its four constant limbs, with no MBA work.
  EXPECT_EQ(Prove().Status, FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Work.WorkUsed, 5u);
  EXPECT_EQ(Rewrites.reusedRoots(), Hits);
  Work.MaxWork = 6;
  EXPECT_EQ(Prove().Status, FrameOffsetStatus::Exact);
  EXPECT_EQ(Work.WorkUsed, 6u);
  EXPECT_GT(Rewrites.reusedRoots(), Hits);
  Options.MaxSymbolicNodes = Ctx.numNodes() - 1;
  EXPECT_EQ(Prove().Status, FrameOffsetStatus::BudgetExceeded);
  EXPECT_EQ(Work.WorkUsed, 0u);
}

TEST(FrameOffsets, CachedChildUnderANewRootStillNeedsThePredicateProof) {
  for (bool ReuseEncoding : {false, true}) {
    SCOPED_TRACE(ReuseEncoding);
    SymContext Ctx;
    const auto Root = Ctx.mkVar("frame", 64);
    const auto Input = Ctx.mkVar("data", 64);
    const auto Child = mbaFrameAddress(Ctx, Root, Input, uint64_t{0} - 73);
    const auto True = Ctx.mkTrue();
    MBARewriteCache Rewrites(Ctx);
    FrameOffsetNormalization Work{65536, 0, &Rewrites};
    SpecializationOptions WarmOptions;
    WarmOptions.MaxSolverQueries = 0;
    uint64_t Queries = 0;
    FiniteQueryCache Domains(65536, Ctx);
    const auto Warm = proveFrameOffset(Ctx, True, Child, Root, WarmOptions,
                                       Queries, &Domains, &Work);
    ASSERT_EQ(Warm.Status, FrameOffsetStatus::Exact);
    ASSERT_GT(Rewrites.size(), 0u);
    EXPECT_FALSE(Domains.lookup(Ctx, True, {Ctx.mkSub(Child, Root)}, 1));
    const auto Hits = Rewrites.reusedRoots();
    const auto Value =
        Ctx.mkAdd(Ctx.mkAnd(Child, Ctx.mkConst(64, -16)), Ctx.mkConst(64, 11));
    const auto Predicate = residue(Ctx, Root, 15, 6);
    neverd::solver::SolverOptions Settings;
    Settings.Blast.MaxGates = 256;
    FiniteDomainEncoding Encoding(Ctx, Settings);
    const auto Prove = [&](SymRef Condition, uint64_t MaxQueries) {
      return ReuseEncoding
                 ? proveFrameOffset(Encoding, Condition, Value, Root,
                                    MaxQueries, 65536, Queries, &Domains, &Work)
                 : proveFrameOffset(Ctx, Condition, Value, Root, Settings,
                                    MaxQueries, 65536, Queries, &Domains,
                                    &Work);
    };
    EXPECT_EQ(Prove(Predicate, 0).Status, FrameOffsetStatus::BudgetExceeded);
    EXPECT_EQ(Queries, 0u);
    EXPECT_GT(Rewrites.reusedRoots(), Hits);
    const auto Key = Ctx.mkSub(Ctx.mkAnd(Child, Ctx.mkConst(64, -16)), Root);
    EXPECT_FALSE(Domains.lookup(Ctx, Predicate, {Key}, 1));
    const auto Proved = Prove(Predicate, 2);
    ASSERT_EQ(Proved.Status, FrameOffsetStatus::Exact);
    EXPECT_EQ(Proved.Offset, uint64_t{0} - 75);
    EXPECT_EQ(Queries, 2u); // A model and its independent uniqueness check.
    ASSERT_TRUE(Domains.lookup(Ctx, Predicate, {Key}, 1));
    // Algebraic rewrites are reusable, but the frame residue was never one.
    Queries = 0;
    EXPECT_EQ(Prove(True, 2).Status, FrameOffsetStatus::NonUnique);
    EXPECT_EQ(Queries, 2u);
    for (uint64_t Stack :
         {uint64_t{6}, uint64_t{0x123456789abcdef6}, UINT64_MAX - 9})
      for (uint64_t Data : {uint64_t{0}, uint64_t{37}, UINT64_MAX}) {
        const auto Expected =
            (((Stack ^ Data) + 2 * (Stack & Data) - Data - 73) &
             ~uint64_t{15}) +
            11;
        EXPECT_EQ(Ctx.evalU64(Value, {Stack, Data}), Expected);
        EXPECT_EQ(Expected, Stack + Proved.Offset);
      }
  }
}

TEST(FrameOffsets, FreshNormalizationCachesDoNotReuseRecycledContextStorage) {
  // Recreate the context at exactly the same address and with colliding node
  // indices. A context-address-only persistent cache would mix these values.
  std::optional<SymContext> Storage;
  for (uint64_t Bias : {uint64_t{24}, uint64_t{0} - 40, UINT64_MAX}) {
    auto &Ctx = Storage.emplace();
    const auto Root = Ctx.mkVar("frame", 64);
    const auto Input = Ctx.mkVar("data", 64);
    const auto Value = mbaFrameAddress(Ctx, Root, Input, Bias);
    MBARewriteCache Rewrites(Ctx);
    ASSERT_EQ(Rewrites.size(), 0u);
    ASSERT_EQ(Rewrites.reusedRoots(), 0u);
    FrameOffsetNormalization Work{65536, 0, &Rewrites};
    SpecializationOptions Options;
    Options.MaxSolverQueries = 0;
    uint64_t Queries = 0;
    const auto R = proveFrameOffset(Ctx, Ctx.mkTrue(), Value, Root, Options,
                                    Queries, nullptr, &Work);
    ASSERT_EQ(R.Status, FrameOffsetStatus::Exact);
    EXPECT_EQ(R.Offset, Bias);
    EXPECT_GT(Rewrites.size(), 0u);
    EXPECT_EQ(Queries, 0u);
    for (uint64_t Stack :
         {uint64_t{0}, uint64_t{0x123456789abcdef0}, UINT64_MAX})
      for (uint64_t Data : {uint64_t{0}, uint64_t{7}, UINT64_MAX})
        EXPECT_EQ(Ctx.evalU64(Value, {Stack, Data}), Stack + R.Offset);
  }
}

} // namespace
