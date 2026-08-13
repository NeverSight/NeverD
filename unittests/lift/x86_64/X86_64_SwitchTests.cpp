//===- X86_64_SwitchTests.cpp - Switch / jump-table tests (x86-64) --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

class X86_64_SwitchTest : public NeverDLiftTest {};

static fs::path switchObj() {
  return fs::path(TEST_OBJ_DIR) / "test_switch.o";
}

TEST_F(X86_64_SwitchTest, LowIRProduced) {
  auto R = liftToLowIR(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_FALSE(R.out.empty());
}

TEST_F(X86_64_SwitchTest, MedIRProduced) {
  auto R = liftToMedIR(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_FALSE(R.out.empty());
}

TEST_F(X86_64_SwitchTest, HighIRProduced) {
  auto R = liftToHighIR(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_FALSE(R.out.empty());
}

TEST_F(X86_64_SwitchTest, LLVMIRUnoptProduced) {
  auto R = liftToLLVMIRUnopt(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_FALSE(R.out.empty());
}

TEST_F(X86_64_SwitchTest, LowIRHasBranchInd) {
  auto R = liftToLowIR(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("INDIR_BR"))
      << "Expected at least one INDIR_BR (jump table) in LowIR";
}

TEST_F(X86_64_SwitchTest, SwitchFunctionsDetected) {
  auto R = liftToLowIR(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("switch_simple") || R.contains("switch_dense") ||
              R.contains("switch_large"))
      << "Expected switch function names in LowIR";
}

TEST_F(X86_64_SwitchTest, DecompileProducesOutput) {
  auto R = decompileToHighC(switchObj());
  ASSERT_EQ(R.exitCode, 0) << "Decompile failed: " << R.err;
  auto CFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CFile)) << "C output file not created";
}

TEST_F(X86_64_SwitchTest, LLVMIRUnoptNoVerifierErrors) {
  auto R = liftToLLVMIRUnopt(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.err.find("Incorrect number of arguments") == std::string::npos)
      << "LLVM verifier error found:\n"
      << R.err;
}

TEST_F(X86_64_SwitchTest, LLVMIRUnoptHasConditionalLogic) {
  auto R = liftToLLVMIRUnopt(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  bool HasCond = R.out.find("br i1 %") != std::string::npos ||
                 R.out.find("select i1") != std::string::npos ||
                 R.out.find("icmp") != std::string::npos;
  EXPECT_TRUE(HasCond)
      << "Expected conditional logic (br i1/select/icmp) in LLVM IR";
}

TEST_F(X86_64_SwitchTest, MultipleFunctionsLifted) {
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

TEST_F(X86_64_SwitchTest, HighIRContainsSwitchKeyword) {
  auto R = liftToHighIR(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  bool HasSwitch = R.out.find("switch") != std::string::npos ||
                   R.out.find("case ") != std::string::npos;
  EXPECT_TRUE(HasSwitch)
      << "Expected 'switch' or 'case' keyword in HighIR output";
}

TEST_F(X86_64_SwitchTest, PatchObjRejects) {
  auto R = patchBinary(switchObj());
  EXPECT_NE(R.exitCode, 0) << "ELF .o should not be directly patchable";
}

TEST_F(X86_64_SwitchTest, PatchELFExecutable) {
  auto ElfExe = fs::path(TEST_OBJ_DIR) / "test_switch_elf";
  if (!fs::exists(ElfExe))
    GTEST_SKIP() << "ELF executable not built (ld.lld not available)";
  auto R = patchBinary(ElfExe);
  ASSERT_EQ(R.exitCode, 0) << "ELF patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  EXPECT_TRUE(fs::exists(PatchedFile)) << "Patched binary not created";
  if (fs::exists(PatchedFile))
    EXPECT_GT(fs::file_size(PatchedFile), 0u);
}

TEST_F(X86_64_SwitchTest, LiftRoundTripNoVerifierErrors) {
  auto Lift = liftToLLVMIRUnopt(switchObj());
  ASSERT_EQ(Lift.exitCode, 0) << Lift.err;
  EXPECT_TRUE(Lift.err.find("verif") == std::string::npos &&
              Lift.err.find("Verif") == std::string::npos)
      << "Verifier error in round-trip:\n"
      << Lift.err;
}

TEST_F(X86_64_SwitchTest, OffsetBaseFunctionDetected) {
  auto R = liftToLowIR(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("switch_offset_base") ||
              R.contains("switch_enum_like"))
      << "Expected offset-base switch functions to be detected";
}

TEST_F(X86_64_SwitchTest, CompoundGuardFunctionsLift) {
  auto R = liftToHighIR(switchObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_FALSE(R.out.empty())
      << "Expected HighIR output for compound guard patterns";
}

TEST_F(X86_64_SwitchTest, DecompileContainsAllFunctions) {
  auto R = decompileToHighC(switchObj());
  ASSERT_EQ(R.exitCode, 0) << "Decompile failed: " << R.err;
  auto CFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CFile)) << "C output file not created";
  EXPECT_GT(fs::file_size(CFile), 100u) << "C output suspiciously small";
}
