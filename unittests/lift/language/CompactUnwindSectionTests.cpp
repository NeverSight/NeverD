//===- CompactUnwindSectionTests.cpp - whole-section compact unwind tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CompactUnwindTestsDetail.h"
#include "gtest/gtest.h"

#include <cstring>

namespace {

using namespace neverd;
using namespace neverd::macho_unwind;
using namespace neverd::compact_unwind_test;

void putU16(std::vector<uint8_t> &Bytes, size_t Offset, uint16_t Value) {
  ASSERT_LE(Offset + sizeof(Value), Bytes.size());
  std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value));
}

void putU32(std::vector<uint8_t> &Bytes, size_t Offset, uint32_t Value) {
  ASSERT_LE(Offset + sizeof(Value), Bytes.size());
  std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value));
}

void putU16BE(std::vector<uint8_t> &Bytes, size_t Offset, uint16_t Value) {
  ASSERT_LE(Offset + sizeof(Value), Bytes.size());
  llvm::support::endian::write16be(Bytes.data() + Offset, Value);
}

void putU32BE(std::vector<uint8_t> &Bytes, size_t Offset, uint32_t Value) {
  ASSERT_LE(Offset + sizeof(Value), Bytes.size());
  llvm::support::endian::write32be(Bytes.data() + Offset, Value);
}

bool rawParseFails(llvm::ArrayRef<uint8_t> Bytes) {
  auto Parsed = parseCompactUnwindRaw(Bytes);
  if (Parsed)
    return false;
  llvm::consumeError(Parsed.takeError());
  return true;
}

std::vector<uint8_t> buildRawCompressedInfo() {
  constexpr uint32_t CommonOffset = 28;
  constexpr uint32_t PersonalityOffset = CommonOffset + 4;
  constexpr uint32_t IndexOffset = PersonalityOffset + 4;
  constexpr uint32_t LSDAOffset = IndexOffset + 24;
  constexpr uint32_t PageOffset = LSDAOffset + 8;
  constexpr uint32_t LocalEncodingOffset = 20;
  const uint32_t CommonEncoding = kX86_64ModeRBPFrame;
  const uint32_t LocalEncoding =
      kX86_64ModeStackImmediate | kHasLSDA | (1u << kPersonalityShift);

  ByteBuilder B;
  B.u32(kUnwindSectionVersion);
  B.u32(CommonOffset);
  B.u32(1);
  B.u32(PersonalityOffset);
  B.u32(1);
  B.u32(IndexOffset);
  B.u32(2);
  B.u32(CommonEncoding);
  B.u32(0x1800); // image-relative personality pointer slot
  B.u32(0x100);
  B.u32(PageOffset);
  B.u32(LSDAOffset);
  B.u32(0x300);
  B.u32(0);
  B.u32(PageOffset);
  B.u32(0x200);
  B.u32(0x900);
  B.u32(kSecondLevelCompressed);
  B.u16(12);
  B.u16(2);
  B.u16(LocalEncodingOffset);
  B.u16(1);
  B.u32(0);                  // common encoding 0, delta 0
  B.u32((1u << 24) | 0x100); // local encoding 0, delta 0x100
  B.u32(LocalEncoding);
  return B.data();
}

std::vector<uint8_t> buildRawRegularWithLSDAs() {
  constexpr uint32_t IndexOffset = 28;
  constexpr uint32_t LSDAOffset = IndexOffset + 24;
  constexpr uint32_t PageOffset = LSDAOffset + 16;
  const uint32_t Encoding = kX86_64ModeRBPFrame | kHasLSDA;

  ByteBuilder B;
  B.u32(kUnwindSectionVersion);
  B.u32(IndexOffset);
  B.u32(0);
  B.u32(IndexOffset);
  B.u32(0);
  B.u32(IndexOffset);
  B.u32(2);
  B.u32(0x100);
  B.u32(PageOffset);
  B.u32(LSDAOffset);
  B.u32(0x300);
  B.u32(0);
  B.u32(PageOffset);
  B.u32(0x100);
  B.u32(0x800);
  B.u32(0x200);
  B.u32(0x900);
  B.u32(kSecondLevelRegular);
  B.u16(8);
  B.u16(2);
  B.u32(0x100);
  B.u32(Encoding);
  B.u32(0x200);
  B.u32(Encoding);
  return B.data();
}

