//===- PatchFormatCOFFARMTests.cpp - ARM32 and AArch64 COFF/PE patch tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "PatchFormatTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::patch_format_test;

class PatchCOFF_ARM32 : public NeverDLiftTest {};

TEST_F(PatchCOFF_ARM32, RejectsInteriorExceptionDirectoryPadding) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_arm.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "ARM32 PE executable not built";

  auto Corrupt = tmpFile("coff-arm-interior-pdata-zero.exe");
  ASSERT_TRUE(zeroFirstExceptionRecord(PE, Corrupt, 2 * sizeof(uint32_t)));
  auto ImageOrErr = loadBinary(Corrupt);
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  EXPECT_EQ(ImageOrErr->ExceptionMetadata.ParseStatus,
            ExceptionParseStatus::Malformed);
  EXPECT_FALSE(ImageOrErr->ExceptionMetadata.Functions.empty());

  auto Patch = patchBinary(Corrupt);
  EXPECT_NE(Patch.exitCode, 0);
  EXPECT_NE(Patch.err.find("input exception directory is malformed"),
            std::string::npos);
}

TEST_F(PatchCOFF_ARM32, SectionPatchInstallsTrampolineAndRelifts) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_arm.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "ARM32 PE executable not built (lld-link not available)";

  auto R = patchBinary(PE);
  ASSERT_EQ(R.exitCode, 0) << "ARM32 PE patch failed: " << R.err;
  EXPECT_FALSE(R.contains(", 0 trampolines)")) << R.out;

  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile));
  EXPECT_GT(fs::file_size(PatchedFile), fs::file_size(PE));

  auto ImgOrErr = loadBinary(PatchedFile);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_EQ(ImgOrErr->Format, BinaryFormat::COFF);
  EXPECT_EQ(ImgOrErr->Arch, Arch::ARM);
  EXPECT_EQ(ImgOrErr->Mode, InstructionMode::Thumb);

  auto Relift = liftToLowIR(PatchedFile);
  ASSERT_EQ(Relift.exitCode, 0) << Relift.err;
  std::string Output = Relift.out + "\n" + Relift.err;
  std::transform(Output.begin(), Output.end(), Output.begin(),
                 [](unsigned char C) { return std::tolower(C); });
  EXPECT_EQ(Output.find("unlifted"), std::string::npos) << Output;
}

TEST_F(PatchCOFF_ARM32, EntryTrampolineBranchesIntoAppendedExecSection) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_arm.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "ARM32 PE executable not built (lld-link not available)";

  auto R = patchBinary(PE);
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto ImgOrErr = loadBinary(tmpFile("patched"));
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;

  auto ExportIt =
      std::find_if(Img.Exports.begin(), Img.Exports.end(),
                   [](const Export &E) { return E.Name == "pe_leaf"; });
  ASSERT_NE(ExportIt, Img.Exports.end());
  const Segment *OrigText = Img.getSegmentFor(ExportIt->Addr);
  ASSERT_NE(OrigText, nullptr);
  ASSERT_TRUE(OrigText->isExecutable());
  uint64_t EntryOff = ExportIt->Addr - OrigText->VA;
  ASSERT_LE(EntryOff, OrigText->Data.size());
  ASSERT_GE(OrigText->Data.size() - EntryOff, 4u);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::Thumb));
  DecodedInsn Insn{};
  ASSERT_EQ(
      Dec.decodeOne(OrigText->Data.data() + EntryOff, 4, ExportIt->Addr, Insn),
      4);
  ASSERT_STREQ(Insn.Raw->mnemonic, "b.w");
  ASSERT_NE(Insn.Raw->detail, nullptr);
  ASSERT_EQ(Insn.Raw->detail->arm.op_count, 1u);
  ASSERT_EQ(Insn.Raw->detail->arm.operands[0].type, ARM_OP_IMM);
  va_t Target = Insn.Raw->detail->arm.operands[0].imm;

  auto NewText = std::find_if(
      Img.Sections.begin(), Img.Sections.end(),
      [](const Section &S) { return S.Name == kNdTextSection && S.isExecutable(); });
  ASSERT_NE(NewText, Img.Sections.end());
  EXPECT_TRUE(NewText->contains(Target))
      << "Thumb b.w target 0x" << std::hex << Target
      << " is outside appended executable section";
}

