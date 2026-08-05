#include "NeverDLiftFixture.h"

class X86_64_X87FPU : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_x87_fpu.o";
}

TEST_F(X86_64_X87FPU, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_x87_fpu.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_X87FPU, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_X87FPU, NoUnreachable) {
    verifyLLVMIRNoUnreachable(testObj());
}

TEST_F(X86_64_X87FPU, FaddPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fadd", "FLOAT_ADD");
}

TEST_F(X86_64_X87FPU, FsubPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fsub", "FLOAT_SUB");
}

TEST_F(X86_64_X87FPU, FmulPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fmul", "FLOAT_MULT");
}

TEST_F(X86_64_X87FPU, FdivPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fdiv", "FLOAT_DIV");
}

TEST_F(X86_64_X87FPU, FabsPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fabs", "FLOAT_ABS");
}

TEST_F(X86_64_X87FPU, FchsPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fchs", "FLOAT_NEG");
}

TEST_F(X86_64_X87FPU, FsqrtPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fsqrt", "FLOAT_SQRT");
}

TEST_F(X86_64_X87FPU, FildFistpPreservesSemantics) {
    auto r = liftToLowIR(testObj());
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.contains("FLOAT_INT2FLOAT") || r.contains("FLOAT_FLOAT2INT"))
        << "FILD/FISTP should produce float conversion ops";
}

TEST_F(X86_64_X87FPU, FxchPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fxch", "COPY");
}

TEST_F(X86_64_X87FPU, Fld1FldZPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fld1_fldz", "COPY");
}

TEST_F(X86_64_X87FPU, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_64_X87FPU, NoConstantTrueBranch) {
    verifyNoConstantTrueBranch(testObj());
}

TEST_F(X86_64_X87FPU, DecompileSucceeds) {
    verifyDecompileProducesOutput(testObj());
}

TEST_F(X86_64_X87FPU, AllModesSucceed) {
    verifyAllModesSucceed(testObj());
}
