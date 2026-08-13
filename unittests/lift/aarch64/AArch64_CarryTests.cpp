#include "NeverDLiftFixture.h"

class AArch64_Carry : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_carry_a64.o";
}

TEST_F(AArch64_Carry, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_carry_a64.o not built";
    verifyAllStages(testObj());
}

TEST_F(AArch64_Carry, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(AArch64_Carry, AdcPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_adc_a64", "INT_ADD");
}

TEST_F(AArch64_Carry, SbcPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_sbc_a64", "INT_SUB");
}

TEST_F(AArch64_Carry, NegPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_neg_a64", "INT_SUB");
}

TEST_F(AArch64_Carry, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(AArch64_Carry, AllModesSucceed) {
    verifyAllModesSucceed(testObj());
}
