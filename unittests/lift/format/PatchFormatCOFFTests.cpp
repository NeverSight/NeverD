//===- PatchFormatCOFFTests.cpp - x86-64 COFF/PE patch tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PatchFormatTestsDetail.h"
#include "gtest/gtest.h"

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
  auto PatchedOrErr = loadBinary(tmpFile("patched"));
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
  auto PatchedOrErr = loadBinary(tmpFile("patched"));
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

//===----------------------------------------------------------------------===//
// ARM32 COFF/PE patch
//===----------------------------------------------------------------------===//

} // namespace
