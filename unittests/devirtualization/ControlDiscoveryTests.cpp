//===- ControlDiscoveryTests.cpp - Bounded control input discovery -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/core/ControlDiscovery.h"
#include "gtest/gtest.h"

#include <limits>
#include <tuple>

using namespace neverd::analysis;
using namespace neverd::analysis::detail;
using namespace neverd::symbolic;

TEST(ControlDiscovery, PureVariableDemandDoesNotNominateUnknownLocations) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Variable = Ctx.mkVar("unlocated", 48);
  const auto Other = Ctx.mkFreshVar(8, "other");
  const auto Value = Ctx.mkConcat(Ctx.mkExtract(Variable, 17, 4), Other);
  const auto Nodes = Ctx.numNodes();
  const auto Pure = gatherVariableBitDemand(Ctx, Value, Variable, 100);
  ASSERT_TRUE(Pure.Bits);
  EXPECT_EQ(*Pure.Bits, uint64_t{15} << 17);
  EXPECT_GT(Pure.Visited, 0U);
  const auto StateResult = gatherControlDependencies(State, Value, {}, 100);
  EXPECT_EQ(StateResult.Status, ControlDiscoveryStatus::UnsupportedOrigin);
  EXPECT_FALSE(StateResult.FrameRootBits);
  EXPECT_TRUE(StateResult.RegisterRanges.empty());
  EXPECT_TRUE(StateResult.FrameSlots.empty());
  EXPECT_EQ(Ctx.numNodes(), Nodes);
  const auto Exact =
      gatherVariableBitDemand(Ctx, Value, Variable, Pure.Visited);
  EXPECT_EQ(Exact.Bits, Pure.Bits);
  const auto Short =
      gatherVariableBitDemand(Ctx, Value, Variable, Pure.Visited - 1);
  EXPECT_FALSE(Short.Bits);
  EXPECT_EQ(Short.Visited, Pure.Visited - 1);
}

TEST(ControlDiscovery, PureVariableDemandBoundsRepeatedOperandScans) {
  SymContext Ctx;
  const auto Variable = Ctx.mkVar("word", 64);
  const auto Bit = Ctx.mkExtract(Variable, 0, 1);
  constexpr unsigned Count = 2048;
  llvm::SmallVector<SymRef, 8> Parts(Count, Bit);
  const auto Repeated = Ctx.mkConcat(Parts);
  ASSERT_EQ(Ctx.op(Repeated), SymOp::Concat);
  EXPECT_FALSE(
      gatherVariableBitDemand(Ctx, Repeated, Variable, Count / 2).Bits);
  const auto Full = gatherVariableBitDemand(Ctx, Repeated, Variable, Count * 4);
  ASSERT_TRUE(Full.Bits);
  EXPECT_EQ(*Full.Bits, 1U);
  EXPECT_GE(Full.Visited, Count);
  EXPECT_TRUE(
      gatherVariableBitDemand(Ctx, Repeated, Variable, Full.Visited).Bits);
  EXPECT_FALSE(
      gatherVariableBitDemand(Ctx, Repeated, Variable, Full.Visited - 1).Bits);
  for (auto Invalid : {SymRef(), SymRef(0x7fffffff), Ctx.mkConst(64, 0),
                       Ctx.mkVar("wide", 4096)})
    EXPECT_FALSE(gatherVariableBitDemand(Ctx, Repeated, Invalid, 100).Bits);
  EXPECT_FALSE(gatherVariableBitDemand(Ctx, {}, Variable, 100).Bits);
  EXPECT_FALSE(gatherVariableBitDemand(Ctx, Repeated, Variable, 0).Bits);
}

TEST(ControlDiscovery, BoundedFrameCardinalityRetainsRuntimeInputs) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto Low = Ctx.mkEq(Ctx.mkExtract(Root, 0, 32), Ctx.mkConst(32, 5));
  const auto Input = State.read(SymSpace::Register, 16, 8);
  const auto Guard = Ctx.mkAnd(Low, Ctx.mkUle(Input, Ctx.mkConst(64, 17)));
  const auto False = Ctx.mkFalse();
  const auto Nodes = Ctx.numNodes();
  EXPECT_TRUE(frameRootDomainExceedsLimit(State, Guard, Root, std::nullopt,
                                          UINT32_MAX, 100));
  EXPECT_TRUE(
      frameRootDomainExceedsLimit(State, False, Root, std::nullopt, 16, 100));
  // The predicate may be unreachable: the result is deliberately conditional.
  // It must never authorize a reachable edge or a constant input by itself.
  EXPECT_EQ(Ctx.numNodes(), Nodes);
  for (SpecializationEntryFrameBounds Bounds :
       {SpecializationEntryFrameBounds{-64, 8}, {-64, -8}, {0, 8}, {0, 1}}) {
    EXPECT_TRUE(
        frameRootDomainExceedsLimit(State, Guard, Root, Bounds, 16, 100));
    if (Bounds.Begin == 0 && Bounds.End == 1)
      EXPECT_TRUE(frameRootDomainExceedsLimit(State, Guard, Root, Bounds,
                                              UINT32_MAX, 100));
    else
      EXPECT_FALSE(frameRootDomainExceedsLimit(State, Guard, Root, Bounds,
                                               UINT32_MAX, 100));
  }
}

TEST(ControlDiscovery, BoundedFrameCardinalityUsesExactStrictLimits) {
  constexpr uint64_t Lower = uint64_t{1} << 63;
  constexpr uint64_t Upper = Lower + (uint64_t{2} << 32) - 1;
  const SpecializationEntryFrameBounds Bounds{
      std::numeric_limits<int64_t>::min(),
      static_cast<int64_t>(UINT64_MAX - Upper + 1)};
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto LowerGuard = Ctx.mkUle(Ctx.mkConst(64, Lower), Root);
  const auto UpperGuard = Ctx.mkUle(Root, Ctx.mkConst(64, Upper));
  // Each residue has exactly two members in this interval, independently
  // checked by their explicit values, including both boundary residues.
  for (uint64_t Residue : {uint64_t{0}, uint64_t{17}, uint64_t{UINT32_MAX}}) {
    EXPECT_GE(Lower + Residue, Lower);
    EXPECT_LE(Lower + Residue + (uint64_t{1} << 32), Upper);
    EXPECT_GT(Lower + Residue + (uint64_t{2} << 32), Upper);
    const auto Low =
        Ctx.mkEq(Ctx.mkExtract(Root, 0, 32), Ctx.mkConst(32, Residue));
    const auto Guard = Ctx.mkAnd({LowerGuard, UpperGuard, Low});
    EXPECT_TRUE(
        frameRootDomainExceedsLimit(State, Guard, Root, Bounds, 1, 100));
    EXPECT_FALSE(
        frameRootDomainExceedsLimit(State, Guard, Root, Bounds, 2, 100));
    if (Residue == UINT32_MAX) {
      const auto Tighter = Ctx.mkUle(Root, Ctx.mkConst(64, Upper - 1));
      // One member remains. A same-shaped bound with a different constant
      // cannot be stripped, whether it replaces or accompanies the contract.
      for (auto Predicate :
           {Ctx.mkAnd({LowerGuard, Low, Tighter}), Ctx.mkAnd(Guard, Tighter)})
        EXPECT_FALSE(frameRootDomainExceedsLimit(State, Predicate, Root, Bounds,
                                                 1, 100));
    }
    if (Residue == 0) {
      const auto Tighter = Ctx.mkUle(Ctx.mkConst(64, Lower + 1), Root);
      EXPECT_FALSE(frameRootDomainExceedsLimit(State, Ctx.mkAnd(Guard, Tighter),
                                               Root, Bounds, 1, 100));
    }
  }
  // The largest signed frame interval leaves only two adjacent 64-bit roots;
  // fixing low32 can leave one. It has no uniform two-value lower bound.
  const SpecializationEntryFrameBounds Extreme{
      std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()};
  EXPECT_FALSE(
      frameRootDomainExceedsLimit(State, Ctx.mkTrue(), Root, Extreme, 1, 100));
}

TEST(ControlDiscovery, BoundedFrameCardinalityKeepsNonconjunctiveRestrictions) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto Other = Ctx.mkFreshVar(64, "other");
  const SpecializationEntryFrameBounds Bounds{-64, 8};
  const auto Lower = Ctx.mkUle(Ctx.mkConst(64, 64), Root);
  const auto Upper = Ctx.mkUle(Root, Ctx.mkConst(64, UINT64_MAX - 7));
  const auto Low = Ctx.mkEq(Ctx.mkExtract(Root, 0, 32), Ctx.mkConst(32, 5));
  const auto High = Ctx.mkEq(Ctx.mkExtract(Root, 32, 32), Ctx.mkConst(32, 7));
  EXPECT_TRUE(frameRootDomainExceedsLimit(State, Ctx.mkAnd({Lower, Upper, Low}),
                                          Root, Bounds, 16, 100));
  for (auto Restriction : {High, Ctx.mkNot(Lower), Ctx.mkOr(Lower, Low),
                           Ctx.mkUle(Root, Ctx.mkConst(64, 64)),
                           Ctx.mkUle(Ctx.mkConst(64, UINT64_MAX - 7), Root),
                           Ctx.mkUle(Ctx.mkConst(64, 64), Other)}) {
    EXPECT_FALSE(
        frameRootDomainExceedsLimit(State, Restriction, Root, Bounds, 16, 100));
  }
  const auto External = State.load(Ctx.mkConst(64, 0x800), 8);
  EXPECT_FALSE(frameRootDomainExceedsLimit(
      State, Ctx.mkEq(External, Ctx.mkConst(64, 7)), Root, Bounds, 16, 100));
  const auto Narrow = Ctx.mkUle(Ctx.mkExtract(Root, 0, 32), Ctx.mkConst(32, 7));
  EXPECT_TRUE(frameRootDomainExceedsLimit(
      State, Ctx.mkAnd({Lower, Upper, Narrow}), Root, Bounds, 16, 100));
}

