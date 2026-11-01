//===- RegistrationFrameSnapshotTests.cpp - Isolated frame snapshots -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep branch and callback snapshots independent through shared-cell edits.
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationFrame.h"
#include "gtest/gtest.h"

#include "neverd/lift/X86Regs.h"

#include <array>
#include <cstdint>
#include <utility>

namespace {
using namespace neverd::registration_state;

FrameState deepSnapshot(const FrameState &Frame) {
  FrameState Copy = Frame;
  for (auto [Dest, Source] :
       {std::pair{&Copy.Cells, &Frame.Cells},
        std::pair{&Copy.EntryCells, &Frame.EntryCells},
        std::pair{&Copy.CallbackCells, &Frame.CallbackCells}}) {
    Dest->clear();
    for (const auto &[Offset, Value] : *Source)
      Dest->set(Offset, Value);
  }
  return Copy;
}

TEST(RegistrationFrameSnapshot, PartialWritesDoNotChangeSiblingCoordinates) {
  for (int32_t Offset : {INT32_MIN, -64, INT32_MAX - 3}) {
    SCOPED_TRACE(Offset);
    FrameState Parent;
    Parent.store(Offset, 4, FrameValue::frame(-128));
    Parent.storeEntry(Offset, 4, FrameValue::entryFrame(16));
    Parent.CallbackEntry = 0x401000;
    Parent.storeCallback(Offset, 4, FrameValue::callbackFrame(0x401000, -32));
    const auto Saved = deepSnapshot(Parent);
    const auto Hash = Parent.snapshotHash();
    auto First = Parent;
    auto Second = Parent;
    First.store(Offset + 1, 1, FrameValue::constant(7));
    First.storeEntry(Offset, 4, FrameValue::constant(11));
    Second.storeCallback(Offset, 4, {});

    EXPECT_EQ(Parent, Saved);
    EXPECT_EQ(Parent.snapshotHash(), Hash);
    EXPECT_TRUE(First.load(Offset, 4).MayBeFrame);
    EXPECT_FALSE(First.load(Offset, 4).Offset);
    EXPECT_EQ(First.loadEntry(Offset, 4).Constant, 11u);
    EXPECT_TRUE(First.loadCallback(Offset, 4).CallbackAddress);
    EXPECT_EQ(Second.load(Offset, 4).Offset, -128);
    EXPECT_EQ(Second.loadEntry(Offset, 4).EntryOffset, 16);
    EXPECT_FALSE(Second.loadCallback(Offset, 4).MayBeFrame);
  }
}

TEST(RegistrationFrameSnapshot, MergeAndReleaseKeepTheirSourceSnapshots) {
  FrameState Left;
  Left.store(-36, 4, FrameValue::frame(-128));
  Left.store(-28, 4, FrameValue::constant(7));
  auto Right = Left;
  Right.store(-36, 4, FrameValue::constant(9));
  Right.store(-16, 4, FrameValue::entryFrame(16));
  const auto SavedLeft = deepSnapshot(Left);
  const auto SavedRight = deepSnapshot(Right);
  auto Joined = Left;
  ASSERT_TRUE(Joined.merge(Right));
  EXPECT_EQ(Left, SavedLeft);
  EXPECT_EQ(Right, SavedRight);
  EXPECT_TRUE(Joined.load(-36, 4).MayBeFrame);
  EXPECT_FALSE(Joined.load(-36, 4).Offset);
  EXPECT_EQ(Joined.load(-28, 4).Constant, 7u);
  EXPECT_TRUE(Joined.load(-16, 4).MayBeFrame);
  EXPECT_FALSE(Joined.load(-16, 4).EntryOffset);
  EXPECT_FALSE(Joined.merge(Right));
  auto Released = Joined;
  discardFrameCellsBelow(Released.Cells, -34);
  Released.forgetCellValues(-28, 4);
  EXPECT_FALSE(Released.load(-28, 4).Constant);
  EXPECT_TRUE(Released.load(-34, 2).MayBeFrame);
  EXPECT_EQ(Joined.load(-28, 4).Constant, 7u);
  EXPECT_EQ(Left, SavedLeft);
  EXPECT_EQ(Right, SavedRight);
}

TEST(RegistrationFrameSnapshot, CallbackReentryKeepsTheSuspendedInvocation) {
  FrameState Active;
  Active.enterCallback(0x401000);
  auto &SP =
      Active.Registers[neverd::x86reg::RSP / neverd::x86reg::GeneralRegStride];
  SP = FrameValue::callbackFrame(0x401000, -32);
  const auto Pointer = FrameValue::callbackFrame(0x401000, -16);
  Active.store(-64, 4, Pointer);
  Active.storeEntry(-8, 4, Pointer);
  Active.storeCallback(-16, 4, Pointer);
  const auto Suspended = Active;
  const auto Hash = Suspended.snapshotHash();
  Active.enterCallback(0x402000);
  EXPECT_FALSE(Active.load(-64, 4).CallbackAddress);
  EXPECT_TRUE(Active.load(-64, 4).MayBeFrame);
  EXPECT_FALSE(Active.loadEntry(-8, 4).CallbackAddress);
  EXPECT_TRUE(Active.CallbackCells.empty());
  EXPECT_EQ(Suspended.load(-64, 4), Pointer);
  EXPECT_EQ(Suspended.loadEntry(-8, 4), Pointer);
  EXPECT_EQ(Suspended.loadCallback(-16, 4), Pointer);
  EXPECT_TRUE(Suspended.callbackMemoryIsPrivate({0x401000, -16}, 4, true));
  EXPECT_EQ(Suspended.snapshotHash(), Hash);
}

TEST(RegistrationFrameSnapshot, EqualSnapshotsStillCanonicalizeEmptyCells) {
  FrameState Parent;
  Parent.Cells.set(-4, {});
  Parent.store(-16, 4, FrameValue::taint(true));
  auto Branch = Parent;
  EXPECT_FALSE(Branch.merge(Parent));
  EXPECT_EQ(Branch.Cells.count(-4), 0u);
  EXPECT_EQ(Parent.Cells.count(-4), 1u);
  EXPECT_TRUE(Branch.load(-16, 4).MayBeFrame);
  Parent.forgetCellValues();
  EXPECT_EQ(Parent, Branch);
  EXPECT_EQ(Parent.snapshotHash(), Branch.snapshotHash());
  EXPECT_FALSE(Parent.merge(Parent));
}

TEST(RegistrationFrameSnapshot, MutationSequencesMatchIndependentCopies) {
  // The reference bank reconstructs every cell map when forking. A mutation
  // of one shared branch must never change a saved sibling or its lookup key.
  for (uint32_t Seed : {1u, 17u, 0x13579bdfu, 0xabcdef01u}) {
    SCOPED_TRACE(Seed);
    std::array<FrameState, 8> Shared, Independent;
    auto Next = [&] { return Seed = Seed * 1664525u + 1013904223u; };
    for (unsigned Step = 0; Step != 320; ++Step) {
      const unsigned Target = (Next() >> 16) % Shared.size();
      const unsigned Source = (Next() >> 16) % Shared.size();
      const unsigned Operation = (Next() >> 16) % 12;
      const int32_t Offset = -4 * int32_t(1 + (Next() >> 16) % 24);
      const auto Value = FrameValue::frame(Offset - 128);
      if (Operation == 0) {
        Shared[Target] = Shared[Source];
        Independent[Target] = deepSnapshot(Independent[Source]);
      } else {
        auto Apply = [&](auto &Bank) {
          auto &Frame = Bank[Target];
          switch (Operation) {
          case 1:
            Frame.store(Offset, 4, Value);
            break;
          case 2:
            Frame.store(Offset + 1, 1, FrameValue::constant(7));
            break;
          case 3:
            Frame.storeEntry(Offset, 4, Value);
            break;
          case 4:
            Frame.storeCallback(Offset, 4, Value);
            break;
          case 5:
            Frame.forgetCellValues();
            break;
          case 6:
            Frame.forgetCellValues(Offset, 8);
            break;
          case 7:
            discardFrameCellsBelow(Frame.Cells, Offset + 1);
            break;
          case 8:
            Frame.merge(Bank[Source]);
            break;
          case 9:
            Frame.enterCallback(0x401000 + Target * 16);
            break;
          case 10:
            Frame.Cells.erase(Frame.Cells.begin(),
                              Frame.Cells.lower_bound(Offset));
            break;
          case 11:
            if (auto It = Frame.Cells.find(Offset); It != Frame.Cells.end())
              Frame.Cells.erase(It);
            break;
          }
        };
        Apply(Shared);
        Apply(Independent);
      }
      for (unsigned I = 0; I != Shared.size(); ++I) {
        SCOPED_TRACE(Step);
        SCOPED_TRACE(I);
        EXPECT_EQ(Shared[I], Independent[I]);
        EXPECT_EQ(Shared[I].snapshotHash(), Independent[I].snapshotHash());
        EXPECT_EQ(Shared[I].cellCount(), Independent[I].cellCount());
      }
    }
  }
}
} // namespace
