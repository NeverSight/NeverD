#include "../x86_64/X86FPStateFixture.h"

#include <fstream>
#include <iterator>

class X86_32_FP : public X86FPStateLiftTest {};

static fs::path testObj() { return fs::path(TEST_OBJ_DIR) / "test_fp32.o"; }

TEST_F(X86_32_FP, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_fp32.o not built";
  verifyAllStages(testObj());
}

TEST_F(X86_32_FP, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(X86_32_FP, AddssLifts) {
  verifyScalarFPState(testObj(), "test_addss32",
                      neverd::Intrinsic::X86FPAddState);
}

TEST_F(X86_32_FP, SubssLifts) {
  verifyScalarFPState(testObj(), "test_subss32",
                      neverd::Intrinsic::X86FPSubState);
}

TEST_F(X86_32_FP, MulssLifts) {
  verifyScalarFPState(testObj(), "test_mulss32",
                      neverd::Intrinsic::X86FPMulState);
}

TEST_F(X86_32_FP, DivssLifts) {
  verifyScalarFPState(testObj(), "test_divss32",
                      neverd::Intrinsic::X86FPDivState);
}

TEST_F(X86_32_FP, Cvtss2siLifts) {
  verifyScalarFPState(testObj(), "test_cvtss2si32",
                      neverd::Intrinsic::X86FPCvtToIntState);
}

TEST_F(X86_32_FP, Cvtsi2ssLifts) {
  verifyLowIRContains(testObj(), "test_cvtsi2ss32", "FLOAT_INT2FLOAT");
}

TEST_F(X86_32_FP, AddsdLifts) {
  verifyScalarFPState(testObj(), "test_addsd32",
                      neverd::Intrinsic::X86FPAddState);
}

TEST_F(X86_32_FP, Cvttss2siLifts) {
  verifyScalarFPState(testObj(), "test_cvttss2si32",
                      neverd::Intrinsic::X86FPTruncToIntState);
}

TEST_F(X86_32_FP, NoUnreachableInFunctions) {
  verifyLLVMIRNotContains(testObj(), "", "unreachable");
}

// i386 has no SSE: a function the C prints an SSE2 intrinsic in takes the
// sse2 target feature, so the C compiles for a plain i386.
TEST_F(X86_32_FP, Sse2IntrinsicsTakeTheirTargetFeature) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "assembling i386 requires Clang";
  const auto Source = tmpFile("unpack32.s");
  const auto Object = tmpFile("unpack32.o");
  std::ofstream(Source) << R"S(
.text
.globl unpack32
.type unpack32,@function
unpack32:
  movsd 4(%esp), %xmm0
  movsd 12(%esp), %xmm1
  unpcklpd %xmm1, %xmm0
  unpckhpd %xmm0, %xmm1
  movd %xmm1, %eax
  ret
.size unpack32,.-unpack32
)S";
  const auto Assembled =
      exec(NEVERD_TEST_CLANG, {"-target", "i386-linux-gnu", "-c",
                               Source.string(), "-o", Object.string()});
  ASSERT_EQ(Assembled.exitCode, 0) << Assembled.err;
  const auto Decompiled = decompileToHighC(Object);
  ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  for (const char *Name : {"_mm_unpacklo_pd(", "_mm_unpackhi_pd("})
    EXPECT_NE(C.find(Name), std::string::npos) << Name << "\n" << C;
  EXPECT_EQ(C.find("unknown"), std::string::npos) << C;
  const auto CFile = tmpFile("unpack32_high.c");
  std::ofstream(CFile) << C;
  const auto Compiled =
      checkHighCClangCompile(CFile, {"-target", "i386-linux-gnu", "-march=i386",
                                     "-ffreestanding", "-std=gnu11"});
  EXPECT_EQ(Compiled.exitCode, 0) << Compiled.err << "\n" << C;
}