TEST_F(PatchCOFF_ARM32, TwoByteExportHasAdjacentBoundary) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_arm.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "ARM32 PE executable not built (lld-link not available)";

  auto ImgOrErr = loadBinary(PE);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const auto &Exports = ImgOrErr->Exports;
  auto Tiny = std::find_if(Exports.begin(), Exports.end(),
                           [](const Export &E) { return E.Name == "pe_tiny"; });
  auto After =
      std::find_if(Exports.begin(), Exports.end(),
                   [](const Export &E) { return E.Name == "pe_after_tiny"; });
  ASSERT_NE(Tiny, Exports.end());
  ASSERT_NE(After, Exports.end());
  EXPECT_EQ(After->Addr - Tiny->Addr, 2u);
}

TEST_F(PatchCOFF_ARM32, InstallerSkipsTwoByteFunctionWithoutTouchingSentinel) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_arm.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "ARM32 PE executable not built (lld-link not available)";

  auto ImgOrErr = loadBinary(PE);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  auto Tiny = std::find_if(Img.Exports.begin(), Img.Exports.end(),
                           [](const Export &E) { return E.Name == "pe_tiny"; });
  auto After =
      std::find_if(Img.Exports.begin(), Img.Exports.end(),
                   [](const Export &E) { return E.Name == "pe_after_tiny"; });
  ASSERT_NE(Tiny, Img.Exports.end());
  ASSERT_NE(After, Img.Exports.end());
  ASSERT_EQ(After->Addr - Tiny->Addr, 2u);

  std::ifstream In(PE, std::ios::binary);
  ASSERT_TRUE(In.good());
  std::vector<uint8_t> Bytes(std::istreambuf_iterator<char>(In), {});
  std::vector<uint8_t> Before = Bytes;
  auto Headers = locatePEHeaders(Bytes.data(), Bytes.size());
  ASSERT_TRUE(Headers.valid());
  PESectionFields Text;
  ASSERT_TRUE(findPESection(Headers, section_names::coff::Text, Text));

  COFFPatcher Patcher;
  Patcher.setImageContext(&Img);
  uint64_t AppendedVA = Patcher.plannedExecSegmentVA(Bytes, Arch::ARM);
  ASSERT_NE(AppendedVA, 0u);

  std::map<std::string, uint64_t> OnlyTiny{{"pe_tiny", AppendedVA}};
  uint64_t TextSize = Text.VirtualSize ? Text.VirtualSize : Text.SizeOfRawData;
  size_t Count = BinaryPatcher::installTrampolines(
      Bytes, OnlyTiny, Text.VirtualAddress, TextSize, Text.PointerToRawData,
      Img.Base, Img.Arch, Img.Mode, &Img.Symbols, &Img.KnownCodeRanges,
      &Img.Exports);
  EXPECT_EQ(Count, 0u);

  ASSERT_GE(Tiny->Addr, Img.Base + Text.VirtualAddress);
  uint64_t TinyDelta = Tiny->Addr - Img.Base - Text.VirtualAddress;
  ASSERT_LE(TinyDelta, InvalidVA - Text.PointerToRawData);
  uint64_t TinyOff = Text.PointerToRawData + TinyDelta;
  constexpr size_t TinyAndSentinelSize = 6;
  ASSERT_LE(TinyOff, Bytes.size());
  ASSERT_GE(Bytes.size() - TinyOff, TinyAndSentinelSize);
  EXPECT_TRUE(std::equal(Before.begin() + TinyOff,
                         Before.begin() + TinyOff + TinyAndSentinelSize,
                         Bytes.begin() + TinyOff));
}

//===----------------------------------------------------------------------===//
// AArch64 COFF/PE patch
//===----------------------------------------------------------------------===//

class PatchCOFF_AArch64 : public NeverDLiftTest {};

