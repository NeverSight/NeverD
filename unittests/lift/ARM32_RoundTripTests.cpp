//===- ARM32_RoundTripTests.cpp - Semantic round-trip (ARM32) --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

class ARM32_RoundTrip : public NeverDLiftTest {};

static fs::path roundtripObj() {
  return fs::path(TEST_OBJ_DIR) / "test_roundtrip_arm.o";
}

TEST_F(ARM32_RoundTrip, AllStagesSucceed) {
  verifyAllStages(roundtripObj());
}

TEST_F(ARM32_RoundTrip, NoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(roundtripObj());
}

TEST_F(ARM32_RoundTrip, DecompileProducesC) {
  auto R = decompileToHighC(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto CFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CFile));
}

TEST_F(ARM32_RoundTrip, HighIRHasControlFlowKeywords) {
  auto R = liftToHighIR(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("if")) << "Expected 'if' in HighIR";
  EXPECT_TRUE(R.contains("return")) << "Expected 'return' in HighIR";
}

TEST_F(ARM32_RoundTrip, LLVMIRHasArithOps) {
  auto R = liftToLLVMIRUnopt(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("add ")) << "Expected 'add' in LLVM IR";
}

TEST_F(ARM32_RoundTrip, LLVMIRHasBitwiseOps) {
  auto R = liftToLLVMIRUnopt(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  bool HasBitwise = R.contains("and ") || R.contains("or ") ||
                    R.contains("xor ");
  EXPECT_TRUE(HasBitwise) << "Expected bitwise ops in LLVM IR";
}

TEST_F(ARM32_RoundTrip, MultipleFunctionsLifted) {
  auto R = liftToLowIR(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  int FuncCount = 0;
  std::string::size_type Pos = 0;
  while ((Pos = R.out.find("func ", Pos)) != std::string::npos) {
    ++FuncCount;
    ++Pos;
  }
  EXPECT_GE(FuncCount, 5) << "Expected at least 5 functions in LowIR";
}

TEST_F(ARM32_RoundTrip, LowIRHasExpectedOpcodes) {
  auto R = liftToLowIR(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("INT_ADD")) << "Expected INT_ADD in LowIR";
  EXPECT_TRUE(R.contains("RETURN")) << "Expected RETURN in LowIR";
}
