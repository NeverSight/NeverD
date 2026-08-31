#include "NeverDLiftFixture.h"

class X86_64_MulDiv : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_muldiv.o";
}

TEST_F(X86_64_MulDiv, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_muldiv.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_MulDiv, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_MulDiv, Imul2OpHasMult) {
    verifyLowIRContains(testObj(), "test_imul_2op", "INT_MULT");
}

TEST_F(X86_64_MulDiv, Imul3OpHasMult) {
    verifyLowIRContains(testObj(), "test_imul_3op", "INT_MULT");
}

TEST_F(X86_64_MulDiv, Mul1OpHasZextAndMult) {
    verifyLowIRContains(testObj(), "test_mul_1op", "INT_ZEXT");
    verifyLowIRContains(testObj(), "test_mul_1op", "INT_MULT");
}

TEST_F(X86_64_MulDiv, Imul1OpHasSextAndMult) {
    verifyLowIRContains(testObj(), "test_imul_1op", "INT_SEXT");
    verifyLowIRContains(testObj(), "test_imul_1op", "INT_MULT");
}

TEST_F(X86_64_MulDiv, DivHasDiv) {
    verifyLowIRContains(testObj(), "test_div", "INT_DIV");
}

TEST_F(X86_64_MulDiv, DivRemainderHasRem) {
    verifyLowIRContains(testObj(), "test_div_remainder", "INT_REM");
}

TEST_F(X86_64_MulDiv, IdivHasSdiv) {
    verifyLowIRContains(testObj(), "test_idiv", "INT_SDIV");
}

TEST_F(X86_64_MulDiv, IdivRemainderHasSrem) {
    verifyLowIRContains(testObj(), "test_idiv_remainder", "INT_SREM");
}

TEST_F(X86_64_MulDiv, CdqHasSright) {
    verifyLowIRContains(testObj(), "test_cdq", "INT_ASHR");
}

TEST_F(X86_64_MulDiv, CdqeHasSext) {
    verifyLowIRContains(testObj(), "test_cdqe", "INT_SEXT");
}

TEST_F(X86_64_MulDiv, CqoHasSright) {
    verifyLowIRContains(testObj(), "test_cqo_hi", "INT_ASHR");
}

TEST_F(X86_64_MulDiv, Mul1OpWritesRdx) {
    auto r = liftToLowIR(testObj(), "test_mul_1op");
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("SUBBYTES") != std::string::npos)
        << "MUL 1-op should use SUBBYTES to split hi:lo result";
}

TEST_F(X86_64_MulDiv, NoUnreachableInFunctions) {
    verifyLLVMIRNoUnreachable(testObj());
}

TEST_F(X86_64_MulDiv, DivPreservesInLLVMIR) {
    verifyLLVMIRContains(testObj(), "test_div", "udiv ");
}

TEST_F(X86_64_MulDiv, IdivPreservesInLLVMIR) {
    verifyLLVMIRContains(testObj(), "test_idiv", "sdiv ");
}
