//===- X86_64_RoundTripTests.cpp - Semantic round-trip validation ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// End-to-end tests: compile → lift → decompile and verify that the
/// decompiled output preserves key semantic properties of the original C.
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

class X86_64_RoundTrip : public NeverDLiftTest {};

static fs::path roundtripObj() {
  return fs::path(TEST_OBJ_DIR) / "test_roundtrip.o";
}

TEST_F(X86_64_RoundTrip, AllStagesSucceed) {
  verifyAllStages(roundtripObj());
}

TEST_F(X86_64_RoundTrip, NoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(roundtripObj());
}

TEST_F(X86_64_RoundTrip, NoConstantTrueBranches) {
  verifyNoConstantTrueBranch(roundtripObj());
}

TEST_F(X86_64_RoundTrip, DecompileProducesC) {
  auto R = decompileToHighC(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto CFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CFile));
}

TEST_F(X86_64_RoundTrip, DecompiledCHasFunctionNames) {
  auto R = decompileToHighC(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto CFile = tmpFile("decompiled_high.c");
  std::ifstream Ifs(CFile);
  std::string Content((std::istreambuf_iterator<char>(Ifs)),
                       std::istreambuf_iterator<char>());
  EXPECT_TRUE(Content.find("rt_simple_add") != std::string::npos ||
              Content.find("sub_") != std::string::npos)
      << "Expected function names or sub_ prefixes in decompiled C";
}

TEST_F(X86_64_RoundTrip, HighIRHasControlFlowKeywords) {
  auto R = liftToHighIR(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  bool HasIf = R.out.find("if") != std::string::npos;
  bool HasReturn = R.out.find("return") != std::string::npos;
  EXPECT_TRUE(HasIf)
      << "Expected 'if' keyword in HighIR (from rt_if_else, rt_nested_if)";
  EXPECT_TRUE(HasReturn) << "Expected 'return' keyword in HighIR";
}

TEST_F(X86_64_RoundTrip, LLVMIRHasSwitchOrBranching) {
  auto R = liftToLLVMIRUnopt(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  bool HasBranch = R.out.find("br i1") != std::string::npos ||
                   R.out.find("icmp") != std::string::npos ||
                   R.out.find("switch") != std::string::npos;
  EXPECT_TRUE(HasBranch) << "Expected branching logic in LLVM IR";
}

TEST_F(X86_64_RoundTrip, LLVMIRHasArithOps) {
  auto R = liftToLLVMIRUnopt(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("add ")) << "Expected 'add' in LLVM IR";
}

TEST_F(X86_64_RoundTrip, LLVMIRHasBitwiseOps) {
  auto R = liftToLLVMIRUnopt(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  bool HasBitwise = R.contains("and ") || R.contains("or ") ||
                    R.contains("xor ");
  EXPECT_TRUE(HasBitwise) << "Expected bitwise ops in LLVM IR";
}

TEST_F(X86_64_RoundTrip, LLVMIRHasShifts) {
  auto R = liftToLLVMIRUnopt(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  bool HasShift = R.contains("shl ") || R.contains("lshr ") ||
                  R.contains("ashr ");
  EXPECT_TRUE(HasShift) << "Expected shift ops in LLVM IR";
}

TEST_F(X86_64_RoundTrip, MultipleFunctionsLifted) {
  auto R = liftToLowIR(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  int FuncCount = 0;
  std::string::size_type Pos = 0;
  while ((Pos = R.out.find("func ", Pos)) != std::string::npos) {
    ++FuncCount;
    ++Pos;
  }
  EXPECT_GE(FuncCount, 8) << "Expected at least 8 functions in LowIR";
}

TEST_F(X86_64_RoundTrip, LowIRHasExpectedOpcodes) {
  auto R = liftToLowIR(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("INT_ADD")) << "Expected INT_ADD in LowIR";
  EXPECT_TRUE(R.contains("COND_BR")) << "Expected COND_BR in LowIR";
  EXPECT_TRUE(R.contains("RETURN")) << "Expected RETURN in LowIR";
}

TEST_F(X86_64_RoundTrip, PatchRoundTrip) {
  auto ElfExe = fs::path(TEST_OBJ_DIR) / "test_roundtrip_elf";
  if (!fs::exists(ElfExe))
    GTEST_SKIP() << "ELF executable not built (ld.lld not available)";
  auto R = patchBinary(ElfExe);
  ASSERT_EQ(R.exitCode, 0) << "Patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  EXPECT_TRUE(fs::exists(PatchedFile)) << "Patched binary not created";
  if (fs::exists(PatchedFile))
    EXPECT_GT(fs::file_size(PatchedFile), 0u) << "Patched binary is empty";
}

TEST_F(X86_64_RoundTrip, DecompileLLVMRoute) {
  auto R = decompileToC(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << "LLVM-route decompile failed: " << R.err;
  auto CFile = tmpFile("decompiled.c");
  ASSERT_TRUE(fs::exists(CFile));
}
