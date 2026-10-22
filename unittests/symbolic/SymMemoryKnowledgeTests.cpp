//===- SymMemoryKnowledgeTests.cpp - Symbolic state contracts
//---------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymState.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace neverd::symbolic;

TEST(SymMemoryKnowledge, RelativeCoordinatesFollowModularBankWrites) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState State(C, Order), Copy(C, Order);
    auto P = C.mkVar("p", 64);
    auto Last = std::numeric_limits<uint64_t>::max();
    SymState::MemoryKnowledge Facts{
        &C, Order, {{P, Last, C.mkConst(16, 0x2211)}}};
    ASSERT_TRUE(State.restoreMemoryKnowledge(Facts));
    EXPECT_EQ(State.load(C.mkAdd(P, C.mkConst(64, Last)), 2),
              C.mkConst(16, 0x2211));
    EXPECT_EQ(State.load(P, 1),
              C.mkConst(8, Order == llvm::endianness::little ? 0x22 : 0x11));
    ASSERT_TRUE(Copy.restoreMemoryKnowledge(State.memoryKnowledge()));
    EXPECT_EQ(Copy.load(C.mkAdd(P, C.mkConst(64, Last)), 2),
              C.mkConst(16, 0x2211));
    Facts.Facts = {{C.mkAdd(P, C.mkConst(64, Last)), 2, C.mkConst(8, 0x77)}};
    ASSERT_TRUE(State.restoreMemoryKnowledge(Facts));
    EXPECT_EQ(State.load(C.mkAdd(P, C.mkOne(64)), 1), C.mkConst(8, 0x77));
  }
}

TEST(SymMemoryKnowledge, AbsoluteBaseAdditionCannotWrapBeforeValidation) {
  SymContext C;
  SymState State(C);
  auto Address = C.mkConst(64, 0x1000);
  ASSERT_TRUE(State.store(Address, C.mkConst(32, 0x44332211)));
  auto Facts = State.memoryKnowledge();
  Facts.Facts.push_back({C.mkConst(64, std::numeric_limits<uint64_t>::max()), 1,
                         C.mkConst(8, 0xaa)});
  EXPECT_FALSE(State.restoreMemoryKnowledge(Facts));
  EXPECT_EQ(State.load(Address, 4), C.mkConst(32, 0x44332211));
  EXPECT_FALSE(State.memoryIsUnknown());
}

TEST(SymMemoryKnowledge,
     InvalidReferencesAndNonByteValuesRejectTheWholeImport) {
  SymContext C;
  SymState State(C);
  auto P = C.mkVar("p", 64);
  State.store(P, C.mkConst(16, 0x1234));
  auto Good = State.memoryKnowledge();
  auto BadRef = SymRef(uint32_t(C.numNodes() + 20));
  for (auto Fact :
       std::vector<SymState::MemoryFact>{{BadRef, 0, C.mkConst(8, 1)},
                                         {P, 0, BadRef},
                                         {P, 0, C.mkConst(1, 1)},
                                         {P, 0, C.mkConst(17, 1)}}) {
    auto Bad = Good;
    Bad.Facts.push_back(Fact);
    EXPECT_FALSE(State.restoreMemoryKnowledge(Bad));
    EXPECT_EQ(State.load(P, 2), C.mkConst(16, 0x1234));
  }
}

TEST(SymMemoryKnowledge, ByteOracleAndScalarBanksSurviveRoundTrips) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    for (unsigned Width : {32u, 64u, 128u}) {
      SymContext C;
      SymState Source(C, Order), Target(C, Order);
      auto P = C.mkVar("p", Width);
      for (unsigned I = 0; I != 19; ++I)
        Source.write(SymSpace::Memory, 0x1000 + I, C.mkConst(8, I * 7 + 3));
      auto Region = Source.load(P, 3);
      Target.write(SymSpace::Register, 8, C.mkConst(64, 0x77));
      Target.write(SymSpace::Temporary, 24, C.mkConst(32, 0x99));
      ASSERT_TRUE(Target.restoreMemoryKnowledge(Source.memoryKnowledge()));
      EXPECT_EQ(Target.load(P, 3), Region);
      EXPECT_EQ(Target.read(SymSpace::Register, 8, 8), C.mkConst(64, 0x77));
      EXPECT_EQ(Target.read(SymSpace::Temporary, 24, 4), C.mkConst(32, 0x99));
      for (unsigned Start = 0; Start != 12; ++Start) {
        uint64_t Want = 0;
        for (unsigned I = 0; I != 8; ++I)
          Want |= uint64_t((Start + I) * 7 + 3)
                  << (8 * (Order == llvm::endianness::little ? I : 7 - I));
        EXPECT_EQ(Target.load(C.mkConst(64, 0x1000 + Start), 8),
                  C.mkConst(64, Want));
      }
    }
  }
}

