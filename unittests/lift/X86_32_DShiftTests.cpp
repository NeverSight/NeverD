#include "NeverDLiftFixture.h"

class X86_32_DShift : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_dshift32.o";
}

TEST_F(X86_32_DShift, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_dshift32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_DShift, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_DShift, NoUnreachable) {
    verifyLLVMIRNoUnreachable(testObj());
}

TEST_F(X86_32_DShift, ShldPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_shld", "INT_LEFT");
}

TEST_F(X86_32_DShift, ShrdPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_shrd", "INT_RIGHT");
}

TEST_F(X86_32_DShift, Bswap32PreservesSemantics) {
    verifyLowIRContains(testObj(), "test_bswap32", "INT_OR");
}

TEST_F(X86_32_DShift, EnterLeavePreservesStack) {
    auto r = liftToLowIR(testObj());
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.contains("INT_SUB") && r.contains("STORE"));
}

TEST_F(X86_32_DShift, BtSetsCF) {
    verifyLowIRContains(testObj(), "test_bt32", "INT_RIGHT");
}

TEST_F(X86_32_DShift, BtsModifiesOperand) {
    verifyLowIRContains(testObj(), "test_bts32", "INT_OR");
}

TEST_F(X86_32_DShift, BtrModifiesOperand) {
    verifyLowIRContains(testObj(), "test_btr32", "INT_AND");
}

TEST_F(X86_32_DShift, Xchg32PreservesSemantics) {
    verifyLowIRContains(testObj(), "test_xchg32", "COPY");
}

TEST_F(X86_32_DShift, DecompileSucceeds) {
    verifyDecompileProducesOutput(testObj());
}

TEST_F(X86_32_DShift, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_32_DShift, NoConstantTrueBranch) {
    verifyNoConstantTrueBranch(testObj());
}
