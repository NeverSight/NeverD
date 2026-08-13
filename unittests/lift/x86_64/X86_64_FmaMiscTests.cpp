#include "NeverDLiftFixture.h"

class X86_64_FmaMisc : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_fma_misc.o";
}

TEST_F(X86_64_FmaMisc, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_fma_misc.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_FmaMisc, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_FmaMisc, NoUnreachable) {
    verifyLLVMIRNoUnreachable(testObj());
}

TEST_F(X86_64_FmaMisc, EnterLeavePreservesStack) {
    auto r = liftToLowIR(testObj());
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.contains("INT_SUB") || r.contains("STORE"))
        << "ENTER should emit stack operations";
}

TEST_F(X86_64_FmaMisc, Bswap32PreservesSemantics) {
    verifyLowIRContains(testObj(), "test_bswap32", "INT_OR");
}

TEST_F(X86_64_FmaMisc, Bswap64PreservesSemantics) {
    verifyLowIRContains(testObj(), "test_bswap64", "INT_OR");
}

TEST_F(X86_64_FmaMisc, LahfSahfPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_lahf_sahf", "INT_AND");
}

TEST_F(X86_64_FmaMisc, CpuidEmitsIntrinsic) {
    verifyLowIRContains(testObj(), "test_cpuid", "INTRINSIC");
}

TEST_F(X86_64_FmaMisc, RdtscEmitsIntrinsic) {
    verifyLowIRContains(testObj(), "test_rdtsc", "INTRINSIC");
}

TEST_F(X86_64_FmaMisc, BtSetsCF) {
    verifyLowIRContains(testObj(), "test_bt", "INT_RIGHT");
}

TEST_F(X86_64_FmaMisc, BtsModifiesOperand) {
    verifyLowIRContains(testObj(), "test_bts", "INT_OR");
}

TEST_F(X86_64_FmaMisc, BtrModifiesOperand) {
    verifyLowIRContains(testObj(), "test_btr", "INT_AND");
}

TEST_F(X86_64_FmaMisc, BtcModifiesOperand) {
    verifyLowIRContains(testObj(), "test_btc", "INT_XOR");
}

TEST_F(X86_64_FmaMisc, FenceEmitsIntrinsic) {
    verifyLowIRContains(testObj(), "test_fence", "INTRINSIC");
}

TEST_F(X86_64_FmaMisc, Int3EmitsIntrinsic) {
    verifyLowIRContains(testObj(), "test_int3", "INTRINSIC");
}

TEST_F(X86_64_FmaMisc, DecompileSucceeds) {
    verifyDecompileProducesOutput(testObj());
}

TEST_F(X86_64_FmaMisc, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_64_FmaMisc, NoConstantTrueBranch) {
    verifyNoConstantTrueBranch(testObj());
}
