//===- PatchFormatCOFFARMTests.cpp - ARM32 and AArch64 COFF/PE patch tests
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PatchFormatTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/support/BinaryEncoding.h"

#include <set>

namespace {

using namespace neverd;
using namespace neverd::patch_format_test;

class PatchCOFF_ARM32 : public NeverDLiftTest {};

std::optional<size_t> fileOffsetForRVA(llvm::ArrayRef<uint8_t> Bytes,
                                       const PEHeaderPtrs &Headers,
                                       uint32_t RVA, size_t Size) {
  std::optional<size_t> Result;
  forEachPESection(Headers, [&](const PESectionFields &Section, uint16_t) {
    if (Result || RVA < Section.VirtualAddress)
      return;
    const uint64_t Delta = uint64_t(RVA) - Section.VirtualAddress;
    if (Delta > Section.SizeOfRawData || Size > Section.SizeOfRawData - Delta)
      return;
    const uint64_t Offset = uint64_t(Section.PointerToRawData) + Delta;
    if (Offset > std::numeric_limits<size_t>::max() ||
        !rangeInBounds(Offset, Size, Bytes.size()))
      return;
    Result = static_cast<size_t>(Offset);
  });
  return Result;
}

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

TEST_F(PatchCOFF_ARM32,
       ReconstructsNativeCatchAllSEHWithTaggedScopeTableAndRelifts) {
  const auto PE =
      fs::path(TEST_OBJ_DIR) / "test_patch_coff_arm_seh.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "ARM32 SEH PE executable not built (lld-link not "
                    "available)";

  auto OriginalOrErr = loadBinary(PE);
  ASSERT_TRUE(static_cast<bool>(OriginalOrErr))
      << llvm::toString(OriginalOrErr.takeError());
  const BinaryImage &Original = *OriginalOrErr;
  ASSERT_EQ(Original.Arch, Arch::ARM);
  ASSERT_EQ(Original.Mode, InstructionMode::Thumb);

  const ExceptionFunction *OriginalSEH = nullptr;
  for (const ExceptionFunction &EH : Original.ExceptionMetadata.Functions) {
    if (EH.Personality != ExceptionPersonality::CSpecificHandler)
      continue;
    ASSERT_EQ(OriginalSEH, nullptr);
    OriginalSEH = &EH;
  }
  ASSERT_NE(OriginalSEH, nullptr);
  ASSERT_EQ(OriginalSEH->Encoding, ExceptionEncoding::ARM32Unpacked);
  ASSERT_EQ(OriginalSEH->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_FALSE(OriginalSEH->GSCookie.has_value());
  ASSERT_TRUE(OriginalSEH->SEH.has_value());
  ASSERT_EQ(OriginalSEH->SEH->Scopes.size(), 1u);
  ASSERT_EQ(OriginalSEH->SEH->Scopes.front().Kind, SEHScopeKind::CatchAll);
  const WindowsEHNativeSourceClassification OriginalClassification =
      classifyWindowsEHNativeSource(*OriginalSEH, Arch::ARM, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::OutputPatch);
  ASSERT_TRUE(OriginalClassification.canPatchOutput())
      << getWindowsEHNativeSourceReasonName(OriginalClassification.Reason);

  std::ifstream OriginalInput(PE, std::ios::binary);
  ASSERT_TRUE(OriginalInput.good());
  const std::vector<uint8_t> OriginalBytes(
      std::istreambuf_iterator<char>(OriginalInput), {});
  auto OriginalHeaders = locatePEHeaders(
      const_cast<uint8_t *>(OriginalBytes.data()), OriginalBytes.size());
  ASSERT_TRUE(OriginalHeaders.valid());
  ASSERT_GE(OriginalSEH->HandlerDataVA, Original.Base);
  ASSERT_LE(OriginalSEH->HandlerDataVA - Original.Base,
            std::numeric_limits<uint32_t>::max());
  const auto OriginalHandlerDataOffset = fileOffsetForRVA(
      OriginalBytes, OriginalHeaders,
      static_cast<uint32_t>(OriginalSEH->HandlerDataVA - Original.Base),
      5 * sizeof(uint32_t));
  ASSERT_TRUE(OriginalHandlerDataOffset.has_value());
  const uint8_t *OriginalTable =
      OriginalBytes.data() + *OriginalHandlerDataOffset;
  ASSERT_EQ(readLE<uint32_t>(OriginalTable), 1u);
  EXPECT_EQ((readLE<uint32_t>(OriginalTable + sizeof(uint32_t)) &
             readLE<uint32_t>(OriginalTable + 2 * sizeof(uint32_t)) &
             readLE<uint32_t>(OriginalTable + 4 * sizeof(uint32_t))) &
                1u,
            1u);

  const auto Patch = patchBinary(PE);
  ASSERT_EQ(Patch.exitCode, 0) << Patch.err;
  EXPECT_FALSE(Patch.contains(", 0 trampolines)")) << Patch.out;

  const fs::path PatchedFile = tmpFile("patched");
  auto PatchedOrErr = loadBinary(PatchedFile);
  ASSERT_TRUE(static_cast<bool>(PatchedOrErr))
      << llvm::toString(PatchedOrErr.takeError());
  const BinaryImage &Patched = *PatchedOrErr;
  ASSERT_EQ(Patched.ExceptionMetadata.ParseStatus,
            ExceptionParseStatus::Complete);
  const auto Injected = std::find_if(
      Patched.Sections.begin(), Patched.Sections.end(), [](const Section &S) {
        return S.Name == kNdTextSection && S.isExecutable();
      });
  ASSERT_NE(Injected, Patched.Sections.end());

  const ExceptionFunction *GeneratedSEH = nullptr;
  for (const ExceptionFunction &EH : Patched.ExceptionMetadata.Functions) {
    if (!Injected->contains(EH.CodeRange.Begin) ||
        EH.Personality != ExceptionPersonality::CSpecificHandler)
      continue;
    ASSERT_EQ(GeneratedSEH, nullptr);
    GeneratedSEH = &EH;
  }
  ASSERT_NE(GeneratedSEH, nullptr);
  ASSERT_EQ(GeneratedSEH->Encoding, ExceptionEncoding::ARM32Unpacked);
  ASSERT_EQ(GeneratedSEH->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_FALSE(GeneratedSEH->GSCookie.has_value());
  ASSERT_TRUE(GeneratedSEH->SEH.has_value());
  ASSERT_EQ(GeneratedSEH->SEH->Scopes.size(), 1u);
  const SEHScopeRecord &GeneratedScope =
      GeneratedSEH->SEH->Scopes.front();
  EXPECT_EQ(GeneratedScope.Kind, SEHScopeKind::CatchAll);
  EXPECT_TRUE(GeneratedSEH->CodeRange.contains(GeneratedScope.GuardedRange));
  EXPECT_TRUE(GeneratedSEH->CodeRange.contains(GeneratedScope.HandlerVA));
  EXPECT_FALSE(GeneratedScope.GuardedRange.contains(GeneratedScope.HandlerVA));

  const WindowsEHNativeSourceClassification GeneratedClassification =
      classifyWindowsEHNativeSource(
          *GeneratedSEH, Arch::ARM, BinaryFormat::COFF,
          WindowsEHNativeCapability::OutputPatch);
  EXPECT_TRUE(GeneratedClassification.canPatchOutput())
      << getWindowsEHNativeSourceReasonName(GeneratedClassification.Reason);

  std::ifstream PatchedInput(PatchedFile, std::ios::binary);
  ASSERT_TRUE(PatchedInput.good());
  const std::vector<uint8_t> PatchedBytes(
      std::istreambuf_iterator<char>(PatchedInput), {});
  auto Headers = locatePEHeaders(const_cast<uint8_t *>(PatchedBytes.data()),
                                 PatchedBytes.size());
  ASSERT_TRUE(Headers.valid());
  ASSERT_GE(GeneratedSEH->HandlerDataVA, Patched.Base);
  ASSERT_LE(GeneratedSEH->HandlerDataVA - Patched.Base,
            std::numeric_limits<uint32_t>::max());
  const uint32_t HandlerDataRVA =
      static_cast<uint32_t>(GeneratedSEH->HandlerDataVA - Patched.Base);
  const std::optional<size_t> HandlerDataOffset = fileOffsetForRVA(
      PatchedBytes, Headers, HandlerDataRVA, 5 * sizeof(uint32_t));
  ASSERT_TRUE(HandlerDataOffset.has_value());
  const uint8_t *Table = PatchedBytes.data() + *HandlerDataOffset;
  ASSERT_EQ(readLE<uint32_t>(Table), 1u);
  const uint32_t RawBegin = readLE<uint32_t>(Table + sizeof(uint32_t));
  const uint32_t RawEnd = readLE<uint32_t>(Table + 2 * sizeof(uint32_t));
  const uint32_t Action = readLE<uint32_t>(Table + 3 * sizeof(uint32_t));
  const uint32_t RawHandler = readLE<uint32_t>(Table + 4 * sizeof(uint32_t));
  EXPECT_EQ((RawBegin & RawEnd & RawHandler) & 1u, 1u);
  EXPECT_EQ(Action, 1u);
  EXPECT_EQ(Patched.Base + (RawBegin & ~uint32_t(1)),
            GeneratedScope.GuardedRange.Begin);
  EXPECT_EQ(Patched.Base + (RawEnd & ~uint32_t(1)),
            GeneratedScope.GuardedRange.End);
  EXPECT_EQ(Patched.Base + (RawHandler & ~uint32_t(1)),
            GeneratedScope.HandlerVA);

  llvm::Error Validation = validatePatchedCOFFImage(
      PatchedBytes, Arch::ARM,
      /*RequireGeneratedExceptionDirectory=*/true);
  EXPECT_FALSE(static_cast<bool>(Validation))
      << llvm::toString(std::move(Validation));

  auto ExpectUntaggedScopeRejected = [&](size_t WordIndex) {
    std::vector<uint8_t> Tampered = PatchedBytes;
    uint8_t *Word = Tampered.data() + *HandlerDataOffset +
                    WordIndex * sizeof(uint32_t);
    writeLE<uint32_t>(Word, readLE<uint32_t>(Word) & ~uint32_t(1));
    llvm::Error Result = validatePatchedCOFFImage(
        Tampered, Arch::ARM,
        /*RequireGeneratedExceptionDirectory=*/true);
    ASSERT_TRUE(static_cast<bool>(Result));
    const std::string Message = llvm::toString(std::move(Result));
    EXPECT_NE(Message.find("Thumb"), std::string::npos) << Message;
  };
  ExpectUntaggedScopeRejected(1);
  ExpectUntaggedScopeRejected(2);
  ExpectUntaggedScopeRejected(4);

  const auto Relift = liftToLowIR(PatchedFile);
  ASSERT_EQ(Relift.exitCode, 0) << Relift.err;
  std::string ReliftOutput = Relift.out + "\n" + Relift.err;
  std::transform(ReliftOutput.begin(), ReliftOutput.end(), ReliftOutput.begin(),
                 [](unsigned char C) { return std::tolower(C); });
  EXPECT_EQ(ReliftOutput.find("unlifted"), std::string::npos) << ReliftOutput;
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

TEST_F(PatchCOFF_AArch64,
       ReconstructsNativeCatchAllSEHInAppendedSectionAndRelifts) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_a64_seh.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "AArch64 SEH PE executable not built (lld-link not "
                    "available)";

  auto OriginalOrErr = loadBinary(PE);
  ASSERT_TRUE(static_cast<bool>(OriginalOrErr))
      << llvm::toString(OriginalOrErr.takeError());
  const BinaryImage &Original = *OriginalOrErr;
  EXPECT_EQ(Original.Format, BinaryFormat::COFF);
  EXPECT_EQ(Original.Arch, Arch::AArch64);
  EXPECT_EQ(Original.ExceptionMetadata.ParseStatus,
            ExceptionParseStatus::Complete);
  ASSERT_NE(Original.ExceptionMetadata.DirectoryRVA, 0u);
  const auto OriginalPData =
      std::find_if(Original.Sections.begin(), Original.Sections.end(),
                   [](const Section &S) { return S.Name == ".pdata"; });
  ASSERT_NE(OriginalPData, Original.Sections.end());
  EXPECT_TRUE(OriginalPData->contains(Original.Base +
                                      Original.ExceptionMetadata.DirectoryRVA));

  const ExceptionFunction *OriginalSEH = nullptr;
  size_t OriginalSEHCount = 0;
  for (const ExceptionFunction &EH : Original.ExceptionMetadata.Functions) {
    EXPECT_EQ(EH.Kind, RuntimeFunctionKind::Primary);
    if (EH.Personality != ExceptionPersonality::CSpecificHandler)
      continue;
    ++OriginalSEHCount;
    OriginalSEH = &EH;
  }
  ASSERT_EQ(OriginalSEHCount, 1u);
  ASSERT_NE(OriginalSEH, nullptr);
  EXPECT_EQ(OriginalSEH->Encoding, ExceptionEncoding::ARM64Unpacked);
  EXPECT_EQ(OriginalSEH->ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_FALSE(OriginalSEH->GSCookie.has_value());
  // lld-link merges the input .xdata contribution into a read-only output
  // section.  Authenticate its actual RVA owner instead of depending on the
  // optional section spelling.
  const auto OriginalUnwindSection = std::find_if(
      Original.Sections.begin(), Original.Sections.end(),
      [&](const Section &S) { return S.contains(OriginalSEH->UnwindInfoVA); });
  ASSERT_NE(OriginalUnwindSection, Original.Sections.end());
  EXPECT_FALSE(OriginalUnwindSection->isExecutable());
  EXPECT_TRUE(OriginalUnwindSection->contains(OriginalSEH->HandlerDataVA));
  ASSERT_TRUE(OriginalSEH->SEH.has_value());
  ASSERT_EQ(OriginalSEH->SEH->Scopes.size(), 1u);
  const SEHScopeRecord &OriginalScope = OriginalSEH->SEH->Scopes.front();
  EXPECT_EQ(OriginalScope.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(OriginalScope.Kind, SEHScopeKind::CatchAll);
  EXPECT_EQ(OriginalScope.FilterOrFinallyVA, 0u);
  EXPECT_TRUE(OriginalSEH->CodeRange.contains(OriginalScope.GuardedRange));
  EXPECT_TRUE(OriginalSEH->CodeRange.contains(OriginalScope.HandlerVA));
  EXPECT_EQ(OriginalScope.ContinuationVA, OriginalScope.HandlerVA);

  auto Patch = patchBinary(PE);
  ASSERT_EQ(Patch.exitCode, 0) << Patch.err;
  EXPECT_FALSE(Patch.contains(", 0 trampolines)")) << Patch.out;

  const fs::path PatchedFile = tmpFile("patched");
  auto PatchedOrErr = loadBinary(PatchedFile);
  ASSERT_TRUE(static_cast<bool>(PatchedOrErr))
      << llvm::toString(PatchedOrErr.takeError());
  const BinaryImage &Patched = *PatchedOrErr;
  EXPECT_EQ(Patched.ExceptionMetadata.ParseStatus,
            ExceptionParseStatus::Complete);

  auto Injected = std::find_if(
      Patched.Sections.begin(), Patched.Sections.end(), [](const Section &S) {
        return S.Name == kNdTextSection && S.isExecutable();
      });
  ASSERT_NE(Injected, Patched.Sections.end());
  const va_t DirectoryVA =
      Patched.Base + Patched.ExceptionMetadata.DirectoryRVA;
  EXPECT_TRUE(Injected->contains(DirectoryVA));

  const ExceptionFunction *GeneratedSEH = nullptr;
  size_t GeneratedCatchAllCount = 0;
  for (const ExceptionFunction &EH : Patched.ExceptionMetadata.Functions) {
    if (!Injected->contains(EH.CodeRange.Begin) ||
        EH.Personality != ExceptionPersonality::CSpecificHandler || !EH.SEH ||
        EH.SEH->Scopes.size() != 1 ||
        EH.SEH->Scopes.front().Kind != SEHScopeKind::CatchAll)
      continue;
    ++GeneratedCatchAllCount;
    GeneratedSEH = &EH;
  }
  ASSERT_EQ(GeneratedCatchAllCount, 1u);
  ASSERT_NE(GeneratedSEH, nullptr);
  EXPECT_EQ(GeneratedSEH->Kind, RuntimeFunctionKind::Primary);
  EXPECT_EQ(GeneratedSEH->Encoding, ExceptionEncoding::ARM64Unpacked);
  EXPECT_EQ(GeneratedSEH->ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_FALSE(GeneratedSEH->GSCookie.has_value());
  EXPECT_TRUE(Injected->contains(GeneratedSEH->CodeRange.Begin));
  EXPECT_TRUE(Injected->contains(GeneratedSEH->CodeRange.End - 1));
  EXPECT_TRUE(
      Injected->contains(Patched.Base + GeneratedSEH->RuntimeFunctionRVA));
  EXPECT_TRUE(Injected->contains(GeneratedSEH->UnwindInfoVA));
  EXPECT_TRUE(Injected->contains(GeneratedSEH->HandlerDataVA));
  // The runtime personality remains the original executable stub; generated
  // language metadata resolves to it without replacing it with lifted code.
  EXPECT_EQ(GeneratedSEH->PersonalityVA, OriginalSEH->PersonalityVA);

  ASSERT_TRUE(GeneratedSEH->SEH.has_value());
  ASSERT_EQ(GeneratedSEH->SEH->Scopes.size(), 1u);
  const SEHScopeRecord &GeneratedScope = GeneratedSEH->SEH->Scopes.front();
  EXPECT_EQ(GeneratedScope.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(GeneratedScope.Kind, SEHScopeKind::CatchAll);
  EXPECT_EQ(GeneratedScope.FilterOrFinallyVA, 0u);
  EXPECT_TRUE(Injected->contains(GeneratedScope.GuardedRange.Begin));
  EXPECT_TRUE(Injected->contains(GeneratedScope.GuardedRange.End - 1));
  EXPECT_TRUE(Injected->contains(GeneratedScope.HandlerVA));
  EXPECT_EQ(GeneratedScope.ContinuationVA, GeneratedScope.HandlerVA);
  EXPECT_TRUE(Injected->contains(GeneratedScope.ContinuationVA));

  const WindowsEHNativeSourceClassification GeneratedClassification =
      classifyWindowsEHNativeSource(
          *GeneratedSEH, Arch::AArch64, BinaryFormat::COFF,
          WindowsEHNativeCapability::OutputPatch);
  EXPECT_TRUE(GeneratedClassification.canPatchOutput());
  EXPECT_EQ(GeneratedClassification.Reason,
            WindowsEHNativeSourceReason::Eligible);

  const Segment *OriginalCode =
      Patched.getSegmentFor(OriginalSEH->CodeRange.Begin);
  ASSERT_NE(OriginalCode, nullptr);
  ASSERT_TRUE(OriginalCode->isExecutable());
  const uint64_t OriginalEntryOffset =
      OriginalSEH->CodeRange.Begin - OriginalCode->VA;
  ASSERT_LE(OriginalEntryOffset, OriginalCode->Data.size());
  ASSERT_GE(OriginalCode->Data.size() - OriginalEntryOffset,
            sizeof(uint32_t));
  Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Arch::AArch64));
  DecodedInsn Branch{};
  ASSERT_EQ(Decoder.decodeOne(OriginalCode->Data.data() + OriginalEntryOffset,
                              sizeof(uint32_t),
                              OriginalSEH->CodeRange.Begin, Branch),
            sizeof(uint32_t));
  ASSERT_STREQ(Branch.Raw->mnemonic, "b");
  ASSERT_NE(Branch.Raw->detail, nullptr);
  ASSERT_EQ(Branch.Raw->detail->aarch64.op_count, 1u);
  ASSERT_EQ(Branch.Raw->detail->aarch64.operands[0].type, AARCH64_OP_IMM);
  EXPECT_EQ(static_cast<va_t>(
                Branch.Raw->detail->aarch64.operands[0].imm),
            GeneratedSEH->CodeRange.Begin);

  std::ifstream PatchedInput(PatchedFile, std::ios::binary);
  ASSERT_TRUE(PatchedInput.good());
  const std::vector<uint8_t> PatchedBytes(
      std::istreambuf_iterator<char>(PatchedInput), {});
  auto Headers = locatePEHeaders(const_cast<uint8_t *>(PatchedBytes.data()),
                                 PatchedBytes.size());
  ASSERT_TRUE(Headers.valid());
  ASSERT_GE(GeneratedSEH->HandlerDataVA, Patched.Base);
  ASSERT_LE(GeneratedSEH->HandlerDataVA - Patched.Base,
            std::numeric_limits<uint32_t>::max());
  const uint32_t HandlerDataRVA =
      static_cast<uint32_t>(GeneratedSEH->HandlerDataVA - Patched.Base);
  const std::optional<size_t> HandlerDataFileOffset = fileOffsetForRVA(
      PatchedBytes, Headers, HandlerDataRVA, 5 * sizeof(uint32_t));
  ASSERT_TRUE(HandlerDataFileOffset.has_value());
  ASSERT_GE(OriginalSEH->PersonalityVA, Patched.Base);
  ASSERT_LE(OriginalSEH->PersonalityVA - Patched.Base,
            std::numeric_limits<uint32_t>::max());
  const uint32_t ExternalExecutableRVA =
      static_cast<uint32_t>(OriginalSEH->PersonalityVA - Patched.Base);
  ASSERT_GE(GeneratedSEH->CodeRange.Begin, Patched.Base + sizeof(uint32_t));
  ASSERT_LE(GeneratedSEH->CodeRange.End - Patched.Base + sizeof(uint32_t),
            std::numeric_limits<uint32_t>::max());
  const uint32_t OwnerBeginRVA =
      static_cast<uint32_t>(GeneratedSEH->CodeRange.Begin - Patched.Base);
  const uint32_t OwnerEndRVA =
      static_cast<uint32_t>(GeneratedSEH->CodeRange.End - Patched.Base);
  ASSERT_LE(GeneratedScope.GuardedRange.Begin - Patched.Base,
            std::numeric_limits<uint32_t>::max());
  const uint32_t GuardBeginRVA = static_cast<uint32_t>(
      GeneratedScope.GuardedRange.Begin - Patched.Base);

  auto ExpectTamperedTableRejected = [&](llvm::StringRef Case,
                                         uint32_t FieldOffset, uint32_t Value,
                                         llvm::StringRef Expected) {
    SCOPED_TRACE(Case.str());
    std::vector<uint8_t> TamperedBytes = PatchedBytes;
    ASSERT_LE(*HandlerDataFileOffset + FieldOffset,
              TamperedBytes.size() - sizeof(uint32_t));
    writeLE<uint32_t>(TamperedBytes.data() + *HandlerDataFileOffset +
                          FieldOffset,
                      Value);
    llvm::Error Validation = validatePatchedCOFFImage(
        TamperedBytes, Arch::AArch64,
        /*RequireGeneratedExceptionDirectory=*/true);
    ASSERT_TRUE(static_cast<bool>(Validation));
    const std::string Message = llvm::toString(std::move(Validation));
    EXPECT_NE(Message.find(Expected), std::string::npos) << Message;
  };
  constexpr uint32_t CountOffset = 0;
  constexpr uint32_t BeginOffset = sizeof(uint32_t);
  constexpr uint32_t EndOffset = 2 * sizeof(uint32_t);
  constexpr uint32_t FilterOffset = 3 * sizeof(uint32_t);
  constexpr uint32_t HandlerOffset = 4 * sizeof(uint32_t);
  ExpectTamperedTableRejected("non-catch-all", FilterOffset,
                              ExternalExecutableRVA, "non-catch-all scope");
  ExpectTamperedTableRejected("guard begins before owner", BeginOffset,
                              OwnerBeginRVA - sizeof(uint32_t),
                              "guarded range leaves its exact owner");
  ExpectTamperedTableRejected("guard ends after owner", EndOffset,
                              OwnerEndRVA + sizeof(uint32_t),
                              "guarded range leaves its exact owner");
  ExpectTamperedTableRejected("handler leaves owner", HandlerOffset,
                              ExternalExecutableRVA,
                              "handler placement is invalid");
  ExpectTamperedTableRejected("handler enters guarded range", HandlerOffset,
                              GuardBeginRVA, "handler placement is invalid");
  ExpectTamperedTableRejected("unaligned guard", BeginOffset, GuardBeginRVA + 2,
                              "scope address is unaligned");
  ExpectTamperedTableRejected("oversized scope table", CountOffset,
                              (1u << 16) + 1, "scope count is invalid");

  std::vector<uint8_t> RenamedSectionBytes = PatchedBytes;
  auto RenamedSectionHeaders =
      locatePEHeaders(RenamedSectionBytes.data(), RenamedSectionBytes.size());
  ASSERT_TRUE(RenamedSectionHeaders.valid());
  llvm::object::coff_section *GeneratedSection =
      findPESectionMut(RenamedSectionHeaders, kNdTextSection);
  ASSERT_NE(GeneratedSection, nullptr);
  std::fill_n(GeneratedSection->Name, sizeof(GeneratedSection->Name), '\0');
  constexpr llvm::StringLiteral TamperedSectionName = ".badtext";
  static_assert(TamperedSectionName.size() <= kCOFFNameSize);
  std::copy(TamperedSectionName.begin(), TamperedSectionName.end(),
            GeneratedSection->Name);
  llvm::Error RenamedSectionValidation = validatePatchedCOFFImage(
      RenamedSectionBytes, Arch::AArch64,
      /*RequireGeneratedExceptionDirectory=*/true);
  ASSERT_TRUE(static_cast<bool>(RenamedSectionValidation));
  const std::string RenamedSectionMessage =
      llvm::toString(std::move(RenamedSectionValidation));
  EXPECT_NE(RenamedSectionMessage.find(
                "final replacement exception directory has no generated "
                "section owner"),
            std::string::npos)
      << RenamedSectionMessage;

  auto Relift = liftToLowIR(PatchedFile);
  ASSERT_EQ(Relift.exitCode, 0) << Relift.err;
  std::string Output = Relift.out + "\n" + Relift.err;
  std::transform(Output.begin(), Output.end(), Output.begin(),
                 [](unsigned char C) { return std::tolower(C); });
  EXPECT_EQ(Output.find("unlifted"), std::string::npos) << Output;
}

TEST_F(PatchCOFF_AArch64, ReconstructsBoundedNativeFH3GroupAndRelifts) {
  const fs::path PE =
      fs::path(TEST_OBJ_DIR) / "test_patch_coff_a64_cxx_fh3.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "AArch64 FH3 PE executable not built (lld-link not "
                    "available)";

  auto OriginalOrErr = loadBinary(PE);
  ASSERT_TRUE(static_cast<bool>(OriginalOrErr))
      << llvm::toString(OriginalOrErr.takeError());
  const BinaryImage &Original = *OriginalOrErr;
  ASSERT_EQ(Original.Arch, Arch::AArch64);
  ASSERT_EQ(Original.ExceptionMetadata.ParseStatus,
            ExceptionParseStatus::Complete);

  const ExceptionFunction *OriginalCxx = nullptr;
  for (const ExceptionFunction &EH : Original.ExceptionMetadata.Functions) {
    if (EH.Personality != ExceptionPersonality::CxxFrameHandler3)
      continue;
    ASSERT_EQ(OriginalCxx, nullptr);
    OriginalCxx = &EH;
  }
  ASSERT_NE(OriginalCxx, nullptr);
  ASSERT_EQ(OriginalCxx->Kind, RuntimeFunctionKind::Primary);
  ASSERT_EQ(OriginalCxx->Encoding, ExceptionEncoding::ARM64Unpacked);
  ASSERT_EQ(OriginalCxx->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(OriginalCxx->Cxx.has_value());
  ASSERT_EQ(OriginalCxx->Cxx->NativeEncoding, CxxExceptionInfo::Encoding::FH3);
  ASSERT_TRUE(OriginalCxx->Cxx->hasValidStateGraph());
  ASSERT_FALSE(OriginalCxx->Cxx->IsSeparated);
  ASSERT_FALSE(OriginalCxx->Cxx->IsCatchFunclet);
  ASSERT_EQ(OriginalCxx->Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(OriginalCxx->Cxx->TryBlocks.front().Handlers.size(), 2u);
  const std::vector<CxxCatchHandler> &OriginalHandlers =
      OriginalCxx->Cxx->TryBlocks.front().Handlers;
  ASSERT_NE(OriginalHandlers[0].TypeDescriptorVA, 0u);
  EXPECT_EQ(OriginalHandlers[1].TypeDescriptorVA, 0u);
  for (const CxxCatchHandler &Handler : OriginalHandlers) {
    EXPECT_EQ(Handler.CatchObjectOffset, 0);
    EXPECT_EQ(Handler.ParentFrameOffset, 0);
  }
  const WindowsEHNativeSourceClassification OriginalClassification =
      classifyWindowsEHNativeSource(*OriginalCxx, Arch::AArch64,
                                    BinaryFormat::COFF,
                                    WindowsEHNativeCapability::OutputPatch);
  ASSERT_TRUE(OriginalClassification.canPatchOutput())
      << getWindowsEHNativeSourceReasonName(OriginalClassification.Reason);

  const uint8_t *OriginalPersonality =
      Original.readVA(OriginalCxx->PersonalityVA, 1);
  ASSERT_NE(OriginalPersonality, nullptr);
  const uint8_t OriginalPersonalityOpcode = *OriginalPersonality;

  const auto Decompile = decompileToHighC(PE);
  ASSERT_EQ(Decompile.exitCode, 0) << Decompile.err;
  std::ifstream HighCInput(tmpFile("decompiled_high.c"));
  ASSERT_TRUE(HighCInput.good());
  const std::string HighC((std::istreambuf_iterator<char>(HighCInput)),
                          std::istreambuf_iterator<char>());
  EXPECT_NE(HighC.find("personality=__CxxFrameHandler3"), std::string::npos);
  EXPECT_NE(HighC.find("cxx.try[0]"), std::string::npos);
  EXPECT_NE(HighC.find("catch[1]"), std::string::npos);
  EXPECT_NE(HighC.find("type_descriptor@0x"), std::string::npos);

  const auto Patch = patchBinary(PE);
  ASSERT_EQ(Patch.exitCode, 0) << Patch.err;
  const fs::path PatchedFile = tmpFile("patched");
  auto PatchedOrErr = loadBinary(PatchedFile);
  ASSERT_TRUE(static_cast<bool>(PatchedOrErr))
      << llvm::toString(PatchedOrErr.takeError());
  const BinaryImage &Patched = *PatchedOrErr;
  ASSERT_EQ(Patched.Arch, Arch::AArch64);
  ASSERT_EQ(Patched.ExceptionMetadata.ParseStatus,
            ExceptionParseStatus::Complete);

  const uint8_t *PatchedPersonality =
      Patched.readVA(OriginalCxx->PersonalityVA, 1);
  ASSERT_NE(PatchedPersonality, nullptr);
  EXPECT_EQ(*PatchedPersonality, OriginalPersonalityOpcode);

  const auto Injected = std::find_if(
      Patched.Sections.begin(), Patched.Sections.end(), [](const Section &S) {
        return S.Name == kNdTextSection && S.isExecutable();
      });
  ASSERT_NE(Injected, Patched.Sections.end());

  std::vector<const ExceptionFunction *> GeneratedGroup;
  const ExceptionFunction *GeneratedRoot = nullptr;
  std::vector<const ExceptionFunction *> GeneratedCatches;
  for (const ExceptionFunction &EH : Patched.ExceptionMetadata.Functions) {
    if (!Injected->contains(EH.CodeRange.Begin) ||
        EH.Personality != ExceptionPersonality::CxxFrameHandler3 || !EH.Cxx)
      continue;
    GeneratedGroup.push_back(&EH);
    if (EH.Cxx->IsCatchFunclet)
      GeneratedCatches.push_back(&EH);
    else {
      ASSERT_EQ(GeneratedRoot, nullptr);
      GeneratedRoot = &EH;
    }
  }
  ASSERT_EQ(GeneratedGroup.size(), 3u);
  ASSERT_NE(GeneratedRoot, nullptr);
  ASSERT_EQ(GeneratedCatches.size(), 2u);
  for (const ExceptionFunction *EH : GeneratedGroup) {
    EXPECT_EQ(EH->Kind, RuntimeFunctionKind::Primary);
    EXPECT_EQ(EH->Encoding, ExceptionEncoding::ARM64Unpacked);
    EXPECT_EQ(EH->ParseStatus, ExceptionParseStatus::Complete);
    ASSERT_TRUE(EH->Cxx.has_value());
    EXPECT_TRUE(EH->Cxx->IsSeparated);
    EXPECT_EQ(EH->Cxx->NativeFuncInfoVA, GeneratedRoot->Cxx->NativeFuncInfoVA);
    EXPECT_TRUE(Injected->contains(EH->UnwindInfoVA));
    EXPECT_TRUE(Injected->contains(EH->HandlerDataVA));
  }
  ASSERT_TRUE(GeneratedRoot->Cxx->hasValidStateGraph());
  ASSERT_EQ(GeneratedRoot->Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(GeneratedRoot->Cxx->TryBlocks.front().Handlers.size(), 2u);
  const std::vector<CxxCatchHandler> &GeneratedHandlers =
      GeneratedRoot->Cxx->TryBlocks.front().Handlers;
  std::set<va_t> GeneratedCatchEntries;
  for (const ExceptionFunction *Catch : GeneratedCatches)
    ASSERT_TRUE(GeneratedCatchEntries.insert(Catch->CodeRange.Begin).second);
  for (size_t I = 0; I < GeneratedHandlers.size(); ++I) {
    const CxxCatchHandler &GeneratedHandler = GeneratedHandlers[I];
    EXPECT_EQ(GeneratedCatchEntries.count(GeneratedHandler.HandlerVA), 1u);
    EXPECT_EQ(GeneratedHandler.TypeDescriptorVA,
              OriginalHandlers[I].TypeDescriptorVA);
    EXPECT_EQ(GeneratedHandler.Adjectives, OriginalHandlers[I].Adjectives);
    EXPECT_EQ(GeneratedHandler.CatchObjectOffset, 0);
    EXPECT_EQ(GeneratedHandler.ParentFrameOffset, 0);
  }
  for (const CxxIPState &State : GeneratedRoot->Cxx->IPMap) {
    const bool IsGeneratedGroupEnd =
        llvm::any_of(GeneratedGroup, [&](const ExceptionFunction *Member) {
          return State.IP == Member->CodeRange.End;
        });
    EXPECT_TRUE(Injected->contains(State.IP) || IsGeneratedGroupEnd);
  }

  const auto Relift = liftToLowIR(PatchedFile);
  ASSERT_EQ(Relift.exitCode, 0) << Relift.err;
  EXPECT_NE(Relift.out.find("personality=__CxxFrameHandler3"),
            std::string::npos);

  std::ifstream PatchedInput(PatchedFile, std::ios::binary);
  ASSERT_TRUE(PatchedInput.good());
  const std::vector<uint8_t> PatchedBytes(
      std::istreambuf_iterator<char>(PatchedInput), {});
  llvm::Error UnreceiptedValidation =
      validatePatchedCOFFImage(PatchedBytes, Arch::AArch64,
                               /*RequireGeneratedExceptionDirectory=*/true);
  ASSERT_TRUE(static_cast<bool>(UnreceiptedValidation));
  EXPECT_EQ(llvm::toString(std::move(UnreceiptedValidation)),
            "coff exception patch: final generated C++ language group lacks "
            "a prepared receipt");
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
