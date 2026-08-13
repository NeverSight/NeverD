#include "NeverDLiftFixture.h"

class X86_64_SSEMisc : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_sse_misc.o";
}

TEST_F(X86_64_SSEMisc, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_sse_misc.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_SSEMisc, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_SSEMisc, NoUnreachable) {
    verifyLLVMIRNoUnreachable(testObj());
}

TEST_F(X86_64_SSEMisc, MovapsPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_movaps", "COPY");
}

TEST_F(X86_64_SSEMisc, PxorClearPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_pxor_clear", "COPY");
}

TEST_F(X86_64_SSEMisc, PadddPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_paddd", "INT_ADD");
}

TEST_F(X86_64_SSEMisc, PsubdPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_psubd", "INT_SUB");
}

TEST_F(X86_64_SSEMisc, PcmpeqdPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_pcmpeqd", "INT_EQUAL");
}

TEST_F(X86_64_SSEMisc, AddpsPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_addps", "FLOAT_ADD");
}

TEST_F(X86_64_SSEMisc, SubpsPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_subps", "FLOAT_SUB");
}

TEST_F(X86_64_SSEMisc, MulpsPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_mulps", "FLOAT_MULT");
}

TEST_F(X86_64_SSEMisc, DivpsPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_divps", "FLOAT_DIV");
}

TEST_F(X86_64_SSEMisc, PshufdPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_pshufd", "INTRINSIC");
}

TEST_F(X86_64_SSEMisc, MovdPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_movd", "COPY");
}

TEST_F(X86_64_SSEMisc, MovqPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_movq", "COPY");
}

TEST_F(X86_64_SSEMisc, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_64_SSEMisc, NoConstantTrueBranch) {
    verifyNoConstantTrueBranch(testObj());
}

TEST_F(X86_64_SSEMisc, DecompileSucceeds) {
    verifyDecompileProducesOutput(testObj());
}

TEST_F(X86_64_SSEMisc, AllModesSucceed) {
    verifyAllModesSucceed(testObj());
}