TEST(ControlDiscovery, BoundedFrameCardinalityRejectsInvalidContracts) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto True = Ctx.mkTrue();
  for (auto InvalidRoot :
       {SymRef(), SymRef(0x7fffffff), Ctx.mkFreshVar(32, "narrow"),
        Ctx.mkVar("named", 64), State.read(SymSpace::Register, 0, 8),
        Ctx.mkAdd(Root, Ctx.mkConst(64, 1))})
    EXPECT_FALSE(frameRootDomainExceedsLimit(State, True, InvalidRoot,
                                             std::nullopt, 16, 100));
  for (auto Predicate : {SymRef(), SymRef(0x7fffffff), Root})
    EXPECT_FALSE(frameRootDomainExceedsLimit(State, Predicate, Root,
                                             std::nullopt, 16, 100));
  for (SpecializationEntryFrameBounds Bounds :
       {SpecializationEntryFrameBounds{8, 8}, {9, 8}})
    EXPECT_FALSE(
        frameRootDomainExceedsLimit(State, True, Root, Bounds, 16, 100));
  EXPECT_FALSE(
      frameRootDomainExceedsLimit(State, True, Root, std::nullopt, 0, 100));
  EXPECT_FALSE(
      frameRootDomainExceedsLimit(State, True, Root, std::nullopt, 16, 0));
}

TEST(ControlDiscovery, BoundedFrameCardinalitySharesOneWalkBudget) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const SpecializationEntryFrameBounds Bounds{-64, 8};
  const auto Low = Ctx.mkEq(Ctx.mkExtract(Root, 0, 32), Ctx.mkConst(32, 5));
  const auto Input =
      Ctx.mkEq(State.read(SymSpace::Register, 16, 8), Ctx.mkConst(64, 7));
  const auto Lower = Ctx.mkUle(Ctx.mkConst(64, 64), Root);
  const auto Upper = Ctx.mkUle(Root, Ctx.mkConst(64, UINT64_MAX - 7));
  const auto Guard = Ctx.mkAnd({Low, Input, Lower, Upper});
  const auto Nodes = Ctx.numNodes();
  const auto MinimumBudget = [&](SymRef Predicate) {
    for (uint64_t Budget = 0; Budget <= 100; ++Budget)
      if (frameRootDomainExceedsLimit(State, Predicate, Root, Bounds, 16,
                                      Budget))
        return Budget;
    return uint64_t{0};
  };
  const auto Full = MinimumBudget(Guard);
  ASSERT_GT(Full, 0U);
  for (auto Clause : {Low, Input, Lower, Upper}) {
    const auto Separate = MinimumBudget(Clause);
    ASSERT_GT(Separate, 0U);
    EXPECT_GT(Full, Separate);
    EXPECT_FALSE(
        frameRootDomainExceedsLimit(State, Guard, Root, Bounds, 16, Separate));
  }
  EXPECT_TRUE(
      frameRootDomainExceedsLimit(State, Guard, Root, Bounds, 16, Full));
  EXPECT_FALSE(
      frameRootDomainExceedsLimit(State, Guard, Root, Bounds, 16, Full - 1));
  EXPECT_EQ(Ctx.numNodes(), Nodes);
}

TEST(ControlDiscovery, ExactRegisterByteSlicesUseMachineByteOrder) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    auto Word = State.read(SymSpace::Register, 16, 8);
    auto Byte = Ctx.mkExtract(Word, 24, 8);
    auto Nodes = Ctx.numNodes();
    auto R = gatherControlDependencies(State, Byte, {}, 100);
    ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
    ASSERT_EQ(R.RegisterRanges.size(), 1u);
    EXPECT_EQ(R.RegisterRanges[0].Offset,
              Order == llvm::endianness::little ? 19u : 20u);
    EXPECT_EQ(R.RegisterRanges[0].Bytes, 1u);
    EXPECT_EQ(Ctx.numNodes(), Nodes);
  }
}

TEST(ControlDiscovery, PartialBitsNominateOnlyTheirContainingBytes) {
  SymContext Ctx;
  SymState State(Ctx);
  auto R = gatherControlDependencies(
      State, Ctx.mkExtract(State.read(SymSpace::Register, 16, 8), 5, 6), {},
      100);
  ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  ASSERT_EQ(R.RegisterRanges.size(), 1u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 16u);
  EXPECT_EQ(R.RegisterRanges[0].Bytes, 2u);
}

TEST(ControlDiscovery, ConcatRetainsTheSelectedLanesOnly) {
  SymContext Ctx;
  SymState State(Ctx);
  auto A = State.read(SymSpace::Register, 16, 8);
  auto B = State.read(SymSpace::Register, 32, 8);
  auto Join = Ctx.mkConcat({Ctx.mkExtract(A, 8, 8), Ctx.mkExtract(B, 40, 8)});
  auto R = gatherControlDependencies(State, Join, {}, 100);
  ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  ASSERT_EQ(R.RegisterRanges.size(), 2u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 17u);
  EXPECT_EQ(R.RegisterRanges[1].Offset, 37u);
  EXPECT_EQ(R.RegisterRanges[0].Bytes, 1u);
  EXPECT_EQ(R.RegisterRanges[1].Bytes, 1u);
}

TEST(ControlDiscovery, FrameLoadSlicesUseEntryRelativeOffsets) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    auto Root = Ctx.mkFreshVar(64, "frame");
    auto Loaded =
        State.load(Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t(-16))), 8);
    auto R = gatherControlDependencies(State, Ctx.mkExtract(Loaded, 56, 8),
                                       Root, 100);
    ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
    ASSERT_EQ(R.FrameSlots.size(), 1u);
    EXPECT_TRUE(R.RegisterRanges.empty());
    EXPECT_EQ(R.FrameSlots[0].Offset,
              Order == llvm::endianness::little ? -9 : -16);
    EXPECT_EQ(R.FrameSlots[0].Bytes, 1u);
  }
}

TEST(ControlDiscovery, ExternalMemoryOnlyNominatesAddressDependencies) {
  SymContext Ctx;
  SymState State(Ctx);
  auto Pointer = State.read(SymSpace::Register, 24, 8);
  auto Loaded = State.load(Ctx.mkAdd(Pointer, Ctx.mkConst(64, 0x800)), 8);
  auto R = gatherControlDependencies(State, Loaded, {}, 100);
  ASSERT_EQ(R.Status, ControlDiscoveryStatus::UnsupportedOrigin);
  ASSERT_EQ(R.RegisterRanges.size(), 1u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 24u);
  EXPECT_EQ(R.RegisterRanges[0].Bytes, 8u);
  EXPECT_TRUE(R.FrameSlots.empty());
}

TEST(ControlDiscovery, FreshTemporaryAndAbsoluteInputsAreNotLocations) {
  SymContext Ctx;
  SymState State(Ctx);
  for (auto Value :
       {Ctx.mkFreshVar(64, "reg$16"), State.read(SymSpace::Temporary, 16, 8),
        State.load(Ctx.mkConst(64, 0x800), 8),
        Ctx.mkInputVar("later_register", 64,
                       {SymInputKind::Register, 16, 8, 1})}) {
    auto R = gatherControlDependencies(State, Value, {}, 100);
    EXPECT_EQ(R.Status, ControlDiscoveryStatus::UnsupportedOrigin);
    EXPECT_TRUE(R.RegisterRanges.empty());
    EXPECT_TRUE(R.FrameSlots.empty());
  }
}

TEST(ControlDiscovery, BudgetExhaustionClearsEveryCandidate) {
  SymContext Ctx;
  SymState State(Ctx);
  auto A = State.read(SymSpace::Register, 16, 8);
  auto B = State.read(SymSpace::Register, 32, 8);
  auto Value = Ctx.mkAdd(A, B);
  for (uint64_t Budget : {0u, 1u, 2u}) {
    auto R = gatherControlDependencies(State, Value, {}, Budget);
    EXPECT_EQ(R.Status, ControlDiscoveryStatus::BudgetExceeded);
    EXPECT_EQ(R.Visited, Budget);
    EXPECT_TRUE(R.RegisterRanges.empty());
    EXPECT_TRUE(R.FrameSlots.empty());
  }
  auto R = gatherControlDependencies(State, Value, {}, 3);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  EXPECT_EQ(R.Visited, 3u);
  EXPECT_EQ(R.RegisterRanges.size(), 2u);
}

TEST(ControlDiscovery, SharedExpressionsAreChargedOncePerSlice) {
  SymContext Ctx;
  SymState State(Ctx);
  auto A = State.read(SymSpace::Register, 16, 8);
  auto B = State.read(SymSpace::Register, 32, 8);
  auto Shared = Ctx.mkAdd(A, B);
  auto Value = Ctx.mkOr(Ctx.mkEq(Shared, Ctx.mkConst(64, 2)),
                        Ctx.mkEq(Shared, Ctx.mkConst(64, 5)));
  auto R = gatherControlDependencies(State, Value, {}, 8);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  EXPECT_EQ(R.Visited, 8u);
  EXPECT_EQ(R.RegisterRanges.size(), 2u);
}

