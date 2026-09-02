//===- PatchFormatTests.cpp - Cross-format ELF patch tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PatchFormatTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/pipeline/Pipeline.h"

#include "llvm/Transforms/Utils/Cloning.h"

#include <array>

namespace {

using namespace neverd;
using namespace neverd::patch_format_test;

class ReceiptInplaceRewriter final : public InplaceRewriter {
public:
  static PatchResult writeMappings(const fs::path &OutputPath,
                                   std::initializer_list<va_t> Entries) {
    RewriteState State;
    State.Binary.assign(4, 0xaa);
    for (va_t Entry : Entries) {
      InplaceMapping Mapping;
      Mapping.OrigVA = Entry;
      State.Mappings.push_back(std::move(Mapping));
    }
    return writeResult(OutputPath, State, /*SetExecPerm=*/false);
  }

protected:
  BinaryFormat getBinaryFormat() const override { return BinaryFormat::ELF; }

  std::unique_ptr<RelocResolver> createRelocResolver() const override {
    return nullptr;
  }
};

class PatchReceipt : public NeverDLiftTest {};

class ReceiptPreparationProbe : public BinaryPatcher {
public:
  static bool prepareSources(llvm::Module &Module, const BinaryImage &Image,
                             SourceFunctionPreparation &Preparation,
                             std::string &Detail) {
    return prepareSourceFunctionsForPatch(Module, &Image, Preparation, Detail);
  }
};

TEST_F(PatchReceipt, InplacePublishesSortedUniqueCommittedMappings) {
  const PatchResult Result = ReceiptInplaceRewriter::writeMappings(
      tmpFile("receipt.bin"), {0x3010, 0x1010, 0x3010, 0x2010});

  ASSERT_TRUE(Result.Success);
  EXPECT_EQ(Result.PatchedOriginalEntries,
            (std::vector<va_t>{0x1010, 0x2010, 0x3010}));
}

TEST_F(PatchReceipt, FailedInplaceWritePublishesNoMappings) {
  const PatchResult Result = ReceiptInplaceRewriter::writeMappings(
      tmpFile("missing") / "receipt.bin", {0x1010});

  EXPECT_FALSE(Result.Success);
  EXPECT_TRUE(Result.PatchedOriginalEntries.empty());
}

TEST_F(PatchReceipt,
       SectionReceiptsMatchExactReplaceableSourcesAcrossELFAndCOFF) {
  const std::array<std::pair<const char *, const char *>, 2> Fixtures{{
      {"ELF", "test_patch_switch_elf"},
      {"COFF", "test_patch_coff.exe"},
  }};

  for (const auto &[FormatName, FileName] : Fixtures) {
    SCOPED_TRACE(FormatName);
    const fs::path InputPath = fs::path(TEST_OBJ_DIR) / FileName;
    if (!fs::exists(InputPath)) {
      ADD_FAILURE() << FormatName << " patch fixture was not built";
      continue;
    }

    auto ImageOrError = loadBinary(InputPath);
    ASSERT_TRUE(static_cast<bool>(ImageOrError))
        << llvm::toString(ImageOrError.takeError());
    BinaryImage Image = std::move(*ImageOrError);

    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.PatchMode = true;
    PipelineResult PipelineOutput = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(PipelineOutput.Success) << PipelineOutput.Error;
    ASSERT_NE(PipelineOutput.LlvmModule, nullptr);

    auto PreparationModule = llvm::CloneModule(*PipelineOutput.LlvmModule);
    SourceFunctionPreparation Preparation;
    std::string Detail;
    ASSERT_TRUE(ReceiptPreparationProbe::prepareSources(
        *PreparationModule, Image, Preparation, Detail))
        << Detail;
    ASSERT_FALSE(Preparation.ReplaceableOriginalVAs.empty());

    std::vector<va_t> Expected;
    Expected.reserve(Preparation.ReplaceableOriginalVAs.size());
    for (const auto &[FunctionName, OriginalVA] :
         Preparation.ReplaceableOriginalVAs) {
      (void)FunctionName;
      Expected.push_back(OriginalVA);
    }
    std::sort(Expected.begin(), Expected.end());
    Expected.erase(std::unique(Expected.begin(), Expected.end()),
                   Expected.end());

    std::unique_ptr<BinaryPatcher> Patcher =
        BinaryPatcher::create(Image.Format);
    ASSERT_NE(Patcher, nullptr);
    Patcher->setImageContext(&Image);
    const PatchResult Result =
        Patcher->patch(InputPath, tmpFile(std::string(FormatName) + ".patched"),
                       *PipelineOutput.LlvmModule, Image.Arch);

    ASSERT_TRUE(Result.Success);
    EXPECT_EQ(Result.PatchedOriginalEntries, Expected);
    EXPECT_EQ(Result.PatchedOriginalEntries.size(), Result.TrampolineCount);
  }
}

TEST(BinaryPatcherTrampolines, AcceptsMachOObjectPrefixForAutoNamedFunctions) {
  std::vector<uint8_t> Binary(64, 0);
  const std::map<std::string, uint64_t> CompiledSymbols{{"_sub_1004", 0x2000},
                                                        {"_sub_103f", 0x3000}};
  std::vector<std::pair<va_t, va_t>> Mappings;
  std::vector<PatchedFunctionEntry> InstalledFunctions;

  EXPECT_EQ(BinaryPatcher::installTrampolines(
                Binary, CompiledSymbols, /*OrigTextVA=*/0x1000,
                /*OrigTextSize=*/Binary.size(), /*OrigTextFileOff=*/0,
                /*ImageBase=*/0, Arch::AArch64, InstructionMode::Default,
                nullptr, nullptr, nullptr, nullptr, &Mappings,
                &InstalledFunctions),
            1u);
  EXPECT_EQ(Mappings, (std::vector<std::pair<va_t, va_t>>{{0x1004, 0x2000}}));
  ASSERT_EQ(InstalledFunctions.size(), 1u);
  EXPECT_EQ(InstalledFunctions.front().OriginalVA, 0x1004u);
  EXPECT_EQ(InstalledFunctions.front().OwnerVA, 0x2000u);

  PatchResult Result;
  Result.Success = true;
  patch_receipt_detail::publishCommitted(
      Result, InstalledFunctions, [](const PatchedFunctionEntry &Installed) {
        return Installed.OriginalVA;
      });
  EXPECT_EQ(Result.PatchedOriginalEntries, (std::vector<va_t>{0x1004}));
}
//===----------------------------------------------------------------------===//
// x86-64 ELF patch
//===----------------------------------------------------------------------===//

class PatchELF_X64 : public NeverDLiftTest {};

TEST_F(PatchELF_X64, SwitchPatchSucceeds) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ELF executable not built (ld.lld not available)";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "ELF patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile)) << "Patched binary not created";
  EXPECT_GT(fs::file_size(PatchedFile), 0u) << "Patched binary is empty";
}