TEST(SymMemoryKnowledge, RoundTripRetainsSeparateRegionsAndByteOrder) {
  for (auto order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext ctx;
    SymState source(ctx, order), target(ctx, order);
    auto p = ctx.mkVar("p", 64), q = ctx.mkVar("q", 64);
    auto absolute = ctx.mkConst(64, 0x1000);
    auto a = source.load(absolute, 8), b = source.load(p, 8),
         c = source.load(q, 8);
    auto live = source.numLiveBytes();
    auto knowledge = source.memoryKnowledge();
    EXPECT_EQ(source.numLiveBytes(), live);
    ASSERT_EQ(knowledge.Facts.size(), 3u);
    ASSERT_TRUE(target.restoreMemoryKnowledge(knowledge));
    EXPECT_EQ(target.load(absolute, 8), a);
    EXPECT_EQ(target.load(p, 8), b);
    EXPECT_EQ(target.load(q, 8), c);
    EXPECT_EQ(
        target.load(ctx.mkAdd(p, ctx.mkConst(64, 2)), 2),
        ctx.mkExtract(b, order == llvm::endianness::little ? 16 : 32, 16));
  }
}

TEST(SymMemoryKnowledge,
     RebindingBasesPreservesDisplacementsAndCoexistingFacts) {
  SymContext ctx;
  SymState source(ctx), target(ctx);
  auto p = ctx.mkVar("p", 64), q = ctx.mkVar("q", 64);
  auto p2 = ctx.mkVar("p2", 64), q2 = ctx.mkVar("q2", 64);
  source.load(p, 16);
  source.load(q, 8);
  auto knowledge = source.memoryKnowledge();
  ASSERT_EQ(knowledge.Facts.size(), 3u);
  for (auto &fact : knowledge.Facts) {
    fact.Base = fact.Base == p ? p2 : q2;
    fact.Value = ctx.mkConst(64, fact.Base == p2 ? 100 + fact.Offset : 200);
  }
  ASSERT_TRUE(target.restoreMemoryKnowledge(knowledge));
  EXPECT_EQ(target.load(p2, 8), ctx.mkConst(64, 100));
  EXPECT_EQ(target.load(ctx.mkAdd(p2, ctx.mkConst(64, 8)), 8),
            ctx.mkConst(64, 108));
  EXPECT_EQ(target.load(q2, 8), ctx.mkConst(64, 200));
  EXPECT_NE(target.load(p, 8), ctx.mkConst(64, 100));
}

TEST(SymMemoryKnowledge, ExactCollisionsAreOrderIndependentAndTransactional) {
  SymContext ctx;
  auto p = ctx.mkVar("p", 64);
  auto a = ctx.mkConst(64, 0x1000);
  SymState::MemoryKnowledge knowledge{&ctx,
                                      llvm::endianness::little,
                                      {{a, 0, ctx.mkConst(32, 0x44332211)},
                                       {{}, 0x1001, ctx.mkConst(16, 0x3322)}}};
  for (bool reverse : {false, true}) {
    if (reverse)
      std::reverse(knowledge.Facts.begin(), knowledge.Facts.end());
    SymState state(ctx);
    auto old = state.load(p, 8);
    ASSERT_TRUE(state.restoreMemoryKnowledge(knowledge));
    EXPECT_EQ(state.load(a, 4), ctx.mkConst(32, 0x44332211));
    auto conflict = knowledge;
    for (auto &fact : conflict.Facts)
      if (ctx.width(fact.Value) == 16)
        fact.Value = ctx.mkConst(16, 0x99aa);
    auto before = state.load(p, 8);
    EXPECT_FALSE(state.restoreMemoryKnowledge(conflict));
    EXPECT_EQ(state.load(a, 4), ctx.mkConst(32, 0x44332211));
    EXPECT_EQ(state.load(p, 8), before);
    EXPECT_NE(before, old);
  }
}

TEST(SymMemoryKnowledge, InvalidSnapshotsCannotPartlyUpdateTheState) {
  SymContext ctx, other;
  SymState state(ctx);
  auto p = ctx.mkVar("p", 64);
  state.store(p, ctx.mkConst(64, 17));
  auto good = state.memoryKnowledge();
  auto bad = good;
  bad.Owner = &other;
  EXPECT_FALSE(state.restoreMemoryKnowledge(bad));
  bad = good;
  bad.Order = llvm::endianness::big;
  EXPECT_FALSE(state.restoreMemoryKnowledge(bad));
  bad = good;
  bad.Facts.push_back({{}, 0, {}});
  EXPECT_FALSE(state.restoreMemoryKnowledge(bad));
  bad = good;
  bad.Facts.push_back(
      {{}, std::numeric_limits<uint64_t>::max(), ctx.mkConst(16, 0)});
  EXPECT_FALSE(state.restoreMemoryKnowledge(bad));
  EXPECT_EQ(state.load(p, 8), ctx.mkConst(64, 17));
}

