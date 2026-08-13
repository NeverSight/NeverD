//===- COFFARMFormatTests.cpp - Windows ARM PE format tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "COFFARMFormatTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::coff_arm_test;

TEST_F(COFFARMFormat, AArch64UsesEightBytePDataRecords) {
  const fs::path Path = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Path))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  auto ImgOrErr = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;

  EXPECT_EQ(Img.Format, BinaryFormat::COFF);
  EXPECT_EQ(Img.Arch, Arch::AArch64);
  EXPECT_EQ(Img.Bits, Bitness::Bits64);
  EXPECT_EQ(Img.Mode, InstructionMode::Default);
  expectAllFunctionRangesInsideExecutableSegments(Img);
  EXPECT_GE(Img.KnownCodeRanges.size(), 2u);
  expectFullPDataStartsHaveBoundedSymbols(Img, Path);
  EXPECT_GT(maxFunctionSize(Img), 0u);
  EXPECT_LT(maxFunctionSize(Img), 4096u);
}

TEST_F(COFFARMFormat, ARM32IsThumbWithNormalizedEntryAndBoundedRanges) {
  const fs::path Path = fixture("test_patch_coff_arm.exe");
  if (!fs::exists(Path))
    GTEST_SKIP() << "ARM32 PE fixture not built (lld-link unavailable)";

  // push.w {r11, lr}: Thumb-2 encodes the first halfword (0xE92D) first in
  // memory, so a little-endian 32-bit load places it in the low 16 bits.
  constexpr std::array<uint8_t, 4> WidePush = {0x2d, 0xe9, 0x00, 0x48};
  ASSERT_TRUE(isPrologueAt(WidePush.data(), WidePush.size(), Arch::ARM));

  auto ImgOrErr = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;

  EXPECT_EQ(Img.Format, BinaryFormat::COFF);
  EXPECT_EQ(Img.Arch, Arch::ARM);
  EXPECT_EQ(Img.Bits, Bitness::Bits32);
  EXPECT_EQ(Img.Mode, InstructionMode::Thumb);
  EXPECT_EQ(Img.Entry & 1, 0u);
  expectAllFunctionRangesInsideExecutableSegments(Img);
  EXPECT_GE(Img.KnownCodeRanges.size(), 2u);
  expectFullPDataStartsHaveBoundedSymbols(Img, Path);
  EXPECT_GT(maxFunctionSize(Img), 0u);
  EXPECT_LT(maxFunctionSize(Img), 4096u);
}

TEST_F(COFFARMFormat, ARM32CodeExportsAreNormalizedAndSerializedAsThumb) {
  const fs::path Path = fixture("test_patch_coff_arm.exe");
  if (!fs::exists(Path))
    GTEST_SKIP() << "ARM32 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Path);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto RawLeaf = rawExportRVA(*Obj, "pe_leaf");
  auto RawStacky = rawExportRVA(*Obj, "pe_stacky");
  ASSERT_TRUE(RawLeaf.has_value());
  ASSERT_TRUE(RawStacky.has_value());
  EXPECT_EQ(*RawLeaf & 1u, 1u);
  EXPECT_EQ(*RawStacky & 1u, 1u);

  auto ImgOrErr = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  for (llvm::StringRef Name :
       {llvm::StringRef("pe_leaf"), llvm::StringRef("pe_stacky")}) {
    uint32_t RawRVA = Name == "pe_leaf" ? *RawLeaf : *RawStacky;
    const Export *Exp = findExport(Img, Name);
    ASSERT_NE(Exp, nullptr);
    EXPECT_EQ(Exp->Addr, Img.Base + clearThumbBit(RawRVA));
    EXPECT_EQ(Exp->Addr & 1u, 0u);
    EXPECT_EQ(std::count_if(Img.Exports.begin(), Img.Exports.end(),
                            [&](const Export &E) { return E.Name == Name; }),
              1);
    EXPECT_EQ(serializeExportAddress(Img, Exp->Addr), Exp->Addr | 1u);
  }
  EXPECT_EQ(std::count_if(
                Img.Symbols.begin(), Img.Symbols.end(),
                [](const Symbol &S) { return S.IsFunc && (S.Addr & 1u) != 0; }),
            0);
}

TEST_F(COFFARMFormat, ARM32OddDataExportRemainsData) {
  const fs::path Source = fixture("test_patch_coff_arm.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "ARM32 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = exportAddressEntryFileOffset(*Obj, "pe_stacky");
  auto DataRVA = oddDataRVA(*Obj);
  ASSERT_TRUE(EntryOff.has_value());
  ASSERT_TRUE(DataRVA.has_value());
  writeLE<uint32_t>(Bytes.data() + *EntryOff, *DataRVA);

  fs::path Mutated = writeMutation("odd-data-export.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  const Export *Exp = findExport(Img, "pe_stacky");
  ASSERT_NE(Exp, nullptr);
  va_t Expected = Img.Base + *DataRVA;
  EXPECT_EQ(Expected & 1u, 1u);
  EXPECT_EQ(Exp->Addr, Expected);
  const Segment *Seg = Img.getSegmentFor(Exp->Addr);
  ASSERT_NE(Seg, nullptr);
  EXPECT_FALSE(Seg->isExecutable());
  EXPECT_EQ(serializeExportAddress(Img, Exp->Addr), Expected);
}

} // namespace
