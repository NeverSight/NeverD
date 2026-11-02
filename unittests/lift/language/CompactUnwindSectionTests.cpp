//===- CompactUnwindSectionTests.cpp - whole-section compact unwind tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CompactUnwindTestsDetail.h"
#include "LanguageEHTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/loader/MachO/MachOExceptions.h"
#include "neverd/loader/SourceUnwind.h"

#include <cstring>
#include <limits>

namespace {

using namespace neverd;
using namespace neverd::macho_unwind;
using namespace neverd::compact_unwind_test;

namespace fixture {
#define NEVERD_COMPACT_ARM64_FIXTURE_VALUE(Type, Name, Value)                  \
  [[maybe_unused]] constexpr Type Name = Value;
#include "CompactUnwindARM64Fixtures.def"
#define NEVERD_COMPACT_PAGE_FIXTURE_VALUE(Type, Name, Value)                   \
  [[maybe_unused]] constexpr Type Name = Value;
#include "CompactUnwindPageFixtures.def"

struct PageMode {
  Arch TargetArch;
  uint32_t Encoding;
};
constexpr PageMode PageModes[] = {
#define NEVERD_COMPACT_PAGE_FIXTURE_MODE(TargetArch, Encoding)                 \
  {Arch::TargetArch, Encoding},
#include "CompactUnwindPageFixtures.def"
};
} // namespace fixture

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

std::vector<uint8_t> buildPageBoundaryInfo(uint32_t Encoding,
                                           bool MultiplePages = false) {
  std::vector<uint8_t> Bytes(MultiplePages ? fixture::kMultiPageSectionSize
                                           : fixture::kSinglePageSectionSize);
  if (MultiplePages) {
#define NEVERD_COMPACT_PAGE_FIXTURE_MULTI_WORD(Offset, Value)                  \
  putU32(Bytes, Offset, Value);
#define NEVERD_COMPACT_PAGE_FIXTURE_MULTI_HALF(Offset, Value)                  \
  putU16(Bytes, Offset, Value);
#include "CompactUnwindPageFixtures.def"
    putU32(Bytes, fixture::kMultiFirstEncodingOffset, Encoding);
    putU32(Bytes, fixture::kMultiLastEncodingOffset, Encoding);
  } else {
#define NEVERD_COMPACT_PAGE_FIXTURE_SINGLE_WORD(Offset, Value)                 \
  putU32(Bytes, Offset, Value);
#define NEVERD_COMPACT_PAGE_FIXTURE_SINGLE_HALF(Offset, Value)                 \
  putU16(Bytes, Offset, Value);
#include "CompactUnwindPageFixtures.def"
  }
  putU32(Bytes, fixture::kPageFixtureEncodingOffset, Encoding);
  return Bytes;
}

void expectRejectedPage(std::vector<uint8_t> Bytes, Arch TargetArch,
                        llvm::StringRef ExpectedDiagnostic) {
  auto Raw = parseCompactUnwindRaw(Bytes);
  ASSERT_FALSE(Raw);
  const std::string RawDiagnostic = llvm::toString(Raw.takeError());
  EXPECT_NE(RawDiagnostic.find(ExpectedDiagnostic.str()), std::string::npos);
  BinaryImage Img = makeImage(TargetArch);
  attachUnwindInfo(Img, std::move(Bytes));
  const ParseResult Parsed = parseCompactUnwind(Img);
  EXPECT_EQ(Parsed.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_TRUE(Parsed.Entries.empty());
  EXPECT_NE(std::find(Parsed.Diagnostics.begin(), Parsed.Diagnostics.end(),
                      ExpectedDiagnostic.str()),
            Parsed.Diagnostics.end());
  parseDarwinExceptions(Img);
  EXPECT_TRUE(Img.ExceptionMetadata.Functions.empty());
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

TEST(CompactUnwindSection, PAuthLRSigningPCUsesEachRegularRecipeStart) {
  BinaryImage Img = makeImage(Arch::AArch64);
  attachUnwindInfo(
      Img,
      buildUnwindInfo({{fixture::kFirstRecipeRVA, fixture::kPAuthLRNextPair},
                       {fixture::kSecondRecipeRVA, fixture::kPAuthLRNextPair}},
                      fixture::kTerminalRVA));
  const ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), fixture::kTwoEntryCount);
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
  for (const auto &Entry : Result.Entries) {
    EXPECT_EQ(Entry.NativeEncoding, fixture::kPAuthLRNextPair);
    EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramePointer);
    ASSERT_TRUE(Entry.ReturnAddressSigningOffset);
    EXPECT_EQ(*Entry.ReturnAddressSigningOffset, fixture::kNextSigningOffset);
  }
  const auto &First = Result.Entries.front();
  const auto &Second = Result.Entries.back();
  EXPECT_EQ(First.CodeRange.Begin + *First.ReturnAddressSigningOffset,
            kImageBase + fixture::kFirstRecipeSigningRVA);
  EXPECT_EQ(Second.CodeRange.Begin + *Second.ReturnAddressSigningOffset,
            kImageBase + fixture::kSecondRecipeSigningRVA);
}

