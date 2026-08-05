#include "NeverDLiftFixture.h"

class X86_32_Flags : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_flags32.o";
}

TEST_F(X86_32_Flags, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_flags32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_Flags, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_Flags, ClcAdc32Lifts) {
    verifyLowIRContains(testObj(), "test_clc_adc32", "INT_ADD");
}

TEST_F(X86_32_Flags, StcAdc32Lifts) {
    verifyLowIRContains(testObj(), "test_stc_adc32", "INT_ADD");
}

TEST_F(X86_32_Flags, Cmc32Lifts) {
    verifyLowIRContains(testObj(), "test_cmc32", "BOOL_NOT");
}

TEST_F(X86_32_Flags, Bt32Lifts) {
    verifyLowIRContains(testObj(), "test_bt32", "INT_RIGHT");
}

TEST_F(X86_32_Flags, Bts32Lifts) {
    verifyLowIRContains(testObj(), "test_bts32", "INT_OR");
}

TEST_F(X86_32_Flags, Btr32Lifts) {
    verifyLowIRContains(testObj(), "test_btr32", "INT_AND");
}

TEST_F(X86_32_Flags, Btc32Lifts) {
    verifyLowIRContains(testObj(), "test_btc32", "INT_XOR");
}

TEST_F(X86_32_Flags, Rol32Lifts) {
    verifyLowIRContains(testObj(), "test_rol32", "INT_LEFT");
}

TEST_F(X86_32_Flags, Ror32Lifts) {
    verifyLowIRContains(testObj(), "test_ror32", "INT_RIGHT");
}

TEST_F(X86_32_Flags, Shld32Lifts) {
    verifyLowIRContains(testObj(), "test_shld32", "INT_LEFT");
}

TEST_F(X86_32_Flags, Shrd32Lifts) {
    verifyLowIRContains(testObj(), "test_shrd32", "INT_RIGHT");
}

TEST_F(X86_32_Flags, Lahf32Lifts) {
    verifyLowIRContains(testObj(), "test_lahf32", "INT_OR");
}

TEST_F(X86_32_Flags, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
