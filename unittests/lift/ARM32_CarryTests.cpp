#include "NeverDLiftFixture.h"

class ARM32_Carry : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_carry_arm.o";
}

TEST_F(ARM32_Carry, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_carry_arm.o not built";
    verifyAllStages(testObj());
}

TEST_F(ARM32_Carry, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(ARM32_Carry, AdcPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_adc_arm", "INT_ADD");
}

TEST_F(ARM32_Carry, SbcPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_sbc_arm", "INT_SUB");
}

TEST_F(ARM32_Carry, RscPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_rsc_arm", "INT_SUB");
}

TEST_F(ARM32_Carry, NegUsesRsb) {
    verifyLowIRContains(testObj(), "test_neg_arm", "INT_SUB");
}

TEST_F(ARM32_Carry, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(ARM32_Carry, AllModesSucceed) {
    verifyAllModesSucceed(testObj());
}
