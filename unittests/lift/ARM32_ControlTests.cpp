#include "NeverDLiftFixture.h"

class ARM32_Control : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_control_arm.o";
}

TEST_F(ARM32_Control, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_control_arm.o not built";
    verifyAllStages(testObj());
}

TEST_F(ARM32_Control, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(ARM32_Control, CmpBeqLifts) {
    verifyLowIRContains(testObj(), "test_cmp_beq_arm", "INT_SUB");
}

TEST_F(ARM32_Control, TstLifts) {
    verifyLowIRContains(testObj(), "test_tst_arm", "INT_AND");
}

TEST_F(ARM32_Control, CmnLifts) {
    verifyLowIRContains(testObj(), "test_cmn_arm", "INT_ADD");
}

TEST_F(ARM32_Control, ClzLifts) {
    verifyLowIRContains(testObj(), "test_clz_arm", "LZCOUNT");
}

TEST_F(ARM32_Control, RevLifts) {
    verifyLowIRContains(testObj(), "test_rev_arm", "INT_AND");
}

TEST_F(ARM32_Control, SxtbLifts) {
    verifyLowIRContains(testObj(), "test_sxtb_arm", "INT_SEXT");
}

TEST_F(ARM32_Control, SxthLifts) {
    verifyLowIRContains(testObj(), "test_sxth_arm", "INT_SEXT");
}

TEST_F(ARM32_Control, UxtbLifts) {
    verifyLowIRContains(testObj(), "test_uxtb_arm", "INT_AND");
}

TEST_F(ARM32_Control, AdcLifts) {
    verifyLowIRContains(testObj(), "test_adc_arm", "INT_ADD");
}

TEST_F(ARM32_Control, SbcLifts) {
    verifyLowIRContains(testObj(), "test_sbc_arm", "INT_SUB");
}

TEST_F(ARM32_Control, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
