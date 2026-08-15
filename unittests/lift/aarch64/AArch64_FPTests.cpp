#include "NeverDLiftFixture.h"

class AArch64_FP : public NeverDLiftTest {
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
                        "-march=armv8.3-a+jscvt", "-std=gnu11", "-I",
                        tmp().string(), "-fsyntax-only", CFile.string()});
    EXPECT_EQ(syntax.exitCode, 0) << syntax.err << "\n" << Source;
  }
};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_fp_a64.o";
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

static std::string functionSource(const std::string &Source,
                                  const std::string &Name) {
  auto NamePos = Source.find(Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = Source.rfind('\n', NamePos);
  Begin = Begin == std::string::npos ? 0 : Begin + 1;
  auto End = Source.find("\n}", NamePos);
  if (End == std::string::npos)
    return {};
  return Source.substr(Begin, End + 2 - Begin);
}

TEST_F(AArch64_FP, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_fp_a64.o not built";
    verifyAllStages(testObj());
}

TEST_F(AArch64_FP, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(AArch64_FP, FaddLifts) {
    verifyLowIRContains(testObj(), "test_fadd_a64", "FLOAT_ADD");
}

TEST_F(AArch64_FP, FsubLifts) {
    verifyLowIRContains(testObj(), "test_fsub_a64", "FLOAT_SUB");
}

TEST_F(AArch64_FP, FmulLifts) {
    verifyLowIRContains(testObj(), "test_fmul_a64", "FLOAT_MULT");
}

TEST_F(AArch64_FP, FdivLifts) {
    verifyLowIRContains(testObj(), "test_fdiv_a64", "FLOAT_DIV");
}

TEST_F(AArch64_FP, FsqrtLifts) {
    verifyLowIRContains(testObj(), "test_fsqrt_a64", "FLOAT_SQRT");
}

TEST_F(AArch64_FP, FnegLifts) {
    verifyLowIRContains(testObj(), "test_fneg_a64", "FLOAT_NEG");
}

TEST_F(AArch64_FP, FabsLifts) {
    verifyLowIRContains(testObj(), "test_fabs_a64", "FLOAT_ABS");
}

TEST_F(AArch64_FP, ScvtfLifts) {
    verifyLowIRContains(testObj(), "test_scvtf_a64", "FLOAT_INT2FLOAT");
}

TEST_F(AArch64_FP, FcvtzsLifts) {
    verifyLowIRContains(testObj(), "test_fcvtzs_a64", "FLOAT_FLOAT2INT");
}

TEST_F(AArch64_FP, FjcvtzsUpdatesExactnessFlag) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = functionIR(r.out, "test_fjcvtzs_z_a64");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("@llvm.aarch64.fjcvtzs"), std::string::npos) << F;
  EXPECT_NE(F.find("sitofp i32"), std::string::npos) << F;
  EXPECT_NE(F.find("fcmp oeq double"), std::string::npos) << F;
  EXPECT_EQ(F.find("ret i64 1"), std::string::npos) << F;
}

TEST_F(AArch64_FP, FjcvtzsHighCUsesBuiltinAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("__jcvt(__builtin_bit_cast(double"), std::string::npos)
      << source;
  EXPECT_NE(source.find("0x8000000000000000"), std::string::npos) << source;
  EXPECT_EQ(source.find("__neverd"), std::string::npos) << source;
  auto F = functionSource(source, "test_fjcvtzs_z_a64");
  ASSERT_FALSE(F.empty()) << source;
  EXPECT_NE(F.find("int32_t test_fjcvtzs_z_a64"), std::string::npos) << F;
  EXPECT_NE(F.find("return ("), std::string::npos) << F;

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_FP, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