TEST(SymMemoryKnowledge, ReboundSymbolicRegionsMergeOnlyCompatibleBytes) {
  SymContext ctx;
  auto p = ctx.mkVar("p", 64);
  SymState::MemoryKnowledge knowledge{
      &ctx,
      llvm::endianness::little,
      {{ctx.mkAdd(p, ctx.mkConst(64, 8)), 0, ctx.mkConst(32, 0x44332211)},
       {p, 9, ctx.mkConst(16, 0x3322)}}};
  for (bool reverse : {false, true}) {
    if (reverse)
      std::reverse(knowledge.Facts.begin(), knowledge.Facts.end());
    SymState state(ctx);
    ASSERT_TRUE(state.restoreMemoryKnowledge(knowledge));
    EXPECT_EQ(state.load(ctx.mkAdd(p, ctx.mkConst(64, 8)), 4),
              ctx.mkConst(32, 0x44332211));
    auto bad = knowledge;
    for (auto &fact : bad.Facts)
      if (ctx.width(fact.Value) == 16)
        fact.Value = ctx.mkConst(16, 0x1234);
    EXPECT_FALSE(state.restoreMemoryKnowledge(bad));
    EXPECT_EQ(state.load(ctx.mkAdd(p, ctx.mkConst(64, 8)), 4),
              ctx.mkConst(32, 0x44332211));
  }
}

TEST(SymMemoryKnowledge,
     HistoricalOriginsAndSharedLazyBytesAreNotCurrentFacts) {
  SymContext ctx;
  SymState source(ctx);
  auto p = ctx.mkVar("p", 64);
  auto old = source.load(p, 8);
  source.write(SymSpace::Register, 0, old);
  source.clobberMemory();
  ASSERT_NE(source.loadOrigin(old), nullptr);
  EXPECT_TRUE(source.memoryKnowledge().Facts.empty());
  SymState sibling(source);
  sibling.load(ctx.mkVar("q", 64), 8);
  EXPECT_TRUE(source.memoryKnowledge().Facts.empty());
  auto empty = source.memoryKnowledge();
  ASSERT_TRUE(source.restoreMemoryKnowledge(empty));
  EXPECT_EQ(source.read(SymSpace::Register, 0, 8), old);
  EXPECT_NE(source.loadOrigin(old), nullptr);
  EXPECT_NE(source.load(p, 8), old);
}

TEST(SymMemoryKnowledge,
     ImportedUnknownEpochsForkButDoNotCrossIndependentImports) {
  SymContext ctx;
  auto p = ctx.mkVar("p", 64);
  SymState first(ctx), second(ctx);
  auto empty = first.memoryKnowledge();
  ASSERT_TRUE(first.restoreMemoryKnowledge(empty));
  ASSERT_TRUE(second.restoreMemoryKnowledge(empty));
  SymState fork(first);
  auto a = first.load(p, 8);
  EXPECT_EQ(fork.load(p, 8), a);
  EXPECT_NE(second.load(p, 8), a);
}

TEST(SymMemoryKnowledge, LaterWritesAndHavocStillInvalidateMayAliases) {
  SymContext ctx;
  auto p = ctx.mkVar("p", 64), q = ctx.mkVar("q", 64);
  auto a = ctx.mkConst(64, 0x1000);
  SymState source(ctx), target(ctx);
  auto pv = source.load(p, 8), qv = source.load(q, 8), av = source.load(a, 8);
  ASSERT_TRUE(target.restoreMemoryKnowledge(source.memoryKnowledge()));
  target.store(ctx.mkAdd(p, ctx.mkConst(64, 1)), ctx.mkConst(8, 0x55));
  EXPECT_EQ(target.load(p, 1), ctx.mkExtract(pv, 0, 8));
  EXPECT_EQ(target.load(ctx.mkAdd(p, ctx.mkConst(64, 1)), 1),
            ctx.mkConst(8, 0x55));
  EXPECT_NE(target.load(q, 8), qv);
  EXPECT_NE(target.load(a, 8), av);
  auto current = target.load(p, 8);
  target.clobberMemory();
  EXPECT_TRUE(target.memoryKnowledge().Facts.empty());
  EXPECT_NE(target.load(p, 8), current);
}

TEST(SymMaterializedRanges, ProjectionDoesNotCreateValues) {
  SymContext C;
  SymState S(C);
  const auto P = C.mkVar("p", 64);
  S.store(P, C.mkVar("value", 64));
  S.store(C.mkAdd(P, C.mkConst(64, 16)), C.mkConst(32, 17));
  const auto Nodes = C.numNodes();
  const auto Live = S.numLiveBytes();
  uint64_t Budget = 13;
  const auto R = S.materializedRegionRanges(P, Budget);
  ASSERT_TRUE(R);
  ASSERT_EQ(R->size(), 2u);
  EXPECT_EQ((*R)[0].Offset, 0u);
  EXPECT_EQ((*R)[0].Bytes, 8u);
  EXPECT_EQ((*R)[1].Offset, 16u);
  EXPECT_EQ((*R)[1].Bytes, 4u);
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(C.numNodes(), Nodes);
  EXPECT_EQ(S.numLiveBytes(), Live);
}