TEST(CompactUnwindSection,
     PAuthLRCompressedCommonAndLocalRecipesKeepSigningPC) {
  BinaryImage Img = makeImage(Arch::AArch64);
  std::vector<uint8_t> Bytes = buildRawCompressedInfo();
  // Replace only the fixture's common/local encoding words, preserving its
  // personality and LSDA linkage.
  putU32(Bytes, fixture::kCompressedCommonEncodingOffset,
         fixture::kPAuthLRFirstPair);
  putU32(Bytes, fixture::kCompressedLocalEncodingOffset,
         fixture::kPAuthLRCompressedLocal);
  attachUnwindInfo(Img, std::move(Bytes));
  const ParseResult Result = parseCompactUnwind(Img);
  ASSERT_EQ(Result.Entries.size(), fixture::kTwoEntryCount);
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
  const auto &First = Result.Entries.front();
  const auto &Second = Result.Entries.back();
  ASSERT_TRUE(First.ReturnAddressSigningOffset);
  EXPECT_EQ(*First.ReturnAddressSigningOffset, fixture::kFirstSigningOffset);
  ASSERT_TRUE(Second.ReturnAddressSigningOffset);
  EXPECT_EQ(*Second.ReturnAddressSigningOffset, fixture::kLocalSigningOffset);
  EXPECT_EQ(Second.CodeRange.Begin + *Second.ReturnAddressSigningOffset,
            kImageBase + fixture::kCompressedLocalSigningRVA);
  EXPECT_EQ(Second.NativeEncoding, fixture::kPAuthLRCompressedLocal);
}

TEST(CompactUnwindSection, PAuthLRBadSigningRangeCannotBecomePlainUnwind) {
  struct RangeCase {
    uint32_t Begin;
    uint32_t End;
    uint32_t Encoding;
  };
  const RangeCase Cases[] = {
#define NEVERD_COMPACT_ARM64_FIXTURE_BAD_SECTION_RANGE(Begin, End, Encoding)   \
  {Begin, End, Encoding},
#include "CompactUnwindARM64Fixtures.def"
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Begin);
    SCOPED_TRACE(Case.End);
    BinaryImage Img = makeImage(Arch::AArch64);
    attachUnwindInfo(Img,
                     buildUnwindInfo({{Case.Begin, Case.Encoding}}, Case.End));
    const ParseResult Result = parseCompactUnwind(Img);
    ASSERT_EQ(Result.Entries.size(), fixture::kOneEntryCount);
    EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Partial);
    EXPECT_EQ(Result.Entries.front().SemanticStatus,
              CompactUnwindSemanticStatus::Partial);
    EXPECT_NE(std::find(Result.Diagnostics.begin(), Result.Diagnostics.end(),
                        kARM64PAuthLRInvalidSigningRange),
              Result.Diagnostics.end());
    parseDarwinExceptions(Img);
    const ExceptionFunction *Function =
        Img.ExceptionMetadata.findFunction(kImageBase + Case.Begin);
    ASSERT_NE(Function, nullptr);
    EXPECT_FALSE(isPlainSourceUnwind(*Function));
  }
}

TEST(CompactUnwindSection, PAuthLRImageRelativeRangeCannotWrapAddressSpace) {
  BinaryImage Img = makeImage(Arch::AArch64);
  Img.Base = std::numeric_limits<va_t>::max() - fixture::kOverflowImageHeadroom;
  Img.Segments.front().VA = Img.Base;
  attachUnwindInfo(
      Img, buildUnwindInfo({{fixture::kFirstRecipeRVA, fixture::kPAuthLRFirst}},
                           fixture::kSecondRecipeRVA));
  const ParseResult Result = parseCompactUnwind(Img);
  EXPECT_TRUE(Result.Entries.empty());
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_NE(std::find(Result.Diagnostics.begin(), Result.Diagnostics.end(),
                      kCompactUnwindCodeRangeOverflow),
            Result.Diagnostics.end());
}

//===----------------------------------------------------------------------===//
// Darwin function symbols
//===----------------------------------------------------------------------===//

