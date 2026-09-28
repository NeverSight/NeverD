//===- ControlDiscoveryTests.cpp - Bounded control input discovery -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/interpreter/ControlDiscovery.h"
#include "gtest/gtest.h"

using namespace neverd::analysis;
using namespace neverd::analysis::detail;
using namespace neverd::symbolic;

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
    ASSERT_EQ(R.FrameSlots.size(), 3u);
    for (size_t I = 0; I < 3; ++I) {
      EXPECT_EQ(R.FrameSlots[I].Offset, I == 0 ? -16 : -15 + int(I));
      EXPECT_EQ(R.FrameSlots[I].Bytes, 1u);
    }
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
