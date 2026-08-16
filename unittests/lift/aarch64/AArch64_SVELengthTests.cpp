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

static std::string sveLengthFunctionC(const std::string &Source,
                                      const std::string &Name) {
  auto NamePos = Source.find(Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = Source.rfind('\n', NamePos);
  auto End = Source.find("\n}", NamePos);
  if (End == std::string::npos)
    return {};
  Begin = Begin == std::string::npos ? 0 : Begin + 1;
  return Source.substr(Begin, End + 2 - Begin);
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

TEST_F(AArch64_SVELength, IncAndDecHonorEncodedMultiplier) {
  auto r = liftToLLVMIR(sveLengthObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto Inc = sveLengthFunctionIR(r.out, "test_sve_incb_mul2");
  ASSERT_FALSE(Inc.empty()) << r.out;
  EXPECT_NE(Inc.find("@llvm.aarch64.sve.cntb(i32 31)"), std::string::npos)
      << Inc;
  EXPECT_NE(Inc.find("shl i64 %svcnt, 1"), std::string::npos) << Inc;

  auto Dec = sveLengthFunctionIR(r.out, "test_sve_decw_mul4");
  ASSERT_FALSE(Dec.empty()) << r.out;
  EXPECT_NE(Dec.find("@llvm.aarch64.sve.cntw(i32 31)"), std::string::npos)
      << Dec;
  EXPECT_NE(Dec.find("shl i64 %svcnt, 2"), std::string::npos) << Dec;
}

TEST_F(AArch64_SVELength, AddvlScalesSignedImmediateByRuntimeVectorLength) {
  auto r = liftToLLVMIR(sveLengthObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto Positive = sveLengthFunctionIR(r.out, "test_sve_addvl_two");
  ASSERT_FALSE(Positive.empty()) << r.out;
  EXPECT_NE(Positive.find("@llvm.aarch64.sve.cntb(i32 31)"),
            std::string::npos)
      << Positive;
  EXPECT_NE(Positive.find("shl i64 %svcnt, 1"), std::string::npos)
      << Positive;

  auto Negative = sveLengthFunctionIR(r.out, "test_sve_addvl_negative");
  ASSERT_FALSE(Negative.empty()) << r.out;
  EXPECT_NE(Negative.find("@llvm.aarch64.sve.cntb(i32 31)"),
            std::string::npos)
      << Negative;
  EXPECT_NE(Negative.find("-3"), std::string::npos) << Negative;
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

TEST_F(AArch64_SVELength, HighCHonorsEncodedMultiplierAndCompiles) {
  auto r = decompileToHighC(sveLengthObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());

  auto Inc = sveLengthFunctionC(source, "test_sve_incb_mul2");
  ASSERT_FALSE(Inc.empty()) << source;
  EXPECT_NE(Inc.find("svcntb()"), std::string::npos) << Inc;
  EXPECT_NE(Inc.find("* 2"), std::string::npos) << Inc;

  auto Dec = sveLengthFunctionC(source, "test_sve_decw_mul4");
  ASSERT_FALSE(Dec.empty()) << source;
  EXPECT_NE(Dec.find("svcntw()"), std::string::npos) << Dec;
  EXPECT_NE(Dec.find("* 4"), std::string::npos) << Dec;

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_SVELength, HighCAddvlUsesRuntimeVectorLengthAndCompiles) {
  auto r = decompileToHighC(sveLengthObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());

  auto Positive = sveLengthFunctionC(source, "test_sve_addvl_two");
  ASSERT_FALSE(Positive.empty()) << source;
  EXPECT_NE(Positive.find("svcntb()"), std::string::npos) << Positive;
  EXPECT_NE(Positive.find("* 2"), std::string::npos) << Positive;

  auto Negative = sveLengthFunctionC(source, "test_sve_addvl_negative");
  ASSERT_FALSE(Negative.empty()) << source;
  EXPECT_NE(Negative.find("svcntb()"), std::string::npos) << Negative;
  EXPECT_NE(Negative.find("-3"), std::string::npos) << Negative;

  expectPairedClangSyntax(cFile, source);
}
