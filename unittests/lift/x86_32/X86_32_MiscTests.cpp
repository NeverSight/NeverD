#include "NeverDLiftFixture.h"

class X86_32_Misc : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_misc32.o";
}

TEST_F(X86_32_Misc, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_misc32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_Misc, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_Misc, Bswap32Lifts) {
    verifyLowIRContains(testObj(), "test_bswap32_misc", "INT_AND");
}

TEST_F(X86_32_Misc, EnterLeave32Lifts) {
    verifyLowIRContains(testObj(), "test_enter_leave32", "STORE");
}

TEST_F(X86_32_Misc, Cdq32Lifts) {
    verifyLowIRContains(testObj(), "test_cdq32", "INT_ASHR");
}

TEST_F(X86_32_Misc, Cwd32Lifts) {
    verifyLowIRContains(testObj(), "test_cwd32", "INT_ASHR");
}

TEST_F(X86_32_Misc, Cbw32Lifts) {
    verifyLowIRContains(testObj(), "test_cbw32", "INT_SEXT");
}

TEST_F(X86_32_Misc, Cwde32Lifts) {
    verifyLowIRContains(testObj(), "test_cwde32", "INT_SEXT");
}

TEST_F(X86_32_Misc, Xchg32Lifts) {
    verifyLowIRContains(testObj(), "test_xchg32", "COPY");
}

TEST_F(X86_32_Misc, Bsf32Lifts) {
    verifyLowIRContains(testObj(), "test_bsf32", "LZCOUNT");
}

TEST_F(X86_32_Misc, Bsr32Lifts) {
    verifyLowIRContains(testObj(), "test_bsr32", "LZCOUNT");
}

TEST_F(X86_32_Misc, Cmpxchg32Lifts) {
    verifyLowIRContains(testObj(), "test_cmpxchg32", "INT_EQUAL");
}

TEST_F(X86_32_Misc, Xadd32Lifts) {
    verifyLowIRContains(testObj(), "test_xadd32", "INT_ADD");
}

TEST_F(X86_32_Misc, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
