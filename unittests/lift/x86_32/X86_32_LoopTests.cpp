#include "NeverDLiftFixture.h"

class X86_32_Loop : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_loop32.o";
}

TEST_F(X86_32_Loop, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_loop32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_Loop, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_Loop, LoopDecrementsCounter) {
    verifyLowIRContains(testObj(), "test_loop_countdown", "INT_SUB");
}

TEST_F(X86_32_Loop, CwdeSignExtends) {
    verifyLowIRContains(testObj(), "test_cwde", "INT_SEXT");
}

TEST_F(X86_32_Loop, CbwSignExtends) {
    verifyLowIRContains(testObj(), "test_cbw", "INT_SEXT");
}

TEST_F(X86_32_Loop, CwdSignExtends) {
    verifyLowIRContains(testObj(), "test_cwd", "INT_ASHR");
}

TEST_F(X86_32_Loop, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_32_Loop, NoConstantTrueBranch) {
    verifyNoConstantTrueBranch(testObj());
}
