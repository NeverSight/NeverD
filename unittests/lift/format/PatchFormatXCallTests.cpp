//===- PatchFormatXCallTests.cpp - Cross-function call patch tests ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "PatchFormatTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::patch_format_test;

class PatchXCall_X64 : public NeverDLiftTest {};

TEST_F(PatchXCall_X64, PatchSucceeds) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_xcall_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "xcall ELF executable not built";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "xcall patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile)) << "Patched binary not created";
  EXPECT_GT(fs::file_size(PatchedFile), 0u);
}

TEST_F(PatchXCall_X64, AllStagesThenPatch) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_xcall_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "xcall ELF executable not built";
  verifyAllStages(Elf);
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "Patch after lift failed: " << R.err;
}

TEST_F(PatchXCall_X64, PatchedBinaryReliftable) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_xcall_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "xcall ELF executable not built";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile));
  auto VerifyR = liftToLowIR(PatchedFile);
  EXPECT_EQ(VerifyR.exitCode, 0) << "Patched xcall binary should be liftable";
}

class PatchXCall_AArch64 : public NeverDLiftTest {};

TEST_F(PatchXCall_AArch64, PatchSucceeds) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_xcall_a64_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "AArch64 xcall ELF not built";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "AArch64 xcall patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  EXPECT_TRUE(fs::exists(PatchedFile));
}

TEST_F(PatchXCall_AArch64, AllStagesThenPatch) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_xcall_a64_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "AArch64 xcall ELF not built";
  verifyAllStages(Elf);
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "Patch after lift failed: " << R.err;
}

class PatchXCall_ARM32 : public NeverDLiftTest {};

TEST_F(PatchXCall_ARM32, PatchSucceeds) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_xcall_arm_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ARM32 xcall ELF not built";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "ARM32 xcall patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  EXPECT_TRUE(fs::exists(PatchedFile));
}

TEST_F(PatchXCall_ARM32, AllStagesThenPatch) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_xcall_arm_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ARM32 xcall ELF not built";
  verifyAllStages(Elf);
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "Patch after lift failed: " << R.err;
}

//===----------------------------------------------------------------------===//
// --text-section override (target a renamed / packed code section)
//===----------------------------------------------------------------------===//

} // namespace