TEST_F(PatchCOFF_AArch64, RejectsInteriorExceptionDirectoryPadding) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_a64.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "AArch64 PE executable not built";

  auto Corrupt = tmpFile("coff-a64-interior-pdata-zero.exe");
  ASSERT_TRUE(zeroFirstExceptionRecord(PE, Corrupt, 2 * sizeof(uint32_t)));
  auto ImageOrErr = loadBinary(Corrupt);
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  EXPECT_EQ(ImageOrErr->ExceptionMetadata.ParseStatus,
            ExceptionParseStatus::Malformed);
  EXPECT_FALSE(ImageOrErr->ExceptionMetadata.Functions.empty());

  auto Patch = patchBinary(Corrupt);
  EXPECT_NE(Patch.exitCode, 0);
  EXPECT_NE(Patch.err.find("input exception directory is malformed"),
            std::string::npos);
}

TEST_F(PatchCOFF_AArch64, SectionPatchInstallsTrampolineAndRelifts) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_a64.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "AArch64 PE executable not built (lld-link not available)";

  auto R = patchBinary(PE);
  ASSERT_EQ(R.exitCode, 0) << "AArch64 PE patch failed: " << R.err;
  EXPECT_FALSE(R.contains(", 0 trampolines)")) << R.out;

  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile));
  EXPECT_GT(fs::file_size(PatchedFile), fs::file_size(PE));

  auto ImgOrErr = loadBinary(PatchedFile);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_EQ(ImgOrErr->Format, BinaryFormat::COFF);
  EXPECT_EQ(ImgOrErr->Arch, Arch::AArch64);
  EXPECT_EQ(ImgOrErr->Mode, InstructionMode::Default);

  auto Relift = liftToLowIR(PatchedFile);
  ASSERT_EQ(Relift.exitCode, 0) << Relift.err;
  std::string Output = Relift.out + "\n" + Relift.err;
  std::transform(Output.begin(), Output.end(), Output.begin(),
                 [](unsigned char C) { return std::tolower(C); });
  EXPECT_EQ(Output.find("unlifted"), std::string::npos) << Output;
}

TEST_F(PatchCOFF_AArch64, EntryTrampolineBranchesIntoAppendedExecSection) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_a64.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "AArch64 PE executable not built (lld-link not available)";

  auto R = patchBinary(PE);
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto ImgOrErr = loadBinary(tmpFile("patched"));
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;

  auto ExportIt =
      std::find_if(Img.Exports.begin(), Img.Exports.end(),
                   [](const Export &E) { return E.Name == "pe_leaf"; });
  ASSERT_NE(ExportIt, Img.Exports.end());
  const Segment *OrigText = Img.getSegmentFor(ExportIt->Addr);
  ASSERT_NE(OrigText, nullptr);
  ASSERT_TRUE(OrigText->isExecutable());
  uint64_t EntryOff = ExportIt->Addr - OrigText->VA;
  ASSERT_LE(EntryOff, OrigText->Data.size());
  ASSERT_GE(OrigText->Data.size() - EntryOff, 4u);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  DecodedInsn Insn{};
  ASSERT_EQ(
      Dec.decodeOne(OrigText->Data.data() + EntryOff, 4, ExportIt->Addr, Insn),
      4);
  ASSERT_STREQ(Insn.Raw->mnemonic, "b");
  ASSERT_NE(Insn.Raw->detail, nullptr);
  ASSERT_EQ(Insn.Raw->detail->aarch64.op_count, 1u);
  ASSERT_EQ(Insn.Raw->detail->aarch64.operands[0].type, AARCH64_OP_IMM);
  va_t Target = Insn.Raw->detail->aarch64.operands[0].imm;

  auto NewText = std::find_if(
      Img.Sections.begin(), Img.Sections.end(),
      [](const Section &S) { return S.Name == kNdTextSection && S.isExecutable(); });
  ASSERT_NE(NewText, Img.Sections.end());
  EXPECT_TRUE(NewText->contains(Target))
      << "AArch64 b target 0x" << std::hex << Target
      << " is outside appended executable section";
}

//===----------------------------------------------------------------------===//
// Cross-function call patch tests (xcall binaries)
//===----------------------------------------------------------------------===//

} // namespace
