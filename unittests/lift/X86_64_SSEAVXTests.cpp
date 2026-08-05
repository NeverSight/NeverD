#include "NeverDLiftFixture.h"

class X86_64_SSEAVX : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_sse_avx.o";
}

TEST_F(X86_64_SSEAVX, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_sse_avx.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_SSEAVX, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_SSEAVX, AddssLifts) {
    verifyLowIRContains(testObj(), "test_addss", "FLOAT_ADD");
}

TEST_F(X86_64_SSEAVX, SubssLifts) {
    verifyLowIRContains(testObj(), "test_subss", "FLOAT_SUB");
}

TEST_F(X86_64_SSEAVX, MulssLifts) {
    verifyLowIRContains(testObj(), "test_mulss", "FLOAT_MULT");
}

TEST_F(X86_64_SSEAVX, DivssLifts) {
    verifyLowIRContains(testObj(), "test_divss", "FLOAT_DIV");
}

TEST_F(X86_64_SSEAVX, AddsdLifts) {
    verifyLowIRContains(testObj(), "test_addsd", "FLOAT_ADD");
}

TEST_F(X86_64_SSEAVX, SubsdLifts) {
    verifyLowIRContains(testObj(), "test_subsd", "FLOAT_SUB");
}

TEST_F(X86_64_SSEAVX, MulsdLifts) {
    verifyLowIRContains(testObj(), "test_mulsd", "FLOAT_MULT");
}

TEST_F(X86_64_SSEAVX, DivsdLifts) {
    verifyLowIRContains(testObj(), "test_divsd", "FLOAT_DIV");
}

TEST_F(X86_64_SSEAVX, SqrtssLifts) {
    verifyLowIRContains(testObj(), "test_sqrtss", "FLOAT_SQRT");
}

TEST_F(X86_64_SSEAVX, SqrtsdLifts) {
    verifyLowIRContains(testObj(), "test_sqrtsd", "FLOAT_SQRT");
}

TEST_F(X86_64_SSEAVX, CvtSs2SiLifts) {
    verifyLowIRContains(testObj(), "test_cvtss2si", "FLOAT2INT");
}

TEST_F(X86_64_SSEAVX, CvtSi2SsLifts) {
    verifyLowIRContains(testObj(), "test_cvtsi2ss", "INT2FLOAT");
}

TEST_F(X86_64_SSEAVX, CvtSd2SiLifts) {
    verifyLowIRContains(testObj(), "test_cvtsd2si", "FLOAT2INT");
}

TEST_F(X86_64_SSEAVX, CvtSi2SdLifts) {
    verifyLowIRContains(testObj(), "test_cvtsi2sd", "INT2FLOAT");
}

TEST_F(X86_64_SSEAVX, CvtSd2SsLifts) {
    verifyLowIRContains(testObj(), "test_cvtsd2ss", "FLOAT_FLOAT2FLOAT");
}

TEST_F(X86_64_SSEAVX, CvtSs2SdLifts) {
    verifyLowIRContains(testObj(), "test_cvtss2sd", "FLOAT_FLOAT2FLOAT");
}

TEST_F(X86_64_SSEAVX, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
