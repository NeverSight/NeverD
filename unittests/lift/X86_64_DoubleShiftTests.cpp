#include "NeverDLiftFixture.h"

class X86_64_DoubleShift : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_double_shift.o";
}

TEST_F(X86_64_DoubleShift, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_double_shift.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_DoubleShift, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_DoubleShift, ShldLifts) {
    verifyLowIRContains(testObj(), "test_shld", "INT_LEFT");
}

TEST_F(X86_64_DoubleShift, ShrdLifts) {
    verifyLowIRContains(testObj(), "test_shrd", "INT_RIGHT");
}

TEST_F(X86_64_DoubleShift, Shld32Lifts) {
    verifyLowIRContains(testObj(), "test_shld32", "INT_LEFT");
}

TEST_F(X86_64_DoubleShift, Shrd32Lifts) {
    verifyLowIRContains(testObj(), "test_shrd32", "INT_RIGHT");
}

TEST_F(X86_64_DoubleShift, RclLifts) {
    verifyLowIRContains(testObj(), "test_rcl", "INT_LEFT");
}

TEST_F(X86_64_DoubleShift, RcrLifts) {
    verifyLowIRContains(testObj(), "test_rcr", "INT_RIGHT");
}

TEST_F(X86_64_DoubleShift, Bswap64Lifts) {
    verifyLowIRContains(testObj(), "test_bswap64", "INT_AND");
}

TEST_F(X86_64_DoubleShift, Bswap32Lifts) {
    verifyLowIRContains(testObj(), "test_bswap32", "INT_AND");
}

TEST_F(X86_64_DoubleShift, EnterLeaveLifts) {
    verifyLowIRContains(testObj(), "test_enter_leave", "STORE");
}

TEST_F(X86_64_DoubleShift, LLVMIRHasShld) {
    verifyLLVMIRContains(testObj(), "test_shld", "fshl");
}

TEST_F(X86_64_DoubleShift, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
