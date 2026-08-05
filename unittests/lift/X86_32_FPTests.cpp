#include "NeverDLiftFixture.h"

class X86_32_FP : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_fp32.o";
}

TEST_F(X86_32_FP, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_fp32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_FP, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_FP, AddssLifts) {
    verifyLowIRContains(testObj(), "test_addss32", "FLOAT_ADD");
}

TEST_F(X86_32_FP, SubssLifts) {
    verifyLowIRContains(testObj(), "test_subss32", "FLOAT_SUB");
}

TEST_F(X86_32_FP, MulssLifts) {
    verifyLowIRContains(testObj(), "test_mulss32", "FLOAT_MULT");
}

TEST_F(X86_32_FP, DivssLifts) {
    verifyLowIRContains(testObj(), "test_divss32", "FLOAT_DIV");
}

TEST_F(X86_32_FP, Cvtss2siLifts) {
    verifyLowIRContains(testObj(), "test_cvtss2si32", "FLOAT_FLOAT2INT");
}

TEST_F(X86_32_FP, Cvtsi2ssLifts) {
    verifyLowIRContains(testObj(), "test_cvtsi2ss32", "FLOAT_INT2FLOAT");
}

TEST_F(X86_32_FP, AddsdLifts) {
    verifyLowIRContains(testObj(), "test_addsd32", "FLOAT_ADD");
}

TEST_F(X86_32_FP, Cvttss2siLifts) {
    verifyLowIRContains(testObj(), "test_cvttss2si32", "FLOAT_TRUNC");
}

TEST_F(X86_32_FP, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
