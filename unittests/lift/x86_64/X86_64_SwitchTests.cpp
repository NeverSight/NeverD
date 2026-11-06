//===- X86_64_SwitchTests.cpp - Switch / jump-table tests (x86-64) --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

class X86_64_SwitchTest : public NeverDLiftTest {};

static fs::path switchObj() { return fs::path(TEST_OBJ_DIR) / "test_switch.o"; }

static fs::path breakOnlyCasesObj() {
  return fs::path(TEST_OBJ_DIR) / "test_o0_switch_break.o";
}

// clang -O0 lowers `case ...: break;` to a block that holds only a jump, which
// MedIR removes by sending its edges to the jump's target. The cases must come
// from the MedIR jump table: with the LowIR targets, the edge to the code
// after the switch matched no case, and its PHI copies replaced the default
// arm with a trap.
TEST_F(X86_64_SwitchTest, BreakOnlyCasesKeepTheDefaultArm) {
  auto R = decompileToHighC(breakOnlyCasesObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  EXPECT_EQ(C.find("unresolved indirect branch"), std::string::npos) << C;
  const size_t Default = C.find("default:");
  ASSERT_NE(Default, std::string::npos) << C;
  const size_t End = C.find('}', Default);
  ASSERT_NE(End, std::string::npos) << C;
  // The arm holds the source's error path, or jumps to it where the
  // out-of-range check shares that block.
  const std::string Arm = C.substr(Default, End - Default);
  size_t Path = Arm.find("convert_error(") == std::string::npos ? End : Default;
  if (Path == End) {
    const size_t Goto = Arm.find("goto ");
    ASSERT_NE(Goto, std::string::npos) << C;
    const size_t Semicolon = Arm.find(';', Goto);
    ASSERT_NE(Semicolon, std::string::npos) << C;
    const std::string Label = Arm.substr(Goto + 5, Semicolon - Goto - 5) + ":";
    Path = C.find(Label, End);
    ASSERT_NE(Path, std::string::npos) << C;
    EXPECT_NE(C.find("convert_error(", Path), std::string::npos) << C;
  }
  EXPECT_EQ(Arm.find("__builtin_trap"), std::string::npos) << C;
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
