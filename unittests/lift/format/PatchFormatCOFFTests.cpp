//===- PatchFormatCOFFTests.cpp - x86-64 COFF/PE patch tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PatchFormatTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/codegen/COFF/COFFFH4Encoding.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"

#include "llvm/Support/SHA256.h"
#include "llvm/Support/Win64EH.h"

namespace {

using namespace neverd;
using namespace neverd::patch_format_test;

class PatchCOFF_X64 : public NeverDLiftTest {};

TEST_F(PatchCOFF_X64, SwitchPatchSucceeds) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "COFF executable not built (lld-link not available)";
  auto R = patchBinary(PE);
  ASSERT_EQ(R.exitCode, 0) << "COFF patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile)) << "Patched binary not created";
  EXPECT_GT(fs::file_size(PatchedFile), 0u) << "Patched binary is empty";
}

TEST_F(PatchCOFF_X64, AllStagesThenPatch) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "COFF executable not built";
  verifyAllStages(PE);
  auto R = patchBinary(PE);
  ASSERT_EQ(R.exitCode, 0) << "Patch after lift failed: " << R.err;
}

TEST_F(PatchCOFF_X64, PatchedBinaryIsValidPE) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "COFF executable not built";
  auto R = patchBinary(PE);
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile));

  auto VerifyR = liftToLowIR(PatchedFile);
  EXPECT_EQ(VerifyR.exitCode, 0)
      << "Patched PE should be liftable: " << VerifyR.err;
}

TEST_F(PatchCOFF_X64, RejectsInteriorExceptionDirectoryPadding) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_cxx_fh3.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "FH3 COFF executable not built";

  auto Corrupt = tmpFile("coff-interior-pdata-zero.exe");
  ASSERT_TRUE(zeroFirstExceptionRecord(PE, Corrupt, 3 * sizeof(uint32_t)));

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

TEST_F(PatchCOFF_X64, RebuildsSortedExceptionDirectoryInAppendedSection) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "COFF executable not built";
  auto R = patchBinary(PE);
  ASSERT_EQ(R.exitCode, 0) << R.err;

  auto ImgOrErr = loadBinary(tmpFile("patched"));
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  ASSERT_FALSE(Img.ExceptionMetadata.Functions.empty());
  ASSERT_NE(Img.ExceptionMetadata.DirectoryRVA, 0u);

  const Section *Injected = nullptr;
  for (const Section &S : Img.Sections)
    if (S.Name == kNdTextSection) {
      Injected = &S;
      break;
    }
  ASSERT_NE(Injected, nullptr);
  va_t DirectoryVA = Img.Base + Img.ExceptionMetadata.DirectoryRVA;
  EXPECT_TRUE(Injected->contains(DirectoryVA));

  va_t PreviousBegin = 0;
  bool HasGeneratedRecord = false;
  for (const ExceptionFunction &EH : Img.ExceptionMetadata.Functions) {
    EXPECT_GE(EH.CodeRange.Begin, PreviousBegin);
    PreviousBegin = EH.CodeRange.Begin;
    if (Injected->contains(EH.CodeRange.Begin))
      HasGeneratedRecord = true;
  }
  EXPECT_TRUE(HasGeneratedRecord);
}