TEST(SymMaterializedRanges, GapsAndModularWrapStaySeparate) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState S(C, Order);
    const auto P = C.mkVar("p", 64);
    const auto Last = std::numeric_limits<uint64_t>::max();
    S.store(C.mkAdd(P, C.mkConst(64, Last - 2)), C.mkConst(32, 0x11223344));
    S.store(C.mkAdd(P, C.mkConst(64, 8)), C.mkConst(16, 0x5566));
    uint64_t Budget = 7;
    const auto R = S.materializedRegionRanges(P, Budget);
    ASSERT_TRUE(R);
    ASSERT_EQ(R->size(), 3u);
    EXPECT_EQ((*R)[0].Offset, 0u);
    EXPECT_EQ((*R)[0].Bytes, 1u);
    EXPECT_EQ((*R)[1].Offset, 8u);
    EXPECT_EQ((*R)[1].Bytes, 2u);
    EXPECT_EQ((*R)[2].Offset, Last - 2);
    EXPECT_EQ((*R)[2].Bytes, 3u);
    EXPECT_EQ(Budget, 0u);
  }
}

TEST(SymMaterializedRanges, AdjacentPartialWritesFormOneCurrentExtent) {
  SymContext C;
  SymState S(C);
  const auto P = C.mkVar("p", 64);
  S.store(P, C.mkConst(16, 0x1122));
  S.store(C.mkAdd(P, C.mkConst(64, 2)), C.mkVar("tail", 32));
  S.store(C.mkAdd(P, C.mkConst(64, 1)), C.mkConst(8, 0x33));
  uint64_t Budget = 7;
  const auto R = S.materializedRegionRanges(P, Budget);
  ASSERT_TRUE(R);
  ASSERT_EQ(R->size(), 1u);
  EXPECT_EQ(R->front().Offset, 0u);
  EXPECT_EQ(R->front().Bytes, 6u);
  EXPECT_EQ(Budget, 0u);
}

TEST(SymMaterializedRanges, RefusedProjectionLeavesStateUnchanged) {
  SymContext C;
  SymState S(C);
  const auto P = C.mkVar("p", 64);
  const auto Value = C.mkVar("value", 64);
  S.store(P, Value);
  const auto Nodes = C.numNodes();
  uint64_t Zero = 0;
  EXPECT_FALSE(S.materializedRegionRanges(P, Zero));
  EXPECT_EQ(Zero, 0u);
  uint64_t Short = 8;
  EXPECT_FALSE(S.materializedRegionRanges(P, Short));
  EXPECT_EQ(Short, 7u);
  EXPECT_EQ(C.numNodes(), Nodes);
  EXPECT_EQ(S.load(P, 8), Value);
  uint64_t Exact = 9;
  ASSERT_TRUE(S.materializedRegionRanges(P, Exact));
  EXPECT_EQ(Exact, 0u);
}

TEST(SymMaterializedRanges, MissingAndNoncanonicalBasesSupplyNoFacts) {
  SymContext C;
  SymState S(C);
  const auto P = C.mkVar("p", 64);
  const auto Shifted = C.mkAdd(P, C.mkConst(64, 8));
  S.store(Shifted, C.mkConst(8, 0x55));
  const auto Q = C.mkVar("q", 64);
  const auto Invalid = SymRef(uint32_t(C.numNodes() + 20));
  const auto Nodes = C.numNodes();
  for (auto Base : {SymRef(), Invalid, Q, Shifted}) {
    uint64_t Budget = 1;
    const auto R = S.materializedRegionRanges(Base, Budget);
    ASSERT_TRUE(R);
    EXPECT_TRUE(R->empty());
    EXPECT_EQ(Budget, 0u);
  }
  EXPECT_EQ(C.numNodes(), Nodes);
}

TEST(SymMaterializedRanges, HavocDoesNotResurrectHistoricalCells) {
  SymContext C;
  SymState S(C);
  const auto P = C.mkVar("p", 64);
  const auto Value = S.load(P, 8);
  ASSERT_FALSE(S.loadOrigins(Value).empty());
  S.store(C.mkVar("may_alias", 64), C.mkConst(8, 0));
  uint64_t Budget = 1;
  auto R = S.materializedRegionRanges(P, Budget);
  ASSERT_TRUE(R);
  EXPECT_TRUE(R->empty());
  S.load(C.mkAdd(P, C.mkConst(64, 16)), 1);
  Budget = 2;
  R = S.materializedRegionRanges(P, Budget);
  ASSERT_TRUE(R);
  ASSERT_EQ(R->size(), 1u);
  EXPECT_EQ(R->front().Offset, 16u);
  EXPECT_EQ(R->front().Bytes, 1u);
}

