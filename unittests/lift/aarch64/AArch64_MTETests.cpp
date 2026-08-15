#include "NeverDLiftFixture.h"

class AArch64_MTE : public NeverDLiftTest {
protected:
  void expectPairedClangSyntax(const fs::path &CFile,
                               const std::string &Source) {
    auto stringHeader = tmpFile("string.h");
    std::ofstream shim(stringHeader);
    ASSERT_TRUE(shim.good());
    shim << "void *memcpy(void *, const void *, __SIZE_TYPE__);\n";
    shim.close();

    auto syntax = exec(NEVERD_TEST_CLANG,
                       {"-target", "aarch64-none-elf", "-ffreestanding",
                        "-march=armv8.5-a+memtag", "-std=gnu11", "-I",
                        tmp().string(), "-fsyntax-only", CFile.string()});
    EXPECT_EQ(syntax.exitCode, 0) << syntax.err << "\n" << Source;
  }
};

static fs::path testObj() { return fs::path(TEST_OBJ_DIR) / "test_mte_a64.o"; }

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

TEST_F(AArch64_MTE, StgAndLdgUseArchitecturalLLVMIntrinsics) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = functionIR(r.out, "test_ldg_after_stg_a64");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("@llvm.aarch64.stg"), std::string::npos) << F;
  EXPECT_NE(F.find("@llvm.aarch64.ldg"), std::string::npos) << F;
  EXPECT_EQ(F.find("asm sideeffect \"stg\""), std::string::npos) << F;
}

TEST_F(AArch64_MTE, HighCUsesACLEAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("__arm_mte_set_tag"), std::string::npos) << source;
  EXPECT_NE(source.find("__arm_mte_get_tag"), std::string::npos) << source;
  EXPECT_EQ(source.find("__neverd"), std::string::npos) << source;
  EXPECT_EQ(source.find("return arg0;"), std::string::npos) << source;

  expectPairedClangSyntax(cFile, source);
}
