#include "NeverDLiftFixture.h"

class AArch64_SVELength : public NeverDLiftTest {
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
                        "-march=armv8.2-a+sve", "-std=gnu11", "-I",
                        tmp().string(), "-fsyntax-only", CFile.string()});
    EXPECT_EQ(syntax.exitCode, 0) << syntax.err << "\n" << Source;
  }
};

static fs::path sveLengthObj() {
  return fs::path(TEST_OBJ_DIR) / "test_sve_length_a64.o";
}

static std::string sveLengthFunctionIR(const std::string &IR,
                                       const std::string &Name) {
  auto NamePos = IR.find("@" + Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = IR.rfind("define ", NamePos);
  auto End = IR.find("\n}", NamePos);
  if (Begin == std::string::npos || End == std::string::npos)
    return {};
  return IR.substr(Begin, End + 2 - Begin);
}

TEST_F(AArch64_SVELength, CntbAndIncbUseRuntimeVectorLength) {
  auto r = liftToLLVMIR(sveLengthObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = sveLengthFunctionIR(r.out, "test_sve_length_a64");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("@llvm.aarch64.sve.cntb(i32 31)"), std::string::npos) << F;
  EXPECT_EQ(F.find("store i64 16"), std::string::npos) << F;
  EXPECT_EQ(F.find("add i64 %arg0, 1"), std::string::npos) << F;
}

TEST_F(AArch64_SVELength, HighCUsesSVEACLEAndCompiles) {
  auto r = decompileToHighC(sveLengthObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("#include <arm_sve.h>"), std::string::npos) << source;
  EXPECT_NE(source.find("svcntb()"), std::string::npos) << source;
  EXPECT_NE(source.find("arg0"), std::string::npos) << source;
  EXPECT_EQ(source.find("return 17"), std::string::npos) << source;

  expectPairedClangSyntax(cFile, source);
}