TEST_F(PatchCOFF_X64, ReconstructsGuardedSEHAndContinuationTable) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_seh_guard.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "guarded COFF executable not built";

  auto OriginalOrErr = loadBinary(PE);
  ASSERT_TRUE(static_cast<bool>(OriginalOrErr))
      << llvm::toString(OriginalOrErr.takeError());
  const BinaryImage &Original = *OriginalOrErr;
  const uint32_t EHContPresent =
      uint32_t(llvm::COFF::GuardFlags::EH_CONTINUATION_TABLE_PRESENT);
  const uint32_t CFPresent =
      uint32_t(llvm::COFF::GuardFlags::CF_FUNCTION_TABLE_PRESENT);
  EXPECT_NE(Original.DynInfo.GuardFlags & EHContPresent, 0u);
  EXPECT_NE(Original.DynInfo.GuardFlags & CFPresent, 0u);
  ASSERT_GT(Original.DynInfo.GuardEHContinuationCount, 0u);

  const ExceptionFunction *OriginalSEH = nullptr;
  for (const ExceptionFunction &EH : Original.ExceptionMetadata.Functions)
    if (EH.Personality == ExceptionPersonality::CSpecificHandler) {
      OriginalSEH = &EH;
      break;
    }
  ASSERT_NE(OriginalSEH, nullptr);
  EXPECT_EQ(OriginalSEH->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(OriginalSEH->SEH.has_value());
  ASSERT_EQ(OriginalSEH->SEH->Scopes.size(), 1u);
  const uint8_t *OriginalPersonality =
      Original.readVA(OriginalSEH->PersonalityVA, 1);
  ASSERT_NE(OriginalPersonality, nullptr);
  const uint8_t OriginalPersonalityOpcode = *OriginalPersonality;

  auto Decompile = decompileToHighC(PE);
  ASSERT_EQ(Decompile.exitCode, 0) << Decompile.err;
  std::ifstream HighCInput(tmpFile("decompiled_high.c"));
  ASSERT_TRUE(HighCInput.good());
  std::string HighC((std::istreambuf_iterator<char>(HighCInput)),
                    std::istreambuf_iterator<char>());
  EXPECT_NE(HighC.find("__try"), std::string::npos);
  EXPECT_NE(HighC.find("__except"), std::string::npos);

  auto Patch = patchBinary(PE);
  ASSERT_EQ(Patch.exitCode, 0) << Patch.err;
  const fs::path PatchedPath = tmpFile("patched");
  auto PatchedOrErr = loadBinary(PatchedPath);
  ASSERT_TRUE(static_cast<bool>(PatchedOrErr))
      << llvm::toString(PatchedOrErr.takeError());
  const BinaryImage &Patched = *PatchedOrErr;
  const uint8_t *PatchedPersonality =
      Patched.readVA(OriginalSEH->PersonalityVA, 1);
  ASSERT_NE(PatchedPersonality, nullptr);
  // The local runtime handler owns this address. Native EH references resolve
  // to it, but it must not be replaced by a trampoline to a generically typed
  // lifted body.
  EXPECT_EQ(*PatchedPersonality, OriginalPersonalityOpcode);
  EXPECT_NE(Patched.DynInfo.GuardFlags & EHContPresent, 0u);
  EXPECT_NE(Patched.DynInfo.GuardFlags & CFPresent, 0u);
  EXPECT_GT(Patched.DynInfo.GuardCFFunctionCount,
            Original.DynInfo.GuardCFFunctionCount);
  EXPECT_GE(Patched.DynInfo.GuardEHContinuationCount,
            Original.DynInfo.GuardEHContinuationCount);

  const Section *Injected = nullptr;
  for (const Section &S : Patched.Sections)
    if (S.Name == kNdTextSection) {
      Injected = &S;
      break;
    }
  ASSERT_NE(Injected, nullptr);

  const ExceptionFunction *GeneratedSEH = nullptr;
  for (const ExceptionFunction &EH : Patched.ExceptionMetadata.Functions)
    if (Injected->contains(EH.CodeRange.Begin) &&
        EH.Personality == ExceptionPersonality::CSpecificHandler) {
      GeneratedSEH = &EH;
      break;
    }
  ASSERT_NE(GeneratedSEH, nullptr);
  EXPECT_EQ(GeneratedSEH->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(GeneratedSEH->SEH.has_value());
  ASSERT_FALSE(GeneratedSEH->SEH->Scopes.empty());
  for (const SEHScopeRecord &Scope : GeneratedSEH->SEH->Scopes) {
    ASSERT_TRUE(Scope.GuardedRange.isValid());
    EXPECT_TRUE(Injected->contains(Scope.GuardedRange.Begin));
    EXPECT_TRUE(Injected->contains(Scope.GuardedRange.End - 1));
    EXPECT_TRUE(Injected->contains(Scope.HandlerVA));
    if (Scope.Kind == SEHScopeKind::Filter)
      EXPECT_TRUE(Injected->contains(Scope.FilterOrFinallyVA));
    if (Scope.Kind == SEHScopeKind::Finally)
      EXPECT_TRUE(Injected->contains(Scope.FilterOrFinallyVA));
    if (Scope.ContinuationVA != 0)
      EXPECT_TRUE(Injected->contains(Scope.ContinuationVA));
  }

  ASSERT_NE(Patched.DynInfo.GuardEHContinuationTableRVA, 0u);
  const uint8_t *Table = Patched.readVA(
      Patched.Base + Patched.DynInfo.GuardEHContinuationTableRVA,
      Patched.DynInfo.GuardEHContinuationCount * sizeof(uint32_t));
  ASSERT_NE(Table, nullptr);
  uint32_t Previous = 0;
  for (uint64_t I = 0; I < Patched.DynInfo.GuardEHContinuationCount; ++I) {
    uint32_t RVA = readLE<uint32_t>(Table + I * sizeof(uint32_t));
    if (I != 0)
      EXPECT_GT(RVA, Previous);
    const Segment *Target = Patched.getSegmentFor(Patched.Base + RVA);
    ASSERT_NE(Target, nullptr);
    EXPECT_TRUE(Target->isExecutable());
    Previous = RVA;
  }
}

TEST_F(PatchCOFF_X64, RejectsGeneratedSEHHandlerInsideGuardedRange) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_seh_guard.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "guarded COFF executable not built";

  auto Patch = patchBinary(PE);
  ASSERT_EQ(Patch.exitCode, 0) << Patch.err;
  const fs::path PatchedPath = tmpFile("patched");
  auto PatchedOrErr = loadBinary(PatchedPath);
  ASSERT_TRUE(static_cast<bool>(PatchedOrErr))
      << llvm::toString(PatchedOrErr.takeError());
  const BinaryImage &Patched = *PatchedOrErr;

  const Section *Injected = nullptr;
  for (const Section &Section : Patched.Sections)
    if (Section.Name == kNdTextSection) {
      Injected = &Section;
      break;
    }
  ASSERT_NE(Injected, nullptr);

  const ExceptionFunction *GeneratedSEH = nullptr;
  for (const ExceptionFunction &EH : Patched.ExceptionMetadata.Functions)
    if (Injected->contains(EH.CodeRange.Begin) &&
        EH.Personality == ExceptionPersonality::CSpecificHandler && EH.SEH) {
      GeneratedSEH = &EH;
      break;
    }
  ASSERT_NE(GeneratedSEH, nullptr);
  ASSERT_EQ(GeneratedSEH->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_EQ(GeneratedSEH->SEH->Scopes.size(), 1u);
  const SEHScopeRecord &Scope = GeneratedSEH->SEH->Scopes.front();
  ASSERT_EQ(Scope.Kind, SEHScopeKind::CatchAll);
  ASSERT_TRUE(Scope.GuardedRange.isValid());
  ASSERT_TRUE(GeneratedSEH->CodeRange.contains(Scope.GuardedRange.Begin));

  std::ifstream PatchedInput(PatchedPath, std::ios::binary);
  ASSERT_TRUE(PatchedInput.good());
  const std::vector<uint8_t> PatchedBytes(
      std::istreambuf_iterator<char>(PatchedInput), {});
  ASSERT_FALSE(PatchedBytes.empty());
  if (llvm::Error Err =
          validatePatchedCOFFImage(PatchedBytes, Arch::X64,
                                   /*RequireGeneratedExceptionDirectory=*/true))
    FAIL() << llvm::toString(std::move(Err));

  const uint64_t HandlerDataOffset =
      Patched.vaToFileOffset(GeneratedSEH->HandlerDataVA);
  ASSERT_NE(HandlerDataOffset, InvalidVA);
  constexpr size_t HandlerFieldOffset = 4 * sizeof(uint32_t);
  ASSERT_LE(HandlerDataOffset, PatchedBytes.size());
  ASSERT_GE(PatchedBytes.size() - HandlerDataOffset,
            HandlerFieldOffset + sizeof(uint32_t));
  ASSERT_GE(Scope.GuardedRange.Begin, Patched.Base);
  ASSERT_LE(Scope.GuardedRange.Begin - Patched.Base,
            std::numeric_limits<uint32_t>::max());

  std::vector<uint8_t> TamperedBytes = PatchedBytes;
  writeLE<uint32_t>(
      TamperedBytes.data() + HandlerDataOffset + HandlerFieldOffset,
      static_cast<uint32_t>(Scope.GuardedRange.Begin - Patched.Base));
  llvm::Error Validation =
      validatePatchedCOFFImage(TamperedBytes, Arch::X64,
                               /*RequireGeneratedExceptionDirectory=*/true);
  ASSERT_TRUE(static_cast<bool>(Validation));
  EXPECT_EQ(llvm::toString(std::move(Validation)),
            "coff exception patch: final generated x64 language metadata "
            "failed output checks: invalid-seh-scope");
}

TEST_F(PatchCOFF_X64, RequiresExactGeneratedLanguageEHRuntimeOwnerMapping) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_seh_guard.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "guarded COFF executable not built";

  auto OriginalOrErr = loadBinary(PE);
  ASSERT_TRUE(static_cast<bool>(OriginalOrErr))
      << llvm::toString(OriginalOrErr.takeError());
  const BinaryImage &Original = *OriginalOrErr;

  const ExceptionFunction *OriginalSEH = nullptr;
  for (const ExceptionFunction &EH : Original.ExceptionMetadata.Functions)
    if (EH.Kind == RuntimeFunctionKind::Primary &&
        EH.Personality == ExceptionPersonality::CSpecificHandler) {
      OriginalSEH = &EH;
      break;
    }
  ASSERT_NE(OriginalSEH, nullptr);
  ASSERT_TRUE(OriginalSEH->SEH.has_value());
  ASSERT_EQ(OriginalSEH->SEH->Scopes.size(), 1u);
  ASSERT_EQ(OriginalSEH->SEH->Scopes.front().Kind, SEHScopeKind::CatchAll);
  ASSERT_GT(OriginalSEH->PersonalityVA, Original.Base);
  ASSERT_LE(OriginalSEH->PersonalityVA - Original.Base,
            std::numeric_limits<uint32_t>::max());

  std::ifstream Input(PE, std::ios::binary);
  ASSERT_TRUE(Input.good());
  const std::vector<uint8_t> Bytes(std::istreambuf_iterator<char>(Input), {});
  ASSERT_FALSE(Bytes.empty());

  COFFPatcher Patcher;
  const va_t NewSectionVA = Patcher.plannedExecSegmentVA(Bytes, Arch::X64);
  ASSERT_GT(NewSectionVA, Original.Base);
  ASSERT_LE(NewSectionVA - Original.Base, std::numeric_limits<uint32_t>::max());

  const uint32_t NewSectionRVA =
      static_cast<uint32_t>(NewSectionVA - Original.Base);
  const uint32_t PersonalityRVA =
      static_cast<uint32_t>(OriginalSEH->PersonalityVA - Original.Base);
  const auto SemanticToken =
      windows_eh_semantics::getSEHScopeSemanticToken(*OriginalSEH, Arch::X64,
                                                     /*ScopeIndex=*/0);
  ASSERT_TRUE(SemanticToken.has_value());
  auto MakeCompiledImage = [&] {
    CompiledImage Compiled;
    Compiled.Bytes.resize(0x4c, 0);
    Compiled.BaseVA = NewSectionVA;
    Compiled.TargetArch = Arch::X64;
    Compiled.Format = BinaryFormat::COFF;
    Compiled.PointerWidth = 8;
    Compiled.Success = true;
    Compiled.FunctionOwnerAddrs.emplace("seh.owner", NewSectionVA);
    Compiled.FunctionRanges.push_back({1, "seh.owner", NewSectionVA,
                                       "seh.begin", NewSectionVA, "seh.end",
                                       NewSectionVA + 0x20});
    Compiled.SourceFunctionOwners.push_back(
        {"source.seh", "seh.owner", NewSectionVA});
    Compiled.SourceFunctionOriginalVAs.emplace("source.seh",
                                               OriginalSEH->CodeRange.Begin);
    Compiled.WinEHSemanticRecords.push_back(
        {*SemanticToken, "source.seh", "seh.owner", NewSectionVA,
         "seh.scope.table", NewSectionVA + 0x3c, NewSectionVA + 0x3c,
         4 * sizeof(uint32_t), "seh.scope.begin", NewSectionVA, "seh.scope.end",
         NewSectionVA + 0x10, "seh.handler", NewSectionVA + 0x10});
    Compiled.Sections.push_back({".text", 0, NewSectionVA, 0x20, 16,
                                 llvm::mc_rewrite::RewriteSectionKind::Code,
                                 true});
    Compiled.Sections.push_back(
        {".pdata", 0x20, NewSectionVA + 0x20, 0x0c, 4,
         llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData, true});
    Compiled.Sections.push_back(
        {".xdata", 0x30, NewSectionVA + 0x30, 0x1c, 4,
         llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData, true});

    writeLE<uint32_t>(Compiled.Bytes.data() + 0x20, NewSectionRVA);
    writeLE<uint32_t>(Compiled.Bytes.data() + 0x24, NewSectionRVA + 0x20);
    writeLE<uint32_t>(Compiled.Bytes.data() + 0x28, NewSectionRVA + 0x30);
    Compiled.Bytes[0x30] = 1 | (llvm::Win64EH::UNW_ExceptionHandler << 3);
    writeLE<uint32_t>(Compiled.Bytes.data() + 0x34, PersonalityRVA);
    writeLE<uint32_t>(Compiled.Bytes.data() + 0x38, 1);
    writeLE<uint32_t>(Compiled.Bytes.data() + 0x3c, NewSectionRVA);
    writeLE<uint32_t>(Compiled.Bytes.data() + 0x40, NewSectionRVA + 0x10);
    writeLE<uint32_t>(Compiled.Bytes.data() + 0x44, 1);
    writeLE<uint32_t>(Compiled.Bytes.data() + 0x48, NewSectionRVA + 0x10);
    return Compiled;
  };

  const std::array<va_t, 1> PatchedEntries{{
      OriginalSEH->CodeRange.Begin,
  }};
  const std::array<std::pair<va_t, va_t>, 1> ExactMapping{{
      {OriginalSEH->CodeRange.Begin, NewSectionVA},
  }};
  CompiledImage ExactCompiled = MakeCompiledImage();
  auto ExactUpdate = prepareCOFFExceptionDirectory(
      Bytes, Original, ExactCompiled, PatchedEntries, ExactMapping,
      NewSectionVA, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(ExactUpdate))
      << llvm::toString(ExactUpdate.takeError());
  EXPECT_TRUE(ExactUpdate->Apply);
  EXPECT_NE(ExactUpdate->RVA, 0u);
  EXPECT_NE(ExactUpdate->Size, 0u);

  CompiledImage TokenTamperedCompiled = MakeCompiledImage();
  ASSERT_FALSE(TokenTamperedCompiled.WinEHSemanticRecords.empty());
  TokenTamperedCompiled.WinEHSemanticRecords.front().Token.Digest[0] ^= 1u;
  auto TokenTamperedUpdate = prepareCOFFExceptionDirectory(
      Bytes, Original, TokenTamperedCompiled, PatchedEntries, ExactMapping,
      NewSectionVA, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(TokenTamperedUpdate));
  EXPECT_EQ(llvm::toString(TokenTamperedUpdate.takeError()),
            "coff exception patch: compiler EH semantic row does not match "
            "its immutable source");

  CompiledImage MissingSemanticCompiled = MakeCompiledImage();
  MissingSemanticCompiled.WinEHSemanticRecords.clear();
  auto MissingSemanticUpdate = prepareCOFFExceptionDirectory(
      Bytes, Original, MissingSemanticCompiled, PatchedEntries, ExactMapping,
      NewSectionVA, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(MissingSemanticUpdate));
  EXPECT_EQ(llvm::toString(MissingSemanticUpdate.takeError()),
            "coff exception patch: compiler EH semantic tokens do not close "
            "the immutable source graph");

  CompiledImage ContainerTamperedCompiled = MakeCompiledImage();
  ASSERT_FALSE(ContainerTamperedCompiled.WinEHSemanticRecords.empty());
  ContainerTamperedCompiled.WinEHSemanticRecords.front().ContainerVA +=
      sizeof(uint32_t);
  auto ContainerTamperedUpdate = prepareCOFFExceptionDirectory(
      Bytes, Original, ContainerTamperedCompiled, PatchedEntries, ExactMapping,
      NewSectionVA, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(ContainerTamperedUpdate));
  EXPECT_EQ(llvm::toString(ContainerTamperedUpdate.takeError()),
            "coff exception patch: generated language owners lack valid "
            "compiler EH semantics");

  CompiledImage ExtendedCompiled = MakeCompiledImage();
  writeLE<uint32_t>(ExtendedCompiled.Bytes.data() + 0x24, NewSectionRVA + 0x24);
  auto ExtendedUpdate = prepareCOFFExceptionDirectory(
      Bytes, Original, ExtendedCompiled, PatchedEntries, ExactMapping,
      NewSectionVA, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(ExtendedUpdate));
  EXPECT_EQ(llvm::toString(ExtendedUpdate.takeError()),
            "coff exception patch: generated language runtime extent does not "
            "match its compiler owner");

  const std::array<std::pair<va_t, va_t>, 1> InteriorMapping{{
      {OriginalSEH->CodeRange.Begin, NewSectionVA + 4},
  }};
  CompiledImage InteriorCompiled = MakeCompiledImage();
  auto InteriorUpdate = prepareCOFFExceptionDirectory(
      Bytes, Original, InteriorCompiled, PatchedEntries, InteriorMapping,
      NewSectionVA, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(InteriorUpdate));
  EXPECT_EQ(llvm::toString(InteriorUpdate.takeError()),
            "coff exception patch: generated language-EH function has no "
            "matching language-handler runtime record");
}