TEST(CompactUnwindSection, CompressedPageAddressesUseCheckedSharedValidation) {
  struct InvalidCase {
    const char *Name;
    uint32_t Begin;
    uint32_t End;
    uint32_t First;
    uint32_t Second;
    const char *Diagnostic;
  };
  const InvalidCase Cases[] = {
#define NEVERD_COMPACT_PAGE_FIXTURE_INVALID_COMPRESSED(                        \
    Name, Begin, End, First, Second, Diagnostic)                               \
  {#Name, Begin, End, First, Second, Diagnostic},
#include "CompactUnwindPageFixtures.def"
  };
  for (const auto &Mode : fixture::PageModes) {
    SCOPED_TRACE(Mode.Encoding);
    for (const auto &Case : Cases) {
      SCOPED_TRACE(Case.Name);
      auto Bytes = buildPageBoundaryInfo(Mode.Encoding);
      putU32(Bytes, fixture::kSinglePageBeginOffset, Case.Begin);
      putU32(Bytes, fixture::kSinglePageEndOffset, Case.End);
      putU32(Bytes, fixture::kSingleFirstPackedOffset, Case.First);
      putU32(Bytes, fixture::kSingleSecondPackedOffset, Case.Second);
      expectRejectedPage(std::move(Bytes), Mode.TargetArch, Case.Diagnostic);
    }
  }
}

TEST(CompactUnwindSection, RegularPageAddressesUseCheckedSharedValidation) {
  struct InvalidCase {
    const char *Name;
    uint32_t First;
    uint32_t Second;
    const char *Diagnostic;
  };
  const InvalidCase Cases[] = {
#define NEVERD_COMPACT_PAGE_FIXTURE_INVALID_REGULAR(Name, First, Second,       \
                                                    Diagnostic)                \
  {#Name, First, Second, Diagnostic},
#include "CompactUnwindPageFixtures.def"
  };
  for (const auto &Mode : fixture::PageModes) {
    SCOPED_TRACE(Mode.Encoding);
    for (const auto &Case : Cases) {
      SCOPED_TRACE(Case.Name);
      auto Bytes = buildUnwindInfo(
          {{Case.First, Mode.Encoding}, {Case.Second, Mode.Encoding}},
          fixture::kPageFixtureEnd);
      putU32(Bytes, fixture::kRegularPageIndexOffset,
             fixture::kPageFixtureBegin);
      expectRejectedPage(std::move(Bytes), Mode.TargetArch, Case.Diagnostic);
    }
  }
}

TEST(CompactUnwindSection, InvalidIndexCannotSupplySemanticPageBounds) {
  struct InvalidIndex {
    size_t Offset;
    uint32_t Value;
  };
  const InvalidIndex Cases[] = {
#define NEVERD_COMPACT_PAGE_FIXTURE_INVALID_INDEX(Offset, Value)               \
  {Offset, Value},
#include "CompactUnwindPageFixtures.def"
  };
  for (const auto &Case : Cases) {
    auto Bytes = buildPageBoundaryInfo(fixture::kPAuthLRFirst);
    putU32(Bytes, Case.Offset, Case.Value);
    EXPECT_TRUE(rawParseFails(Bytes));
    BinaryImage Img = makeImage(Arch::AArch64);
    attachUnwindInfo(Img, std::move(Bytes));
    const ParseResult Parsed = parseCompactUnwind(Img);
    EXPECT_EQ(Parsed.ParseStatus, ExceptionParseStatus::Malformed);
    EXPECT_TRUE(Parsed.Entries.empty());
  }
}

TEST(CompactUnwindSection, OmittedPageDoesNotWidenAdjacentValidRecipes) {
  for (const auto &Mode : fixture::PageModes) {
    SCOPED_TRACE(Mode.Encoding);
    auto Bytes = buildPageBoundaryInfo(Mode.Encoding, true);
    EXPECT_FALSE(rawParseFails(Bytes));
    BinaryImage ValidImg = makeImage(Mode.TargetArch);
    attachUnwindInfo(ValidImg, Bytes);
    const ParseResult Valid = parseCompactUnwind(ValidImg);
    EXPECT_EQ(Valid.ParseStatus, ExceptionParseStatus::Complete);
    ASSERT_EQ(Valid.Entries.size(), fixture::kValidMultiPageEntryCount);

    putU32(Bytes, fixture::kMultiSecondPackedOffset,
           fixture::kMultiInvalidSecondPacked);
    EXPECT_TRUE(rawParseFails(Bytes));
    BinaryImage Img = makeImage(Mode.TargetArch);
    attachUnwindInfo(Img, std::move(Bytes));
    const ParseResult Parsed = parseCompactUnwind(Img);
    EXPECT_EQ(Parsed.ParseStatus, ExceptionParseStatus::Partial);
    ASSERT_EQ(Parsed.Entries.size(), fixture::kRetainedPageEntryCount);
    EXPECT_EQ(Parsed.Entries.front().CodeRange.Begin,
              kImageBase + fixture::kRetainedFirstBegin);
    EXPECT_EQ(Parsed.Entries.front().CodeRange.End,
              kImageBase + fixture::kRetainedFirstEnd);
    EXPECT_EQ(Parsed.Entries.back().CodeRange.Begin,
              kImageBase + fixture::kRetainedLastBegin);
    EXPECT_EQ(Parsed.Entries.back().CodeRange.End,
              kImageBase + fixture::kRetainedLastEnd);
    for (const auto &Entry : Parsed.Entries)
      EXPECT_EQ(Entry.SemanticStatus, CompactUnwindSemanticStatus::Complete);
  }
}

