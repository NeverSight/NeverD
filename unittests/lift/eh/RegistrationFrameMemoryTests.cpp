//===- RegistrationFrameMemoryTests.cpp - Indexed frame provenance ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve overlapping pointer taint without charging unrelated frame cells.
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationFrame.h"
#include "gtest/gtest.h"

namespace {
using namespace neverd::registration_state;

TEST(RegistrationFrameMemory, LargeFramesKeepExactAndPartialPointerReads) {
  FrameState Frame;
  for (int32_t Offset = -80000; Offset < 0; Offset += 16)
    Frame.store(Offset, 4, FrameValue::frame(Offset + 8));
  EXPECT_EQ(Frame.load(-32, 4).Offset, -24);
  EXPECT_FALSE(Frame.load(-28, 4).MayBeFrame);
  EXPECT_TRUE(Frame.load(-33, 2).MayBeFrame);
  EXPECT_TRUE(Frame.load(std::nullopt, 4).MayBeFrame);
  EXPECT_LT(Frame.memoryAccessWork(FrameValue::frame(-32), 4), 200u);
  EXPECT_GE(Frame.memoryAccessWork({{}, {}, false, true}, 4), 5000u);

  Frame.store(-31, 1, FrameValue::constant(7));
  const auto Partial = Frame.load(-32, 4);
  EXPECT_TRUE(Partial.MayBeFrame);
  EXPECT_FALSE(Partial.Offset);
  EXPECT_EQ(Frame.load(-16, 4).Offset, -8);
  Frame.store(-32, 4, FrameValue::constant(7));
  EXPECT_EQ(Frame.load(-32, 4).Constant, 7u);
  EXPECT_FALSE(Frame.load(-32, 4).MayBeFrame);
}

TEST(RegistrationFrameMemory, FrameCoordinatesHaveSeparateCells) {
  FrameState Frame;
  Frame.CallbackEntry = 0x401000;
  Frame.store(-4, 4, FrameValue::frame(-24));
  Frame.storeEntry(-4, 4, FrameValue::entryFrame(8));
  Frame.storeCallback(-4, 4, FrameValue::callbackFrame(0x401000, -8));
  EXPECT_EQ(Frame.load(-4, 4).Offset, -24);
  EXPECT_EQ(Frame.loadEntry(-4, 4).EntryOffset, 8);
  ASSERT_TRUE(Frame.loadCallback(-4, 4).CallbackAddress);
  EXPECT_EQ(Frame.loadCallback(-4, 4).CallbackAddress->Offset, -8);
  Frame.storeCallback(-4, 4, {});
  EXPECT_FALSE(Frame.loadCallback(-4, 4).MayBeFrame);
  EXPECT_EQ(Frame.load(-4, 4).Offset, -24);
  EXPECT_EQ(Frame.loadEntry(-4, 4).EntryOffset, 8);
}

TEST(RegistrationFrameMemory, OverlapLookupDoesNotWrapSignedCoordinates) {
  FrameState Frame;
  for (int32_t Offset : {INT32_MIN, INT32_MAX - 3})
    Frame.store(Offset, 4, FrameValue::frame(Offset));
  EXPECT_TRUE(Frame.load(INT32_MIN + 1, 2).MayBeFrame);
  EXPECT_TRUE(Frame.load(INT32_MAX, 1).MayBeFrame);
  EXPECT_FALSE(Frame.load(0, 4).MayBeFrame);
  Frame.store(INT32_MAX - 3, 4, {});
  EXPECT_FALSE(Frame.load(INT32_MAX, 1).MayBeFrame);
  EXPECT_EQ(Frame.load(INT32_MIN, 4).Offset, INT32_MIN);
  Frame.store(INT32_MIN + 1, 2, {});
  EXPECT_TRUE(Frame.load(INT32_MIN, 4).MayBeFrame);
  EXPECT_FALSE(Frame.load(INT32_MIN, 4).Offset);
}
} // namespace
