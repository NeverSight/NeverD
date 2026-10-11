#include "X86FPStateFixture.h"

class X86_64_FPConvert : public X86FPStateLiftTest {};

static fs::path testObj() {
  return fs::path(TEST_OBJ_DIR) / "test_fp_convert.o";
}

TEST_F(X86_64_FPConvert, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_fp_convert.o not built";
  verifyAllStages(testObj());
}

TEST_F(X86_64_FPConvert, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(X86_64_FPConvert, Cvtsi2ssLifts) {
  verifyLowIRContains(testObj(), "test_cvtsi2ss", "FLOAT_INT2FLOAT");
}

TEST_F(X86_64_FPConvert, Unsigned64ConversionPreservesMedIRWidths) {
  auto R = liftToMedIR(testObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_EQ(R.err.find("SUBBYTES input smaller than output"), std::string::npos)
      << R.err;
  EXPECT_EQ(R.err.find("ZEXT/SEXT input not narrower than output"),
            std::string::npos)
      << R.err;
}

TEST_F(X86_64_FPConvert, Cvtss2siLifts) {
  verifyScalarFPState(testObj(), "test_cvtss2si",
                      neverd::Intrinsic::X86FPCvtToIntState);
}

TEST_F(X86_64_FPConvert, Cvttss2siLifts) {
  verifyScalarFPState(testObj(), "test_cvttss2si",
                      neverd::Intrinsic::X86FPTruncToIntState);
}

TEST_F(X86_64_FPConvert, Cvtsi2sdLifts) {
  verifyLowIRContains(testObj(), "test_cvtsi2sd", "FLOAT_INT2FLOAT");
}

TEST_F(X86_64_FPConvert, Cvtss2sdLifts) {
  verifyLowIRContains(testObj(), "test_cvtss2sd", "FLOAT_FLOAT2FLOAT");
}

TEST_F(X86_64_FPConvert, AddssLifts) {
  verifyScalarFPState(testObj(), "test_addss",
                      neverd::Intrinsic::X86FPAddState);
}

TEST_F(X86_64_FPConvert, SubssLifts) {
  verifyScalarFPState(testObj(), "test_subss",
                      neverd::Intrinsic::X86FPSubState);
}

TEST_F(X86_64_FPConvert, MulssLifts) {
  verifyScalarFPState(testObj(), "test_mulss",
                      neverd::Intrinsic::X86FPMulState);
}

TEST_F(X86_64_FPConvert, DivssLifts) {
  verifyScalarFPState(testObj(), "test_divss",
                      neverd::Intrinsic::X86FPDivState);
}

TEST_F(X86_64_FPConvert, SqrtssLifts) {
  verifyScalarFPState(testObj(), "test_sqrtss",
                      neverd::Intrinsic::X86FPArithState,
                      unsigned(neverd::X86FPArithKind::SquareRoot) | 16);
}

TEST_F(X86_64_FPConvert, AddsdLifts) {
  verifyScalarFPState(testObj(), "test_addsd",
                      neverd::Intrinsic::X86FPAddState);
}

TEST_F(X86_64_FPConvert, SqrtsdLifts) {
  verifyScalarFPState(testObj(), "test_sqrtsd",
                      neverd::Intrinsic::X86FPArithState,
                      unsigned(neverd::X86FPArithKind::SquareRoot) | 24);
}

TEST_F(X86_64_FPConvert, LLVMIRHasFloat) {
  verifyLLVMIRContains(testObj(), "test_addss", "addss $2,$0");
}

TEST_F(X86_64_FPConvert, NoUnreachableInFunctions) {
  verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