TEST(DarwinUnwindSymbols, PromotesAllAliasesAfterSymbolVectorGrowth) {
  BinaryImage Img = makeImage();
  const va_t NewVA = kImageBase + 0x100;
  const va_t AliasVA = kImageBase + 0x200;
  Img.Symbols = {{"_first_zero", AliasVA, 0, false},
                 {"_sized", AliasVA, 7, false},
                 {"_last_zero", AliasVA, 0, false}};

  // Fill the observed capacity: reserve alone does not promise an exact one.
  // The first range must then grow the vector before the aliases are visited.
  const size_t OriginalCapacity = Img.Symbols.capacity();
  while (Img.Symbols.size() < OriginalCapacity)
    Img.Symbols.push_back({"_unrelated", kImageBase + 0x1000, 3, false});
  const std::vector<Symbol> OriginalSymbols = Img.Symbols;
  ASSERT_EQ(Img.Symbols.size(), OriginalCapacity);
  const std::vector<RawEntry> Entries = {{0x100, kX86_64ModeRBPFrame},
                                         {0x200, kX86_64ModeRBPFrame}};
  attachUnwindInfo(Img, buildUnwindInfo(Entries, 0x280));

  parseDarwinExceptions(Img);

  ASSERT_EQ(Img.Symbols.size(), OriginalCapacity + 1);
  EXPECT_GT(Img.Symbols.capacity(), OriginalCapacity);
  for (size_t I = 0; I < OriginalSymbols.size(); ++I) {
    SCOPED_TRACE(I);
    const Symbol &Before = OriginalSymbols[I];
    const Symbol &After = Img.Symbols[I];
    EXPECT_EQ(After.Name, Before.Name);
    EXPECT_EQ(After.Addr, Before.Addr);
    EXPECT_EQ(After.IsFunc, Before.Addr == AliasVA);
    EXPECT_EQ(After.Size,
              Before.Addr == AliasVA && Before.Size == 0 ? 0x80u : Before.Size);
  }
  const Symbol &Created = Img.Symbols.back();
  EXPECT_EQ(Created.Addr, NewVA);
  EXPECT_EQ(Created.Size, 0x100u);
  EXPECT_TRUE(Created.IsFunc);
  EXPECT_FALSE(Created.Name.empty());
  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 2u);
  EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, ExceptionParseStatus::Complete);
  const std::vector<std::pair<va_t, va_t>> ExpectedRanges = {
      {NewVA, AliasVA}, {AliasVA, kImageBase + 0x280}};
  EXPECT_EQ(Img.KnownCodeRanges, ExpectedRanges);
}

TEST(DarwinUnwindSymbols, ReusesNewSymbolForLaterSameAddressFDE) {
  BinaryImage Img = makeImage();
  const va_t FunctionVA = kImageBase + 0x100;
  const va_t FrameVA = kImageBase + 0x2800;
  auto First =
      language_eh_test::buildSimpleFrame(FrameVA, FunctionVA, 0x40, "zR", 0, 0);
  std::vector<uint8_t> Bytes = First.Bytes;
  ASSERT_GE(Bytes.size(), sizeof(uint32_t));
  Bytes.resize(Bytes.size() - sizeof(uint32_t));
  const size_t SecondOffset = Bytes.size();
  // Each chunk retains its own CIE. Its PC-relative fields must be encoded
  // for the chunk's actual position; only the last terminator is retained.
  auto Second = language_eh_test::buildSimpleFrame(
      FrameVA + SecondOffset, FunctionVA, 0x80, "zR", 0, 0);
  Bytes.insert(Bytes.end(), Second.Bytes.begin(), Second.Bytes.end());

  Section Frames;
  Frames.Name = "__eh_frame";
  Frames.SegmentName = "__TEXT";
  Frames.VA = FrameVA;
  Frames.Size = Bytes.size();
  Frames.Flags = SegmentFlags::Readable;
  Frames.Data = std::move(Bytes);
  Img.Sections.push_back(std::move(Frames));

  // No compact entry consumes either FDE before the standalone FDE loop.
  parseDarwinExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.CIEs.size(), 2u);
  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 2u);
  EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, ExceptionParseStatus::Complete);
  for (size_t I = 0; I < 2; ++I) {
    SCOPED_TRACE(I);
    const ExceptionFunction &Function = Img.ExceptionMetadata.Functions[I];
    ASSERT_TRUE(Function.Dwarf.has_value());
    EXPECT_FALSE(Function.Compact.has_value());
    EXPECT_EQ(Function.Encoding, ExceptionEncoding::DwarfFDE);
    EXPECT_EQ(Function.CodeRange.Begin, FunctionVA);
    EXPECT_EQ(Function.CodeRange.End, FunctionVA + (I == 0 ? 0x40u : 0x80u));
    EXPECT_EQ(Function.Dwarf->SectionOffset,
              I == 0 ? First.FDEOffset : SecondOffset + Second.FDEOffset);
  }
  ASSERT_EQ(Img.Symbols.size(), 1u);
  EXPECT_EQ(Img.Symbols.front().Addr, FunctionVA);
  EXPECT_EQ(Img.Symbols.front().Size, 0x40u);
  EXPECT_TRUE(Img.Symbols.front().IsFunc);
  const std::vector<std::pair<va_t, va_t>> ExpectedRanges = {
      {FunctionVA, FunctionVA + 0x40}, {FunctionVA, FunctionVA + 0x80}};
  EXPECT_EQ(Img.KnownCodeRanges, ExpectedRanges);
}

