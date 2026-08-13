#include "NeverDLiftFixture.h"

class ARM32_Shift : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_shift_arm.o";
}

TEST_F(ARM32_Shift, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_shift_arm.o not built";
    verifyAllStages(testObj());
}

TEST_F(ARM32_Shift, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(ARM32_Shift, LslPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_lsl_imm", "INT_LEFT");
}

TEST_F(ARM32_Shift, LsrPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_lsr_imm", "INT_RIGHT");
}

TEST_F(ARM32_Shift, AsrPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_asr_imm", "INT_ASHR");
}

TEST_F(ARM32_Shift, MvnPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_mvn", "INT_NOT");
}

TEST_F(ARM32_Shift, ShiftedOperand) {
    verifyLowIRContains(testObj(), "test_add_shifted", "INT_LEFT");
}

TEST_F(ARM32_Shift, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(ARM32_Shift, AllModesSucceed) {
    verifyAllModesSucceed(testObj());
}
