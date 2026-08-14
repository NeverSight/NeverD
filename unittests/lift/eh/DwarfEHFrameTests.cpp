//===- DwarfEHFrameTests.cpp - Regenerated frame validation tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/DwarfEHFrame.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd;

void appendU32(std::vector<uint8_t> &Bytes, uint32_t Value) {
  for (unsigned I = 0; I < 4; ++I)
    Bytes.push_back(static_cast<uint8_t>(Value >> (I * 8)));
}

void appendU64(std::vector<uint8_t> &Bytes, uint64_t Value) {
  for (unsigned I = 0; I < 8; ++I)
    Bytes.push_back(static_cast<uint8_t>(Value >> (I * 8)));
}

void appendTerminator(std::vector<uint8_t> &Bytes) { appendU32(Bytes, 0); }

std::vector<uint8_t> makeVersionFourCIE(uint8_t AddressSize,
                                        uint8_t SegmentSelectorSize) {
  std::vector<uint8_t> Bytes;
  appendU32(Bytes, 11);
  appendU32(Bytes, 0);
  Bytes.insert(Bytes.end(),
               {4, 0, AddressSize, SegmentSelectorSize, 1, 0x78, 16});
  appendTerminator(Bytes);
  return Bytes;
}

struct FrameFixture {
  std::vector<uint8_t> Bytes;
  size_t FDEOffset = 0;
  size_t InitialOffset = 0;
};

FrameFixture makeFDE32(uint8_t Encoding, uint32_t Initial, uint32_t Range) {
  FrameFixture Result;
  appendU32(Result.Bytes, 13);
  appendU32(Result.Bytes, 0);
  Result.Bytes.insert(Result.Bytes.end(),
                      {1, 'z', 'R', 0, 1, 0x78, 16, 1, Encoding});

  Result.FDEOffset = Result.Bytes.size();
  appendU32(Result.Bytes, 13);
  appendU32(Result.Bytes, static_cast<uint32_t>(Result.FDEOffset + 4));
  Result.InitialOffset = Result.Bytes.size();
  appendU32(Result.Bytes, Initial);
  appendU32(Result.Bytes, Range);
  Result.Bytes.push_back(0);
  appendTerminator(Result.Bytes);
  return Result;
}

FrameFixture makeAbsoluteFDE64(uint64_t Initial, uint64_t Range) {
  FrameFixture Result;
  appendU32(Result.Bytes, 13);
  appendU32(Result.Bytes, 0);
  Result.Bytes.insert(Result.Bytes.end(), {1, 'z', 'R', 0, 1, 0x78, 16, 1, 0});

  Result.FDEOffset = Result.Bytes.size();
  appendU32(Result.Bytes, 21);
  appendU32(Result.Bytes, static_cast<uint32_t>(Result.FDEOffset + 4));
  Result.InitialOffset = Result.Bytes.size();
  appendU64(Result.Bytes, Initial);
  appendU64(Result.Bytes, Range);
  Result.Bytes.push_back(0);
  appendTerminator(Result.Bytes);
  return Result;
}

FrameFixture makePCRelativeAbsptrFDE64(int64_t Initial, uint64_t Range) {
  FrameFixture Result;
  appendU32(Result.Bytes, 13);
  appendU32(Result.Bytes, 0);
  Result.Bytes.insert(Result.Bytes.end(),
                      {1, 'z', 'R', 0, 1, 0x78, 16, 1, 0x10});

  Result.FDEOffset = Result.Bytes.size();
  appendU32(Result.Bytes, 21);
  appendU32(Result.Bytes, static_cast<uint32_t>(Result.FDEOffset + 4));
  Result.InitialOffset = Result.Bytes.size();
  appendU64(Result.Bytes, static_cast<uint64_t>(Initial));
  appendU64(Result.Bytes, Range);
  Result.Bytes.push_back(0);
  appendTerminator(Result.Bytes);
  return Result;
}

void appendAbsoluteFDE64(std::vector<uint8_t> &Bytes, uint64_t Initial,
                         uint64_t Range) {
  const size_t FDEOffset = Bytes.size();
  appendU32(Bytes, 21);
  appendU32(Bytes, static_cast<uint32_t>(FDEOffset + 4));
  appendU64(Bytes, Initial);
  appendU64(Bytes, Range);
  Bytes.push_back(0);
}

void expectDecodeError(llvm::ArrayRef<uint8_t> Bytes, uint64_t BaseVA,
                       llvm::StringRef ExpectedMessage,
                       bool Is64BitAddress = true) {
  auto Result = decodeDwarfEHFrameRecords(Bytes, BaseVA, Is64BitAddress);
  ASSERT_FALSE(static_cast<bool>(Result));
  const std::string Message = llvm::toString(Result.takeError());
  EXPECT_NE(Message.find(ExpectedMessage.str()), std::string::npos) << Message;
}

