#include "NeverDLiftFixture.h"

class AArch64_Mops : public NeverDLiftTest {};

static fs::path testObj() { return fs::path(TEST_OBJ_DIR) / "test_mops_a64.o"; }

TEST_F(AArch64_Mops, SetpPreservesArchitecturalInstructionAndState) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  EXPECT_NE(r.out.find("asm sideeffect \"setp"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("asm sideeffect \"setpn"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("asm sideeffect \"setpt"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("asm sideeffect \"setptn"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("mrs"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("%arg2"), std::string::npos)
      << "The fill register x2 must remain an input:\n"
      << r.out;
  EXPECT_EQ(r.out.find("load i64, ptr %arg0"), std::string::npos)
      << "SETP must use x0 as the destination address, not load through it:\n"
      << r.out;
}

TEST_F(AArch64_Mops, SetpHighCUsesCompilableInlineAssembly) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("__asm__ volatile"), std::string::npos) << source;
  EXPECT_NE(source.find("setp [%0]!, %1!, %3"), std::string::npos) << source;
  EXPECT_NE(source.find("\"+r\""), std::string::npos) << source;
  EXPECT_NE(source.find("_nd_mops_count = (uint64_t)(arg1)"), std::string::npos)
      << source;
  EXPECT_NE(source.find(": \"r\"(arg2)"), std::string::npos) << source;

  auto syntax = exec("clang", {"-std=gnu11", "-fsyntax-only", cFile.string()});
  EXPECT_EQ(syntax.exitCode, 0) << syntax.err << "\n" << source;
}

TEST_F(AArch64_Mops, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_mops_a64.o not built";
  verifyAllStages(testObj());
  verifyLLVMIRNoVerifierErrors(testObj());
}
