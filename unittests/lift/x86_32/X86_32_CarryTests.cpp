#include "NeverDLiftFixture.h"

class X86_32_Carry : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_carry32.o";
}

TEST_F(X86_32_Carry, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_carry32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_Carry, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_Carry, AdcPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_adc32", "INT_ADD");
}

TEST_F(X86_32_Carry, SbbPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_sbb32", "INT_SUB");
}

TEST_F(X86_32_Carry, AdcChainIntegrity) {
    verifyLowIRContains(testObj(), "test_adc_chain", "INT_CARRY");
}

TEST_F(X86_32_Carry, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_32_Carry, NoConstantTrueBranch) {
    verifyNoConstantTrueBranch(testObj());
}

TEST_F(X86_32_Carry, DecompileProducesOutput) {
    verifyDecompileProducesOutput(testObj());
}
