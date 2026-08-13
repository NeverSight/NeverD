//===- COFFARMFormatMalformedTests.cpp - Malformed ARM pdata/xdata tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "COFFARMFormatTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::coff_arm_test;

TEST_F(COFFARMFormat, PartialARM64PDataRecordIsIgnored) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  size_t DirOff = exceptionDirectoryFileOffset(*Obj);
  ASSERT_LE(DirOff + sizeof(llvm::object::data_directory), Bytes.size());
  writeLE<uint32_t>(
      Bytes.data() + DirOff + offsetof(llvm::object::data_directory, Size), 4u);

  fs::path Mutated = writeMutation("partial-pdata.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_TRUE(ImgOrErr->KnownCodeRanges.empty());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, InvalidMiddleARM64XDataDoesNotStopLaterRecords) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOffsets = firstThreePDataEntryOffsets(*Obj);
  ASSERT_TRUE(EntryOffsets.has_value());
  auto [FirstOff, MiddleOff, LastOff] = *EntryOffsets;
  ASSERT_EQ(readLE<uint32_t>(Bytes.data() + MiddleOff + sizeof(uint32_t)) & 3u,
            0u);

  swapPDataEntries(Bytes, FirstOff, LastOff);
  writeLE<uint32_t>(Bytes.data() + MiddleOff + sizeof(uint32_t), 0x7ffffffcu);
  auto Later = readAArch64PackedFull(Bytes, Obj->getImageBase(), LastOff);
  ASSERT_TRUE(Later.has_value());

  fs::path Mutated = writeMutation("invalid-xdata-rva.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  expectFullFunctionPresent(*ImgOrErr, *Later);
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, TruncatedARM64XDataIsSkipped) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOffsets = firstThreePDataEntryOffsets(*Obj);
  ASSERT_TRUE(EntryOffsets.has_value());
  auto [FirstOff, MiddleOff, LaterOff] = *EntryOffsets;
  (void)FirstOff;
  ASSERT_EQ(readLE<uint32_t>(Bytes.data() + MiddleOff + sizeof(uint32_t)) & 3u,
            0u);

  const llvm::object::coff_section *PData = nullptr;
  for (const llvm::object::SectionRef &SecRef : Obj->sections()) {
    auto NameOrErr = SecRef.getName();
    if (!NameOrErr) {
      llvm::consumeError(NameOrErr.takeError());
      continue;
    }
    if (*NameOrErr == section_names::coff::Pdata) {
      PData = Obj->getCOFFSection(SecRef);
      break;
    }
  }
  ASSERT_NE(PData, nullptr);
  uint32_t PDataVA = PData->VirtualAddress;
  uint32_t PDataRawOff = PData->PointerToRawData;
  uint32_t PDataRawSize = PData->SizeOfRawData;
  ASSERT_GE(PDataRawSize, 8u);
  uint32_t XDataDelta = (PDataRawSize - 4u) & ~3u;
  uint32_t XDataRVA = PDataVA + XDataDelta;
  size_t PDataHeaderOff = static_cast<size_t>(
      reinterpret_cast<const char *>(PData) - Obj->getData().data());
  ASSERT_TRUE(rangeInBounds(PDataHeaderOff, sizeof(*PData), Bytes.size()));
  writeLE<uint32_t>(Bytes.data() + PDataHeaderOff +
                        offsetof(llvm::object::coff_section, VirtualSize),
                    XDataDelta + 4u);
  writeLE<uint32_t>(Bytes.data() + PDataHeaderOff +
                        offsetof(llvm::object::coff_section, SizeOfRawData),
                    XDataDelta + 3u);
  writeLE<uint32_t>(Bytes.data() + MiddleOff + sizeof(uint32_t), XDataRVA);

  auto Later = readAArch64PackedFull(Bytes, Obj->getImageBase(), LaterOff);
  ASSERT_TRUE(Later.has_value());
  size_t TruncatedSize = static_cast<size_t>(PDataRawOff) + XDataDelta + 3u;
  ASSERT_LT(TruncatedSize, Bytes.size());
  Bytes.resize(TruncatedSize);

  auto TruncatedObj = createCOFFObject(Bytes);
  ASSERT_NE(TruncatedObj, nullptr);
  uintptr_t XDataPtr = 0;
  llvm::Error XDataErr = TruncatedObj->getRvaPtr(XDataRVA, XDataPtr);
  ASSERT_FALSE(static_cast<bool>(XDataErr))
      << llvm::toString(std::move(XDataErr));
  uintptr_t FileEnd = reinterpret_cast<uintptr_t>(Bytes.data()) + Bytes.size();
  ASSERT_EQ(FileEnd - XDataPtr, 3u);

  fs::path Mutated = writeMutation("truncated-xdata.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(std::find(ImgOrErr->KnownCodeRanges.begin(),
                      ImgOrErr->KnownCodeRanges.end(),
                      std::make_pair(Later->Addr, Later->Addr + Later->Length)),
            ImgOrErr->KnownCodeRanges.end());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, ReservedMiddleARM64RecordDoesNotStopLaterRecords) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOffsets = firstThreePDataEntryOffsets(*Obj);
  ASSERT_TRUE(EntryOffsets.has_value());
  auto [FirstOff, MiddleOff, LastOff] = *EntryOffsets;

  swapPDataEntries(Bytes, FirstOff, LastOff);
  uint32_t MiddleUnwind =
      readLE<uint32_t>(Bytes.data() + MiddleOff + sizeof(uint32_t));
  writeLE<uint32_t>(Bytes.data() + MiddleOff + sizeof(uint32_t),
                    (MiddleUnwind & ~3u) | 3u);

  auto Later = readAArch64PackedFull(Bytes, Obj->getImageBase(), LastOff);
  ASSERT_TRUE(Later.has_value());

  fs::path Mutated = writeMutation("reserved-middle-record.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  expectFullFunctionPresent(*ImgOrErr, *Later);
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, ZeroARM64PackedLengthIsSkipped) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  auto BaselineOrErr = loadBinary(Source);
  ASSERT_TRUE(static_cast<bool>(BaselineOrErr))
      << llvm::toString(BaselineOrErr.takeError());
  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = findPDataEntryOffsetByFlag(*Obj, 1u);
  ASSERT_TRUE(EntryOff.has_value());
  writeLE<uint32_t>(Bytes.data() + *EntryOff + sizeof(uint32_t), 1u);

  fs::path Mutated = writeMutation("zero-packed-length.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_LT(ImgOrErr->KnownCodeRanges.size(),
            BaselineOrErr->KnownCodeRanges.size());
  EXPECT_FALSE(ImgOrErr->KnownCodeRanges.empty());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, OverflowingARM64UnpackedLengthIsSkipped) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  auto BaselineOrErr = loadBinary(Source);
  ASSERT_TRUE(static_cast<bool>(BaselineOrErr))
      << llvm::toString(BaselineOrErr.takeError());
  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = findUnpackedPDataEntryOffset(*Obj);
  ASSERT_TRUE(EntryOff.has_value());
  auto XDataOff = xdataFileOffset(Bytes, *Obj, *BaselineOrErr, *EntryOff);
  ASSERT_TRUE(XDataOff.has_value());
  writeLE<uint32_t>(Bytes.data() + *XDataOff, 0x3ffffu);

  fs::path Mutated = writeMutation("overflowing-xdata-length.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_LT(ImgOrErr->KnownCodeRanges.size(),
            BaselineOrErr->KnownCodeRanges.size());
  EXPECT_FALSE(ImgOrErr->KnownCodeRanges.empty());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, UnsupportedARM64XDataVersionIsSkipped) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  auto BaselineOrErr = loadBinary(Source);
  ASSERT_TRUE(static_cast<bool>(BaselineOrErr))
      << llvm::toString(BaselineOrErr.takeError());
  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = findUnpackedPDataEntryOffset(*Obj);
  ASSERT_TRUE(EntryOff.has_value());
  auto XDataOff = xdataFileOffset(Bytes, *Obj, *BaselineOrErr, *EntryOff);
  ASSERT_TRUE(XDataOff.has_value());
  uint32_t Header = readLE<uint32_t>(Bytes.data() + *XDataOff);
  Header = (Header & ~(3u << 18)) | (1u << 18);
  writeLE<uint32_t>(Bytes.data() + *XDataOff, Header);

  fs::path Mutated = writeMutation("arm64-xdata-version.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_LT(ImgOrErr->KnownCodeRanges.size(),
            BaselineOrErr->KnownCodeRanges.size());
  EXPECT_FALSE(ImgOrErr->KnownCodeRanges.empty());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

} // namespace