std::vector<uint8_t> buildOverlappingRawPages() {
  constexpr uint32_t IndexOffset = 28;
  constexpr uint32_t Page0Offset = IndexOffset + 36;
  constexpr uint32_t Page1Offset = Page0Offset + 8;
  std::vector<uint8_t> Bytes(104, 0);

  putU32(Bytes, 0, kUnwindSectionVersion);
  putU32(Bytes, 4, IndexOffset);
  putU32(Bytes, 12, IndexOffset);
  putU32(Bytes, 20, IndexOffset);
  putU32(Bytes, 24, 3);
  putU32(Bytes, 28, 0x100);
  putU32(Bytes, 32, Page0Offset);
  putU32(Bytes, 36, Page0Offset);
  putU32(Bytes, 40, 0x200);
  putU32(Bytes, 44, Page1Offset);
  putU32(Bytes, 48, Page0Offset);
  putU32(Bytes, 52, 0x300);
  putU32(Bytes, 56, 0);
  putU32(Bytes, 60, Page0Offset);

  putU32(Bytes, Page0Offset, kSecondLevelRegular);
  putU16(Bytes, Page0Offset + 4, 32);
  putU16(Bytes, Page0Offset + 6, 1);
  putU32(Bytes, Page0Offset + 32, 0x100);
  putU32(Bytes, Page0Offset + 36, kX86_64ModeRBPFrame);

  putU32(Bytes, Page1Offset, kSecondLevelRegular);
  putU16(Bytes, Page1Offset + 4, 8);
  putU16(Bytes, Page1Offset + 6, 1);
  putU32(Bytes, Page1Offset + 8, 0x200);
  putU32(Bytes, Page1Offset + 12, kX86_64ModeRBPFrame);
  return Bytes;
}

std::vector<uint8_t> buildBigEndianRawRegularInfo() {
  constexpr uint32_t IndexOffset = 28;
  constexpr uint32_t PageOffset = IndexOffset + 24;
  std::vector<uint8_t> Bytes(PageOffset + 16, 0);

  putU32BE(Bytes, 0, kUnwindSectionVersion);
  putU32BE(Bytes, 4, IndexOffset);
  putU32BE(Bytes, 8, 0);
  putU32BE(Bytes, 12, IndexOffset);
  putU32BE(Bytes, 16, 0);
  putU32BE(Bytes, 20, IndexOffset);
  putU32BE(Bytes, 24, 2);

  putU32BE(Bytes, IndexOffset, 0x100);
  putU32BE(Bytes, IndexOffset + 4, PageOffset);
  putU32BE(Bytes, IndexOffset + 8, PageOffset);
  putU32BE(Bytes, IndexOffset + 12, 0x200);
  putU32BE(Bytes, IndexOffset + 16, 0);
  putU32BE(Bytes, IndexOffset + 20, PageOffset);

  putU32BE(Bytes, PageOffset, kSecondLevelRegular);
  putU16BE(Bytes, PageOffset + 4, 8);
  putU16BE(Bytes, PageOffset + 6, 1);
  putU32BE(Bytes, PageOffset + 8, 0x100);
  putU32BE(Bytes, PageOffset + 12, kX86_64ModeRBPFrame);
  return Bytes;
}

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

//===----------------------------------------------------------------------===//
// Strict, lossless decoding for rewrite
//===----------------------------------------------------------------------===//