TEST(DarwinUnwindSymbols, KeepsNonExecutableRangesWithoutChangingSymbols) {
  for (bool Mapped : {false, true}) {
    for (bool HasSymbol : {false, true}) {
      SCOPED_TRACE(Mapped ? "read-only segment" : "unmapped range");
      SCOPED_TRACE(HasSymbol ? "existing symbol" : "no symbol");
      BinaryImage Img = makeImage();
      const va_t FunctionVA = kImageBase + 0x8000;
      if (Mapped) {
        Segment Data;
        Data.Name = "__DATA";
        Data.VA = FunctionVA;
        Data.Size = 0x100;
        Data.Flags = SegmentFlags::Readable;
        Data.Data.assign(0x100, 0);
        Img.Segments.push_back(std::move(Data));
      }
      if (HasSymbol)
        Img.Symbols.push_back({"_data", FunctionVA, 0, false});
      attachUnwindInfo(
          Img, buildUnwindInfo({{0x8000, kX86_64ModeRBPFrame}}, 0x8080));

      parseDarwinExceptions(Img);

      ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
      const ExceptionFunction &Function = Img.ExceptionMetadata.Functions[0];
      ASSERT_TRUE(Function.Compact.has_value());
      EXPECT_EQ(Function.CodeRange.Begin, FunctionVA);
      EXPECT_EQ(Function.CodeRange.End, FunctionVA + 0x80);
      const std::vector<std::pair<va_t, va_t>> ExpectedRanges = {
          {FunctionVA, FunctionVA + 0x80}};
      EXPECT_EQ(Img.KnownCodeRanges, ExpectedRanges);
      ASSERT_EQ(Img.Symbols.size(), HasSymbol ? 1u : 0u);
      if (HasSymbol) {
        EXPECT_EQ(Img.Symbols.front().Name, "_data");
        EXPECT_EQ(Img.Symbols.front().Addr, FunctionVA);
        EXPECT_EQ(Img.Symbols.front().Size, 0u);
        EXPECT_FALSE(Img.Symbols.front().IsFunc);
      }
    }
  }
}

TEST(DarwinUnwindSymbols,
     IncompleteFrameSectionsCoverConsumedAndStandaloneFDEs) {
  for (Arch A : {Arch::X64, Arch::AArch64})
    for (unsigned Binding : {0u, 1u, 2u})
      for (unsigned Damage : {0u, 1u, 2u})
        for (bool BadFirst : {false, true}) {
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(Binding);
          SCOPED_TRACE(Damage);
          SCOPED_TRACE(BadFirst);
          auto Img = makeImage(A);
          const va_t FrameVA = kImageBase + 0x2800;
          std::vector<uint8_t> Bytes;
          std::vector<uint32_t> Offsets;
          for (unsigned I = 0; I < 2; ++I) {
            const bool Bad = (I == 0) == BadFirst;
            auto Frame = language_eh_test::buildSimpleFrame(
                FrameVA + Bytes.size(), kImageBase + 0x100 + I * 0x100, 0x80,
                Bad && Damage == 0   ? "zRX"
                : Bad && Damage == 1 ? "zRL"
                                     : "zR",
                0, Bad && Damage == 1 ? kImageBase + 0xdead000 : 0,
                Bad && Damage == 1 ? 0x9b : 0x1b);
            Offsets.push_back(Bytes.size() + Frame.FDEOffset);
            Frame.Bytes.resize(Frame.Bytes.size() - sizeof(uint32_t));
            Bytes.insert(Bytes.end(), Frame.Bytes.begin(), Frame.Bytes.end());
          }
          if (Damage == 2) {
            language_eh_test::ByteBuilder Truncated;
            Truncated.u32(0x1000);
            Truncated.u32(0);
            Bytes.insert(Bytes.end(), Truncated.data().begin(),
                         Truncated.data().end());
          } else
            Bytes.resize(Bytes.size() + sizeof(uint32_t));
          Section Frames;
          Frames.Name = "__eh_frame";
          Frames.VA = FrameVA;
          Frames.Size = Bytes.size();
          Frames.Data = Bytes;
          Img.Sections.push_back(std::move(Frames));
          const uint32_t PlainMode =
              A == Arch::X64 ? kX86_64ModeRBPFrame : kARM64ModeFrame;
          const uint32_t DwarfMode =
              A == Arch::X64 ? kX86_64ModeDwarf : kARM64ModeDwarf;
          if (Binding)
            attachUnwindInfo(
                Img,
                buildUnwindInfo(
                    {{0x100, Binding == 1 ? PlainMode : DwarfMode | Offsets[0]},
                     {0x200, Binding == 1 ? PlainMode : DwarfMode | Offsets[1]},
                     {0x300, PlainMode}},
                    0x380));
          parseDarwinExceptions(Img);
          ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), Binding ? 3u : 2u);
          const auto Status = Damage == 2 ? ExceptionParseStatus::Malformed
                                          : ExceptionParseStatus::Partial;
          EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, Status);
          for (const auto &F : Img.ExceptionMetadata.Functions) {
            if (!F.Dwarf) {
              ASSERT_TRUE(F.Compact);
              EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
              EXPECT_FALSE(F.DecodeProvenance);
              EXPECT_TRUE(isPlainSourceUnwind(F));
              continue;
            }
            EXPECT_EQ(F.ParseStatus, Status);
            ASSERT_TRUE(F.DecodeProvenance);
            EXPECT_EQ(F.DecodeProvenance->Structural.ParseStatus, Status);
            EXPECT_FALSE(isPlainSourceUnwind(F));
          }
        }
}