TEST(SymMaterializedRanges, RangeInspectionIsReadOnlyAndUsesCurrentBytes) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState S(C, Order);
    const auto P = C.mkVar("p", 64);
    const auto Q = C.mkVar("may_alias", 64);
    const auto Begin = std::numeric_limits<uint64_t>::max() - 2;
    S.store(C.mkAdd(P, C.mkConst(64, Begin)), C.mkVar("wide_value", 128));
    const auto Nodes = C.numNodes();
    const auto Live = S.numLiveBytes();
    EXPECT_TRUE(S.hasMaterializedRegionRange(P, Begin, 16));
    EXPECT_TRUE(S.hasMaterializedRegionRange(P, 0, 8));
    EXPECT_FALSE(S.hasMaterializedRegionRange(P, Begin - 1, 16));
    EXPECT_FALSE(S.hasMaterializedRegionRange(P, 12, 2));
    EXPECT_FALSE(S.hasMaterializedRegionRange(P, 0, 0));
    EXPECT_FALSE(S.hasMaterializedRegionRange(Q, 0, 1));
    EXPECT_FALSE(S.hasMaterializedRegionRange({}, 0, 1));
    EXPECT_EQ(C.numNodes(), Nodes);
    EXPECT_EQ(S.numLiveBytes(), Live);
    S.store(C.mkAdd(P, C.mkConst(64, 7)), C.mkConst(8, 37));
    EXPECT_TRUE(S.hasMaterializedRegionRange(P, Begin, 16));
    S.store(Q, C.mkConst(8, 0));
    EXPECT_FALSE(S.hasMaterializedRegionRange(P, Begin, 16));
    const auto Unknown = S.load(C.mkAdd(P, C.mkConst(64, 64)), 1);
    EXPECT_TRUE(S.hasMaterializedRegionRange(P, 64, 1));
    EXPECT_FALSE(S.hasMaterializedRegionRange(P, 64, 2));
    EXPECT_FALSE(C.asConst(Unknown));
  }
}

TEST(SymMemoryPreservation, CurrentBytesMatchIndependentByteArray) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState Source(C, Order);
    const auto P = C.mkVar("p", 64);
    std::array<uint8_t, 16> Expected;
    for (unsigned I = 0; I != Expected.size(); ++I) {
      Expected[I] = static_cast<uint8_t>(17 + 11 * I);
      if (I != 6 && I != 7)
        Source.store(C.mkAdd(P, C.mkConst(64, I)), C.mkConst(8, Expected[I]));
    }
    Source.store(C.mkAdd(P, C.mkConst(64, 3)), C.mkConst(16, 0xabc5));
    Expected[3] = Order == llvm::endianness::little ? 0xc5 : 0xab;
    Expected[4] = Order == llvm::endianness::little ? 0xab : 0xc5;
    for (int64_t Begin = 0; Begin != 18; Begin += 3) {
      SCOPED_TRACE(Begin);
      SymState S(Source), Before(Source);
      const auto Hole = Before.load(C.mkAdd(P, C.mkConst(64, 6)), 2);
      SymMemoryPreservation Keep{P, Begin, Begin + 5, 1000};
      const auto Nodes = C.numNodes(), Vars = C.numVars();
      S.clobberMemory(Keep);
      ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
      EXPECT_EQ(C.numNodes(), Nodes);
      EXPECT_EQ(C.numVars(), Vars);
      for (unsigned I = 0; I != 20; ++I) {
        const auto Value = S.load(C.mkAdd(P, C.mkConst(64, I)), 1);
        if (I < Expected.size() && I != 6 && I != 7 && I >= Begin &&
            I < Begin + 5)
          EXPECT_EQ(Value, C.mkConst(8, Expected[I]));
        else
          EXPECT_FALSE(C.isConst(Value));
      }
      EXPECT_NE(S.load(C.mkAdd(P, C.mkConst(64, 6)), 2), Hole);
    }
  }
}

TEST(SymMemoryPreservation, RetainsCurrentSymbolicValuesAndHistoricalOrigins) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState S(C, Order);
    const auto P = C.mkVar("p", 64);
    const auto Old = S.load(P, 8);
    const auto Value = C.mkVar("replacement", 64);
    S.store(P, Value);
    S.store(C.mkAdd(P, C.mkConst(64, 2)), C.mkConst(16, 0x5321));
    const auto Current = S.load(P, 8);
    ASSERT_NE(Current, Old);
    SymMemoryPreservation Keep{P, 0, 8, 1000};
    const auto Nodes = C.numNodes(), Vars = C.numVars();
    S.clobberMemory(Keep);
    ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
    EXPECT_EQ(C.numNodes(), Nodes);
    EXPECT_EQ(C.numVars(), Vars);
    EXPECT_EQ(S.load(P, 8), Current);
    EXPECT_NE(S.loadOrigin(Old), nullptr);
    EXPECT_FALSE(S.memoryInputOrigins(Old).empty());
    Keep.Begin = 8;
    Keep.End = 16;
    S.clobberMemory(Keep);
    ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
    const auto Fresh = S.load(P, 8);
    EXPECT_NE(Fresh, Old);
    EXPECT_NE(Fresh, Current);
    EXPECT_NE(S.loadOrigin(Old), nullptr);
    EXPECT_TRUE(S.memoryInputOrigins(Fresh).empty());
  }
}

