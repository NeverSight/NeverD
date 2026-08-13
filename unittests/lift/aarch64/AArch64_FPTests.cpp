#include "NeverDLiftFixture.h"

class AArch64_FP : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_fp_a64.o";
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

TEST_F(AArch64_FP, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