TEST_F(PatchCOFF_X64, RejectsSemanticReceiptWithoutAppliedUpdate) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_seh_guard.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "guarded COFF executable not built";

  std::ifstream Input(PE, std::ios::binary);
  ASSERT_TRUE(Input.good());
  const std::vector<uint8_t> Bytes(std::istreambuf_iterator<char>(Input), {});
  ASSERT_FALSE(Bytes.empty());

  COFFExceptionDirectoryUpdate Unexpected;
  Unexpected.GeneratedEHSemantics.emplace_back();
  llvm::Error Validation = validatePatchedCOFFImage(
      Bytes, Arch::X64, /*RequireGeneratedExceptionDirectory=*/false,
      Unexpected);
  ASSERT_TRUE(static_cast<bool>(Validation));
  EXPECT_EQ(llvm::toString(std::move(Validation)),
            "coff exception patch: non-applied exception update carries "
            "replacement state");
}

TEST_F(PatchCOFF_X64, ReconstructsNativeFH3StateGraph) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_cxx_fh3.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "FH3 COFF executable not built";

  auto OriginalOrErr = loadBinary(PE);
  ASSERT_TRUE(static_cast<bool>(OriginalOrErr))
      << llvm::toString(OriginalOrErr.takeError());
  const BinaryImage &Original = *OriginalOrErr;
  const ExceptionFunction *OriginalCxx = nullptr;
  for (const ExceptionFunction &EH : Original.ExceptionMetadata.Functions)
    if (EH.Personality == ExceptionPersonality::CxxFrameHandler3) {
      OriginalCxx = &EH;
      break;
    }
  ASSERT_NE(OriginalCxx, nullptr);
  EXPECT_EQ(OriginalCxx->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(OriginalCxx->Cxx.has_value());
  EXPECT_TRUE(OriginalCxx->Cxx->hasValidStateGraph());
  const WindowsEHNativeSourceClassification NativeSource =
      classifyWindowsEHNativeSource(*OriginalCxx, Arch::X64,
                                    BinaryFormat::COFF);
  ASSERT_TRUE(NativeSource.canRegenerateLanguageMetadata())
      << getWindowsEHNativeSourceReasonName(NativeSource.Reason);
  ASSERT_EQ(OriginalCxx->Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(OriginalCxx->Cxx->TryBlocks.front().Handlers.size(), 1u);
  const uint8_t *OriginalPersonality =
      Original.readVA(OriginalCxx->PersonalityVA, 1);
  ASSERT_NE(OriginalPersonality, nullptr);
  const uint8_t OriginalPersonalityOpcode = *OriginalPersonality;

  auto Decompile = decompileToHighC(PE);
  ASSERT_EQ(Decompile.exitCode, 0) << Decompile.err;
  std::ifstream HighCInput(tmpFile("decompiled_high.c"));
  ASSERT_TRUE(HighCInput.good());
  std::string HighC((std::istreambuf_iterator<char>(HighCInput)),
                    std::istreambuf_iterator<char>());
  EXPECT_NE(HighC.find("personality=__CxxFrameHandler3"), std::string::npos);
  EXPECT_NE(HighC.find("cxx.try[0]"), std::string::npos);

  auto Patch = patchBinary(PE);
  ASSERT_EQ(Patch.exitCode, 0) << Patch.err;
  const fs::path PatchedPath = tmpFile("patched");
  auto PatchedOrErr = loadBinary(PatchedPath);
  ASSERT_TRUE(static_cast<bool>(PatchedOrErr))
      << llvm::toString(PatchedOrErr.takeError());
  const BinaryImage &Patched = *PatchedOrErr;
  const uint8_t *PatchedPersonality =
      Patched.readVA(OriginalCxx->PersonalityVA, 1);
  ASSERT_NE(PatchedPersonality, nullptr);
  EXPECT_EQ(*PatchedPersonality, OriginalPersonalityOpcode);

  const Section *Injected = nullptr;
  for (const Section &S : Patched.Sections)
    if (S.Name == kNdTextSection) {
      Injected = &S;
      break;
    }
  ASSERT_NE(Injected, nullptr);

  const ExceptionFunction *GeneratedCxx = nullptr;
  for (const ExceptionFunction &EH : Patched.ExceptionMetadata.Functions)
    if (Injected->contains(EH.CodeRange.Begin) &&
        EH.Personality == ExceptionPersonality::CxxFrameHandler3) {
      GeneratedCxx = &EH;
      break;
    }
  ASSERT_NE(GeneratedCxx, nullptr);
  EXPECT_EQ(GeneratedCxx->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(GeneratedCxx->Cxx.has_value());
  EXPECT_EQ(GeneratedCxx->Cxx->NativeEncoding, CxxExceptionInfo::Encoding::FH3);
  EXPECT_TRUE(GeneratedCxx->Cxx->hasValidStateGraph());
  ASSERT_FALSE(GeneratedCxx->Cxx->TryBlocks.empty());
  for (const CxxTryBlock &Try : GeneratedCxx->Cxx->TryBlocks)
    for (const CxxCatchHandler &Catch : Try.Handlers)
      EXPECT_TRUE(Injected->contains(Catch.HandlerVA));
  for (const CxxIPState &State : GeneratedCxx->Cxx->IPMap)
    EXPECT_TRUE(Injected->contains(State.IP) ||
                State.IP == GeneratedCxx->CodeRange.End);

}

TEST_F(PatchCOFF_X64, ReconstructsBoundedNativeFH4StateGraph) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_cxx_fh4.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "FH4 COFF executable not built";

  auto OriginalOrErr = loadBinary(PE);
  ASSERT_TRUE(static_cast<bool>(OriginalOrErr))
      << llvm::toString(OriginalOrErr.takeError());
  const BinaryImage &Original = *OriginalOrErr;
  const ExceptionFunction *OriginalCxx = nullptr;
  for (const ExceptionFunction &EH : Original.ExceptionMetadata.Functions)
    if (EH.Personality == ExceptionPersonality::CxxFrameHandler4) {
      ASSERT_EQ(OriginalCxx, nullptr);
      OriginalCxx = &EH;
    }
  ASSERT_NE(OriginalCxx, nullptr);
  ASSERT_EQ(OriginalCxx->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(OriginalCxx->Cxx.has_value());
  EXPECT_EQ(OriginalCxx->Cxx->NativeEncoding, CxxExceptionInfo::Encoding::FH4);
  EXPECT_TRUE(OriginalCxx->Cxx->hasValidStateGraph());
  const WindowsEHNativeSourceClassification NativeSource =
      classifyWindowsEHNativeSource(*OriginalCxx, Arch::X64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::OutputPatch);
  ASSERT_TRUE(NativeSource.canPatchOutput())
      << getWindowsEHNativeSourceReasonName(NativeSource.Reason);

  auto Lift = liftToLowIR(PE);
  ASSERT_EQ(Lift.exitCode, 0) << Lift.err;
  EXPECT_NE(Lift.out.find("personality=__CxxFrameHandler4"), std::string::npos);

  auto Decompile = decompileToHighC(PE);
  ASSERT_EQ(Decompile.exitCode, 0) << Decompile.err;
  std::ifstream HighCInput(tmpFile("decompiled_high.c"));
  ASSERT_TRUE(HighCInput.good());
  std::string HighC((std::istreambuf_iterator<char>(HighCInput)),
                    std::istreambuf_iterator<char>());
  EXPECT_NE(HighC.find("personality=__CxxFrameHandler4"), std::string::npos);
  EXPECT_NE(HighC.find("cxx.try[0]"), std::string::npos);

  auto Patch = patchBinary(PE);
  ASSERT_EQ(Patch.exitCode, 0) << Patch.err;
  const fs::path PatchedPath = tmpFile("patched");
  auto PatchedOrErr = loadBinary(PatchedPath);
  ASSERT_TRUE(static_cast<bool>(PatchedOrErr))
      << llvm::toString(PatchedOrErr.takeError());
  const BinaryImage &Patched = *PatchedOrErr;

  auto Relift = liftToLowIR(PatchedPath);
  ASSERT_EQ(Relift.exitCode, 0) << Relift.err;
  EXPECT_NE(Relift.out.find("personality=__CxxFrameHandler4"),
            std::string::npos);

  const Section *Injected = nullptr;
  for (const Section &Section : Patched.Sections)
    if (Section.Name == kNdTextSection) {
      Injected = &Section;
      break;
    }
  ASSERT_NE(Injected, nullptr);

  const ExceptionFunction *GeneratedCxx = nullptr;
  for (const ExceptionFunction &EH : Patched.ExceptionMetadata.Functions)
    if (Injected->contains(EH.CodeRange.Begin) &&
        EH.Personality == ExceptionPersonality::CxxFrameHandler4) {
      ASSERT_EQ(GeneratedCxx, nullptr);
      GeneratedCxx = &EH;
    }
  ASSERT_NE(GeneratedCxx, nullptr);
  ASSERT_EQ(GeneratedCxx->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(GeneratedCxx->Cxx.has_value());
  EXPECT_EQ(GeneratedCxx->Cxx->NativeEncoding, CxxExceptionInfo::Encoding::FH4);
  EXPECT_TRUE(GeneratedCxx->Cxx->hasValidStateGraph());
  ASSERT_EQ(GeneratedCxx->Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(GeneratedCxx->Cxx->TryBlocks.front().Handlers.size(), 1u);
  EXPECT_TRUE(Injected->contains(
      GeneratedCxx->Cxx->TryBlocks.front().Handlers.front().HandlerVA));
  for (const CxxIPState &State : GeneratedCxx->Cxx->IPMap)
    EXPECT_TRUE(Injected->contains(State.IP) ||
                State.IP == GeneratedCxx->CodeRange.End);

  std::ifstream PatchedInput(PatchedPath, std::ios::binary);
  ASSERT_TRUE(PatchedInput.good());
  const std::vector<uint8_t> PatchedBytes(
      std::istreambuf_iterator<char>(PatchedInput), {});
  ASSERT_FALSE(PatchedBytes.empty());
  ASSERT_GE(GeneratedCxx->Cxx->NativeFuncInfoVA, Patched.Base);
  const uint32_t GroupRVA =
      static_cast<uint32_t>(GeneratedCxx->Cxx->NativeFuncInfoVA - Patched.Base);
  auto Layout = coff_fh4::parseFuncInfoLayout(
      GroupRVA,
      [&](uint32_t RVA,
          uint32_t Size) -> llvm::Expected<llvm::ArrayRef<uint8_t>> {
        const uint8_t *Bytes = Patched.readVA(Patched.Base + RVA, Size);
        if (!Bytes)
          return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                         "unmapped FH4 test bytes");
        return llvm::ArrayRef<uint8_t>(Bytes, Size);
      });
  ASSERT_TRUE(static_cast<bool>(Layout)) << llvm::toString(Layout.takeError());
  ASSERT_TRUE(Layout->Try.has_value());
  ASSERT_EQ(Layout->Try->Entries.size(), 1u);
  const coff_fh4::TryEntry &GeneratedTry = Layout->Try->Entries.front();
  ASSERT_EQ(GeneratedTry.Handlers.Entries.size(), 1u);
  const coff_fh4::HandlerEntry &GeneratedCatch =
      GeneratedTry.Handlers.Entries.front();
  ASSERT_EQ(GeneratedCatch.Range.size(), 6u);

  COFFExceptionDirectoryUpdate Expected;
  Expected.Apply = true;
  Expected.RVA = Patched.ExceptionMetadata.DirectoryRVA;
  Expected.Size = Patched.ExceptionMetadata.DirectorySize;
  ASSERT_NE(Expected.RVA, 0u);
  ASSERT_NE(Expected.Size, 0u);
  ASSERT_GE(Injected->VA, Patched.Base);
  ASSERT_LE(Injected->VA - Patched.Base, std::numeric_limits<uint32_t>::max());
  ASSERT_GT(Injected->Size, 0u);
  ASSERT_LE(Injected->Size, std::numeric_limits<uint32_t>::max());
  ASSERT_LE(Injected->FileOff, PatchedBytes.size());
  ASSERT_LE(Injected->Size, PatchedBytes.size() - Injected->FileOff);
  COFFGeneratedSectionReceipt SectionReceipt;
  SectionReceipt.RVA = static_cast<uint32_t>(Injected->VA - Patched.Base);
  SectionReceipt.Size = static_cast<uint32_t>(Injected->Size);
  SectionReceipt.SHA256 = llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
      PatchedBytes.data() + static_cast<size_t>(Injected->FileOff),
      static_cast<size_t>(Injected->Size)));
  Expected.GeneratedSections.push_back(SectionReceipt);

  const uint64_t RuntimeOffset =
      Patched.vaToFileOffset(Patched.Base + GeneratedCxx->RuntimeFunctionRVA);
  ASSERT_NE(RuntimeOffset, InvalidVA);
  ASSERT_LE(RuntimeOffset, PatchedBytes.size());
  ASSERT_GE(PatchedBytes.size() - RuntimeOffset, 3 * sizeof(uint32_t));
  COFFGeneratedLanguageOwnerReceipt OwnerReceipt;
  OwnerReceipt.RuntimeFunctionRVA = GeneratedCxx->RuntimeFunctionRVA;
  OwnerReceipt.RuntimeWords = {
      readLE<uint32_t>(PatchedBytes.data() + RuntimeOffset),
      readLE<uint32_t>(PatchedBytes.data() + RuntimeOffset + 4),
      readLE<uint32_t>(PatchedBytes.data() + RuntimeOffset + 8)};
  OwnerReceipt.RuntimeWordCount = 3;
  OwnerReceipt.BeginRVA =
      static_cast<uint32_t>(GeneratedCxx->CodeRange.Begin - Patched.Base);
  OwnerReceipt.EndRVA =
      static_cast<uint32_t>(GeneratedCxx->CodeRange.End - Patched.Base);
  OwnerReceipt.UnwindRVA = GeneratedCxx->UnwindInfoRVA;
  OwnerReceipt.HandlerRVA =
      static_cast<uint32_t>(GeneratedCxx->PersonalityVA - Patched.Base);
  OwnerReceipt.HandlerDataRVA =
      static_cast<uint32_t>(GeneratedCxx->HandlerDataVA - Patched.Base);
  OwnerReceipt.LanguageGroupRVA = GroupRVA;
  OwnerReceipt.Model = COFFGeneratedLanguageModel::CxxFH4;
  OwnerReceipt.Role = COFFGeneratedLanguageOwnerRole::MappedRoot;
  Expected.GeneratedLanguageOwners.push_back(OwnerReceipt);

  const auto Token = windows_eh_semantics::getCxxCatchSemanticToken(
      *OriginalCxx, Arch::X64, 0, 0);
  ASSERT_TRUE(Token.has_value());
  const uint64_t RecordOffset =
      Patched.vaToFileOffset(Patched.Base + GeneratedCatch.Range.BeginRVA);
  ASSERT_NE(RecordOffset, InvalidVA);
  ASSERT_LE(RecordOffset, PatchedBytes.size());
  ASSERT_GE(PatchedBytes.size() - RecordOffset, GeneratedCatch.Range.size());
  COFFGeneratedEHSemanticBinding Binding;
  Binding.Kind = COFFGeneratedEHSemanticKind::CxxCatch;
  Binding.Model = COFFGeneratedLanguageModel::CxxFH4;
  Binding.SourceFunctionVA = OriginalCxx->CodeRange.Begin;
  Binding.Region = 0;
  Binding.Clause = 0;
  Binding.SourceDigest = Token->Digest;
  Binding.GeneratedOwnerRVA = OwnerReceipt.BeginRVA;
  Binding.ContainerRVA = GeneratedTry.Range.BeginRVA;
  Binding.RecordRVA = GeneratedCatch.Range.BeginRVA;
  Binding.RecordBytes.assign(
      PatchedBytes.begin() + static_cast<size_t>(RecordOffset),
      PatchedBytes.begin() + static_cast<size_t>(RecordOffset) +
          GeneratedCatch.Range.size());
  Binding.HandlerRVA = GeneratedCatch.HandlerRVA;
  Expected.GeneratedEHSemantics.push_back(Binding);

  if (llvm::Error Err = validatePatchedCOFFImage(
          PatchedBytes, Arch::X64,
          /*RequireGeneratedExceptionDirectory=*/true, Expected))
    FAIL() << llvm::toString(std::move(Err));

  std::vector<uint8_t> TamperedBytes = PatchedBytes;
  TamperedBytes[RecordOffset + 1] ^= 1u;
  Expected.GeneratedSections.front().SHA256 =
      llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
          TamperedBytes.data() + static_cast<size_t>(Injected->FileOff),
          static_cast<size_t>(Injected->Size)));
  llvm::Error TamperedValidation = validatePatchedCOFFImage(
      TamperedBytes, Arch::X64,
      /*RequireGeneratedExceptionDirectory=*/true, Expected);
  ASSERT_TRUE(static_cast<bool>(TamperedValidation));
  EXPECT_EQ(llvm::toString(std::move(TamperedValidation)),
            "coff exception patch: final generated EH semantic row does not "
            "match its prepared binding");
}

