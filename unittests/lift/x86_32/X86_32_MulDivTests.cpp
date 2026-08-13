#include "NeverDLiftFixture.h"

class X86_32_MulDiv : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_muldiv32.o";
}

TEST_F(X86_32_MulDiv, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_muldiv32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_MulDiv, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_MulDiv, Imul2OpLifts) {
    verifyLowIRContains(testObj(), "test_imul32_2op", "INT_MULT");
}

TEST_F(X86_32_MulDiv, Imul3OpLifts) {
    verifyLowIRContains(testObj(), "test_imul32_3op", "INT_MULT");
}

TEST_F(X86_32_MulDiv, MulLifts) {
    verifyLowIRContains(testObj(), "test_mul32", "INT_MULT");
}

TEST_F(X86_32_MulDiv, DivLifts) {
    verifyLowIRContains(testObj(), "test_div32", "INT_DIV");
}

TEST_F(X86_32_MulDiv, IdivLifts) {
    verifyLowIRContains(testObj(), "test_idiv32", "INT_SDIV");
}

TEST_F(X86_32_MulDiv, ModLifts) {
    verifyLowIRContains(testObj(), "test_mod32", "INT_REM");
}

TEST_F(X86_32_MulDiv, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