TEST_F(PatchELF_X64, AllStagesThenPatch) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ELF executable not built";
  verifyAllStages(Elf);
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "Patch after lift failed: " << R.err;
}

TEST_F(PatchELF_X64, PatchedBinaryIsValidELF) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ELF executable not built";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile));

  auto VerifyR = liftToLowIR(PatchedFile);
  EXPECT_EQ(VerifyR.exitCode, 0)
      << "Patched binary should be liftable: " << VerifyR.err;
}

//===----------------------------------------------------------------------===//
// AArch64 ELF patch
//===----------------------------------------------------------------------===//

class PatchELF_AArch64 : public NeverDLiftTest {};

TEST_F(PatchELF_AArch64, SwitchPatchSucceeds) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_a64_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "AArch64 ELF executable not built";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "AArch64 ELF patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile)) << "Patched binary not created";
  EXPECT_GT(fs::file_size(PatchedFile), 0u);
}

TEST_F(PatchELF_AArch64, AllStagesThenPatch) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_a64_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "AArch64 ELF executable not built";
  verifyAllStages(Elf);
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "Patch after lift failed: " << R.err;
}

//===----------------------------------------------------------------------===//
// ARM32 ELF patch
//===----------------------------------------------------------------------===//

class PatchELF_ARM32 : public NeverDLiftTest {};

TEST_F(PatchELF_ARM32, SwitchPatchSucceeds) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_arm_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ARM32 ELF executable not built";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "ARM32 ELF patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile)) << "Patched binary not created";
  EXPECT_GT(fs::file_size(PatchedFile), 0u);
}

TEST_F(PatchELF_ARM32, AllStagesThenPatch) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_arm_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ARM32 ELF executable not built";
  verifyAllStages(Elf);
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "Patch after lift failed: " << R.err;
}

//===----------------------------------------------------------------------===//
// x86-64 COFF/PE patch
//===----------------------------------------------------------------------===//

} // namespace