TEST(ControlDiscovery, FrameOffsetNeedsTheExactRoot) {
  SymContext Ctx;
  auto Root = Ctx.mkFreshVar(64, "frame");
  auto Other = Ctx.mkFreshVar(64, "other");
  EXPECT_EQ(frameRelativeOffset(Ctx, Root, Root), 0u);
  EXPECT_EQ(frameRelativeOffset(
                Ctx, Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t(-32))), Root),
            uint64_t(-32));
  EXPECT_FALSE(frameRelativeOffset(Ctx, Other, Root));
  EXPECT_FALSE(frameRelativeOffset(Ctx, Ctx.mkAdd(Root, Other), Root));
  EXPECT_FALSE(frameRelativeOffset(Ctx, Ctx.mkAdd(Root, Root), Root));
}

TEST(ControlDiscovery, WideningKeepsNarrowLanes) {
  SymContext Ctx;
  SymState State(Ctx);
  auto Byte = State.read(SymSpace::Register, 17, 1);
  auto Sign = Ctx.mkExtract(Ctx.mkSExt(Byte, 64), 56, 8);
  auto R = gatherControlDependencies(State, Sign, {}, 100);
  ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  ASSERT_EQ(R.RegisterRanges.size(), 1u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 17u);
  EXPECT_EQ(R.RegisterRanges[0].Bytes, 1u);
  auto Zero = Ctx.mkExtract(Ctx.mkZExt(Byte, 64), 56, 8);
  R = gatherControlDependencies(State, Zero, {}, 100);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  EXPECT_TRUE(R.RegisterRanges.empty());
}

TEST(ControlDiscovery, AliasingStoreDoesNotReuseTheOldLoadedInput) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto Address = Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t(-16)));
  const auto Original = State.read(SymSpace::Register, 16, 8);
  State.store(Address, Original);
  const auto Before = State.load(Address, 8);
  auto R = gatherControlDependencies(State, Before, Root, 100);
  ASSERT_EQ(R.RegisterRanges.size(), 1u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 16u);

  // The external store starts a new unknown epoch for the frame bank. That
  // reload is no longer an entry-memory input, despite its exact address.
  State.store(State.read(SymSpace::Register, 32, 8), Ctx.mkConst(64, 0));
  const auto After = State.load(Address, 8);
  ASSERT_NE(Before, After);
  EXPECT_FALSE(Ctx.asConst(After));
  R = gatherControlDependencies(State, After, Root, 100);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::UnsupportedOrigin);
  EXPECT_TRUE(R.RegisterRanges.empty());
  EXPECT_TRUE(R.FrameSlots.empty());
  EXPECT_TRUE(State.memoryInputOrigins(After).empty());
}

TEST(ControlDiscovery, NarrowFrameDiscoveryChargesInputOrigins) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto Loaded = State.load(Root, 8);
  const auto Byte = Ctx.mkExtract(Loaded, 40, 8);
  auto R = gatherControlDependencies(State, Byte, Root, 2);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::BudgetExceeded);
  EXPECT_EQ(R.Visited, 2u);
  EXPECT_TRUE(R.FrameSlots.empty());
  R = gatherControlDependencies(State, Byte, Root, 3);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  EXPECT_EQ(R.Visited, 3u);
  ASSERT_EQ(R.FrameSlots.size(), 1u);
  EXPECT_EQ(R.FrameSlots[0].Offset, 5);
  EXPECT_EQ(R.FrameSlots[0].Bytes, 1u);
}

TEST(ControlDiscovery, SelectTracksItsConditionAndOnlyDemandedArmBytes) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Flag = State.read(SymSpace::Register, 0, 1);
  const auto Word = State.read(SymSpace::Register, 16, 8);
  const auto Select =
      Ctx.mkIte(Ctx.mkNe(Flag, Ctx.mkConst(8, 0)), Word, Ctx.mkConst(64, 0));
  const auto R =
      gatherControlDependencies(State, Ctx.mkExtract(Select, 16, 8), {}, 100);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  ASSERT_EQ(R.RegisterRanges.size(), 2u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 0u);
  EXPECT_EQ(R.RegisterRanges[0].Bytes, 1u);
  EXPECT_EQ(R.RegisterRanges[1].Offset, 18u);
  EXPECT_EQ(R.RegisterRanges[1].Bytes, 1u);
}

TEST(ControlDiscovery, RegisterSpillsDoNotBecomeEntryMemoryDependencies) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto Value = State.read(SymSpace::Register, 16, 8);
  for (uint64_t Offset : {uint64_t(-16), uint64_t(-8)}) {
    const auto Address = Ctx.mkAdd(Root, Ctx.mkConst(64, Offset));
    State.store(Address, Value);
    ASSERT_EQ(State.load(Address, 8), Value);
  }
  ASSERT_EQ(State.loadOrigins(Value).size(), 2u);
  auto R = gatherControlDependencies(State, Value, Root, 100);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  EXPECT_TRUE(R.FrameSlots.empty());
  ASSERT_EQ(R.RegisterRanges.size(), 1u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 16u);
  EXPECT_TRUE(State.memoryInputOrigins(Value).empty());

  // Historical load consumers still see all three occurrences. None changes
  // the entry register that supplies this value, or adds an address operand
  // to its value dependencies.
  const auto External = State.read(SymSpace::Register, 32, 8);
  State.store(External, Value);
  ASSERT_EQ(State.load(External, 8), Value);
  ASSERT_EQ(State.loadOrigins(Value).size(), 3u);
  EXPECT_EQ(State.loadOrigin(Value), nullptr);
  R = gatherControlDependencies(State, Value, Root, 100);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  EXPECT_TRUE(R.FrameSlots.empty());
  ASSERT_EQ(R.RegisterRanges.size(), 1u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 16u);
}

TEST(ControlDiscovery, CompositeFrameLoadsRetainTheirUntouchedInputBytes) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    const auto Root = Ctx.mkFreshVar(64, "frame");
    const auto Address = Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t(-16)));
    State.store(Ctx.mkAdd(Address, Ctx.mkConst(64, 1)), Ctx.mkConst(8, 42));
    const auto Loaded = State.load(Address, 4);
    ASSERT_EQ(Ctx.op(Loaded), SymOp::Concat);
    EXPECT_TRUE(State.memoryInputOrigins(Loaded).empty());
    ASSERT_EQ(State.loadOrigins(Loaded).size(), 1u);
    const auto R = gatherControlDependencies(State, Loaded, Root, 100);
    EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
    EXPECT_TRUE(R.RegisterRanges.empty());
    // The stored byte splits the load; each unread run around it is one input.
    ASSERT_EQ(R.FrameSlots.size(), 2u);
    EXPECT_EQ(R.FrameSlots[0].Offset, -16);
    EXPECT_EQ(R.FrameSlots[0].Bytes, 1u);
    EXPECT_EQ(R.FrameSlots[1].Offset, -14);
    EXPECT_EQ(R.FrameSlots[1].Bytes, 2u);
    const unsigned Low = Order == llvm::endianness::little ? 16 : 8;
    const auto Slice = Ctx.mkExtract(Loaded, Low, 8);
    const auto Narrow = gatherControlDependencies(State, Slice, Root, 100);
    ASSERT_EQ(Narrow.FrameSlots.size(), 1u);
    EXPECT_EQ(Narrow.FrameSlots[0].Offset, -14);
    EXPECT_EQ(Narrow.FrameSlots[0].Bytes, 1u);
  }
}

TEST(ControlDiscovery, StoredExpressionsKeepTheirInputFrontier) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto Input = State.read(SymSpace::Register, 16, 8);
  const auto Value = Ctx.mkAnd(Input, Ctx.mkConst(64, 3));
  State.store(Root, Value);
  ASSERT_EQ(State.load(Root, 8), Value);
  ASSERT_EQ(State.loadOrigins(Value).size(), 1u);
  const auto R = gatherControlDependencies(State, Value, Root, 100);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  EXPECT_TRUE(R.FrameSlots.empty());
  ASSERT_EQ(R.RegisterRanges.size(), 1u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 16u);
}

TEST(ControlDiscovery, MemoryInputBirthSurvivesCopiesAndLaterClobbers) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto Address = Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t(-16)));
  const auto Original = State.load(Address, 8);
  ASSERT_EQ(State.memoryInputOrigins(Original).size(), 1u);
  SymState Copy = State;
  const auto Spill = Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t(-8)));
  Copy.store(Spill, Original);
  ASSERT_EQ(Copy.load(Spill, 8), Original);
  ASSERT_EQ(Copy.loadOrigins(Original).size(), 2u);
  ASSERT_EQ(Copy.memoryInputOrigins(Original).size(), 1u);
  Copy.clobberMemory();
  const auto Reloaded = Copy.load(Address, 8);
  EXPECT_NE(Reloaded, Original);
  EXPECT_TRUE(Copy.memoryInputOrigins(Reloaded).empty());
  const auto R = gatherControlDependencies(Copy, Original, Root, 100);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  ASSERT_EQ(R.FrameSlots.size(), 1u);
  EXPECT_EQ(R.FrameSlots[0].Offset, -16);
  EXPECT_EQ(R.FrameSlots[0].Bytes, 8u);
  EXPECT_EQ(State.loadOrigins(Original).size(), 1u);
  EXPECT_EQ(State.load(Address, 8), Original);
}