TEST(DarwinUnwindSymbols, ConsumedFDEKeepsItsCIEPersonality) {
  for (bool DwarfMode : {false, true})
    for (bool Known : {false, true}) {
      auto Img = makeImage();
      const va_t FuncVA = kImageBase + 0x100;
      const va_t PersonalityVA = kImageBase + 0x500;
      if (Known)
        Img.Symbols.push_back(
            {"___gxx_personality_v0", PersonalityVA, 0, true});
      const auto Frame = language_eh_test::buildSimpleFrame(
          kImageBase + 0x2800, FuncVA, 0x80, "zPR", PersonalityVA, 0);
      Section Frames;
      Frames.Name = "__eh_frame";
      Frames.VA = kImageBase + 0x2800;
      Frames.Size = Frame.Bytes.size();
      Frames.Data = Frame.Bytes;
      Img.Sections.push_back(std::move(Frames));
      attachUnwindInfo(
          Img,
          buildUnwindInfo(
              {{0x100, DwarfMode ? kX86_64ModeDwarf | uint32_t(Frame.FDEOffset)
                                 : kX86_64ModeRBPFrame}},
              0x180));
      parseDarwinExceptions(Img);
      ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
      const auto &F = Img.ExceptionMetadata.Functions.front();
      ASSERT_TRUE(F.Dwarf);
      ASSERT_TRUE(F.Compact);
      EXPECT_EQ(F.PersonalityVA, PersonalityVA);
      EXPECT_NE(F.Personality, ExceptionPersonality::None);
      EXPECT_EQ(F.ParseStatus, Known ? ExceptionParseStatus::Complete
                                     : ExceptionParseStatus::Partial);
      EXPECT_FALSE(isPlainSourceUnwind(F));
      // A complete section must not introduce language-refresh authority.
      EXPECT_FALSE(F.DecodeProvenance);
    }
}

