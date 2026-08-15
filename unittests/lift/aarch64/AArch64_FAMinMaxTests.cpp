#include "NeverDLiftFixture.h"

class AArch64_FAMinMax : public NeverDLiftTest {};

static fs::path testObj() {
  return fs::path(TEST_OBJ_DIR) / "test_faminmax_a64.o";
}

static std::string functionIR(const std::string &IR, const std::string &Name) {
  auto NamePos = IR.find("@" + Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = IR.rfind("define ", NamePos);
  auto End = IR.find("\n}", NamePos);
  if (Begin == std::string::npos || End == std::string::npos)
    return {};
  return IR.substr(Begin, End + 2 - Begin);
}

TEST_F(AArch64_FAMinMax, FamaxUsesPerHalfLaneAbsoluteMaximum) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = functionIR(r.out, "famax_fp16_bits");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("llvm.aarch64.neon.famax.v4f16"), std::string::npos) << F;
  EXPECT_EQ(F.find("fmul"), std::string::npos) << F;
  EXPECT_EQ(F.find("double"), std::string::npos) << F;
}

TEST_F(AArch64_FAMinMax, FaminUsesPerHalfLaneAbsoluteMinimum) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = functionIR(r.out, "famin_fp16_bits");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("llvm.aarch64.neon.famin.v4f16"), std::string::npos) << F;
  EXPECT_EQ(F.find("fmul"), std::string::npos) << F;
  EXPECT_EQ(F.find("double"), std::string::npos) << F;
}

TEST_F(AArch64_FAMinMax, HighCUsesACLEBuiltinsWithoutPrivateAliases) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("vamax_f16"), std::string::npos) << source;
  EXPECT_NE(source.find("vamin_f16"), std::string::npos) << source;
  EXPECT_EQ(source.find("__neverd"), std::string::npos) << source;
}

TEST_F(AArch64_FAMinMax, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_faminmax_a64.o not built";
  verifyAllStages(testObj());
  verifyLLVMIRNoVerifierErrors(testObj());
}