TEST(DwarfEHFrame, ContinuesAfterRAugmentation) {
  const std::vector<uint8_t> Bytes = {
      0x0f, 0,    0,    0, 0, 0, 0,    0, 1, 'z', 'R',  'L', 0, 1, 0x78, 16, 2,
      0x1b, 0x1b, 0x11, 0, 0, 0, 0x17, 0, 0, 0,   0xe5, 0,   0, 0, 0x20, 0,  0,
      0,    4,    0,    0, 0, 0, 0,    0, 0, 0,   0,    0,   0,
  };
  auto Records = decodeDwarfEHFrameRecords(Bytes, 0x100000, true);
  ASSERT_TRUE(static_cast<bool>(Records))
      << llvm::toString(Records.takeError());
  ASSERT_EQ(Records->size(), 1u);
  EXPECT_EQ((*Records)[0].BeginVA, 0x100100u);
  EXPECT_EQ((*Records)[0].EndVA, 0x100120u);
  EXPECT_EQ((*Records)[0].RecordVA, 0x100013u);
}

TEST(DwarfEHFrame, RejectsMissingOperandAfterR) {
  const std::vector<uint8_t> Bytes = {
      0x0e, 0, 0, 0,    0,  0, 0,    0, 1, 'z', 'R',
      'L',  0, 1, 0x78, 16, 1, 0x1b, 0, 0, 0,   0,
  };
  expectDecodeError(Bytes, 0, "malformed CIE augmentation");
}

TEST(DwarfEHFrame, RejectsUnclaimedAugmentationBytes) {
  const std::vector<uint8_t> Bytes = {
      0x0e, 0, 0,    0,  0, 0,    0, 0, 1, 'z', 'R',
      0,    1, 0x78, 16, 2, 0x1b, 0, 0, 0, 0,   0,
  };
  expectDecodeError(Bytes, 0, "malformed CIE augmentation");
}

TEST(DwarfEHFrame, AcceptsAbsolutePersonalityAtTargetPointerWidth) {
  std::vector<uint8_t> Bytes;
  appendU32(Bytes, 23);
  appendU32(Bytes, 0);
  Bytes.insert(Bytes.end(), {1, 'z', 'R', 'P', 0, 1, 0x78, 16, 10, 0, 0});
  appendU64(Bytes, 0x100001000);
  appendTerminator(Bytes);

  auto Records = decodeDwarfEHFrameRecords(Bytes, 0, true);
  ASSERT_TRUE(static_cast<bool>(Records))
      << llvm::toString(Records.takeError());
  EXPECT_TRUE(Records->empty());
}

TEST(DwarfEHFrame, VersionOneReturnRegisterOccupiesOneByte) {
  std::vector<uint8_t> Bytes;
  appendU32(Bytes, 9);
  appendU32(Bytes, 0);
  // 0x80 would begin a multi-byte ULEB, but version 1 defines this field as a
  // single uint8 and therefore accepts the value as-is.
  Bytes.insert(Bytes.end(), {1, 0, 1, 0x78, 0x80});
  appendTerminator(Bytes);

  auto Records = decodeDwarfEHFrameRecords(Bytes, 0, true);
  ASSERT_TRUE(static_cast<bool>(Records))
      << llvm::toString(Records.takeError());
  EXPECT_TRUE(Records->empty());
}

TEST(DwarfEHFrame, ValidatesVersionFourTargetAddressFields) {
  for (const auto &[Is64BitAddress, AddressSize] :
       {std::pair{true, uint8_t{8}}, std::pair{false, uint8_t{4}}}) {
    auto Records = decodeDwarfEHFrameRecords(makeVersionFourCIE(AddressSize, 0),
                                             0, Is64BitAddress);
    ASSERT_TRUE(static_cast<bool>(Records))
        << llvm::toString(Records.takeError());
    EXPECT_TRUE(Records->empty());
  }

  expectDecodeError(makeVersionFourCIE(4, 0), 0, "malformed CIE augmentation",
                    true);
  expectDecodeError(makeVersionFourCIE(8, 1), 0, "malformed CIE augmentation",
                    true);
}

TEST(DwarfEHFrame, SupportsTargetWidthSignedPointerEncoding) {
  FrameFixture Fixture = makeFDE32(0x18, 0xe7, 0x20);
  auto Records = decodeDwarfEHFrameRecords(Fixture.Bytes, 0x1000, false);
  ASSERT_TRUE(static_cast<bool>(Records))
      << llvm::toString(Records.takeError());
  ASSERT_EQ(Records->size(), 1u);
  EXPECT_EQ((*Records)[0].BeginVA, 0x1100u);
  EXPECT_EQ((*Records)[0].EndVA, 0x1120u);
}

