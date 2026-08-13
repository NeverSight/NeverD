//===- PatchFormatTests.cpp - Cross-format ELF patch tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "PatchFormatTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::patch_format_test;

TEST(BinaryPatcherTrampolines,
     AcceptsMachOObjectPrefixForAutoNamedFunctions) {
  std::vector<uint8_t> Binary(64, 0);
  const std::map<std::string, uint64_t> CompiledSymbols{
      {"_sub_1004", 0x2000}};
  std::vector<std::pair<va_t, va_t>> Mappings;

  EXPECT_EQ(BinaryPatcher::installTrampolines(
                Binary, CompiledSymbols, /*OrigTextVA=*/0x1000,
                /*OrigTextSize=*/Binary.size(), /*OrigTextFileOff=*/0,
                /*ImageBase=*/0, Arch::AArch64, InstructionMode::Default,
                nullptr, nullptr, nullptr, nullptr, &Mappings),
            1u);
  EXPECT_EQ(Mappings,
            (std::vector<std::pair<va_t, va_t>>{{0x1004, 0x2000}}));
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