TEST(SymMemoryPreservation, FreshEpochsCoverOtherAndPreviouslyUnreadBanks) {
  SymContext C;
  SymState S(C);
  const auto P = C.mkVar("p", 64), Q = C.mkVar("q", 64);
  const auto New = C.mkVar("new_region", 64);
  const auto Absolute = C.mkConst(64, 0x1234);
  const auto PV = S.load(P, 8), QV = S.load(Q, 8);
  const auto AV = S.load(Absolute, 8);
  SymState Before(S);
  const auto NV = Before.load(New, 8);
  SymMemoryPreservation Keep{P, 0, 8, 1000};
  S.clobberMemory(Keep);
  ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
  EXPECT_EQ(S.load(P, 8), PV);
  EXPECT_NE(S.load(Q, 8), QV);
  EXPECT_NE(S.load(Absolute, 8), AV);
  EXPECT_NE(S.load(New, 8), NV);
  EXPECT_TRUE(S.memoryIsUnknown());
}

TEST(SymMemoryPreservation,
     ForksShareNewEpochsButIndependentForgettingDoesNot) {
  SymContext C;
  const auto P = C.mkVar("p", 64), Q = C.mkVar("q", 64);
  SymState A(C), B(A);
  SymMemoryPreservation Keep{P, -8, 0, 1000};
  A.clobberMemory(Keep);
  ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
  B.clobberMemory(Keep);
  ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
  SymState Fork(A);
  const auto First = A.load(Q, 8);
  EXPECT_EQ(Fork.load(Q, 8), First);
  EXPECT_NE(B.load(Q, 8), First);
  const auto Absolute = C.mkConst(64, 0x1234);
  EXPECT_EQ(A.load(Absolute, 8), Fork.load(Absolute, 8));
  EXPECT_NE(A.load(Absolute, 8), B.load(Absolute, 8));
  A.clobberMemory(Keep);
  ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
  EXPECT_NE(A.load(Q, 8), First);
  EXPECT_EQ(Fork.load(Q, 8), First);
}

TEST(SymMemoryPreservation, SiblingOnlyCachesAreNotCurrentFacts) {
  SymContext C;
  const auto P = C.mkVar("p", 64), Q = C.mkVar("q", 64);
  SymState S(C);
  S.load(P, 1);
  S.clobberMemory();
  SymState Sibling(S);
  const auto CachedExisting = Sibling.load(P, 8);
  const auto CachedUnseen = Sibling.load(Q, 8);
  const auto CachedAbsolute = Sibling.load(C.mkConst(64, 0x1234), 8);
  ASSERT_EQ(S.numLiveBytes(), 0u);
  // Three fixed units, two banks, sixteen existing-bank cached bytes,
  // and one default-cache entry with eight bytes. No current bytes survive.
  for (uint64_t Budget = 0; Budget != 30; ++Budget) {
    SymState Attempt(S), Before(S);
    SymMemoryPreservation Keep{P, 0, 8, Budget};
    const auto Nodes = C.numNodes(), Vars = C.numVars();
    Attempt.clobberMemory(Keep);
    EXPECT_EQ(Keep.Status, SymMemoryPreservationStatus::BudgetExceeded);
    EXPECT_LE(Keep.WorkUsed, Budget);
    EXPECT_EQ(C.numNodes(), Nodes);
    EXPECT_EQ(C.numVars(), Vars);
    EXPECT_TRUE(Attempt.mergeIdentical(Before));
    EXPECT_EQ(Attempt.load(P, 8), CachedExisting);
    EXPECT_EQ(Attempt.load(Q, 8), CachedUnseen);
    EXPECT_EQ(Attempt.load(C.mkConst(64, 0x1234), 8), CachedAbsolute);
  }
  SymMemoryPreservation Keep{P, 0, 8, 30};
  const auto Nodes = C.numNodes(), Vars = C.numVars();
  S.clobberMemory(Keep);
  ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
  EXPECT_EQ(Keep.WorkUsed, 30u);
  EXPECT_EQ(C.numNodes(), Nodes);
  EXPECT_EQ(C.numVars(), Vars);
  EXPECT_EQ(S.numLiveBytes(), 0u);
  EXPECT_NE(S.load(P, 8), CachedExisting);
  EXPECT_NE(S.load(Q, 8), CachedUnseen);
  EXPECT_NE(S.load(C.mkConst(64, 0x1234), 8), CachedAbsolute);
  // Retiring this state's ownership must not erase the sibling's lazy maps.
  EXPECT_EQ(Sibling.load(P, 8), CachedExisting);
  EXPECT_EQ(Sibling.load(Q, 8), CachedUnseen);
  EXPECT_EQ(Sibling.load(C.mkConst(64, 0x1234), 8), CachedAbsolute);
}

TEST(SymMemoryPreservation, ModularIntervalsAndAbsoluteHighBytes) {
  for (bool Absolute : {false, true}) {
    SymContext C;
    SymState S(C);
    const auto P = Absolute ? SymRef{} : C.mkVar("p", 64);
    const auto Address = [&](int64_t Offset) {
      const auto Number = C.mkConst(64, static_cast<uint64_t>(Offset));
      return P ? C.mkAdd(P, Number) : Number;
    };
    for (int64_t I = -4; I != 4; ++I)
      S.store(Address(I), C.mkConst(8, static_cast<uint8_t>(I + 40)));
    SymMemoryPreservation Keep{P, -2, 2, 1000};
    S.clobberMemory(Keep);
    ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
    for (int64_t I = -4; I != 4; ++I) {
      const auto Value = S.load(Address(I), 1);
      if (I >= -2 && I < 2)
        EXPECT_EQ(Value, C.mkConst(8, static_cast<uint8_t>(I + 40)));
      else
        EXPECT_FALSE(C.isConst(Value));
    }
  }
}