TEST(DwarfEHFrame, SupportsNegativePCRelativeAbsptrUsedByMachO) {
  constexpr uint64_t BaseVA = 0x100000388;
  FrameFixture Fixture = makePCRelativeAbsptrFDE64(-0x61, 0x12);
  auto Records = decodeDwarfEHFrameRecords(Fixture.Bytes, BaseVA, true);
  ASSERT_TRUE(static_cast<bool>(Records))
      << llvm::toString(Records.takeError());
  ASSERT_EQ(Records->size(), 1u);
  EXPECT_EQ((*Records)[0].BeginVA, 0x100000340u);
  EXPECT_EQ((*Records)[0].EndVA, 0x100000352u);
}

TEST(DwarfEHFrame, SupportsNegative32BitPCRelativeAbsptr) {
  // The initial-location field begins 0x19 bytes into this fixture.
  FrameFixture Fixture = makeFDE32(0x10, static_cast<uint32_t>(-0x19), 0x20);
  auto Records = decodeDwarfEHFrameRecords(Fixture.Bytes, 0x1000, false);
  ASSERT_TRUE(static_cast<bool>(Records))
      << llvm::toString(Records.takeError());
  ASSERT_EQ(Records->size(), 1u);
  EXPECT_EQ((*Records)[0].BeginVA, 0x1000u);
  EXPECT_EQ((*Records)[0].EndVA, 0x1020u);
}

TEST(DwarfEHFrame, RejectsULEBThatExceedsUint64) {
  std::vector<uint8_t> Bytes;
  appendU32(Bytes, 18);
  appendU32(Bytes, 0);
  Bytes.push_back(1);
  Bytes.push_back(0);
  Bytes.insert(Bytes.end(), 9, 0x80);
  Bytes.push_back(2);
  Bytes.push_back(0);
  Bytes.push_back(0);
  appendTerminator(Bytes);
  expectDecodeError(Bytes, 0, "malformed CIE augmentation");
}

TEST(DwarfEHFrame, AcceptsMaximumUint64ULEB) {
  std::vector<uint8_t> Bytes;
  appendU32(Bytes, 18);
  appendU32(Bytes, 0);
  Bytes.push_back(1);
  Bytes.push_back(0);
  Bytes.insert(Bytes.end(), 9, 0xff);
  Bytes.push_back(1);
  Bytes.push_back(0);
  Bytes.push_back(0);
  appendTerminator(Bytes);

  auto Records = decodeDwarfEHFrameRecords(Bytes, 0, true);
  ASSERT_TRUE(static_cast<bool>(Records))
      << llvm::toString(Records.takeError());
  EXPECT_TRUE(Records->empty());
}

TEST(DwarfEHFrame, RejectsSLEBThatExceedsInt64) {
  std::vector<uint8_t> Bytes;
  appendU32(Bytes, 18);
  appendU32(Bytes, 0);
  Bytes.push_back(1);
  Bytes.push_back(0);
  Bytes.push_back(1);
  Bytes.insert(Bytes.end(), 9, 0x80);
  Bytes.push_back(2);
  Bytes.push_back(0);
  appendTerminator(Bytes);
  expectDecodeError(Bytes, 0, "malformed CIE augmentation");
}

TEST(DwarfEHFrame, AllowsOnlyZeroPaddingAfterTerminator) {
  FrameFixture Fixture = makeFDE32(0x1b, 0xe7, 0x20);
  Fixture.Bytes.insert(Fixture.Bytes.end(), 5, 0);
  auto Records = decodeDwarfEHFrameRecords(Fixture.Bytes, 0x1000, false);
  ASSERT_TRUE(static_cast<bool>(Records))
      << llvm::toString(Records.takeError());
  ASSERT_EQ(Records->size(), 1u);
  EXPECT_EQ((*Records)[0].BeginVA, 0x1100u);
  EXPECT_EQ((*Records)[0].EndVA, 0x1120u);
  EXPECT_EQ((*Records)[0].RecordVA, 0x1011u);

  Fixture.Bytes.back() = 1;
  expectDecodeError(Fixture.Bytes, 0x1000,
                    "nonzero bytes follow the terminator", false);
}

TEST(DwarfEHFrame, RejectsDuplicateFDEInitialLocation) {
  FrameFixture Fixture = makeAbsoluteFDE64(0x100001000, 0x10);
  Fixture.Bytes.resize(Fixture.Bytes.size() - 4);
  appendAbsoluteFDE64(Fixture.Bytes, 0x100001000, 0x20);
  appendTerminator(Fixture.Bytes);
  expectDecodeError(Fixture.Bytes, 0, "duplicate FDE initial location");
}