TEST(CompactUnwindRaw, PreservesRegularEntriesAndTerminalSentinel) {
  const uint32_t Encoding0 = kX86_64ModeRBPFrame;
  const uint32_t Encoding1 = kX86_64ModeStackImmediate;
  std::vector<uint8_t> Bytes =
      buildUnwindInfo({{0x100, Encoding0}, {0x200, Encoding1}}, 0x300);

  auto Parsed = parseCompactUnwindRaw(Bytes);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->OriginalBytes, Bytes);
  EXPECT_EQ(Parsed->Header.IndexSectionOffset, 28u);
  ASSERT_EQ(Parsed->Index.size(), 2u);
  EXPECT_EQ(Parsed->Index[0].FunctionOffset, 0x100u);
  EXPECT_EQ(Parsed->Index[1].FunctionOffset, 0x300u);
  EXPECT_EQ(Parsed->Index[1].SecondLevelPageSectionOffset, 0u);
  ASSERT_EQ(Parsed->Pages.size(), 1u);
  ASSERT_EQ(Parsed->Pages[0].RegularEntries.size(), 2u);
  EXPECT_EQ(Parsed->Pages[0].RegularEntries[0].FunctionOffset, 0x100u);
  EXPECT_EQ(Parsed->Pages[0].RegularEntries[0].Encoding, Encoding0);
  EXPECT_EQ(Parsed->Pages[0].RegularEntries[1].FunctionOffset, 0x200u);
  EXPECT_EQ(Parsed->Pages[0].RegularEntries[1].Encoding, Encoding1);
}

TEST(CompactUnwindRaw, UsesExplicitByteOrder) {
  const std::vector<uint8_t> Bytes = buildBigEndianRawRegularInfo();
  CompactUnwindRawParseOptions Options;
  Options.ByteOrder = llvm::endianness::big;
  auto Parsed = parseCompactUnwindRaw(Bytes, Options);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_EQ(Parsed->Pages.size(), 1u);
  ASSERT_EQ(Parsed->Pages.front().RegularEntries.size(), 1u);
  EXPECT_EQ(Parsed->Pages.front().RegularEntries.front().FunctionOffset,
            0x100u);
  EXPECT_EQ(Parsed->Pages.front().RegularEntries.front().Encoding,
            kX86_64ModeRBPFrame);

  EXPECT_TRUE(rawParseFails(Bytes));
}

TEST(CompactUnwindRaw, AppliesOnlyCallerSelectedResourceCeilings) {
  const std::vector<uint8_t> Bytes = buildUnwindInfo(
      {{0x100, kX86_64ModeRBPFrame}, {0x200, kX86_64ModeStackImmediate}},
      0x300);

  auto Unlimited = parseCompactUnwindRaw(Bytes);
  ASSERT_TRUE(static_cast<bool>(Unlimited))
      << llvm::toString(Unlimited.takeError());

  CompactUnwindRawParseOptions Bounded;
  Bounded.MaxEntries = 1;
  auto Rejected = parseCompactUnwindRaw(Bytes, Bounded);
  EXPECT_FALSE(static_cast<bool>(Rejected));
  llvm::consumeError(Rejected.takeError());

  Bounded.MaxEntries = 2;
  Bounded.MaxPages = 0;
  auto PageRejected = parseCompactUnwindRaw(Bytes, Bounded);
  EXPECT_FALSE(static_cast<bool>(PageRejected));
  llvm::consumeError(PageRejected.takeError());

  Bounded.MaxPages = 1;
  auto Accepted = parseCompactUnwindRaw(Bytes, Bounded);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
}