TEST(ControlDiscovery, FrameRootSpillsDoNotBecomeFiniteInputs) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  State.store(Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t(-8))), Root);
  const auto Loaded =
      State.load(Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t(-8))), 8);
  ASSERT_EQ(Loaded, Root);
  const auto R = gatherControlDependencies(State, Loaded, Root, 100);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  EXPECT_TRUE(R.FrameSlots.empty());
  EXPECT_TRUE(R.RegisterRanges.empty());
  ASSERT_TRUE(R.FrameRootBits);
  EXPECT_EQ(*R.FrameRootBits, UINT64_MAX);
}

TEST(ControlDiscovery, RootDependenciesKeepLowArithmeticAndHighSlicesDistinct) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto Low = Ctx.mkExtract(Ctx.mkAdd(Root, Ctx.mkConst(64, 7)), 0, 8);
  const auto High = Ctx.mkExtract(Root, 40, 8);
  const std::vector<std::pair<SymRef, uint64_t>> Cases = {
      {Ctx.mkEq(Low, Ctx.mkConst(8, 23)), 0xff},
      {Ctx.mkEq(Ctx.mkAnd(Root, Ctx.mkConst(64, 0xf0)), Ctx.mkConst(64, 0x20)),
       0xf0},
      {Ctx.mkEq(High, Ctx.mkConst(8, 3)), uint64_t{0xff} << 40},
      {Ctx.mkEq(Ctx.mkAdd(Low, High), Ctx.mkConst(8, 31)),
       (uint64_t{0xff} << 40) | 0xff},
      {Ctx.mkEq(Root, Ctx.mkConst(64, 0x1200)), UINT64_MAX}};
  for (const auto &[Value, Mask] : Cases) {
    const auto Nodes = Ctx.numNodes();
    const auto R = gatherControlDependencies(State, Value, Root, 100);
    ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
    ASSERT_TRUE(R.FrameRootBits);
    EXPECT_EQ(*R.FrameRootBits, Mask);
    EXPECT_EQ(Ctx.numNodes(), Nodes);
    EXPECT_TRUE(R.RegisterRanges.empty());
    EXPECT_TRUE(R.FrameSlots.empty());
    const auto Exact = gatherControlDependencies(State, Value, Root, R.Visited);
    EXPECT_EQ(Exact.Status, ControlDiscoveryStatus::Complete);
    EXPECT_EQ(Exact.FrameRootBits, R.FrameRootBits);
    const auto Short =
        gatherControlDependencies(State, Value, Root, R.Visited - 1);
    EXPECT_EQ(Short.Status, ControlDiscoveryStatus::BudgetExceeded);
    EXPECT_FALSE(Short.FrameRootBits);
  }
  const auto Unknown = Ctx.mkFreshVar(8, "unknown");
  const auto R =
      gatherControlDependencies(State, Ctx.mkEq(Low, Unknown), Root, 100);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::UnsupportedOrigin);
  EXPECT_FALSE(R.FrameRootBits);
}

TEST(ControlDiscovery, CopiesMaterializeTheirOwnInputMetadata) {
  SymContext Ctx;
  SymState State(Ctx);
  SymState Copy = State;
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto Original = State.load(Root, 8);
  const auto Copied = Copy.load(Root, 8);
  ASSERT_EQ(Original, Copied);
  ASSERT_EQ(State.memoryInputOrigins(Original).size(), 1u);
  ASSERT_EQ(Copy.memoryInputOrigins(Copied).size(), 1u);
  EXPECT_EQ(State.memoryInputOrigins(Original).front(),
            Copy.memoryInputOrigins(Copied).front());
  EXPECT_TRUE(State.mergeIdentical(Copy));
  EXPECT_EQ(State.memoryInputOrigins(Original).size(), 1u);
  const auto R = gatherControlDependencies(Copy, Copied, Root, 100);
  ASSERT_EQ(R.FrameSlots.size(), 1u);
  EXPECT_EQ(R.FrameSlots[0].Offset, 0);
  EXPECT_EQ(R.FrameSlots[0].Bytes, 8u);

  const auto Spill = Ctx.mkAdd(Root, Ctx.mkConst(64, 16));
  State.store(Spill, Original);
  SymState Observed = State;
  ASSERT_EQ(Observed.load(Spill, 8), Original);
  ASSERT_EQ(State.loadOrigins(Original).size(), 1u);
  ASSERT_EQ(Observed.loadOrigins(Original).size(), 2u);
  EXPECT_TRUE(State.mergeIdentical(Observed));
  EXPECT_EQ(State.loadOrigins(Original).size(), 2u);
  EXPECT_EQ(State.memoryInputOrigins(Original).size(), 1u);
}

TEST(ControlDiscovery, UnseenFrameAfterExternalStoreHasNoEntryOrigin) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto External = State.read(SymSpace::Register, 24, 8);
  State.store(External, Ctx.mkConst(64, 42));
  const auto Loaded =
      State.load(Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t(-16))), 8);
  ASSERT_TRUE(Ctx.isVar(Loaded));
  EXPECT_TRUE(Ctx.varInfo(Ctx.varId(Loaded)).Fresh);
  EXPECT_TRUE(State.memoryInputOrigins(Loaded).empty());
  ASSERT_EQ(State.loadOrigins(Loaded).size(), 1u);
  const auto R = gatherControlDependencies(State, Loaded, Root, 100);
  EXPECT_EQ(R.Status, ControlDiscoveryStatus::UnsupportedOrigin);
  EXPECT_TRUE(R.FrameSlots.empty());
  EXPECT_TRUE(R.RegisterRanges.empty());
}

TEST(ControlDiscovery, PartialStoreAfterClobberKeepsOnlyItsRegisterInput) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    const auto Root = Ctx.mkFreshVar(64, "frame");
    const auto Address = Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t(-16)));
    const auto Original = State.load(Address, 4);
    ASSERT_EQ(State.memoryInputOrigins(Original).size(), 1u);
    State.clobberMemory();
    const auto Byte = State.read(SymSpace::Register, 16, 1);
    State.store(Ctx.mkAdd(Address, Ctx.mkConst(64, 1)), Byte);
    const auto Loaded = State.load(Address, 4);
    ASSERT_EQ(Ctx.op(Loaded), SymOp::Concat);
    EXPECT_TRUE(State.memoryInputOrigins(Loaded).empty());
    const auto R = gatherControlDependencies(State, Loaded, Root, 100);
    EXPECT_EQ(R.Status, ControlDiscoveryStatus::UnsupportedOrigin);
    EXPECT_TRUE(R.FrameSlots.empty());
    ASSERT_EQ(R.RegisterRanges.size(), 1u);
    EXPECT_EQ(R.RegisterRanges[0].Offset, 16u);
    EXPECT_EQ(R.RegisterRanges[0].Bytes, 1u);
    // The old input still has a valid birthplace, but none of the new unknown
    // bytes can inherit it merely because this is the same frame address.
    EXPECT_EQ(State.memoryInputOrigins(Original).size(), 1u);
  }
}

TEST(ControlDiscovery, PartialOverwritePreservesOriginalWideInputLanes) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    const auto Root = Ctx.mkFreshVar(64, "frame");
    const auto Address = Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t(-16)));
    const auto Original = State.load(Address, 8);
    ASSERT_EQ(State.memoryInputOrigins(Original).size(), 1u);
    State.store(Ctx.mkAdd(Address, Ctx.mkConst(64, 3)), Ctx.mkConst(8, 0xa5));
    const auto Loaded = State.load(Address, 8);
    ASSERT_EQ(Ctx.op(Loaded), SymOp::Concat);
    for (unsigned Offset : {0u, 2u, 4u, 7u}) {
      const unsigned Low =
          8 * (Order == llvm::endianness::little ? Offset : 7 - Offset);
      const auto Slice = Ctx.mkExtract(Loaded, Low, 8);
      const auto R = gatherControlDependencies(State, Slice, Root, 100);
      EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
      EXPECT_TRUE(R.RegisterRanges.empty());
      ASSERT_EQ(R.FrameSlots.size(), 1u);
      EXPECT_EQ(R.FrameSlots[0].Offset, -16 + int(Offset));
      EXPECT_EQ(R.FrameSlots[0].Bytes, 1u);
    }
    const unsigned Low = Order == llvm::endianness::little ? 24 : 32;
    const auto Replaced = Ctx.mkExtract(Loaded, Low, 8);
    ASSERT_EQ(Ctx.asConst(Replaced), llvm::APInt(8, 0xa5));
    const auto R = gatherControlDependencies(State, Replaced, Root, 100);
    EXPECT_EQ(R.Status, ControlDiscoveryStatus::Complete);
    EXPECT_TRUE(R.FrameSlots.empty());
    EXPECT_TRUE(R.RegisterRanges.empty());
  }
}