TEST_F(PatchCOFF_X64, ReconstructsCompilerOwnedGSWrappedFH4StateGraph) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_cxx_fh4_gs.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "GS-wrapped FH4 COFF executable not built";

  auto OriginalOrErr = loadBinary(PE);
  ASSERT_TRUE(static_cast<bool>(OriginalOrErr))
      << llvm::toString(OriginalOrErr.takeError());
  const BinaryImage &Original = *OriginalOrErr;
  const ExceptionFunction *OriginalCxx = nullptr;
  for (const ExceptionFunction &EH : Original.ExceptionMetadata.Functions)
    if (EH.Personality == ExceptionPersonality::GSHandlerCheckEH4) {
      ASSERT_EQ(OriginalCxx, nullptr);
      OriginalCxx = &EH;
    }
  ASSERT_NE(OriginalCxx, nullptr);
  ASSERT_EQ(OriginalCxx->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(OriginalCxx->Cxx.has_value());
  ASSERT_TRUE(OriginalCxx->GSCookie.has_value());
  EXPECT_EQ(OriginalCxx->Cxx->NativeEncoding, CxxExceptionInfo::Encoding::FH4);
  EXPECT_TRUE(OriginalCxx->Cxx->hasValidStateGraph());
  const GSCookieInfo &OriginalCookie = *OriginalCxx->GSCookie;
  ASSERT_EQ(OriginalCookie.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(OriginalCookie.CookieOffset, 0x20);
  EXPECT_TRUE(OriginalCookie.HasExceptionHandler);
  EXPECT_TRUE(OriginalCookie.HasUnwindHandler);
  EXPECT_FALSE(OriginalCookie.HasAlignment);
  ASSERT_EQ(OriginalCookie.Payload.size(), sizeof(uint32_t));
  EXPECT_EQ(readLE<uint32_t>(OriginalCookie.Payload.data()), 0x23u);
  const WindowsEHNativeSourceClassification NativeSource =
      classifyWindowsEHNativeSource(*OriginalCxx, Arch::X64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::OutputPatch);
  ASSERT_TRUE(NativeSource.canPatchOutput())
      << getWindowsEHNativeSourceReasonName(NativeSource.Reason);

  const Export *SecurityCookie = Original.findExport("__security_cookie");
  const Export *SecurityCheck = Original.findExport("__security_check_cookie");
  ASSERT_NE(SecurityCookie, nullptr);
  ASSERT_NE(SecurityCheck, nullptr);
  ASSERT_NE(SecurityCookie->Addr, 0u);
  ASSERT_NE(SecurityCheck->Addr, 0u);

  auto Lift = liftToLowIR(PE);
  ASSERT_EQ(Lift.exitCode, 0) << Lift.err;
  EXPECT_NE(Lift.out.find("personality=__GSHandlerCheck_EH4"),
            std::string::npos);

  auto Decompile = decompileToHighC(PE);
  ASSERT_EQ(Decompile.exitCode, 0) << Decompile.err;
  std::ifstream HighCInput(tmpFile("decompiled_high.c"));
  ASSERT_TRUE(HighCInput.good());
  std::string HighC((std::istreambuf_iterator<char>(HighCInput)),
                    std::istreambuf_iterator<char>());
  EXPECT_NE(HighC.find("personality=__GSHandlerCheck_EH4"), std::string::npos);
  EXPECT_NE(HighC.find("cxx.try[0]"), std::string::npos);

  auto Patch = patchBinary(PE);
  ASSERT_EQ(Patch.exitCode, 0) << Patch.err;
  const fs::path PatchedPath = tmpFile("patched");
  auto PatchedOrErr = loadBinary(PatchedPath);
  ASSERT_TRUE(static_cast<bool>(PatchedOrErr))
      << llvm::toString(PatchedOrErr.takeError());
  const BinaryImage &Patched = *PatchedOrErr;

  auto Relift = liftToLowIR(PatchedPath);
  ASSERT_EQ(Relift.exitCode, 0) << Relift.err;
  EXPECT_NE(Relift.out.find("personality=__GSHandlerCheck_EH4"),
            std::string::npos);

  const Section *Injected = nullptr;
  for (const Section &Section : Patched.Sections)
    if (Section.Name == kNdTextSection) {
      Injected = &Section;
      break;
    }
  ASSERT_NE(Injected, nullptr);

  const ExceptionFunction *GeneratedCxx = nullptr;
  for (const ExceptionFunction &EH : Patched.ExceptionMetadata.Functions)
    if (Injected->contains(EH.CodeRange.Begin) &&
        EH.Personality == ExceptionPersonality::GSHandlerCheckEH4) {
      ASSERT_EQ(GeneratedCxx, nullptr);
      GeneratedCxx = &EH;
    }
  ASSERT_NE(GeneratedCxx, nullptr);
  ASSERT_EQ(GeneratedCxx->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(GeneratedCxx->Cxx.has_value());
  ASSERT_TRUE(GeneratedCxx->GSCookie.has_value());
  EXPECT_EQ(GeneratedCxx->PersonalityVA, OriginalCxx->PersonalityVA);
  EXPECT_EQ(GeneratedCxx->Cxx->NativeEncoding, CxxExceptionInfo::Encoding::FH4);
  EXPECT_TRUE(GeneratedCxx->Cxx->hasValidStateGraph());
  ASSERT_EQ(GeneratedCxx->Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(GeneratedCxx->Cxx->TryBlocks.front().Handlers.size(), 1u);
  EXPECT_TRUE(Injected->contains(
      GeneratedCxx->Cxx->TryBlocks.front().Handlers.front().HandlerVA));
  const GSCookieInfo &GeneratedCookie = *GeneratedCxx->GSCookie;
  ASSERT_EQ(GeneratedCookie.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_GT(GeneratedCookie.CookieOffset, 0);
  EXPECT_EQ(static_cast<uint32_t>(GeneratedCookie.CookieOffset) & 7u, 0u);
  EXPECT_TRUE(GeneratedCookie.HasExceptionHandler);
  EXPECT_TRUE(GeneratedCookie.HasUnwindHandler);
  EXPECT_FALSE(GeneratedCookie.HasAlignment);
  ASSERT_EQ(GeneratedCookie.Payload.size(), sizeof(uint32_t));
  const uint32_t GeneratedGSHeader =
      readLE<uint32_t>(GeneratedCookie.Payload.data());
  EXPECT_EQ(GeneratedGSHeader,
            static_cast<uint32_t>(GeneratedCookie.CookieOffset) | 3u);

  std::ifstream PatchedInput(PatchedPath, std::ios::binary);
  ASSERT_TRUE(PatchedInput.good());
  const std::vector<uint8_t> PatchedBytes(
      std::istreambuf_iterator<char>(PatchedInput), {});
  ASSERT_FALSE(PatchedBytes.empty());
  ASSERT_LE(Injected->FileOff, PatchedBytes.size());
  ASSERT_LE(Injected->Size, PatchedBytes.size() - Injected->FileOff);
  const uint8_t *InjectedBytes =
      PatchedBytes.data() + static_cast<size_t>(Injected->FileOff);
  bool HasSecurityCookieReference = false;
  bool HasSecurityCheckCall = false;
  for (uint64_t Offset = 0; Offset + 7 <= Injected->Size; ++Offset) {
    const va_t InstructionVA = Injected->VA + Offset;
    if (InjectedBytes[Offset] == 0x48 && InjectedBytes[Offset + 1] == 0x8b &&
        (InjectedBytes[Offset + 2] & 0xc7u) == 0x05u) {
      const int32_t Displacement = readLE<int32_t>(InjectedBytes + Offset + 3);
      HasSecurityCookieReference |=
          InstructionVA + 7 + Displacement == SecurityCookie->Addr;
    }
    if (InjectedBytes[Offset] == 0xe8) {
      const int32_t Displacement = readLE<int32_t>(InjectedBytes + Offset + 1);
      HasSecurityCheckCall |=
          InstructionVA + 5 + Displacement == SecurityCheck->Addr;
    }
  }
  EXPECT_TRUE(HasSecurityCookieReference);
  EXPECT_TRUE(HasSecurityCheckCall);

  ASSERT_GE(GeneratedCxx->Cxx->NativeFuncInfoVA, Patched.Base);
  const uint32_t GroupRVA =
      static_cast<uint32_t>(GeneratedCxx->Cxx->NativeFuncInfoVA - Patched.Base);
  auto Layout = coff_fh4::parseFuncInfoLayout(
      GroupRVA,
      [&](uint32_t RVA,
          uint32_t Size) -> llvm::Expected<llvm::ArrayRef<uint8_t>> {
        const uint8_t *Bytes = Patched.readVA(Patched.Base + RVA, Size);
        if (!Bytes)
          return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                         "unmapped GS FH4 test bytes");
        return llvm::ArrayRef<uint8_t>(Bytes, Size);
      });
  ASSERT_TRUE(static_cast<bool>(Layout)) << llvm::toString(Layout.takeError());
  ASSERT_TRUE(Layout->Try.has_value());
  ASSERT_EQ(Layout->Try->Entries.size(), 1u);
  const coff_fh4::TryEntry &GeneratedTry = Layout->Try->Entries.front();
  ASSERT_EQ(GeneratedTry.Handlers.Entries.size(), 1u);
  const coff_fh4::HandlerEntry &GeneratedCatch =
      GeneratedTry.Handlers.Entries.front();
  ASSERT_EQ(GeneratedCatch.Range.size(), 6u);

  COFFExceptionDirectoryUpdate Expected;
  Expected.Apply = true;
  Expected.RVA = Patched.ExceptionMetadata.DirectoryRVA;
  Expected.Size = Patched.ExceptionMetadata.DirectorySize;
  ASSERT_NE(Expected.RVA, 0u);
  ASSERT_NE(Expected.Size, 0u);
  ASSERT_GE(Injected->VA, Patched.Base);
  ASSERT_LE(Injected->VA - Patched.Base, std::numeric_limits<uint32_t>::max());
  ASSERT_GT(Injected->Size, 0u);
  ASSERT_LE(Injected->Size, std::numeric_limits<uint32_t>::max());
  COFFGeneratedSectionReceipt SectionReceipt;
  SectionReceipt.RVA = static_cast<uint32_t>(Injected->VA - Patched.Base);
  SectionReceipt.Size = static_cast<uint32_t>(Injected->Size);
  SectionReceipt.SHA256 = llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
      InjectedBytes, static_cast<size_t>(Injected->Size)));
  Expected.GeneratedSections.push_back(SectionReceipt);

  const uint64_t RuntimeOffset =
      Patched.vaToFileOffset(Patched.Base + GeneratedCxx->RuntimeFunctionRVA);
  ASSERT_NE(RuntimeOffset, InvalidVA);
  ASSERT_LE(RuntimeOffset, PatchedBytes.size());
  ASSERT_GE(PatchedBytes.size() - RuntimeOffset, 3 * sizeof(uint32_t));
  COFFGeneratedLanguageOwnerReceipt OwnerReceipt;
  OwnerReceipt.RuntimeFunctionRVA = GeneratedCxx->RuntimeFunctionRVA;
  OwnerReceipt.RuntimeWords = {
      readLE<uint32_t>(PatchedBytes.data() + RuntimeOffset),
      readLE<uint32_t>(PatchedBytes.data() + RuntimeOffset + 4),
      readLE<uint32_t>(PatchedBytes.data() + RuntimeOffset + 8)};
  OwnerReceipt.RuntimeWordCount = 3;
  OwnerReceipt.BeginRVA =
      static_cast<uint32_t>(GeneratedCxx->CodeRange.Begin - Patched.Base);
  OwnerReceipt.EndRVA =
      static_cast<uint32_t>(GeneratedCxx->CodeRange.End - Patched.Base);
  OwnerReceipt.UnwindRVA = GeneratedCxx->UnwindInfoRVA;
  OwnerReceipt.HandlerRVA =
      static_cast<uint32_t>(GeneratedCxx->PersonalityVA - Patched.Base);
  OwnerReceipt.HandlerDataRVA =
      static_cast<uint32_t>(GeneratedCxx->HandlerDataVA - Patched.Base);
  OwnerReceipt.LanguageGroupRVA = GroupRVA;
  OwnerReceipt.GSWrapped = true;
  OwnerReceipt.GSCookieHeader = GeneratedGSHeader;
  OwnerReceipt.Model = COFFGeneratedLanguageModel::CxxFH4;
  OwnerReceipt.Role = COFFGeneratedLanguageOwnerRole::MappedRoot;
  Expected.GeneratedLanguageOwners.push_back(OwnerReceipt);

  const auto Token = windows_eh_semantics::getCxxCatchSemanticToken(
      *OriginalCxx, Arch::X64, 0, 0);
  ASSERT_TRUE(Token.has_value());
  const uint64_t RecordOffset =
      Patched.vaToFileOffset(Patched.Base + GeneratedCatch.Range.BeginRVA);
  ASSERT_NE(RecordOffset, InvalidVA);
  ASSERT_LE(RecordOffset, PatchedBytes.size());
  ASSERT_GE(PatchedBytes.size() - RecordOffset, GeneratedCatch.Range.size());
  COFFGeneratedEHSemanticBinding Binding;
  Binding.Kind = COFFGeneratedEHSemanticKind::CxxCatch;
  Binding.Model = COFFGeneratedLanguageModel::CxxFH4;
  Binding.SourceFunctionVA = OriginalCxx->CodeRange.Begin;
  Binding.Region = 0;
  Binding.Clause = 0;
  Binding.SourceDigest = Token->Digest;
  Binding.GeneratedOwnerRVA = OwnerReceipt.BeginRVA;
  Binding.ContainerRVA = GeneratedTry.Range.BeginRVA;
  Binding.RecordRVA = GeneratedCatch.Range.BeginRVA;
  Binding.RecordBytes.assign(
      PatchedBytes.begin() + static_cast<size_t>(RecordOffset),
      PatchedBytes.begin() + static_cast<size_t>(RecordOffset) +
          GeneratedCatch.Range.size());
  Binding.HandlerRVA = GeneratedCatch.HandlerRVA;
  Expected.GeneratedEHSemantics.push_back(Binding);

  if (llvm::Error Err = validatePatchedCOFFImage(
          PatchedBytes, Arch::X64,
          /*RequireGeneratedExceptionDirectory=*/true, Expected))
    FAIL() << llvm::toString(std::move(Err));

  const uint64_t GSHeaderOffset =
      Patched.vaToFileOffset(GeneratedCxx->HandlerDataVA + sizeof(uint32_t));
  ASSERT_NE(GSHeaderOffset, InvalidVA);
  ASSERT_LE(GSHeaderOffset, PatchedBytes.size() - sizeof(uint32_t));
  std::vector<uint8_t> TamperedBytes = PatchedBytes;
  TamperedBytes[GSHeaderOffset] ^= 8u;
  Expected.GeneratedSections.front().SHA256 =
      llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
          TamperedBytes.data() + static_cast<size_t>(Injected->FileOff),
          static_cast<size_t>(Injected->Size)));
  llvm::Error TamperedValidation = validatePatchedCOFFImage(
      TamperedBytes, Arch::X64,
      /*RequireGeneratedExceptionDirectory=*/true, Expected);
  ASSERT_TRUE(static_cast<bool>(TamperedValidation));
  EXPECT_EQ(llvm::toString(std::move(TamperedValidation)),
            "coff exception patch: final generated language owner GS header "
            "does not match the prepared receipt");
}

