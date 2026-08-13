#include "NeverDLiftFixture.h"

class X86_64_LoopBit : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_loop.o";
}

TEST_F(X86_64_LoopBit, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_loop.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_LoopBit, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_LoopBit, LoopDecrementsCounter) {
    verifyLowIRContains(testObj(), "test_loop_sum", "INT_SUB");
}

TEST_F(X86_64_LoopBit, BtTestsBit) {
    verifyLowIRContains(testObj(), "test_bt", "INT_RIGHT");
}

TEST_F(X86_64_LoopBit, BtsSetsBit) {
    verifyLowIRContains(testObj(), "test_bts", "INT_OR");
}

TEST_F(X86_64_LoopBit, BtrResetsBit) {
    verifyLowIRContains(testObj(), "test_btr", "INT_AND");
}

TEST_F(X86_64_LoopBit, BtcComplementsBit) {
    verifyLowIRContains(testObj(), "test_btc", "INT_XOR");
}

TEST_F(X86_64_LoopBit, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_64_LoopBit, NoConstantTrueBranch) {
    verifyNoConstantTrueBranch(testObj());
}