TEST(CompactUnwindRaw, PreservesCompressedEncodingAndLSDAOrder) {
  std::vector<uint8_t> Bytes = buildRawCompressedInfo();
  auto Parsed = parseCompactUnwindRaw(Bytes);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());

  EXPECT_EQ(Parsed->CommonEncodings,
            (std::vector<uint32_t>{kX86_64ModeRBPFrame}));
  EXPECT_EQ(Parsed->PersonalitySlotOffsets, (std::vector<uint32_t>{0x1800}));
  ASSERT_EQ(Parsed->LSDAEntries.size(), 1u);
  EXPECT_EQ(Parsed->LSDAEntries[0].FunctionOffset, 0x200u);
  EXPECT_EQ(Parsed->LSDAEntries[0].LSDAOffset, 0x900u);
  ASSERT_EQ(Parsed->Pages.size(), 1u);
  const CompactUnwindRawPage &Page = Parsed->Pages.front();
  EXPECT_EQ(Page.Kind, kSecondLevelCompressed);
  EXPECT_EQ(Page.LocalEncodings,
            (std::vector<uint32_t>{kX86_64ModeStackImmediate | kHasLSDA |
                                   (1u << kPersonalityShift)}));
  ASSERT_EQ(Page.CompressedEntries.size(), 2u);
  EXPECT_EQ(Page.CompressedEntries[0].PackedValue, 0u);
  EXPECT_EQ(Page.CompressedEntries[0].EncodingIndex, 0u);
  EXPECT_EQ(Page.CompressedEntries[1].PackedValue, (1u << 24) | 0x100u);
  EXPECT_EQ(Page.CompressedEntries[1].FunctionOffset, 0x200u);
  EXPECT_EQ(Page.CompressedEntries[1].EncodingIndex, 1u);
  EXPECT_EQ(Page.CompressedEntries[1].Encoding, Page.LocalEncodings.front());
}

TEST(CompactUnwindRaw, RejectsInvalidTopLevelTables) {
  EXPECT_TRUE(rawParseFails(std::vector<uint8_t>(27, 0)));

  std::vector<uint8_t> TooManyPersonalities = buildRawCompressedInfo();
  putU32(TooManyPersonalities, 16, 4);
  EXPECT_TRUE(rawParseFails(TooManyPersonalities));

  std::vector<uint8_t> ArrayOutOfBounds = buildRawCompressedInfo();
  putU32(ArrayOutOfBounds, 4,
         static_cast<uint32_t>(ArrayOutOfBounds.size() - 2));
  EXPECT_TRUE(rawParseFails(ArrayOutOfBounds));

  std::vector<uint8_t> ArraysOverlap = buildRawCompressedInfo();
  putU32(ArraysOverlap, 12, 28);
  EXPECT_TRUE(rawParseFails(ArraysOverlap));
}

TEST(CompactUnwindRaw, RejectsInvalidFirstLevelIndexAndSentinel) {
  std::vector<uint8_t> NotIncreasing =
      buildUnwindInfo({{0x100, kX86_64ModeRBPFrame}}, 0x200);
  putU32(NotIncreasing, 40, 0x100);
  EXPECT_TRUE(rawParseFails(NotIncreasing));

  std::vector<uint8_t> EarlySentinel =
      buildUnwindInfo({{0x100, kX86_64ModeRBPFrame}}, 0x200);
  putU32(EarlySentinel, 32, 0);
  EXPECT_TRUE(rawParseFails(EarlySentinel));

  std::vector<uint8_t> MissingTerminalSentinel =
      buildUnwindInfo({{0x100, kX86_64ModeRBPFrame}}, 0x200);
  putU32(MissingTerminalSentinel, 44, 52);
  EXPECT_TRUE(rawParseFails(MissingTerminalSentinel));
}

TEST(CompactUnwindRaw, RejectsInvalidLSDAOrderAndRange) {
  std::vector<uint8_t> NotSorted = buildRawRegularWithLSDAs();
  putU32(NotSorted, 52, 0x200);
  putU32(NotSorted, 60, 0x100);
  EXPECT_TRUE(rawParseFails(NotSorted));

  std::vector<uint8_t> OutsidePageRange = buildRawRegularWithLSDAs();
  putU32(OutsidePageRange, 52, 0x80);
  EXPECT_TRUE(rawParseFails(OutsidePageRange));

  std::vector<uint8_t> MisalignedSlice = buildRawRegularWithLSDAs();
  putU32(MisalignedSlice, 48, 67);
  EXPECT_TRUE(rawParseFails(MisalignedSlice));
}