TEST(DarwinUnwindSymbols, CompactAndDwarfLanguageDescriptionsMustAgree) {
  for (bool Personality : {false, true})
    for (bool Agree : {false, true}) {
      SCOPED_TRACE(Personality);
      SCOPED_TRACE(Agree);
      auto Img = makeImage();
      const va_t FrameVA = kImageBase + 0x2800;
      const va_t PersonalityVA = kImageBase + 0x500;
      std::vector<uint8_t> CompactBytes;
      if (Personality) {
        Img.Symbols.push_back(
            {"___gxx_personality_v0", PersonalityVA, 0, true});
        Img.Symbols.push_back(
            {"___objc_personality_v0", PersonalityVA + 0x100, 0, true});
        ASSERT_TRUE(Img.patchPtr(kImageBase + 0x1800,
                                 PersonalityVA + (Agree ? 0 : 0x100)));
        CompactBytes = buildUnwindInfo(
            {{0x100, kX86_64ModeRBPFrame | (1u << kPersonalityShift)}}, 0x180);
        const uint32_t PersonalityOffset = CompactBytes.size();
        CompactBytes.resize(CompactBytes.size() + 4);
        putU32(CompactBytes, 12, PersonalityOffset);
        putU32(CompactBytes, 16, 1);
        putU32(CompactBytes, PersonalityOffset, 0x1800);
      } else {
        CompactBytes = buildRawRegularWithLSDAs();
        const std::vector<uint8_t> EmptyLSDA = {0xff, 0xff, 0x01, 0};
        ASSERT_TRUE(Img.writeVA(kImageBase + 0x800, EmptyLSDA.data(),
                                EmptyLSDA.size()));
        ASSERT_TRUE(Img.writeVA(kImageBase + 0x900, EmptyLSDA.data(),
                                EmptyLSDA.size()));
      }
      attachUnwindInfo(Img, std::move(CompactBytes));
      const auto Frame = language_eh_test::buildSimpleFrame(
          FrameVA, kImageBase + 0x100, Personality ? 0x80 : 0x100,
          Personality ? "zPR" : "zLR", Personality ? PersonalityVA : 0,
          Personality ? 0 : kImageBase + (Agree ? 0x800 : 0x900));
      Section Frames;
      Frames.Name = "__eh_frame";
      Frames.VA = FrameVA;
      Frames.Size = Frame.Bytes.size();
      Frames.Data = Frame.Bytes;
      Img.Sections.push_back(std::move(Frames));
      parseDarwinExceptions(Img);
      const auto *F =
          Img.ExceptionMetadata.findFunctionEntry(kImageBase + 0x100);
      ASSERT_TRUE(F);
      ASSERT_TRUE(F->Dwarf);
      ASSERT_TRUE(F->Compact);
      EXPECT_EQ(F->ParseStatus, Agree ? ExceptionParseStatus::Complete
                                      : ExceptionParseStatus::Partial);
      EXPECT_FALSE(isPlainSourceUnwind(*F));
      if (!Agree)
        EXPECT_TRUE(std::any_of(
            F->Diagnostics.begin(), F->Diagnostics.end(), [](const auto &D) {
              return D.find("disagree") != std::string::npos;
            }));
    }
}

TEST(DarwinUnwindSymbols, MissingDeclaredPersonalityCannotBecomePlain) {
  for (bool Declared : {false, true}) {
    auto Img = makeImage();
    attachUnwindInfo(
        Img, buildUnwindInfo(
                 {{0x100, kX86_64ModeRBPFrame |
                              (Declared ? 1u << kPersonalityShift : 0u)}},
                 0x180));
    parseDarwinExceptions(Img);
    ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
    const auto &F = Img.ExceptionMetadata.Functions.front();
    ASSERT_TRUE(F.Compact);
    EXPECT_EQ(F.ParseStatus, Declared ? ExceptionParseStatus::Partial
                                      : ExceptionParseStatus::Complete);
    EXPECT_EQ(F.Personality, Declared ? ExceptionPersonality::Unknown
                                      : ExceptionPersonality::None);
    EXPECT_EQ(isPlainSourceUnwind(F), !Declared);
  }
}

TEST(DarwinUnwindSymbols, EqualFileValuesDoNotEquateDifferentBoundProviders) {
  for (unsigned Binding : {0u, 1u, 2u, 3u})
    for (bool SameSlot : {false, true})
      for (bool SameName : {false, true}) {
        SCOPED_TRACE(Binding);
        SCOPED_TRACE(SameSlot);
        SCOPED_TRACE(SameName);
        auto Img = makeImage();
        const va_t ProviderVA = kImageBase + 0x500;
        const va_t CompactSlot = kImageBase + 0x1800;
        const va_t CIESlot = SameSlot ? CompactSlot : CompactSlot + 8;
        ASSERT_TRUE(Img.patchPtr(CompactSlot, ProviderVA));
        ASSERT_TRUE(Img.patchPtr(CIESlot, ProviderVA));
        const std::string OtherName = SameSlot || SameName
                                          ? "___gxx_personality_v0"
                                          : "___objc_personality_v0";
        for (auto [Slot, Name] :
             {std::pair{CompactSlot, std::string("___gxx_personality_v0")},
              std::pair{CIESlot, OtherName}}) {
          if (Binding == 0)
            Img.ImportPtrSlots[Slot] = Name;
          else if (Binding == 1) {
            Import Imp;
            Imp.IATAddr = Slot;
            Imp.Name = Name;
            Img.Imports.push_back(std::move(Imp));
          } else {
            RelocationEntry Rel;
            Rel.Address = Slot;
            if (Binding == 2)
              Rel.SymbolName = Name;
            else {
              Rel.Addend = ProviderVA + (Slot == CompactSlot ? 0x10 : 0x20);
              Img.Symbols.push_back({Name, va_t(Rel.Addend), 0, true});
            }
            Img.Relocations.push_back(std::move(Rel));
          }
        }
        auto CompactBytes = buildUnwindInfo(
            {{0x100, kX86_64ModeRBPFrame | (1u << kPersonalityShift)}}, 0x180);
        const uint32_t ArrayOffset = CompactBytes.size();
        CompactBytes.resize(ArrayOffset + 4);
        putU32(CompactBytes, 12, ArrayOffset);
        putU32(CompactBytes, 16, 1);
        putU32(CompactBytes, ArrayOffset, 0x1800);
        attachUnwindInfo(Img, std::move(CompactBytes));
        auto Frame = language_eh_test::buildSimpleFrame(
            kImageBase + 0x2800, kImageBase + 0x100, 0x80, "zPR", CIESlot, 0);
        const std::vector<uint8_t> Augmentation = {'z', 'P', 'R', 0};
        const auto Aug = std::search(Frame.Bytes.begin(), Frame.Bytes.end(),
                                     Augmentation.begin(), Augmentation.end());
        ASSERT_NE(Aug, Frame.Bytes.end());
        const size_t Encoding = Aug - Frame.Bytes.begin() + 8;
        ASSERT_EQ(Frame.Bytes[Encoding], 0u);
        Frame.Bytes[Encoding] = 0x80; // absptr | indirect, same encoded width.
        Section Frames;
        Frames.Name = "__eh_frame";
        Frames.VA = kImageBase + 0x2800;
        Frames.Size = Frame.Bytes.size();
        Frames.Data = Frame.Bytes;
        Img.Sections.push_back(std::move(Frames));
        parseDarwinExceptions(Img);
        ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
        const auto &F = Img.ExceptionMetadata.Functions.front();
        ASSERT_TRUE(F.Dwarf);
        ASSERT_TRUE(F.Compact);
        EXPECT_EQ(F.ParseStatus, SameSlot ? ExceptionParseStatus::Complete
                                          : ExceptionParseStatus::Partial);
        EXPECT_EQ(F.Personality, ExceptionPersonality::GxxPersonalityV0);
        EXPECT_FALSE(isPlainSourceUnwind(F));
      }
}