TEST(SymMemoryPreservation, AbsentAndNoncanonicalRegionsContributeNoFacts) {
  SymContext C;
  const auto P = C.mkVar("p", 64), Q = C.mkVar("q", 64);
  const auto PlusEight = C.mkAdd(P, C.mkConst(64, 8));
  for (const auto Base : {Q, PlusEight}) {
    SymState S(C);
    S.store(PlusEight, C.mkConst(64, 17));
    const auto Regions = S.numMemoryRegions();
    SymMemoryPreservation Keep{Base, 0, 16, 1000};
    S.clobberMemory(Keep);
    ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
    EXPECT_EQ(S.numLiveBytes(), 0u);
    EXPECT_EQ(S.numMemoryRegions(), Regions);
    EXPECT_FALSE(C.isConst(S.load(PlusEight, 8)));
  }
}

TEST(SymMemoryPreservation, LaterStoresKeepOrdinaryAliasInvalidation) {
  SymContext C;
  SymState S(C);
  const auto P = C.mkVar("p", 64), Q = C.mkVar("q", 64);
  S.store(P, C.mkConst(64, 0x8877665544332211));
  SymMemoryPreservation Keep{P, 0, 8, 1000};
  S.clobberMemory(Keep);
  ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
  S.store(C.mkAdd(P, C.mkConst(64, 2)), C.mkConst(16, 0xbbaa));
  EXPECT_EQ(S.load(P, 8), C.mkConst(64, 0x88776655bbaa2211));
  S.store(Q, C.mkConst(8, 19));
  EXPECT_FALSE(C.isConst(S.load(P, 8)));
}

TEST(SymMemoryPreservation, LeavesOtherMachineBanksUntouched) {
  SymContext C;
  SymState S(C);
  const auto P = C.mkVar("p", 64);
  const auto Reg = S.read(SymSpace::Register, 8, 8);
  const auto Temp = S.read(SymSpace::Temporary, 16, 8);
  ASSERT_TRUE(
      S.setX64MemoryAddressSpaceBase(neverd::NdMemoryAddressSpace::X86FS, P));
  ASSERT_TRUE(
      S.setX64MemoryAddressSpaceBase(neverd::NdMemoryAddressSpace::X86GS, P));
  SymState Before(S);
  SymMemoryPreservation Keep{P, 0, 8, 1000};
  S.clobberMemory(Keep);
  ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
  EXPECT_EQ(S.read(SymSpace::Register, 8, 8), Reg);
  EXPECT_EQ(S.read(SymSpace::Temporary, 16, 8), Temp);
  EXPECT_EQ(S.read(SymSpace::Register, 128, 8),
            Before.read(SymSpace::Register, 128, 8));
  EXPECT_EQ(S.read(SymSpace::Temporary, 128, 8),
            Before.read(SymSpace::Temporary, 128, 8));
  EXPECT_EQ(S.x64MemoryAddressSpaceBase(neverd::NdMemoryAddressSpace::X86FS),
            P);
  EXPECT_EQ(S.x64MemoryAddressSpaceBase(neverd::NdMemoryAddressSpace::X86GS),
            P);
}

TEST(SymMemoryPreservation, InvalidDescriptorsPreserveStateAndContext) {
  SymContext C;
  SymState S(C);
  const auto P = C.mkVar("p", 64);
  S.load(P, 8);
  const std::vector<SymMemoryPreservation> Invalid{
      {P, 0, 0, 1000},
      {P, 2, 1, 1000},
      {C.mkConst(64, 0), 0, 8, 1000},
      {C.mkVar("narrow", 32), 0, 8, 1000},
      {C.mkVar("wide", 128), 0, 8, 1000},
      {SymRef(static_cast<uint32_t>(C.numNodes() + 20)), 0, 8, 1000}};
  for (auto Keep : Invalid) {
    const auto Nodes = C.numNodes(), Vars = C.numVars();
    const SymState Before(S);
    S.clobberMemory(Keep);
    EXPECT_EQ(Keep.Status, SymMemoryPreservationStatus::Invalid);
    EXPECT_EQ(Keep.WorkUsed, 1u);
    EXPECT_EQ(C.numNodes(), Nodes);
    EXPECT_EQ(C.numVars(), Vars);
    EXPECT_TRUE(S.mergeIdentical(Before));
  }
}