TEST(CompactUnwindRaw, RejectsOverlappingPagesAndUnsortedEntries) {
  EXPECT_TRUE(rawParseFails(buildOverlappingRawPages()));

  std::vector<uint8_t> Duplicate = buildUnwindInfo(
      {{0x100, kX86_64ModeRBPFrame}, {0x200, kX86_64ModeStackImmediate}},
      0x300);
  putU32(Duplicate, 68, 0x100);
  EXPECT_TRUE(rawParseFails(Duplicate));

  std::vector<uint8_t> Unsorted = buildUnwindInfo(
      {{0x100, kX86_64ModeRBPFrame}, {0x200, kX86_64ModeStackImmediate}},
      0x300);
  putU32(Unsorted, 68, 0x80);
  EXPECT_TRUE(rawParseFails(Unsorted));
}

TEST(CompactUnwindRaw, RejectsInvalidCompressedEntryReferences) {
  std::vector<uint8_t> DeltaOverflow = buildRawCompressedInfo();
  putU32(DeltaOverflow, 36, 0xfffffff0u);
  putU32(DeltaOverflow, 48, 0xffffffffu);
  putU32(DeltaOverflow, 84, 0x00000020u);
  EXPECT_TRUE(rawParseFails(DeltaOverflow));

  std::vector<uint8_t> EncodingIndexOutOfBounds = buildRawCompressedInfo();
  putU32(EncodingIndexOutOfBounds, 84, (2u << 24) | 0x100u);
  EXPECT_TRUE(rawParseFails(EncodingIndexOutOfBounds));

  std::vector<uint8_t> TooManyCommonEncodings = buildRawCompressedInfo();
  putU32(TooManyCommonEncodings, 8, 128);
  EXPECT_TRUE(rawParseFails(TooManyCommonEncodings));

  std::vector<uint8_t> ExhaustedIndexSpace = buildRawCompressedInfo();
  constexpr size_t AddedCommonBytes = 126 * sizeof(uint32_t);
  ExhaustedIndexSpace.insert(ExhaustedIndexSpace.begin() + 32, AddedCommonBytes,
                             0);
  putU32(ExhaustedIndexSpace, 8, 127);
  const uint32_t PersonalityOffset = 32 + AddedCommonBytes;
  const uint32_t IndexOffset = PersonalityOffset + 4;
  const uint32_t LSDAOffset = IndexOffset + 24;
  const uint32_t PageOffset = LSDAOffset + 8;
  putU32(ExhaustedIndexSpace, 12, PersonalityOffset);
  putU32(ExhaustedIndexSpace, 20, IndexOffset);
  putU32(ExhaustedIndexSpace, IndexOffset + 4, PageOffset);
  putU32(ExhaustedIndexSpace, IndexOffset + 8, LSDAOffset);
  putU32(ExhaustedIndexSpace, IndexOffset + 20, PageOffset);
  ExhaustedIndexSpace.resize(PageOffset + 20 + 130 * sizeof(uint32_t));
  putU16(ExhaustedIndexSpace, PageOffset + 10, 130);
  EXPECT_TRUE(rawParseFails(ExhaustedIndexSpace));
}

TEST(CompactUnwindRaw, RejectsEncodingAndSideTableDisagreement) {
  std::vector<uint8_t> MissingPersonality = buildRawCompressedInfo();
  const uint32_t LocalEncoding =
      kX86_64ModeStackImmediate | kHasLSDA | (2u << kPersonalityShift);
  putU32(MissingPersonality, 88, LocalEncoding);
  EXPECT_TRUE(rawParseFails(MissingPersonality));

  std::vector<uint8_t> MissingLSDA = buildRawRegularWithLSDAs();
  // End the global LSDA table after its first record while both functions
  // still advertise kHasLSDA.
  putU32(MissingLSDA, 48, 60);
  EXPECT_TRUE(rawParseFails(MissingLSDA));

  std::vector<uint8_t> OrphanLSDA = buildRawRegularWithLSDAs();
  putU32(OrphanLSDA, 80, kX86_64ModeRBPFrame);
  EXPECT_TRUE(rawParseFails(OrphanLSDA));
}

} // namespace
