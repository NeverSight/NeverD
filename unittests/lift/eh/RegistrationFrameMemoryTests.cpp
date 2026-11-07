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

#include "neverd/lift/X86/X86Regs.h"

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

TEST(RegistrationFrameMemory, StackReleaseKeepsSurvivingPointerBytes) {
  for (const int32_t Boundary : {-32, INT32_MIN + 4})
    for (int32_t Consumed = 1; Consumed <= 4; ++Consumed) {
      SCOPED_TRACE(Boundary);
      SCOPED_TRACE(Consumed);
      FrameState Frame;
      Frame.store(Boundary - Consumed, 4, FrameValue::frame(-64));
      Frame.store(Boundary + 4, 4, FrameValue::constant(7));
      discardFrameCellsBelow(Frame.Cells, Boundary);
      const auto Value = Frame.load(Boundary, 4);
      EXPECT_EQ(Value.MayBeFrame, Consumed != 4);
      EXPECT_FALSE(Value.Offset);
      EXPECT_EQ(Frame.load(Boundary + 4, 4).Constant, 7u);
    }
}

TEST(RegistrationFrameMemory, CallbackReleaseRevokesInitializationAndIdentity) {
  FrameState Frame;
  Frame.CallbackEntry = 0x401000;
  auto &Stack =
      Frame.Registers[neverd::x86reg::RSP / neverd::x86reg::GeneralRegStride];
  Stack = FrameValue::callbackFrame(0x401000, -36);
  Frame.storeCallback(-33, 4, FrameValue::callbackFrame(0x401000, -64));
  Stack = FrameValue::callbackFrame(0x401000, -32);
  Frame.trimCallbackCells();
  const auto Value = Frame.loadCallback(-32, 3);
  EXPECT_TRUE(Value.MayBeFrame);
  EXPECT_FALSE(Value.CallbackAddress);
  EXPECT_FALSE(Frame.InitializedCallbackBytes.count(-33));
  EXPECT_TRUE(Frame.InitializedCallbackBytes.count(-32));
  EXPECT_TRUE(Frame.callbackMemoryIsPrivate({0x401000, -32}, 3, true));
  Stack = FrameValue::callbackFrame(0x401000, -36);
  Frame.trimCallbackCells();
  EXPECT_FALSE(Frame.callbackMemoryIsPrivate({0x401000, -33}, 4, true));
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

TEST(RegistrationFrameMemory, SEHScalarsRetainAliasTaintAcrossPartialSpills) {
  FrameState Frame;
  Frame.enterSEHFilter(0x1800, -20);
  const auto Pointers = Frame.load(-20, 4);
  const auto Record = Frame.sehExceptionLoad(*Pointers.SEHException, 4);
  ASSERT_TRUE(Record && Record->SEHException);
  const auto Scalar = Frame.sehExceptionLoad(*Record->SEHException, 4);
  ASSERT_TRUE(Scalar);
  ASSERT_TRUE(Scalar->MayBeFrame && Scalar->FrameOnlyFromSEHScalar);
  EXPECT_FALSE(Scalar->Offset);
  Frame.store(-32, 4, *Scalar);
  Frame.store(-31, 1, FrameValue::constant(7));
  EXPECT_TRUE(Frame.load(-32, 4).MayBeFrame);
  EXPECT_TRUE(Frame.load(-31, 1).FrameOnlyFromSEHScalar);
  FrameState Other = Frame;
  Other.store(-32, 4, FrameValue::constant(42));
  Frame.merge(Other);
  EXPECT_TRUE(Frame.load(-32, 4).MayBeFrame);
  EXPECT_TRUE(Frame.load(-32, 4).FrameOnlyFromSEHScalar);
}

TEST(RegistrationFrameMemory, SEHScalarBytesCannotHidePhysicalFrameBits) {
  for (int32_t Offset = -32; Offset != -28; ++Offset)
    for (bool ScalarFirst : {false, true}) {
      SCOPED_TRACE(Offset);
      SCOPED_TRACE(ScalarFirst);
      FrameState Frame;
      const auto Scalar = FrameValue::taint(true, true);
      const auto Pointer = FrameValue::frame(-64);
      Frame.store(-32, 4, ScalarFirst ? Scalar : Pointer);
      Frame.store(Offset, 1, ScalarFirst ? Pointer : Scalar);
      const auto Mixed = Frame.load(-32, 4);
      EXPECT_TRUE(Mixed.MayBeFrame);
      EXPECT_FALSE(Mixed.FrameOnlyFromSEHScalar);
      EXPECT_FALSE(Mixed.Offset);
      const auto Shifted = Frame.load(Offset, 4);
      EXPECT_TRUE(Shifted.MayBeFrame);
      EXPECT_FALSE(Shifted.FrameOnlyFromSEHScalar);
    }
}
} // namespace
