//===- CompactUnwindSectionTests.cpp - whole-section compact unwind tests -===//
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
// Frameless indirect
//===----------------------------------------------------------------------===//

TEST(CompactUnwindIndirect, ReadsTheFrameSizeFromThePrologue) {
  BinaryImage Img = makeImage();
  writeStackSubtract(Img, 0x100, 0x1234);

  // Offset three is where `subq $imm32, %rsp` keeps its immediate; the adjust
  // field adds the pushes the linker counted on top of it.
  const uint32_t Encoding =
      x86_64FramelessEncoding(kX86_64ModeStackIndirect, 3, 2, {CU_RBX, CU_R15});
  attachUnwindInfo(Img, buildUnwindInfo({{0x100, Encoding}}, 0x140));

  ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), 1u);
  const CompactUnwindEntry &Entry = Result.Entries.front();

  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramelessIndirect);
  EXPECT_TRUE(Entry.HasStackSize);
  EXPECT_EQ(Entry.StackSize, 0x1234u + 2u * 8u);
  EXPECT_EQ(slotRegisters(Entry), (std::vector<int>{RBX, R15}));
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(CompactUnwindIndirect, AddsNoAdjustmentWhenTheFieldIsZero) {
  BinaryImage Img = makeImage();
  writeStackSubtract(Img, 0x200, 0x8000);
  attachUnwindInfo(
      Img, buildUnwindInfo({{0x200, x86_64FramelessEncoding(
                                        kX86_64ModeStackIndirect, 3, 0, {})}},
                           0x280));

  ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), 1u);
  EXPECT_TRUE(Result.Entries.front().HasStackSize);
  EXPECT_EQ(Result.Entries.front().StackSize, 0x8000u);
}

TEST(CompactUnwindIndirect, DegradesWhenTheImmediateLeavesTheFunction) {
  BinaryImage Img = makeImage();
  // The range holds two bytes, so the four-byte immediate at offset three
  // would be read out of some neighbouring function.
  attachUnwindInfo(
      Img, buildUnwindInfo({{0x300, x86_64FramelessEncoding(
                                        kX86_64ModeStackIndirect, 3, 0, {})}},
                           0x302));

  ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), 1u);
  EXPECT_FALSE(Result.Entries.front().HasStackSize);
  EXPECT_EQ(Result.Entries.front().StackSize, 0u);
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Partial);
  ASSERT_FALSE(Result.Diagnostics.empty());
}

TEST(CompactUnwindIndirect, DegradesWhenTheTextIsNotMapped) {
  BinaryImage Img = makeImage();
  // A range past the end of the segment: the section still describes it, but
  // no bytes back it.
  attachUnwindInfo(
      Img, buildUnwindInfo({{0x8000, x86_64FramelessEncoding(
                                         kX86_64ModeStackIndirect, 3, 1, {})}},
                           0x8100));

  ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), 1u);
  EXPECT_FALSE(Result.Entries.front().HasStackSize);
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Partial);
}

//===----------------------------------------------------------------------===//
// Whole-section decoding
//===----------------------------------------------------------------------===//

TEST(CompactUnwindSection, DecodesRegistersThroughTheSection) {
  BinaryImage Img = makeImage();
  attachUnwindInfo(
      Img,
      buildUnwindInfo({{0x100, x86_64FrameEncoding(2, {CU_RBX, CU_R15})},
                       {0x200, x86_64FramelessEncoding(
                                   kX86_64ModeStackImmediate, 4, 0, {CU_R12})}},
                      0x300));

  ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), 2u);

  EXPECT_EQ(Result.Entries[0].CodeRange.Begin, kImageBase + 0x100);
  EXPECT_EQ(Result.Entries[0].CodeRange.End, kImageBase + 0x200);
  EXPECT_EQ(Result.Entries[0].SavedGPRMask, gprMask({RBX, R15}));
  EXPECT_EQ(Result.Entries[0].FrameOffset, 16u);

  EXPECT_EQ(Result.Entries[1].SavedGPRMask, gprMask({R12}));
  EXPECT_EQ(Result.Entries[1].StackSize, 32u);
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(CompactUnwindSection, ReportsAnEntryWhoseRegistersDoNotDecode) {
  BinaryImage Img = makeImage();
  attachUnwindInfo(
      Img, buildUnwindInfo({{0x100, x86_64FrameEncoding(1, {CU_RBP})}}, 0x200));

  ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), 1u);
  EXPECT_TRUE(Result.Entries.front().SavedRegisterSlots.empty());
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Partial);
  ASSERT_FALSE(Result.Diagnostics.empty());
}

TEST(CompactUnwindSection, RejectsAnUnsupportedVersion) {
  BinaryImage Img = makeImage();
  attachUnwindInfo(Img, buildUnwindInfo({{0x100, kX86_64ModeRBPFrame}}, 0x200,
                                        /*Version=*/2));

  ParseResult Result = parseCompactUnwind(Img);
  EXPECT_TRUE(Result.Entries.empty());
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Malformed);
}

TEST(CompactUnwindSection, RejectsAnIndexWithNoSentinel) {
  BinaryImage Img = makeImage();
  attachUnwindInfo(Img, buildUnwindInfo({{0x100, kX86_64ModeRBPFrame}}, 0x200,
                                        /*Version=*/1, /*IndexCount=*/1));

  ParseResult Result = parseCompactUnwind(Img);
  EXPECT_TRUE(Result.Entries.empty());
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Malformed);
}

TEST(CompactUnwindSection, RejectsATruncatedSection) {
  BinaryImage Img = makeImage();
  std::vector<uint8_t> Bytes =
      buildUnwindInfo({{0x100, kX86_64ModeRBPFrame}}, 0x200);
  // Keep the header but cut the page it points at.
  Bytes.resize(40);
  attachUnwindInfo(Img, std::move(Bytes));

  ParseResult Result = parseCompactUnwind(Img);
  EXPECT_TRUE(Result.Entries.empty());
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Malformed);
}

TEST(CompactUnwindSection, ModeZeroIsAnAbsenceOfInformation) {
  BinaryImage Img = makeImage();
  attachUnwindInfo(Img, buildUnwindInfo({{0x100, 0}}, 0x200));

  ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), 1u);
  EXPECT_EQ(Result.Entries.front().Kind, CompactUnwindKind::None);
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
}

} // namespace
