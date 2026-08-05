#include "NeverDLiftFixture.h"

class X86_32_X87FPU : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_x87_fpu32.o";
}

TEST_F(X86_32_X87FPU, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_x87_fpu32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_X87FPU, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_X87FPU, NoUnreachable) {
    verifyLLVMIRNoUnreachable(testObj());
}

TEST_F(X86_32_X87FPU, FaddPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fadd32", "FLOAT_ADD");
}

TEST_F(X86_32_X87FPU, FsubPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fsub32", "FLOAT_SUB");
}

TEST_F(X86_32_X87FPU, FmulPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fmul32", "FLOAT_MULT");
}

TEST_F(X86_32_X87FPU, FdivPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fdiv32", "FLOAT_DIV");
}

TEST_F(X86_32_X87FPU, FabsPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fabs32", "FLOAT_ABS");
}

TEST_F(X86_32_X87FPU, FchsPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fchs32", "FLOAT_NEG");
}

TEST_F(X86_32_X87FPU, FildFistpPreservesSemantics) {
    auto r = liftToLowIR(testObj());
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.contains("FLOAT_INT2FLOAT") || r.contains("FLOAT_FLOAT2INT"))
        << "FILD/FISTP should produce float conversion ops";
}

TEST_F(X86_32_X87FPU, DecompileSucceeds) {
    verifyDecompileProducesOutput(testObj());
}

TEST_F(X86_32_X87FPU, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_32_X87FPU, NoConstantTrueBranch) {
    verifyNoConstantTrueBranch(testObj());
}

TEST_F(X86_32_X87FPU, AllModesSucceed) {
    verifyAllModesSucceed(testObj());
}