TEST(DwarfEHFrame, RejectsOverlappingFDEAddressRanges) {
  FrameFixture Fixture = makeAbsoluteFDE64(0x100001000, 0x20);
  Fixture.Bytes.resize(Fixture.Bytes.size() - 4);
  appendAbsoluteFDE64(Fixture.Bytes, 0x100001010, 0x20);
  appendTerminator(Fixture.Bytes);
  expectDecodeError(Fixture.Bytes, 0, "overlapping FDE address ranges");
}

TEST(DwarfEHFrame, RejectsMissingFDEAugmentationLength) {
  FrameFixture Fixture = makeAbsoluteFDE64(0x100001000, 0x10);
  // The z-augmented CIE requires an augmentation-length byte after the range.
  // Shorten the FDE by that byte and expose the section terminator directly.
  Fixture.Bytes[Fixture.FDEOffset] = 20;
  Fixture.Bytes.erase(Fixture.Bytes.begin() + Fixture.FDEOffset + 24);
  expectDecodeError(Fixture.Bytes, 0, "malformed FDE augmentation");
}

TEST(DwarfEHFrame, RejectsInitialFieldAddressOverflow) {
  FrameFixture Fixture = makeFDE32(0x1b, 0, 1);
  expectDecodeError(Fixture.Bytes, std::numeric_limits<uint64_t>::max() - 24,
                    "initial-location field address overflows", false);
}

TEST(DwarfEHFrame, RejectsSignedPCRelativeAdditionOverflow) {
  FrameFixture Fixture = makeFDE32(0x1b, 1, 1);
  expectDecodeError(Fixture.Bytes, std::numeric_limits<uint64_t>::max() - 25,
                    "initial location overflows", false);
}

TEST(DwarfEHFrame, RejectsSignedPCRelativeSubtractionUnderflow) {
  FrameFixture Fixture = makeFDE32(0x1b, static_cast<uint32_t>(-26), 1);
  expectDecodeError(Fixture.Bytes, 0, "initial location overflows", false);
}

TEST(DwarfEHFrame, RejectsPCRelativeAbsptrAdditionOverflow) {
  FrameFixture Fixture = makePCRelativeAbsptrFDE64(1, 1);
  expectDecodeError(Fixture.Bytes,
                    std::numeric_limits<uint64_t>::max() -
                        Fixture.InitialOffset,
                    "initial location overflows");
}

TEST(DwarfEHFrame, RejectsPCRelativeAbsptrSubtractionUnderflow) {
  FrameFixture Fixture = makePCRelativeAbsptrFDE64(-26, 1);
  expectDecodeError(Fixture.Bytes, 0, "initial location overflows");
}

TEST(DwarfEHFrame, RejectsUnsignedPCRelativeAdditionOverflow) {
  FrameFixture Fixture = makeFDE32(0x13, 1, 1);
  expectDecodeError(Fixture.Bytes, std::numeric_limits<uint64_t>::max() - 25,
                    "initial location overflows", false);
}

TEST(DwarfEHFrame, RejectsAddressRangeOverflow) {
  FrameFixture Fixture =
      makeAbsoluteFDE64(std::numeric_limits<uint64_t>::max() - 15, 16);
  expectDecodeError(Fixture.Bytes, 0, "FDE address range overflows");
}

TEST(DwarfEHFrame, RangeMayEndExactlyAtUint64Max) {
  FrameFixture Fixture =
      makeAbsoluteFDE64(std::numeric_limits<uint64_t>::max() - 15, 15);
  auto Records = decodeDwarfEHFrameRecords(Fixture.Bytes, 0, true);
  ASSERT_TRUE(static_cast<bool>(Records))
      << llvm::toString(Records.takeError());
  ASSERT_EQ(Records->size(), 1u);
  EXPECT_EQ((*Records)[0].BeginVA, std::numeric_limits<uint64_t>::max() - 15);
  EXPECT_EQ((*Records)[0].EndVA, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ((*Records)[0].RecordVA, 17u);
}

TEST(DwarfEHFrame, RejectsFDERecordAddressOverflow) {
  FrameFixture Fixture = makeAbsoluteFDE64(0x1000, 0x10);
  expectDecodeError(Fixture.Bytes, std::numeric_limits<uint64_t>::max() - 16,
                    "FDE record address overflows");
}

TEST(DwarfEHFrame, RejectsIndirectFDEEncoding) {
  const std::vector<uint8_t> Bytes = {
      0x0d, 0, 0,    0,  0, 0,    0, 0, 1, 'z', 'R',
      0,    1, 0x78, 16, 1, 0x9b, 0, 0, 0, 0,
  };
  expectDecodeError(Bytes, 0, "malformed CIE augmentation");
}

} // namespace