TEST(DarwinUnwindSymbols, ForeignOrOversizedFDEKeepsItsOwnRecord) {
  for (bool Foreign : {false, true}) {
    auto Img = makeImage();
    const va_t FrameVA = kImageBase + 0x2800;
    const va_t FunctionVA = kImageBase + (Foreign ? 0x200 : 0x100);
    Img.Symbols.push_back(
        {"___gxx_personality_v0", kImageBase + 0x500, 0, true});
    const auto Frame = language_eh_test::buildSimpleFrame(
        FrameVA, FunctionVA, Foreign ? 0x80 : 0x100, "zPR", kImageBase + 0x500,
        0);
    Section Frames;
    Frames.Name = "__eh_frame";
    Frames.VA = FrameVA;
    Frames.Size = Frame.Bytes.size();
    Frames.Data = Frame.Bytes;
    Img.Sections.push_back(std::move(Frames));
    attachUnwindInfo(
        Img,
        buildUnwindInfo({{0x100, kX86_64ModeDwarf | uint32_t(Frame.FDEOffset)}},
                        0x180));
    parseDarwinExceptions(Img);
    ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 2u);
    unsigned CompactCount = 0, DwarfCount = 0;
    for (const auto &F : Img.ExceptionMetadata.Functions) {
      if (F.Compact) {
        ++CompactCount;
        EXPECT_FALSE(F.Dwarf);
        EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Partial);
      } else {
        ++DwarfCount;
        ASSERT_TRUE(F.Dwarf);
        EXPECT_EQ(F.CodeRange.Begin, FunctionVA);
        EXPECT_EQ(F.Personality, ExceptionPersonality::GxxPersonalityV0);
        EXPECT_FALSE(isPlainSourceUnwind(F));
      }
    }
    EXPECT_EQ(CompactCount, 1u);
    EXPECT_EQ(DwarfCount, 1u);
  }
}

TEST(DarwinUnwindSymbols, InvalidRangeRetainsItsIdentifiableFrameEntry) {
  auto Img = makeImage();
  const va_t Entry = kImageBase + 0x100;
  Img.Symbols.push_back({"_sized_function", Entry, 0x80, true});
  const auto Frame = language_eh_test::buildSimpleFrame(kImageBase + 0x2800,
                                                        Entry, 0, "zR", 0, 0);
  Section Frames;
  Frames.Name = "__eh_frame";
  Frames.VA = kImageBase + 0x2800;
  Frames.Size = Frame.Bytes.size();
  Frames.Data = Frame.Bytes;
  Img.Sections.push_back(std::move(Frames));
  parseDarwinExceptions(Img);
  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const auto &F = Img.ExceptionMetadata.Functions.front();
  ASSERT_TRUE(F.Dwarf);
  EXPECT_EQ(F.Dwarf->InitialLocation, Entry);
  EXPECT_FALSE(F.CodeRange.isValid());
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Malformed);
  EXPECT_FALSE(isPlainSourceUnwind(F));
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
