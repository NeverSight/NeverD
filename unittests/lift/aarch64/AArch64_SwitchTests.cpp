//===- AArch64_SwitchTests.cpp - Switch / jump-table tests (AArch64) ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

class AArch64_SwitchTest : public NeverDLiftTest {};

static fs::path switchObj() {
  return fs::path(TEST_OBJ_DIR) / "test_switch_a64.o";
}

TEST_F(AArch64_SwitchTest, LowIRProduced) {
  auto R = liftToLowIR(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_FALSE(R.out.empty());
}

TEST_F(AArch64_SwitchTest, MedIRProduced) {
  auto R = liftToMedIR(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_FALSE(R.out.empty());
}

TEST_F(AArch64_SwitchTest, HighIRProduced) {
  auto R = liftToHighIR(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_FALSE(R.out.empty());
}

TEST_F(AArch64_SwitchTest, LLVMIRUnoptProduced) {
  auto R = liftToLLVMIRUnopt(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_FALSE(R.out.empty());
}

TEST_F(AArch64_SwitchTest, SwitchFunctionsDetected) {
  auto R = liftToLowIR(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("switch_simple") || R.contains("switch_dense") ||
              R.contains("switch_large"))
      << "Expected switch function names in LowIR";
}

TEST_F(AArch64_SwitchTest, DecompileProducesOutput) {
  auto R = decompileToHighC(switchObj());
  ASSERT_EQ(R.exitCode, 0) << "Decompile failed: " << R.err;
  auto CFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CFile)) << "C output file not created";
}

TEST_F(AArch64_SwitchTest, LLVMIRUnoptNoVerifierErrors) {
  auto R = liftToLLVMIRUnopt(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.err.find("Incorrect number of arguments") == std::string::npos)
      << "LLVM verifier error found:\n"
      << R.err;
}

TEST_F(AArch64_SwitchTest, MultipleFunctionsLifted) {
  auto R = liftToLowIR(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  int FuncCount = 0;
  std::string::size_type Pos = 0;
  while ((Pos = R.out.find("func ", Pos)) != std::string::npos) {
    ++FuncCount;
    ++Pos;
  }
  EXPECT_GE(FuncCount, 5) << "Expected at least 5 functions";
}

TEST_F(AArch64_SwitchTest, HighIRContainsSwitchKeyword) {
  auto R = liftToHighIR(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  bool HasSwitch = R.out.find("switch") != std::string::npos ||
                   R.out.find("case ") != std::string::npos;
  EXPECT_TRUE(HasSwitch)
      << "Expected 'switch' or 'case' keyword in HighIR output";
}

TEST_F(AArch64_SwitchTest, PatchProducesOutput) {
  auto ElfExe = fs::path(TEST_OBJ_DIR) / "test_switch_a64_elf";
  if (!fs::exists(ElfExe))
    GTEST_SKIP() << "ELF executable not built (ld.lld not available)";
  auto R = patchBinary(ElfExe);
  ASSERT_EQ(R.exitCode, 0) << "AArch64 switch patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  EXPECT_TRUE(fs::exists(PatchedFile)) << "Patched binary not created";
  if (fs::exists(PatchedFile))
    EXPECT_GT(fs::file_size(PatchedFile), 0u) << "Patched binary is empty";
}

TEST_F(AArch64_SwitchTest, LowIRHasIndirectOrConditional) {
  auto R = liftToLowIR(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  bool HasSwitch = R.contains("INDIR_BR") || R.contains("COND_BR");
  EXPECT_TRUE(HasSwitch)
      << "Expected INDIR_BR or COND_BR in LowIR (switch may be if-else)";
}