TEST(ControlDiscovery, ConstantBitwiseMasksRetainExactDemandInBothByteOrders) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    const auto A = State.read(SymSpace::Register, 16, 8);
    const auto B = State.read(SymSpace::Register, 32, 8);
    for (auto Value : {Ctx.mkAnd({A, B, Ctx.mkConst(64, 0x500)}),
                       Ctx.mkOr({A, B, Ctx.mkConst(64, ~uint64_t(0x500))})}) {
      const auto R = gatherControlDependencies(State, Value, {}, 100);
      ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
      ASSERT_EQ(R.RegisterRanges.size(), 2u);
      for (unsigned I = 0; I != 2; ++I) {
        EXPECT_EQ(R.RegisterRanges[I].Offset,
                  16u + 16u * I +
                      (Order == llvm::endianness::little ? 1u : 6u));
        EXPECT_EQ(R.RegisterRanges[I].Bytes, 1u);
        EXPECT_EQ(R.RegisterRanges[I].DemandedBits, 5u);
      }
    }
  }
}

TEST(ControlDiscovery, BitwiseSlicesPreservePartialByteMasks) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    const auto A = State.read(SymSpace::Register, 16, 8);
    const auto B = State.read(SymSpace::Register, 32, 8);
    const auto Value = Ctx.mkExtract(Ctx.mkNot(Ctx.mkXor(A, B)), 5, 6);
    const auto R = gatherControlDependencies(State, Value, {}, 100);
    ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
    ASSERT_EQ(R.RegisterRanges.size(), 2u);
    for (unsigned I = 0; I != 2; ++I) {
      EXPECT_EQ(R.RegisterRanges[I].Offset,
                16u + 16u * I + (Order == llvm::endianness::little ? 0u : 6u));
      EXPECT_EQ(R.RegisterRanges[I].Bytes, 2u);
      EXPECT_EQ(R.RegisterRanges[I].DemandedBits, 0x7e0u);
    }
    for (auto Killed : {Ctx.mkAnd(A, Ctx.mkConst(64, 0xff)),
                        Ctx.mkOr(A, Ctx.mkConst(64, ~uint64_t(0xff)))}) {
      const auto Dead = gatherControlDependencies(
          State, Ctx.mkExtract(Killed, 8, 8), {}, 100);
      EXPECT_EQ(Dead.Status, ControlDiscoveryStatus::Complete);
      EXPECT_TRUE(Dead.RegisterRanges.empty());
    }
  }
}

TEST(ControlDiscovery, ModularArithmeticIncludesCarryInputsButNotHigherBits) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    const auto A = State.read(SymSpace::Register, 16, 8);
    const auto B = State.read(SymSpace::Register, 32, 8);
    for (auto Value : {Ctx.mkAdd(A, B), Ctx.mkSub(A, B), Ctx.mkMul(A, B)}) {
      const auto R = gatherControlDependencies(
          State, Ctx.mkExtract(Value, 16, 1), {}, 100);
      ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
      ASSERT_EQ(R.RegisterRanges.size(), 2u);
      for (unsigned I = 0; I != 2; ++I) {
        EXPECT_EQ(R.RegisterRanges[I].Offset,
                  16u + 16u * I +
                      (Order == llvm::endianness::little ? 0u : 5u));
        EXPECT_EQ(R.RegisterRanges[I].Bytes, 3u);
        EXPECT_EQ(R.RegisterRanges[I].DemandedBits, 0x1ffffu);
      }
    }
    const auto Neg = gatherControlDependencies(
        State, Ctx.mkExtract(Ctx.mkNeg(A), 16, 1), {}, 100);
    ASSERT_EQ(Neg.RegisterRanges.size(), 1u);
    EXPECT_EQ(Neg.RegisterRanges[0].Bytes, 3u);
    EXPECT_EQ(Neg.RegisterRanges[0].DemandedBits, 0x1ffffu);
  }
}

TEST(ControlDiscovery, DisjointSumsDoNotInventCarryDependencies) {
  for (unsigned Width : {8U, 16U, 32U, 64U})
    for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
      SCOPED_TRACE(Width);
      SymContext Ctx;
      SymState State(Ctx, Order);
      const unsigned Half = Width / 2;
      const auto Root = Ctx.mkFreshVar(64, "frame");
      const auto Low = Ctx.mkZExt(Ctx.mkExtract(Root, 0, Half), Width);
      const auto Input = State.read(SymSpace::Register, 24, Width / 8);
      const auto High = Ctx.mkShl(Input, Ctx.mkConst(Width, Half));
      const auto Sum = Ctx.mkAdd(Low, High);
      const auto Slice = Ctx.mkExtract(Sum, Half, Half);
      const auto R = gatherControlDependencies(State, Slice, Root, 512);
      ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
      ASSERT_TRUE(R.FrameRootBits);
      EXPECT_EQ(*R.FrameRootBits, 0U);
      ASSERT_EQ(R.RegisterRanges.size(), 1U);
      const unsigned Bytes = (Half + 7) / 8;
      EXPECT_EQ(
          R.RegisterRanges[0].Offset,
          24U + (Order == llvm::endianness::little ? 0 : Width / 8 - Bytes));
      EXPECT_EQ(R.RegisterRanges[0].Bytes, Bytes);
      EXPECT_EQ(R.RegisterRanges[0].DemandedBits, (uint64_t{1} << Half) - 1);
      const auto Payload = gatherControlDependencies(
          State, Ctx.mkExtract(Sum, 0, Half), Root, 512);
      ASSERT_TRUE(Payload.FrameRootBits);
      EXPECT_EQ(*Payload.FrameRootBits, (uint64_t{1} << Half) - 1);
      EXPECT_TRUE(Payload.RegisterRanges.empty());
      const auto Carry = gatherControlDependencies(
          State,
          Ctx.mkExtract(Ctx.mkAdd(Sum, Ctx.mkConst(Width, 1)), Half, Half),
          Root, 512);
      ASSERT_TRUE(Carry.FrameRootBits);
      EXPECT_EQ(*Carry.FrameRootBits, (uint64_t{1} << Half) - 1);
      const auto Exact =
          gatherControlDependencies(State, Slice, Root, R.Visited);
      EXPECT_EQ(Exact.Status, ControlDiscoveryStatus::Complete);
      for (uint64_t Budget = 0; Budget < R.Visited; ++Budget) {
        const auto Short =
            gatherControlDependencies(State, Slice, Root, Budget);
        EXPECT_EQ(Short.Status, ControlDiscoveryStatus::BudgetExceeded);
        EXPECT_TRUE(Short.RegisterRanges.empty());
        EXPECT_TRUE(Short.FrameSlots.empty());
        EXPECT_FALSE(Short.FrameRootBits);
      }
    }
}

TEST(ControlDiscovery, DisjointMaskedSumsRetainConstantsAndUnknownCarries) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto Input = State.read(SymSpace::Register, 24, 8);
  const auto Low = Ctx.mkAnd(Root, Ctx.mkConst(64, 255));
  const auto High = Ctx.mkMul(Input, Ctx.mkConst(64, uint64_t{1} << 32));
  const auto Packed =
      Ctx.mkAdd(Ctx.mkAdd(Low, High), Ctx.mkConst(64, 1U << 16));
  const auto R = gatherControlDependencies(State, Ctx.mkExtract(Packed, 32, 32),
                                           Root, 512);
  ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  ASSERT_TRUE(R.FrameRootBits);
  EXPECT_EQ(*R.FrameRootBits, 0U);
  ASSERT_EQ(R.RegisterRanges.size(), 1U);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 24U);
  EXPECT_EQ(R.RegisterRanges[0].Bytes, 4U);
  EXPECT_EQ(R.RegisterRanges[0].DemandedBits, UINT32_MAX);
  const auto Third = Ctx.mkAnd(State.read(SymSpace::Register, 56, 8),
                               Ctx.mkConst(64, UINT32_MAX));
  const auto Overlap = Ctx.mkAdd(Ctx.mkAdd(Low, High), Third);
  ASSERT_EQ(Ctx.operands(Overlap).size(), 3U);
  ASSERT_EQ(Ctx.operands(Overlap).back(), Third);
  const auto Carry = gatherControlDependencies(
      State, Ctx.mkExtract(Overlap, 32, 32), Root, 512);
  ASSERT_EQ(Carry.Status, ControlDiscoveryStatus::Complete);
  ASSERT_TRUE(Carry.FrameRootBits);
  EXPECT_EQ(*Carry.FrameRootBits, 255U);
  for (auto Other : {Root, Ctx.mkMul(Input, Ctx.mkConst(64, 3)),
                     Ctx.mkMul(Input, Ctx.mkConst(64, 12)),
                     Ctx.mkSExt(State.read(SymSpace::Register, 48, 1), 64),
                     Ctx.mkAShr(Input, Ctx.mkConst(64, 32)),
                     Ctx.mkShl(Input, State.read(SymSpace::Register, 48, 8))}) {
    const auto Unknown = gatherControlDependencies(
        State, Ctx.mkExtract(Ctx.mkAdd(Low, Other), 32, 32), Root, 512);
    ASSERT_TRUE(Unknown.FrameRootBits);
    EXPECT_NE(*Unknown.FrameRootBits, 0U);
  }
}