TEST(SymMemoryPreservation, ExactBudgetAndEveryShortAttemptAreTransactional) {
  SymContext C;
  SymState Source(C);
  const auto P = C.mkVar("p", 64);
  Source.store(P, C.mkConst(64, 17));
  // Three request/lookup/default units, two banks, eight current bytes,
  // and two units for each of eight saved bytes: 3 + 2 + 8 + 16 = 29.
  for (uint64_t Budget = 0; Budget != 29; ++Budget) {
    SCOPED_TRACE(Budget);
    SymState S(Source), Before(Source);
    SymMemoryPreservation Keep{P, 0, 8, Budget};
    const auto Nodes = C.numNodes(), Vars = C.numVars();
    S.clobberMemory(Keep);
    EXPECT_EQ(Keep.Status, SymMemoryPreservationStatus::BudgetExceeded);
    EXPECT_LE(Keep.WorkUsed, Budget);
    if (Budget)
      EXPECT_GE(Keep.WorkUsed, 1u);
    EXPECT_EQ(C.numNodes(), Nodes);
    EXPECT_EQ(C.numVars(), Vars);
    EXPECT_TRUE(S.mergeIdentical(Before));
    const auto Unread = C.mkAdd(P, C.mkConst(64, 16));
    EXPECT_EQ(S.load(Unread, 8), Before.load(Unread, 8));
  }
  SymMemoryPreservation Keep{P, 0, 8, 29};
  Source.clobberMemory(Keep);
  ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
  EXPECT_EQ(Keep.WorkUsed, 29u);
  EXPECT_EQ(Source.load(P, 8), C.mkConst(64, 17));
}

TEST(SymMemoryPreservation, SharedCacheRetirementIsChargedBeforePublication) {
  SymContext C;
  const auto P = C.mkVar("p", 64), Q = C.mkVar("q", 64);
  SymState Source(C);
  Source.clobberMemory();
  const auto Current = Source.load(P, 8);
  SymState Sibling(Source);
  const auto Cached = Sibling.load(Q, 8);
  // P's eight cached bytes are charged in its bank and default-cache entry.
  // Q exists only in the default cache: 3 + 2 + 8 + 8 + 2 + 16 + 16 = 55.
  for (uint64_t Budget = 0; Budget != 55; ++Budget) {
    SymState S(Source), Before(Source);
    SymMemoryPreservation Keep{P, 0, 8, Budget};
    const auto Nodes = C.numNodes(), Vars = C.numVars();
    S.clobberMemory(Keep);
    EXPECT_EQ(Keep.Status, SymMemoryPreservationStatus::BudgetExceeded);
    EXPECT_LE(Keep.WorkUsed, Budget);
    EXPECT_EQ(C.numNodes(), Nodes);
    EXPECT_EQ(C.numVars(), Vars);
    EXPECT_TRUE(S.mergeIdentical(Before));
    EXPECT_EQ(S.load(Q, 8), Cached);
  }
  SymMemoryPreservation Keep{P, 0, 8, 55};
  Source.clobberMemory(Keep);
  ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
  EXPECT_EQ(Keep.WorkUsed, 55u);
  EXPECT_EQ(Source.load(P, 8), Current);
  EXPECT_NE(Source.load(Q, 8), Cached);
  EXPECT_EQ(Sibling.load(Q, 8), Cached);
}

TEST(SymMemoryPreservation, ExtremeIntervalsDoNotEnumerateMissingBytes) {
  SymContext C;
  SymState S(C);
  const auto P = C.mkVar("p", 64);
  const auto Min = std::numeric_limits<int64_t>::min();
  const auto Max = std::numeric_limits<int64_t>::max();
  const auto At = [&](int64_t Offset) {
    return C.mkAdd(P, C.mkConst(64, static_cast<uint64_t>(Offset)));
  };
  for (int64_t Offset : {Min, int64_t{-1}, int64_t{0}, Max})
    S.store(At(Offset), C.mkConst(8, 31));
  // Two banks and four bytes, only three of which are retained.
  SymMemoryPreservation Keep{P, Min, Max, 15};
  S.clobberMemory(Keep);
  ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
  EXPECT_EQ(Keep.WorkUsed, 15u);
  EXPECT_EQ(S.load(At(Min), 1), C.mkConst(8, 31));
  EXPECT_EQ(S.load(At(-1), 1), C.mkConst(8, 31));
  EXPECT_EQ(S.load(At(0), 1), C.mkConst(8, 31));
  EXPECT_FALSE(C.isConst(S.load(At(Max), 1)));
}

TEST(SymMemoryPreservation, UnprotectedNarrowAndWideRegionsAreForgotten) {
  SymContext C;
  SymState S(C);
  const auto P = C.mkVar("p", 64);
  const auto Narrow = C.mkVar("narrow", 32), Wide = C.mkVar("wide", 128);
  const auto PV = S.load(P, 8), N = S.load(Narrow, 8), W = S.load(Wide, 8);
  // Three fixed units, four banks, 24 current bytes and eight saved bytes.
  SymMemoryPreservation Keep{P, 0, 8, 47};
  S.clobberMemory(Keep);
  ASSERT_EQ(Keep.Status, SymMemoryPreservationStatus::Applied);
  EXPECT_EQ(Keep.WorkUsed, 47u);
  EXPECT_EQ(S.numMemoryRegions(), 3u);
  EXPECT_EQ(S.load(P, 8), PV);
  EXPECT_NE(S.load(Narrow, 8), N);
  EXPECT_NE(S.load(Wide, 8), W);
}
