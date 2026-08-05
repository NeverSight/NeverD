#include "NeverDLiftFixture.h"

class X86_64_FPConvert : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_fp_convert.o";
}

TEST_F(X86_64_FPConvert, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_fp_convert.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_FPConvert, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_FPConvert, Cvtsi2ssLifts) {
    verifyLowIRContains(testObj(), "test_cvtsi2ss", "FLOAT_INT2FLOAT");
}

TEST_F(X86_64_FPConvert, Cvtss2siLifts) {
    verifyLowIRContains(testObj(), "test_cvtss2si", "FLOAT_FLOAT2INT");
}

TEST_F(X86_64_FPConvert, Cvttss2siLifts) {
    verifyLowIRContains(testObj(), "test_cvttss2si", "FLOAT_TRUNC");
}

TEST_F(X86_64_FPConvert, Cvtsi2sdLifts) {
    verifyLowIRContains(testObj(), "test_cvtsi2sd", "FLOAT_INT2FLOAT");
}

TEST_F(X86_64_FPConvert, Cvtss2sdLifts) {
    verifyLowIRContains(testObj(), "test_cvtss2sd", "FLOAT_FLOAT2FLOAT");
}

TEST_F(X86_64_FPConvert, AddssLifts) {
    verifyLowIRContains(testObj(), "test_addss", "FLOAT_ADD");
}

TEST_F(X86_64_FPConvert, SubssLifts) {
    verifyLowIRContains(testObj(), "test_subss", "FLOAT_SUB");
}

TEST_F(X86_64_FPConvert, MulssLifts) {
    verifyLowIRContains(testObj(), "test_mulss", "FLOAT_MULT");
}

TEST_F(X86_64_FPConvert, DivssLifts) {
    verifyLowIRContains(testObj(), "test_divss", "FLOAT_DIV");
}

TEST_F(X86_64_FPConvert, SqrtssLifts) {
    verifyLowIRContains(testObj(), "test_sqrtss", "FLOAT_SQRT");
}

TEST_F(X86_64_FPConvert, AddsdLifts) {
    verifyLowIRContains(testObj(), "test_addsd", "FLOAT_ADD");
}

TEST_F(X86_64_FPConvert, SqrtsdLifts) {
    verifyLowIRContains(testObj(), "test_sqrtsd", "FLOAT_SQRT");
}

TEST_F(X86_64_FPConvert, LLVMIRHasFloat) {
    verifyLLVMIRContains(testObj(), "test_addss", "fadd");
}

TEST_F(X86_64_FPConvert, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
