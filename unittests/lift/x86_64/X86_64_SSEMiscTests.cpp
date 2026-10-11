#include "X86FPStateFixture.h"

class X86_64_SSEMisc : public X86FPStateLiftTest {};

static fs::path testObj() { return fs::path(TEST_OBJ_DIR) / "test_sse_misc.o"; }

TEST_F(X86_64_SSEMisc, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_sse_misc.o not built";
  verifyAllStages(testObj());
}

TEST_F(X86_64_SSEMisc, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(X86_64_SSEMisc, NoUnreachable) { verifyLLVMIRNoUnreachable(testObj()); }

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
  verifyScalarFPState(testObj(), "test_addps",
                      neverd::Intrinsic::X86FPArithMemoryState, 0);
}

TEST_F(X86_64_SSEMisc, SubpsPreservesSemantics) {
  verifyScalarFPState(testObj(), "test_subps",
                      neverd::Intrinsic::X86FPArithMemoryState, 1);
}

TEST_F(X86_64_SSEMisc, MulpsPreservesSemantics) {
  verifyScalarFPState(testObj(), "test_mulps",
                      neverd::Intrinsic::X86FPArithMemoryState, 2);
}

TEST_F(X86_64_SSEMisc, DivpsPreservesSemantics) {
  verifyScalarFPState(testObj(), "test_divps",
                      neverd::Intrinsic::X86FPArithMemoryState, 3);
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

TEST_F(X86_64_SSEMisc, AllModesSucceed) { verifyAllModesSucceed(testObj()); }
