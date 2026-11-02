//===- CompactUnwindARM64Tests.cpp - ARM64 compact unwind tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CompactUnwindTestsDetail.h"
#include "gtest/gtest.h"

namespace {

using namespace neverd;
using namespace neverd::macho_unwind;
using namespace neverd::compact_unwind_test;

namespace fixture {
#define NEVERD_COMPACT_ARM64_FIXTURE_VALUE(Type, Name, Value)                  \
  [[maybe_unused]] constexpr Type Name = Value;
#include "CompactUnwindARM64Fixtures.def"
} // namespace fixture

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

TEST(CompactUnwindARM64, PAuthLRPreservesSigningOffsetAndEveryRegisterPair) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(Arch::AArch64, fixture::kPAuthLRAllPairs, Entry));
  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramePointer);
  ASSERT_TRUE(Entry.ReturnAddressSigningOffset);
  EXPECT_EQ(*Entry.ReturnAddressSigningOffset, fixture::kAllPairsSigningOffset);
  const std::vector<int> ExpectedRegisters = {
#define NEVERD_COMPACT_ARM64_FIXTURE_REGISTER(Number) Number,
#include "CompactUnwindARM64Fixtures.def"
  };
  EXPECT_EQ(slotRegisters(Entry), ExpectedRegisters);
  EXPECT_EQ(Entry.SavedGPRMask, fixture::kAllPairsGPRMask);
  EXPECT_EQ(Entry.SavedFPRMask, fixture::kAllPairsFPRMask);
  EXPECT_EQ(Entry.SemanticStatus, CompactUnwindSemanticStatus::Complete);
  EXPECT_FALSE(Entry.HasStackSize);
}

TEST(CompactUnwindARM64, PAuthLRZeroSigningOffsetDiffersFromAbsent) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(Arch::AArch64, fixture::kPAuthLRFirst, Entry));
  ASSERT_TRUE(Entry.ReturnAddressSigningOffset);
  EXPECT_EQ(*Entry.ReturnAddressSigningOffset, fixture::kFirstSigningOffset);
  ASSERT_TRUE(
      decodeEncoding(Arch::AArch64, fixture::kFrameWithoutSigning, Entry));
  EXPECT_FALSE(Entry.ReturnAddressSigningOffset);
  EXPECT_FALSE(returnAddressSigningOffset(Arch::X64, fixture::kPAuthLRFirst));
}

TEST(CompactUnwindARM64, PAuthLRMaximumSigningOffsetUsesFourByteInstructions) {
  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(Arch::AArch64, fixture::kPAuthLRMaxOffset, Entry));
  ASSERT_TRUE(Entry.ReturnAddressSigningOffset);
  EXPECT_EQ(*Entry.ReturnAddressSigningOffset, fixture::kMaxSigningOffset);
  EXPECT_TRUE(isValidEncodingRange(
      Arch::AArch64, fixture::kPAuthLRMaxOffset,
      {fixture::kRangeBegin, fixture::kMaximumOffsetRangeEnd}));
  EXPECT_FALSE(isValidEncodingRange(
      Arch::AArch64, fixture::kPAuthLRMaxOffset,
      {fixture::kRangeBegin, fixture::kTruncatedMaximumOffsetRangeEnd}));
}

TEST(CompactUnwindARM64, PAuthLRRequiresACompleteAlignedSigningInstruction) {
  struct RangeCase {
    const char *Name;
    uint32_t Encoding;
    ExceptionAddressRange Range;
    bool Valid;
  };
  const RangeCase Cases[] = {
#define NEVERD_COMPACT_ARM64_FIXTURE_RANGE_CASE(Name, Encoding, Begin, End,    \
                                                Valid)                         \
  {#Name, Encoding, {Begin, End}, Valid},
#include "CompactUnwindARM64Fixtures.def"
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    EXPECT_EQ(isValidEncodingRange(Arch::AArch64, Case.Encoding, Case.Range),
              Case.Valid);
  }
}

TEST(CompactUnwindARM64, ReservedPayloadCannotAcquireSigningOrRegisterFacts) {
  const uint32_t Encodings[] = {
#define NEVERD_COMPACT_ARM64_FIXTURE_INVALID_ENCODING(Encoding) Encoding,
#include "CompactUnwindARM64Fixtures.def"
  };
  for (uint32_t Encoding : Encodings) {
    SCOPED_TRACE(Encoding);
    CompactUnwindEntry Entry;
    ASSERT_TRUE(
        decodeEncoding(Arch::AArch64, fixture::kPAuthLRNextPair, Entry));
    ASSERT_TRUE(Entry.ReturnAddressSigningOffset);
    ASSERT_FALSE(Entry.SavedRegisterSlots.empty());
    EXPECT_FALSE(decodeEncoding(Arch::AArch64, Encoding, Entry));
    EXPECT_EQ(Entry.Kind, CompactUnwindKind::Unknown);
    EXPECT_EQ(Entry.SemanticStatus, CompactUnwindSemanticStatus::Partial);
    EXPECT_FALSE(Entry.ReturnAddressSigningOffset);
    EXPECT_TRUE(Entry.SavedRegisterSlots.empty());
    EXPECT_EQ(Entry.SavedGPRMask, fixture::kNoSavedRegisters);
    EXPECT_EQ(Entry.SavedFPRMask, fixture::kNoSavedRegisters);
    EXPECT_FALSE(isValidEncodingRange(
        Arch::AArch64, Encoding, {fixture::kRangeBegin, fixture::kRangeEnd}));
  }
}

TEST(CompactUnwindARM64, SharedPayloadContractIncludesOnlyPublishedBits) {
  struct PayloadCase {
    uint32_t Encoding;
    uint32_t Mask;
  };
  const PayloadCase Cases[] = {
#define NEVERD_COMPACT_ARM64_FIXTURE_PAYLOAD_CASE(Encoding, Mask)              \
  {Encoding, Mask},
#include "CompactUnwindARM64Fixtures.def"
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Encoding);
    EXPECT_EQ(getARM64EncodingPayloadMask(Case.Encoding), Case.Mask);
  }
  EXPECT_FALSE(getARM64EncodingPayloadMask(fixture::kUnknownMode));
}

} // namespace
