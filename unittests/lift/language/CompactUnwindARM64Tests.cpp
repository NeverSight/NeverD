//===- CompactUnwindARM64Tests.cpp - ARM64 compact unwind tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "CompactUnwindTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::macho_unwind;
using namespace neverd::compact_unwind_test;

//===----------------------------------------------------------------------===//
// ARM64
//===----------------------------------------------------------------------===//

TEST(CompactUnwindARM64, DecodesFrameRegisterPairs) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(Arch::AArch64,
                             kARM64ModeFrame | kARM64FrameX19X20Pair |
                                 kARM64FrameX23X24Pair | kARM64FrameD8D9Pair,
                             Entry));

  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramePointer);
  EXPECT_EQ(slotRegisters(Entry), (std::vector<int>{19, 20, 23, 24, 8, 9}));
  EXPECT_EQ(Entry.SavedGPRMask, gprMask({19, 20, 23, 24}));
  // d8 and d9 share their numbers with x8 and x9, which is why the two files
  // cannot be reported in one mask.
  EXPECT_EQ(Entry.SavedFPRMask, gprMask({8, 9}));
  EXPECT_EQ(Entry.FrameOffset, 0u);
}

TEST(CompactUnwindARM64, ReportsEveryPair) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(Arch::AArch64,
                             kARM64ModeFrame | kARM64FrameX19X20Pair |
                                 kARM64FrameX21X22Pair | kARM64FrameX23X24Pair |
                                 kARM64FrameX25X26Pair | kARM64FrameX27X28Pair |
                                 kARM64FrameD8D9Pair | kARM64FrameD10D11Pair |
                                 kARM64FrameD12D13Pair | kARM64FrameD14D15Pair,
                             Entry));

  EXPECT_EQ(slotRegisters(Entry),
            (std::vector<int>{19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 8, 9, 10,
                              11, 12, 13, 14, 15}));
  EXPECT_EQ(Entry.SavedGPRMask,
            gprMask({19, 20, 21, 22, 23, 24, 25, 26, 27, 28}));
  EXPECT_EQ(Entry.SavedFPRMask, gprMask({8, 9, 10, 11, 12, 13, 14, 15}));
}

TEST(CompactUnwindARM64, FramelessLeafStillReportsItsSavedPairs) {
  CompactUnwindEntry Entry;
  // A leaf that saves callee-saved registers without establishing a frame
  // pointer is still frameless, and LLVM's ARM64 backend emits exactly this
  // combination of a stack size and pair bits.
  ASSERT_TRUE(decodeEncoding(Arch::AArch64,
                             kARM64ModeFrameless | (3u << 12) |
                                 kARM64FrameX19X20Pair | kARM64FrameD14D15Pair,
                             Entry));

  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramelessImmediate);
  EXPECT_EQ(Entry.StackSize, 48u);
  EXPECT_TRUE(Entry.HasStackSize);
  EXPECT_EQ(slotRegisters(Entry), (std::vector<int>{19, 20, 14, 15}));
  EXPECT_EQ(Entry.SavedGPRMask, gprMask({19, 20}));
  EXPECT_EQ(Entry.SavedFPRMask, gprMask({14, 15}));
}

TEST(CompactUnwindARM64, DwarfModeCarriesNoRegisters) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(Arch::AArch64, kARM64ModeDwarf | 0x1234, Entry));
  EXPECT_EQ(Entry.Kind, CompactUnwindKind::DwarfFDE);
  EXPECT_EQ(Entry.DwarfFDEOffset, 0x1234u);
  EXPECT_TRUE(Entry.SavedRegisterSlots.empty());
  EXPECT_FALSE(Entry.HasStackSize);
}

} // namespace