TEST(ControlDiscovery, SumMasksBoundWideShiftAmountsAndKeepWideFallback) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "frame");
  const auto Input = State.read(SymSpace::Register, 24, 8);
  const auto High = Ctx.mkShl(Input, Ctx.mkConst(64, 32));
  const auto Low = Ctx.mkLShr(Root, Ctx.mkConst(64, 56));
  const auto Precise = gatherControlDependencies(
      State, Ctx.mkExtract(Ctx.mkAdd(Low, High), 32, 32), Root, 512);
  ASSERT_TRUE(Precise.FrameRootBits);
  EXPECT_EQ(*Precise.FrameRootBits, 0U);

  const auto WideAmount = Ctx.mkConst(llvm::APInt(8192, 56));
  const auto WideShift = Ctx.mkLShr(Root, WideAmount);
  ASSERT_EQ(Ctx.op(WideShift), SymOp::LShr);
  // The builder checks the complete numeric count before normalizing its
  // storage. Dependency discovery sees the same expression as the narrow
  // spelling and charges that actual expression, not its old input width.
  ASSERT_EQ(WideShift, Low);
  const auto Slice = Ctx.mkExtract(Ctx.mkAdd(WideShift, High), 32, 32);
  const auto Full = gatherControlDependencies(State, Slice, Root, 512);
  ASSERT_EQ(Full.Status, ControlDiscoveryStatus::Complete);
  EXPECT_EQ(Full.Visited, Precise.Visited);
  ASSERT_TRUE(Full.FrameRootBits);
  EXPECT_EQ(*Full.FrameRootBits, 0U);
  const auto Exact =
      gatherControlDependencies(State, Slice, Root, Full.Visited);
  EXPECT_EQ(Exact.Status, ControlDiscoveryStatus::Complete);
  for (uint64_t Budget : {uint64_t{8}, Full.Visited - 1}) {
    const auto Short = gatherControlDependencies(State, Slice, Root, Budget);
    EXPECT_EQ(Short.Status, ControlDiscoveryStatus::BudgetExceeded);
    EXPECT_TRUE(Short.RegisterRanges.empty());
    EXPECT_TRUE(Short.FrameSlots.empty());
    EXPECT_FALSE(Short.FrameRootBits);
  }

  const auto WideSum =
      Ctx.mkAdd(Ctx.mkZExt(Root, 128),
                Ctx.mkShl(Ctx.mkZExt(Input, 128), Ctx.mkConst(128, 64)));
  const auto Conservative = gatherControlDependencies(
      State, Ctx.mkExtract(WideSum, 64, 64), Root, 512);
  ASSERT_EQ(Conservative.Status, ControlDiscoveryStatus::Complete);
  ASSERT_TRUE(Conservative.FrameRootBits);
  EXPECT_EQ(*Conservative.FrameRootBits, UINT64_MAX);
}

TEST(ControlDiscovery, ConstantMultipliersRetainOnlyRelevantLowPrefixes) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto A = State.read(SymSpace::Register, 16, 8);
  const auto Product = Ctx.mkMul(A, Ctx.mkConst(64, 12));
  const auto R =
      gatherControlDependencies(State, Ctx.mkExtract(Product, 16, 8), {}, 100);
  ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
  ASSERT_EQ(R.RegisterRanges.size(), 1u);
  EXPECT_EQ(R.RegisterRanges[0].Offset, 16u);
  EXPECT_EQ(R.RegisterRanges[0].Bytes, 3u);
  EXPECT_EQ(R.RegisterRanges[0].DemandedBits, 0x3fffffu);
  const auto Killed =
      gatherControlDependencies(State, Ctx.mkExtract(Product, 0, 2), {}, 100);
  EXPECT_EQ(Killed.Status, ControlDiscoveryStatus::Complete);
  EXPECT_TRUE(Killed.RegisterRanges.empty());
}

TEST(ControlDiscovery, ConstantShiftsPreserveSlicesAndSignDependence) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    const auto A = State.read(SymSpace::Register, 16, 8);
    const auto Left = gatherControlDependencies(
        State, Ctx.mkExtract(Ctx.mkShl(A, Ctx.mkConst(64, 8)), 16, 8), {}, 100);
    ASSERT_EQ(Left.RegisterRanges.size(), 1u);
    EXPECT_EQ(Left.RegisterRanges[0].Offset,
              Order == llvm::endianness::little ? 17u : 22u);
    EXPECT_EQ(Left.RegisterRanges[0].Bytes, 1u);
    EXPECT_EQ(Left.RegisterRanges[0].DemandedBits, 0xffu);
    const auto Right = gatherControlDependencies(
        State, Ctx.mkExtract(Ctx.mkLShr(A, Ctx.mkConst(64, 16)), 0, 8), {},
        100);
    ASSERT_EQ(Right.RegisterRanges.size(), 1u);
    EXPECT_EQ(Right.RegisterRanges[0].Offset,
              Order == llvm::endianness::little ? 18u : 21u);
    EXPECT_EQ(Right.RegisterRanges[0].Bytes, 1u);
    EXPECT_EQ(Right.RegisterRanges[0].DemandedBits, 0xffu);
    for (uint64_t Amount : {16u, 64u, 65u}) {
      const auto Sign = gatherControlDependencies(
          State, Ctx.mkExtract(Ctx.mkAShr(A, Ctx.mkConst(64, Amount)), 56, 8),
          {}, 100);
      ASSERT_EQ(Sign.Status, ControlDiscoveryStatus::Complete);
      ASSERT_EQ(Sign.RegisterRanges.size(), 1u);
      EXPECT_EQ(Sign.RegisterRanges[0].Offset,
                Order == llvm::endianness::little ? 23u : 16u);
      EXPECT_EQ(Sign.RegisterRanges[0].Bytes, 1u);
      EXPECT_EQ(Sign.RegisterRanges[0].DemandedBits, 0x80u);
    }
    for (uint64_t Amount : {64u, 65u})
      for (auto Value : {Ctx.mkShl(A, Ctx.mkConst(64, Amount)),
                         Ctx.mkLShr(A, Ctx.mkConst(64, Amount))}) {
        const auto Zero = gatherControlDependencies(State, Value, {}, 100);
        EXPECT_EQ(Zero.Status, ControlDiscoveryStatus::Complete);
        EXPECT_TRUE(Zero.RegisterRanges.empty());
      }
  }
}

TEST(ControlDiscovery, FrameBitMasksUseTheSelectedRangeScalarByteOrder) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    const auto Root = Ctx.mkFreshVar(64, "frame");
    const auto Input =
        State.load(Ctx.mkAdd(Root, Ctx.mkConst(64, uint64_t(-16))), 8);
    const auto R = gatherControlDependencies(State, Ctx.mkExtract(Input, 13, 9),
                                             Root, 100);
    ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
    ASSERT_EQ(R.FrameSlots.size(), 1u);
    EXPECT_EQ(R.FrameSlots[0].Offset,
              Order == llvm::endianness::little ? -15 : -11);
    EXPECT_EQ(R.FrameSlots[0].Bytes, 2u);
    EXPECT_EQ(R.FrameSlots[0].DemandedBits, 0x3fe0u);
    const auto Sparse = gatherControlDependencies(
        State, Ctx.mkAnd(Input, Ctx.mkConst(64, 0x500)), Root, 100);
    ASSERT_EQ(Sparse.FrameSlots.size(), 1u);
    EXPECT_EQ(Sparse.FrameSlots[0].DemandedBits, 5u);
  }
}

TEST(ControlDiscovery, UnsupportedBitTransfersKeepFullOperands) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto A = State.read(SymSpace::Register, 16, 8);
  const auto B = State.read(SymSpace::Register, 32, 8);
  for (auto Value : {Ctx.mkLShr(A, B), Ctx.mkUDiv(A, B)}) {
    const auto R =
        gatherControlDependencies(State, Ctx.mkExtract(Value, 0, 1), {}, 100);
    ASSERT_EQ(R.Status, ControlDiscoveryStatus::Complete);
    ASSERT_EQ(R.RegisterRanges.size(), 2u);
    for (const auto &Range : R.RegisterRanges) {
      EXPECT_EQ(Range.Bytes, 8u);
      EXPECT_EQ(Range.DemandedBits, ~uint64_t(0));
    }
  }
}

TEST(ControlDiscovery, MaskScanningAndRunsConsumeDiscoveryBudget) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto A = State.read(SymSpace::Register, 16, 8);
  const auto Value = Ctx.mkAnd(A, Ctx.mkConst(64, 0x5555));
  const auto Full = gatherControlDependencies(State, Value, {}, 100);
  ASSERT_EQ(Full.Status, ControlDiscoveryStatus::Complete);
  ASSERT_GT(Full.Visited, 8u);
  ASSERT_EQ(Full.RegisterRanges.size(), 2u);
  EXPECT_EQ(Full.RegisterRanges[0].DemandedBits, 0x55u);
  EXPECT_EQ(Full.RegisterRanges[1].DemandedBits, 0x55u);
  const auto Exact = gatherControlDependencies(State, Value, {}, Full.Visited);
  EXPECT_EQ(Exact.Status, ControlDiscoveryStatus::Complete);
  const auto Short =
      gatherControlDependencies(State, Value, {}, Full.Visited - 1);
  EXPECT_EQ(Short.Status, ControlDiscoveryStatus::BudgetExceeded);
  EXPECT_EQ(Short.Visited, Full.Visited - 1);
  EXPECT_TRUE(Short.RegisterRanges.empty());

  // A wide mask must charge its word work even when only one bit survives.
  const auto Wide = State.read(SymSpace::Register, 64, 32);
  const auto One = Ctx.mkAnd(Wide, Ctx.mkConst(llvm::APInt(256, 1)));
  const auto Limited = gatherControlDependencies(State, One, {}, 4);
  EXPECT_EQ(Limited.Status, ControlDiscoveryStatus::BudgetExceeded);
  EXPECT_EQ(Limited.Visited, 4u);
  EXPECT_TRUE(Limited.RegisterRanges.empty());
}