TEST_F(PatchCOFF_X64, ReconstructsBoundedNativeTypedFH4StateGraph) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_cxx_fh4_typed.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "typed FH4 COFF executable not built";

  auto OriginalOrErr = loadBinary(PE);
  ASSERT_TRUE(static_cast<bool>(OriginalOrErr))
      << llvm::toString(OriginalOrErr.takeError());
  const BinaryImage &Original = *OriginalOrErr;
  const ExceptionFunction *OriginalCxx = nullptr;
  for (const ExceptionFunction &EH : Original.ExceptionMetadata.Functions)
    if (EH.Personality == ExceptionPersonality::CxxFrameHandler4) {
      ASSERT_EQ(OriginalCxx, nullptr);
      OriginalCxx = &EH;
    }
  ASSERT_NE(OriginalCxx, nullptr);
  ASSERT_EQ(OriginalCxx->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(OriginalCxx->Cxx.has_value());
  EXPECT_EQ(OriginalCxx->Cxx->NativeEncoding, CxxExceptionInfo::Encoding::FH4);
  EXPECT_TRUE(OriginalCxx->Cxx->hasValidStateGraph());
  ASSERT_EQ(OriginalCxx->Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(OriginalCxx->Cxx->TryBlocks.front().Handlers.size(), 1u);
  const CxxCatchHandler &OriginalCatch =
      OriginalCxx->Cxx->TryBlocks.front().Handlers.front();
  ASSERT_NE(OriginalCatch.TypeDescriptorVA, 0u);
  EXPECT_EQ(OriginalCatch.Adjectives, 9u);
  EXPECT_EQ(OriginalCatch.CatchObjectOffset, 0);
  EXPECT_TRUE(OriginalCatch.ContinuationVAs.empty());
  const WindowsEHNativeSourceClassification NativeSource =
      classifyWindowsEHNativeSource(*OriginalCxx, Arch::X64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::OutputPatch);
  ASSERT_TRUE(NativeSource.canPatchOutput())
      << getWindowsEHNativeSourceReasonName(NativeSource.Reason);

  auto Lift = liftToLowIR(PE);
  ASSERT_EQ(Lift.exitCode, 0) << Lift.err;
  EXPECT_NE(Lift.out.find("personality=__CxxFrameHandler4"), std::string::npos);

  auto Decompile = decompileToHighC(PE);
  ASSERT_EQ(Decompile.exitCode, 0) << Decompile.err;
  std::ifstream HighCInput(tmpFile("decompiled_high.c"));
  ASSERT_TRUE(HighCInput.good());
  std::string HighC((std::istreambuf_iterator<char>(HighCInput)),
                    std::istreambuf_iterator<char>());
  EXPECT_NE(HighC.find("personality=__CxxFrameHandler4"), std::string::npos);
  EXPECT_NE(HighC.find("cxx.try[0]"), std::string::npos);
  EXPECT_NE(HighC.find("type_descriptor@0x"), std::string::npos);

  auto Patch = patchBinary(PE);
  ASSERT_EQ(Patch.exitCode, 0) << Patch.err;
  const fs::path PatchedPath = tmpFile("patched");
  auto PatchedOrErr = loadBinary(PatchedPath);
  ASSERT_TRUE(static_cast<bool>(PatchedOrErr))
      << llvm::toString(PatchedOrErr.takeError());
  const BinaryImage &Patched = *PatchedOrErr;

  auto Relift = liftToLowIR(PatchedPath);
  ASSERT_EQ(Relift.exitCode, 0) << Relift.err;
  EXPECT_NE(Relift.out.find("personality=__CxxFrameHandler4"),
            std::string::npos);

  const Section *Injected = nullptr;
  for (const Section &Section : Patched.Sections)
    if (Section.Name == kNdTextSection) {
      Injected = &Section;
      break;
    }
  ASSERT_NE(Injected, nullptr);

  const ExceptionFunction *GeneratedCxx = nullptr;
  for (const ExceptionFunction &EH : Patched.ExceptionMetadata.Functions)
    if (Injected->contains(EH.CodeRange.Begin) &&
        EH.Personality == ExceptionPersonality::CxxFrameHandler4) {
      ASSERT_EQ(GeneratedCxx, nullptr);
      GeneratedCxx = &EH;
    }
  ASSERT_NE(GeneratedCxx, nullptr);
  ASSERT_EQ(GeneratedCxx->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(GeneratedCxx->Cxx.has_value());
  EXPECT_EQ(GeneratedCxx->Cxx->NativeEncoding, CxxExceptionInfo::Encoding::FH4);
  EXPECT_TRUE(GeneratedCxx->Cxx->hasValidStateGraph());
  ASSERT_EQ(GeneratedCxx->Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(GeneratedCxx->Cxx->TryBlocks.front().Handlers.size(), 1u);
  const CxxCatchHandler &GeneratedCatch =
      GeneratedCxx->Cxx->TryBlocks.front().Handlers.front();
  EXPECT_EQ(GeneratedCatch.TypeDescriptorVA, OriginalCatch.TypeDescriptorVA);
  EXPECT_EQ(GeneratedCatch.Adjectives, OriginalCatch.Adjectives);
  EXPECT_EQ(GeneratedCatch.CatchObjectOffset, 0);
  EXPECT_TRUE(GeneratedCatch.ContinuationVAs.empty());
  EXPECT_TRUE(Injected->contains(GeneratedCatch.HandlerVA));

  ASSERT_GE(GeneratedCxx->Cxx->NativeFuncInfoVA, Patched.Base);
  const uint32_t GroupRVA =
      static_cast<uint32_t>(GeneratedCxx->Cxx->NativeFuncInfoVA - Patched.Base);
  auto Layout = coff_fh4::parseFuncInfoLayout(
      GroupRVA,
      [&](uint32_t RVA,
          uint32_t Size) -> llvm::Expected<llvm::ArrayRef<uint8_t>> {
        const uint8_t *Bytes = Patched.readVA(Patched.Base + RVA, Size);
        if (!Bytes)
          return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                         "unmapped typed FH4 test bytes");
        return llvm::ArrayRef<uint8_t>(Bytes, Size);
      });
  ASSERT_TRUE(static_cast<bool>(Layout)) << llvm::toString(Layout.takeError());
  ASSERT_TRUE(Layout->Try.has_value());
  ASSERT_EQ(Layout->Try->Entries.size(), 1u);
  const coff_fh4::TryEntry &GeneratedTry = Layout->Try->Entries.front();
  ASSERT_EQ(GeneratedTry.Handlers.Entries.size(), 1u);
  const coff_fh4::HandlerEntry &GeneratedRow =
      GeneratedTry.Handlers.Entries.front();
  ASSERT_EQ(GeneratedRow.Range.size(), 10u);
  EXPECT_EQ(GeneratedRow.Header, 0x03u);
  EXPECT_EQ(GeneratedRow.Adjectives, OriginalCatch.Adjectives);
  EXPECT_EQ(GeneratedRow.TypeDescriptorRVA,
            OriginalCatch.TypeDescriptorVA - Patched.Base);
  EXPECT_EQ(GeneratedRow.HandlerRVA, GeneratedCatch.HandlerVA - Patched.Base);
}

TEST_F(PatchCOFF_X64, RejectsBorrowedGeneratedCxxUnwindAgainstReceipt) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_cxx_fh3.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "FH3 COFF executable not built";

  auto OriginalOrErr = loadBinary(PE);
  ASSERT_TRUE(static_cast<bool>(OriginalOrErr))
      << llvm::toString(OriginalOrErr.takeError());
  const ExceptionFunction *OriginalCxx = nullptr;
  for (const ExceptionFunction &EH : OriginalOrErr->ExceptionMetadata.Functions)
    if (EH.Kind == RuntimeFunctionKind::Primary &&
        EH.Personality == ExceptionPersonality::CxxFrameHandler3 && EH.Cxx &&
        !EH.Cxx->IsCatchFunclet) {
      ASSERT_EQ(OriginalCxx, nullptr);
      OriginalCxx = &EH;
    }
  ASSERT_NE(OriginalCxx, nullptr);

  auto Patch = patchBinary(PE);
  ASSERT_EQ(Patch.exitCode, 0) << Patch.err;
  const fs::path PatchedPath = tmpFile("patched");
  auto PatchedOrErr = loadBinary(PatchedPath);
  ASSERT_TRUE(static_cast<bool>(PatchedOrErr))
      << llvm::toString(PatchedOrErr.takeError());
  const BinaryImage &Patched = *PatchedOrErr;

  const Section *Injected = nullptr;
  for (const Section &Section : Patched.Sections)
    if (Section.Name == kNdTextSection) {
      Injected = &Section;
      break;
    }
  ASSERT_NE(Injected, nullptr);

  const ExceptionFunction *Parent = nullptr;
  const ExceptionFunction *Catch = nullptr;
  va_t GroupVA = 0;
  for (const ExceptionFunction &EH : Patched.ExceptionMetadata.Functions) {
    if (!Injected->contains(EH.CodeRange.Begin) ||
        EH.Personality != ExceptionPersonality::CxxFrameHandler3 || !EH.Cxx)
      continue;
    if (GroupVA == 0)
      GroupVA = EH.Cxx->NativeFuncInfoVA;
    if (EH.Cxx->NativeFuncInfoVA != GroupVA)
      continue;
    if (EH.Cxx->IsCatchFunclet)
      Catch = &EH;
    else
      Parent = &EH;
  }
  ASSERT_NE(Parent, nullptr);
  ASSERT_NE(Catch, nullptr);
  ASSERT_NE(Parent->UnwindInfoRVA, Catch->UnwindInfoRVA);
  ASSERT_EQ(Parent->PersonalityVA, Catch->PersonalityVA);
  ASSERT_EQ(Parent->Cxx->NativeFuncInfoVA, Catch->Cxx->NativeFuncInfoVA);

  std::ifstream PatchedInput(PatchedPath, std::ios::binary);
  ASSERT_TRUE(PatchedInput.good());
  const std::vector<uint8_t> PatchedBytes(
      std::istreambuf_iterator<char>(PatchedInput), {});
  ASSERT_FALSE(PatchedBytes.empty());

  COFFExceptionDirectoryUpdate Expected;
  Expected.Apply = true;
  Expected.RVA = Patched.ExceptionMetadata.DirectoryRVA;
  Expected.Size = Patched.ExceptionMetadata.DirectorySize;
  ASSERT_NE(Expected.RVA, 0u);
  ASSERT_NE(Expected.Size, 0u);
  ASSERT_GE(Injected->VA, Patched.Base);
  ASSERT_LE(Injected->VA - Patched.Base, std::numeric_limits<uint32_t>::max());
  ASSERT_GT(Injected->Size, 0u);
  ASSERT_LE(Injected->Size, std::numeric_limits<uint32_t>::max());
  ASSERT_LE(Injected->FileOff, PatchedBytes.size());
  ASSERT_LE(Injected->Size, PatchedBytes.size() - Injected->FileOff);
  COFFGeneratedSectionReceipt SectionReceipt;
  SectionReceipt.RVA = static_cast<uint32_t>(Injected->VA - Patched.Base);
  SectionReceipt.Size = static_cast<uint32_t>(Injected->Size);
  SectionReceipt.SHA256 = llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
      PatchedBytes.data() + static_cast<size_t>(Injected->FileOff),
      static_cast<size_t>(Injected->Size)));
  Expected.GeneratedSections.push_back(SectionReceipt);

  for (const ExceptionFunction &EH : Patched.ExceptionMetadata.Functions) {
    if (!Injected->contains(EH.CodeRange.Begin) || !EH.Cxx ||
        EH.Personality != ExceptionPersonality::CxxFrameHandler3 ||
        EH.Cxx->NativeFuncInfoVA != GroupVA)
      continue;
    ASSERT_GE(EH.CodeRange.Begin, Patched.Base);
    ASSERT_GE(EH.CodeRange.End, Patched.Base);
    ASSERT_GE(EH.PersonalityVA, Patched.Base);
    ASSERT_GE(EH.HandlerDataVA, Patched.Base);
    ASSERT_GE(EH.Cxx->NativeFuncInfoVA, Patched.Base);
    const uint64_t RuntimeOffset =
        Patched.vaToFileOffset(Patched.Base + EH.RuntimeFunctionRVA);
    ASSERT_NE(RuntimeOffset, InvalidVA);
    ASSERT_LE(RuntimeOffset, PatchedBytes.size());
    ASSERT_GE(PatchedBytes.size() - RuntimeOffset, 3 * sizeof(uint32_t));

    COFFGeneratedLanguageOwnerReceipt Receipt;
    Receipt.RuntimeFunctionRVA = EH.RuntimeFunctionRVA;
    Receipt.RuntimeWords = {
        readLE<uint32_t>(PatchedBytes.data() + RuntimeOffset),
        readLE<uint32_t>(PatchedBytes.data() + RuntimeOffset + 4),
        readLE<uint32_t>(PatchedBytes.data() + RuntimeOffset + 8)};
    Receipt.RuntimeWordCount = 3;
    Receipt.BeginRVA = static_cast<uint32_t>(EH.CodeRange.Begin - Patched.Base);
    Receipt.EndRVA = static_cast<uint32_t>(EH.CodeRange.End - Patched.Base);
    Receipt.UnwindRVA = EH.UnwindInfoRVA;
    Receipt.HandlerRVA = static_cast<uint32_t>(EH.PersonalityVA - Patched.Base);
    Receipt.HandlerDataRVA =
        static_cast<uint32_t>(EH.HandlerDataVA - Patched.Base);
    Receipt.LanguageGroupRVA =
        static_cast<uint32_t>(EH.Cxx->NativeFuncInfoVA - Patched.Base);
    Receipt.Model = COFFGeneratedLanguageModel::CxxFH3;
    Receipt.Role = EH.Cxx->IsCatchFunclet
                       ? COFFGeneratedLanguageOwnerRole::CxxAuxiliary
                       : COFFGeneratedLanguageOwnerRole::MappedRoot;
    Expected.GeneratedLanguageOwners.push_back(Receipt);
  }
  std::sort(Expected.GeneratedLanguageOwners.begin(),
            Expected.GeneratedLanguageOwners.end(),
            [](const COFFGeneratedLanguageOwnerReceipt &A,
               const COFFGeneratedLanguageOwnerReceipt &B) {
              return A.RuntimeFunctionRVA < B.RuntimeFunctionRVA;
            });
  ASSERT_EQ(Expected.GeneratedLanguageOwners.size(), 2u);

  ASSERT_GE(Parent->CodeRange.Begin, Patched.Base);
  ASSERT_LE(Parent->CodeRange.Begin - Patched.Base,
            std::numeric_limits<uint32_t>::max());
  const uint32_t GeneratedOwnerRVA =
      static_cast<uint32_t>(Parent->CodeRange.Begin - Patched.Base);
  const uint64_t FuncInfoOffset = Patched.vaToFileOffset(GroupVA);
  ASSERT_NE(FuncInfoOffset, InvalidVA);
  ASSERT_LE(FuncInfoOffset, PatchedBytes.size());
  ASSERT_GE(PatchedBytes.size() - FuncInfoOffset, 5 * sizeof(uint32_t));
  const uint32_t NumTryBlocks = readLE<uint32_t>(
      PatchedBytes.data() + FuncInfoOffset + 3 * sizeof(uint32_t));
  const uint32_t TryMapRVA = readLE<uint32_t>(
      PatchedBytes.data() + FuncInfoOffset + 4 * sizeof(uint32_t));
  ASSERT_EQ(NumTryBlocks, OriginalCxx->Cxx->TryBlocks.size());
  ASSERT_EQ(NumTryBlocks, Parent->Cxx->TryBlocks.size());
  for (uint32_t TryIndex = 0; TryIndex < NumTryBlocks; ++TryIndex) {
    const uint32_t TryRVA = TryMapRVA + TryIndex * 5 * sizeof(uint32_t);
    const uint64_t TryOffset = Patched.vaToFileOffset(Patched.Base + TryRVA);
    ASSERT_NE(TryOffset, InvalidVA);
    ASSERT_LE(TryOffset, PatchedBytes.size());
    ASSERT_GE(PatchedBytes.size() - TryOffset, 5 * sizeof(uint32_t));
    const uint32_t NumCatches = readLE<uint32_t>(
        PatchedBytes.data() + TryOffset + 3 * sizeof(uint32_t));
    const uint32_t HandlerMapRVA = readLE<uint32_t>(
        PatchedBytes.data() + TryOffset + 4 * sizeof(uint32_t));
    ASSERT_EQ(NumCatches,
              OriginalCxx->Cxx->TryBlocks[TryIndex].Handlers.size());
    ASSERT_EQ(NumCatches, Parent->Cxx->TryBlocks[TryIndex].Handlers.size());
    for (uint32_t CatchIndex = 0; CatchIndex < NumCatches; ++CatchIndex) {
      const uint32_t RecordRVA =
          HandlerMapRVA + CatchIndex * 5 * sizeof(uint32_t);
      const uint64_t RecordOffset =
          Patched.vaToFileOffset(Patched.Base + RecordRVA);
      ASSERT_NE(RecordOffset, InvalidVA);
      ASSERT_LE(RecordOffset, PatchedBytes.size());
      ASSERT_GE(PatchedBytes.size() - RecordOffset, 5 * sizeof(uint32_t));
      const auto Token = windows_eh_semantics::getCxxCatchSemanticToken(
          *OriginalCxx, Arch::X64, TryIndex, CatchIndex);
      ASSERT_TRUE(Token.has_value());

      COFFGeneratedEHSemanticBinding Binding;
      Binding.Kind = COFFGeneratedEHSemanticKind::CxxCatch;
      Binding.Model = COFFGeneratedLanguageModel::CxxFH3;
      Binding.SourceFunctionVA = OriginalCxx->CodeRange.Begin;
      Binding.Region = TryIndex;
      Binding.Clause = CatchIndex;
      Binding.SourceDigest = Token->Digest;
      Binding.GeneratedOwnerRVA = GeneratedOwnerRVA;
      Binding.ContainerRVA = TryRVA;
      Binding.RecordRVA = RecordRVA;
      Binding.RecordBytes.assign(
          PatchedBytes.begin() + static_cast<size_t>(RecordOffset),
          PatchedBytes.begin() + static_cast<size_t>(RecordOffset) +
              5 * sizeof(uint32_t));
      Binding.HandlerRVA =
          readLE<uint32_t>(Binding.RecordBytes.data() + 3 * sizeof(uint32_t));
      Expected.GeneratedEHSemantics.push_back(std::move(Binding));
    }
  }
  std::sort(Expected.GeneratedEHSemantics.begin(),
            Expected.GeneratedEHSemantics.end(),
            [](const COFFGeneratedEHSemanticBinding &A,
               const COFFGeneratedEHSemanticBinding &B) {
              return A.RecordRVA < B.RecordRVA;
            });
  if (llvm::Error Err = validatePatchedCOFFImage(
          PatchedBytes, Arch::X64,
          /*RequireGeneratedExceptionDirectory=*/true, Expected))
    FAIL() << llvm::toString(std::move(Err));

  ASSERT_FALSE(Expected.GeneratedEHSemantics.empty());
  ASSERT_EQ(Expected.GeneratedSections.size(), 1u);
  const uint64_t SemanticRecordOffset = Patched.vaToFileOffset(
      Patched.Base + Expected.GeneratedEHSemantics.front().RecordRVA);
  ASSERT_NE(SemanticRecordOffset, InvalidVA);
  ASSERT_LT(SemanticRecordOffset, PatchedBytes.size());
  std::vector<uint8_t> SemanticTamperedBytes = PatchedBytes;
  SemanticTamperedBytes[SemanticRecordOffset] ^= 1u;
  COFFExceptionDirectoryUpdate SemanticExpected = Expected;
  SemanticExpected.GeneratedSections.front().SHA256 =
      llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
          SemanticTamperedBytes.data() + static_cast<size_t>(Injected->FileOff),
          static_cast<size_t>(Injected->Size)));
  llvm::Error SemanticValidation = validatePatchedCOFFImage(
      SemanticTamperedBytes, Arch::X64,
      /*RequireGeneratedExceptionDirectory=*/true, SemanticExpected);
  ASSERT_TRUE(static_cast<bool>(SemanticValidation));
  EXPECT_EQ(llvm::toString(std::move(SemanticValidation)),
            "coff exception patch: final generated EH semantic row does not "
            "match its prepared binding");

  const uint64_t GroupOffset = Patched.vaToFileOffset(GroupVA);
  ASSERT_NE(GroupOffset, InvalidVA);
  ASSERT_LE(GroupOffset, PatchedBytes.size());
  ASSERT_GE(PatchedBytes.size() - GroupOffset, 2 * sizeof(uint32_t));
  std::vector<uint8_t> PayloadTamperedBytes = PatchedBytes;
  PayloadTamperedBytes[GroupOffset + sizeof(uint32_t)] ^= 1u;
  llvm::Error PayloadValidation = validatePatchedCOFFImage(
      PayloadTamperedBytes, Arch::X64,
      /*RequireGeneratedExceptionDirectory=*/true, Expected);
  ASSERT_TRUE(static_cast<bool>(PayloadValidation));
  EXPECT_EQ(llvm::toString(std::move(PayloadValidation)),
            "coff exception patch: final generated section does not match "
            "the prepared receipt");

  std::vector<uint8_t> TamperedBytes = PatchedBytes;
  const uint64_t ParentRuntimeOffset =
      Patched.vaToFileOffset(Patched.Base + Parent->RuntimeFunctionRVA);
  ASSERT_NE(ParentRuntimeOffset, InvalidVA);
  ASSERT_LE(ParentRuntimeOffset, TamperedBytes.size());
  ASSERT_GE(TamperedBytes.size() - ParentRuntimeOffset, 3 * sizeof(uint32_t));
  writeLE<uint32_t>(TamperedBytes.data() + ParentRuntimeOffset + 8,
                    Catch->UnwindInfoRVA);

  llvm::Error Validation = validatePatchedCOFFImage(
      TamperedBytes, Arch::X64,
      /*RequireGeneratedExceptionDirectory=*/true, Expected);
  ASSERT_TRUE(static_cast<bool>(Validation));
  EXPECT_EQ(llvm::toString(std::move(Validation)),
            "coff exception patch: final generated language owner does not "
            "match the prepared receipt");
}

//===----------------------------------------------------------------------===//
// ARM32 COFF/PE patch
//===----------------------------------------------------------------------===//

} // namespace
