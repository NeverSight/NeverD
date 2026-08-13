//===- COFFARMFormatFragmentTests.cpp - ARM unwind fragment record tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "COFFARMFormatTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::coff_arm_test;

TEST_F(COFFARMFormat, ARM32PackedFragmentIsRangeOnly) {
  const fs::path Source = fixture("test_patch_coff_arm.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "ARM32 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  // Clang may encode every source function as unpacked (LLVM 22) or include a
  // packed leaf (older releases).  The mutation below replaces either form
  // with a packed fragment, so do not require the compiler to provide one.
  auto EntryOff = findUnpackedPDataEntryOffset(*Obj);
  if (!EntryOff)
    EntryOff = findPDataEntryOffsetByFlag(*Obj, 1u);
  ASSERT_TRUE(EntryOff.has_value());
  uint32_t BeginWord = readLE<uint32_t>(Bytes.data() + *EntryOff);
  va_t Addr = Obj->getImageBase() + clearThumbBit(BeginWord);
  constexpr uint32_t Length = 4;
  writeLE<uint32_t>(Bytes.data() + *EntryOff + sizeof(uint32_t),
                    ((Length / 2) << 2) | 2u);

  fs::path Mutated = writeMutation("arm32-packed-fragment.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(std::find(ImgOrErr->KnownCodeRanges.begin(),
                      ImgOrErr->KnownCodeRanges.end(),
                      std::make_pair(Addr, Addr + Length)),
            ImgOrErr->KnownCodeRanges.end());
  EXPECT_EQ(
      std::find_if(ImgOrErr->Symbols.begin(), ImgOrErr->Symbols.end(),
                   [&](const Symbol &S) { return S.IsFunc && S.Addr == Addr; }),
      ImgOrErr->Symbols.end());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, ARM64PackedFragmentIsRangeOnly) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = findPDataEntryOffsetByFlag(*Obj, 1u);
  ASSERT_TRUE(EntryOff.has_value());
  uint32_t BeginWord = readLE<uint32_t>(Bytes.data() + *EntryOff);
  va_t Addr = Obj->getImageBase() + BeginWord;
  constexpr uint32_t Length = 4;
  writeLE<uint32_t>(Bytes.data() + *EntryOff + sizeof(uint32_t),
                    ((Length / 4) << 2) | 2u);

  fs::path Mutated = writeMutation("arm64-packed-fragment.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(std::find(ImgOrErr->KnownCodeRanges.begin(),
                      ImgOrErr->KnownCodeRanges.end(),
                      std::make_pair(Addr, Addr + Length)),
            ImgOrErr->KnownCodeRanges.end());
  EXPECT_EQ(
      std::find_if(ImgOrErr->Symbols.begin(), ImgOrErr->Symbols.end(),
                   [&](const Symbol &S) { return S.IsFunc && S.Addr == Addr; }),
      ImgOrErr->Symbols.end());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, ARM32UnpackedFragmentIsRangeOnly) {
  const fs::path Source = fixture("test_patch_coff_arm.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "ARM32 PE fixture not built (lld-link unavailable)";

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
  uint32_t BeginWord = readLE<uint32_t>(Bytes.data() + *EntryOff);
  va_t Addr = Obj->getImageBase() + clearThumbBit(BeginWord);
  constexpr uint32_t Length = 4;
  writeLE<uint32_t>(Bytes.data() + *XDataOff, (Length / 2) | (1u << 22));

  fs::path Mutated = writeMutation("arm32-unpacked-fragment.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(std::find(ImgOrErr->KnownCodeRanges.begin(),
                      ImgOrErr->KnownCodeRanges.end(),
                      std::make_pair(Addr, Addr + Length)),
            ImgOrErr->KnownCodeRanges.end());
  EXPECT_EQ(
      std::find_if(ImgOrErr->Symbols.begin(), ImgOrErr->Symbols.end(),
                   [&](const Symbol &S) { return S.IsFunc && S.Addr == Addr; }),
      ImgOrErr->Symbols.end());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, ARM64CorrespondingXDataBitIsNotFragment) {
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
  uint32_t BeginWord = readLE<uint32_t>(Bytes.data() + *EntryOff);
  va_t Addr = Obj->getImageBase() + BeginWord;
  constexpr uint32_t Length = 4;
  writeLE<uint32_t>(Bytes.data() + *XDataOff, (Length / 4) | (1u << 22));

  fs::path Mutated = writeMutation("arm64-not-fragment.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(std::find(ImgOrErr->KnownCodeRanges.begin(),
                      ImgOrErr->KnownCodeRanges.end(),
                      std::make_pair(Addr, Addr + Length)),
            ImgOrErr->KnownCodeRanges.end());
  EXPECT_NE(std::find_if(ImgOrErr->Symbols.begin(), ImgOrErr->Symbols.end(),
                         [&](const Symbol &S) {
                           return S.IsFunc && S.Addr == Addr &&
                                  S.Size == Length;
                         }),
            ImgOrErr->Symbols.end());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

} // namespace