TEST(ControlDiscovery, WideConstantMasksChargeTheirEntirePayloadBeforeSlicing) {
  constexpr uint32_t Width = 4096;
  constexpr uint32_t Bit = 1025;
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    const auto Input = State.read(SymSpace::Register, 0, Width / 8);
    auto Mask = llvm::APInt::getOneBitSet(Width, Bit);
    Mask.setBit(3);
    for (auto Value : {Ctx.mkAnd(Input, Ctx.mkConst(Mask)),
                       Ctx.mkOr(Input, Ctx.mkConst(~Mask))}) {
      // A nonzero offset keeps the full-width operation below the Extract.
      const auto Slice = Ctx.mkExtract(Value, Bit, 1);
      ASSERT_EQ(Ctx.op(Slice), SymOp::Extract);
      ASSERT_EQ(Ctx.width(Ctx.operand(Slice, 0)), Width);
      const auto Short = gatherControlDependencies(State, Slice, {}, 8);
      EXPECT_EQ(Short.Status, ControlDiscoveryStatus::BudgetExceeded);
      EXPECT_EQ(Short.Visited, 8u);
      EXPECT_TRUE(Short.RegisterRanges.empty());
      EXPECT_TRUE(Short.FrameSlots.empty());
      const auto Full = gatherControlDependencies(State, Slice, {}, 1000);
      ASSERT_EQ(Full.Status, ControlDiscoveryStatus::Complete);
      EXPECT_GE(Full.Visited, Width / 64);
      ASSERT_EQ(Full.RegisterRanges.size(), 1u);
      EXPECT_EQ(Full.RegisterRanges[0].Offset, Order == llvm::endianness::little
                                                   ? Bit / 8
                                                   : Width / 8 - Bit / 8 - 1);
      EXPECT_EQ(Full.RegisterRanges[0].Bytes, 1u);
      EXPECT_EQ(Full.RegisterRanges[0].DemandedBits, 1u << (Bit % 8));
    }
  }
}

TEST(ControlDiscovery, WideMultiplyAndShiftConstantsChargeBeforeReading) {
  constexpr uint32_t Width = 4096;
  constexpr uint32_t Bit = 1025;
  SymContext Ctx;
  SymState State(Ctx);
  const auto Input = State.read(SymSpace::Register, 0, Width / 8);
  const auto Product =
      Ctx.mkMul(Input, Ctx.mkConst(llvm::APInt::getOneBitSet(Width, Bit - 1)));
  ASSERT_EQ(Ctx.op(Product), SymOp::Mul);
  // Shift amount widths are independent of the shifted value's width.
  const auto Amount = Ctx.mkConst(llvm::APInt(Width * 2, 1));
  const auto Logical = Ctx.mkLShr(Input, Amount);
  const auto Arithmetic = Ctx.mkAShr(Input, Amount);
  ASSERT_EQ(Ctx.op(Logical), SymOp::LShr);
  ASSERT_EQ(Ctx.op(Arithmetic), SymOp::AShr);
  EXPECT_EQ(Ctx.width(Ctx.operand(Logical, 1)), Width);
  EXPECT_EQ(Ctx.width(Ctx.operand(Arithmetic, 1)), Width);
  for (auto Value : {Product, Logical, Arithmetic}) {
    const auto Slice = Ctx.mkExtract(Value, Bit, 1);
    ASSERT_EQ(Ctx.op(Slice), SymOp::Extract);
    const auto Short = gatherControlDependencies(State, Slice, {}, 8);
    EXPECT_EQ(Short.Status, ControlDiscoveryStatus::BudgetExceeded);
    EXPECT_EQ(Short.Visited, 8u);
    EXPECT_TRUE(Short.RegisterRanges.empty());
    const auto Full = gatherControlDependencies(State, Slice, {}, 1000);
    ASSERT_EQ(Full.Status, ControlDiscoveryStatus::Complete);
    // In-range shift counts now use the value width after full-value checks.
    const uint32_t ConstantWords = Width / 64;
    EXPECT_GE(Full.Visited, ConstantWords);
    ASSERT_EQ(Full.RegisterRanges.size(), 1u);
    const uint32_t SourceBit = Value == Product ? 1 : Bit + 1;
    EXPECT_EQ(Full.RegisterRanges[0].Offset, SourceBit / 8);
    EXPECT_EQ(Full.RegisterRanges[0].Bytes, 1u);
    EXPECT_EQ(Full.RegisterRanges[0].DemandedBits, 1u << (SourceBit % 8));
    const auto Exact =
        gatherControlDependencies(State, Slice, {}, Full.Visited);
    EXPECT_EQ(Exact.Status, ControlDiscoveryStatus::Complete);
    const auto JustShort =
        gatherControlDependencies(State, Slice, {}, Full.Visited - 1);
    EXPECT_EQ(JustShort.Status, ControlDiscoveryStatus::BudgetExceeded);
    EXPECT_TRUE(JustShort.RegisterRanges.empty());
  }
}

namespace {
SymRef cacheDependencyExpression(SymContext &Ctx, SymRef Input) {
  SymRef Value = Input;
  for (unsigned I = 0; I != 24; ++I)
    Value = Ctx.mkXor(Ctx.mkMul(Value, Ctx.mkConst(64, 3 + 2 * I)),
                      Ctx.mkLShr(Input, Ctx.mkConst(64, I % 7 + 1)));
  return Value;
}

void expectSameDiscovery(const ControlDiscovery &A, const ControlDiscovery &B) {
  EXPECT_EQ(A.Status, B.Status);
  EXPECT_EQ(A.FrameRootBits, B.FrameRootBits);
  ASSERT_EQ(A.RegisterRanges.size(), B.RegisterRanges.size());
  ASSERT_EQ(A.FrameSlots.size(), B.FrameSlots.size());
  for (size_t I = 0; I != A.RegisterRanges.size(); ++I) {
    EXPECT_EQ(A.RegisterRanges[I].Offset, B.RegisterRanges[I].Offset);
    EXPECT_EQ(A.RegisterRanges[I].Bytes, B.RegisterRanges[I].Bytes);
    EXPECT_EQ(A.RegisterRanges[I].DemandedBits,
              B.RegisterRanges[I].DemandedBits);
  }
  for (size_t I = 0; I != A.FrameSlots.size(); ++I) {
    EXPECT_EQ(A.FrameSlots[I].Offset, B.FrameSlots[I].Offset);
    EXPECT_EQ(A.FrameSlots[I].Bytes, B.FrameSlots[I].Bytes);
    EXPECT_EQ(A.FrameSlots[I].DemandedBits, B.FrameSlots[I].DemandedBits);
  }
}
} // namespace

TEST(ControlDiscoveryCache, ReusesCompletedFrameBirthsWithExactWorkBounds) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "root");
  const auto Input = State.load(Ctx.mkAdd(Root, Ctx.mkConst(64, -16)), 8);
  const auto Value = cacheDependencyExpression(Ctx, Input);
  const auto Nodes = Ctx.numNodes();
  const auto Bytes = State.numLiveBytes();
  const auto Fresh = gatherControlDependencies(State, Value, Root, 10000);
  ASSERT_EQ(Fresh.Status, ControlDiscoveryStatus::Complete);
  ASSERT_EQ(Fresh.FrameSlots.size(), 1U);
  EXPECT_EQ(Fresh.FrameSlots[0].Offset, -16);
  EXPECT_EQ(Fresh.FrameSlots[0].Bytes, 8);
  EXPECT_EQ(Fresh.FrameSlots[0].DemandedBits, UINT64_MAX);
  EXPECT_EQ(Fresh.FrameRootBits, 0U);
  ControlDependencyCache Cache(Ctx, 4096);
  expectSameDiscovery(Cache.gather(State, Value, Root, 10000), Fresh);
  const auto Hit = Cache.gather(State, Value, Root, 10000);
  expectSameDiscovery(Hit, Fresh);
  EXPECT_LT(Hit.Visited, Fresh.Visited);
  expectSameDiscovery(Cache.gather(State, Value, Root, Hit.Visited), Fresh);
  const auto Short = Cache.gather(State, Value, Root, Hit.Visited - 1);
  EXPECT_EQ(Short.Status, ControlDiscoveryStatus::BudgetExceeded);
  EXPECT_TRUE(Short.FrameSlots.empty());
  EXPECT_TRUE(Short.RegisterRanges.empty());
  EXPECT_FALSE(Short.FrameRootBits);
  EXPECT_EQ(Short.Visited, Hit.Visited - 1);
  EXPECT_EQ(gatherControlDependencies(State, Value, Root, Hit.Visited).Status,
            ControlDiscoveryStatus::BudgetExceeded);
  EXPECT_EQ(Ctx.numNodes(), Nodes);
  EXPECT_EQ(State.numLiveBytes(), Bytes);
  RecordProperty("fresh_visits", Fresh.Visited);
  RecordProperty("cached_visits", Hit.Visited);
}

TEST(ControlDiscoveryCache, RejectsChangedStateOriginsAndFrameIdentity) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "root");
  const auto Address = Ctx.mkAdd(Root, Ctx.mkConst(64, -16));
  const auto Input = State.load(Address, 8);
  const auto Value = cacheDependencyExpression(Ctx, Input);
  ControlDependencyCache Cache(Ctx, 4096);
  ASSERT_EQ(Cache.gather(State, Value, Root, 10000).Status,
            ControlDiscoveryStatus::Complete);
  // Replacing this same state object must not authenticate its old births.
  State = SymState(Ctx);
  const auto Empty = Cache.gather(State, Value, Root, 10000);
  EXPECT_EQ(Empty.Status, ControlDiscoveryStatus::UnsupportedOrigin);
  EXPECT_TRUE(Empty.FrameSlots.empty());
  expectSameDiscovery(Empty,
                      gatherControlDependencies(State, Value, Root, 10000));
  ASSERT_EQ(State.load(Address, 8), Input);
  EXPECT_EQ(Cache.gather(State, Value, Root, 10000).Status,
            ControlDiscoveryStatus::Complete);
  const auto OtherRoot = Ctx.mkFreshVar(64, "other_root");
  const auto DifferentRoot = Cache.gather(State, Value, OtherRoot, 10000);
  EXPECT_EQ(DifferentRoot.Status, ControlDiscoveryStatus::UnsupportedOrigin);
  expectSameDiscovery(
      DifferentRoot, gatherControlDependencies(State, Value, OtherRoot, 10000));
}

TEST(ControlDiscoveryCache, LaterLoadHistoryDoesNotRelabelInputBirths) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "root");
  const auto Input = State.load(Ctx.mkAdd(Root, Ctx.mkConst(64, -16)), 8);
  const auto Value = cacheDependencyExpression(Ctx, Input);
  ControlDependencyCache Cache(Ctx, 4096);
  const auto Initial = Cache.gather(State, Value, Root, 10000);
  const auto Hit = Cache.gather(State, Value, Root, 10000);
  const auto OtherAddress = Ctx.mkAdd(Root, Ctx.mkConst(64, -64));
  State.store(OtherAddress, Input);
  ASSERT_EQ(State.load(OtherAddress, 8), Input);
  ASSERT_EQ(State.loadOrigins(Input).size(), 2U);
  const auto After = Cache.gather(State, Value, Root, Hit.Visited);
  expectSameDiscovery(After, Initial);
  ASSERT_EQ(After.FrameSlots.size(), 1U);
  EXPECT_EQ(After.FrameSlots[0].Offset, -16);
  State.clobberMemory();
  const auto NewInput = State.load(OtherAddress, 8);
  ASSERT_NE(Input, NewInput);
  EXPECT_EQ(
      Cache.gather(State, cacheDependencyExpression(Ctx, NewInput), Root, 10000)
          .Status,
      ControlDiscoveryStatus::UnsupportedOrigin);
}

TEST(ControlDiscoveryCache, ContextAndByteOrderKeepIndependentCoordinates) {
  SymContext Ctx;
  SymState Little(Ctx, llvm::endianness::little);
  SymState Big(Ctx, llvm::endianness::big);
  const auto Root = Ctx.mkFreshVar(64, "root");
  const auto Address = Ctx.mkAdd(Root, Ctx.mkConst(64, -32));
  const auto L = Little.load(Address, 8);
  const auto B = Big.load(Address, 8);
  ControlDependencyCache Cache(Ctx, 4096);
  for (const auto &[State, Input, Offset] :
       {std::tuple{&Little, L, -31}, std::tuple{&Big, B, -26}}) {
    const auto Value = Ctx.mkExtract(Input, 8, 8);
    const auto First = Cache.gather(*State, Value, Root, 10000);
    ASSERT_EQ(First.Status, ControlDiscoveryStatus::Complete);
    ASSERT_EQ(First.FrameSlots.size(), 1U);
    EXPECT_EQ(First.FrameSlots[0].Offset, Offset);
    EXPECT_EQ(First.FrameSlots[0].Bytes, 1);
    EXPECT_EQ(First.FrameSlots[0].DemandedBits, 255U);
    expectSameDiscovery(Cache.gather(*State, Value, Root, 10000), First);
  }
  SymContext Other;
  SymState OtherState(Other);
  const auto OtherRoot = Other.mkFreshVar(64, "root");
  const auto Input = OtherState.read(SymSpace::Register, 96, 8);
  const auto Value = cacheDependencyExpression(Other, Input);
  const auto Expected =
      gatherControlDependencies(OtherState, Value, OtherRoot, 10000);
  expectSameDiscovery(Cache.gather(OtherState, Value, OtherRoot, 10000),
                      Expected);
  ASSERT_EQ(Expected.RegisterRanges.size(), 1U);
  EXPECT_EQ(Expected.RegisterRanges[0].Offset, 96U);
}

TEST(ControlDiscoveryCache, IncompleteQueriesAndCapacityNeverSupplyFacts) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "root");
  const auto Input = State.load(Ctx.mkAdd(Root, Ctx.mkConst(64, -16)), 8);
  const auto Value = cacheDependencyExpression(Ctx, Input);
  const auto Expected = gatherControlDependencies(State, Value, Root, 10000);
  for (uint64_t Capacity : {0U, 1U, 4U, 10U, 4096U}) {
    ControlDependencyCache Cache(Ctx, Capacity);
    const auto Failed = Cache.gather(State, Value, Root, 1);
    EXPECT_EQ(Failed.Status, ControlDiscoveryStatus::BudgetExceeded);
    EXPECT_TRUE(Failed.FrameSlots.empty());
    EXPECT_FALSE(Failed.FrameRootBits);
    expectSameDiscovery(Cache.gather(State, Value, Root, 10000), Expected);
    expectSameDiscovery(Cache.gather(State, Value, Root, 10000), Expected);
    const auto Unknown = Ctx.mkFreshVar(64, "unknown");
    EXPECT_EQ(Cache.gather(State, Unknown, Root, 10000).Status,
              ControlDiscoveryStatus::UnsupportedOrigin);
    for (auto Invalid : {SymRef(), SymRef(UINT32_MAX - 1)})
      EXPECT_NE(Cache.gather(State, Invalid, Root, 10000).Status,
                ControlDiscoveryStatus::Complete);
  }
}

TEST(ControlDiscoveryCache, MissingBirthNeverRetainsAnIncompleteResult) {
  SymContext Ctx;
  SymState Births(Ctx);
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "root");
  const auto Input = Births.load(Ctx.mkAdd(Root, Ctx.mkConst(64, -16)), 8);
  const auto Register = State.read(SymSpace::Register, 96, 8);
  const auto Value = Ctx.mkXor(cacheDependencyExpression(Ctx, Input), Register);
  ControlDependencyCache Cache(Ctx, 4096);
  const auto Missing = gatherControlDependencies(State, Value, Root, 10000);
  ASSERT_EQ(Missing.Status, ControlDiscoveryStatus::UnsupportedOrigin);
  ASSERT_EQ(Missing.RegisterRanges.size(), 1U);
  EXPECT_EQ(Missing.RegisterRanges[0].Offset, 96U);
  EXPECT_TRUE(Missing.FrameSlots.empty());
  for (unsigned I = 0; I != 2; ++I)
    expectSameDiscovery(Cache.gather(State, Value, Root, 10000), Missing);
  State = Births;
  const auto Complete = gatherControlDependencies(State, Value, Root, 10000);
  ASSERT_EQ(Complete.Status, ControlDiscoveryStatus::Complete);
  ASSERT_EQ(Complete.FrameSlots.size(), 1U);
  EXPECT_EQ(Complete.FrameSlots[0].Offset, -16);
  expectSameDiscovery(Cache.gather(State, Value, Root, 10000), Complete);
  const auto Hit = Cache.gather(State, Value, Root, 10000);
  expectSameDiscovery(Hit, Complete);
  EXPECT_LT(Hit.Visited, Complete.Visited);
}

TEST(ControlDiscoveryCache, FullStorageKeepsExistingEntriesAndFreshWalks) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Root = Ctx.mkFreshVar(64, "root");
  ControlDependencyCache Cache(Ctx, 64);
  SymRef First;
  ControlDiscovery FirstResult;
  for (unsigned I = 1; I != 17; ++I) {
    const auto Input =
        State.load(Ctx.mkAdd(Root, Ctx.mkConst(64, -int64_t(I * 16))), 8);
    const auto Value = cacheDependencyExpression(Ctx, Input);
    const auto Fresh = gatherControlDependencies(State, Value, Root, 10000);
    ASSERT_EQ(Fresh.Status, ControlDiscoveryStatus::Complete);
    ASSERT_EQ(Fresh.FrameSlots.size(), 1U);
    EXPECT_EQ(Fresh.FrameSlots[0].Offset, -int64_t(I * 16));
    expectSameDiscovery(Cache.gather(State, Value, Root, 10000), Fresh);
    const auto Again = Cache.gather(State, Value, Root, 10000);
    expectSameDiscovery(Again, Fresh);
    if (I == 1) {
      First = Value;
      FirstResult = Fresh;
      EXPECT_LT(Again.Visited, Fresh.Visited);
    } else if (I == 16) {
      EXPECT_GE(Again.Visited, Fresh.Visited);
    }
  }
  const auto Retained = Cache.gather(State, First, Root, 10000);
  expectSameDiscovery(Retained, FirstResult);
  EXPECT_LT(Retained.Visited, FirstResult.Visited);
}
