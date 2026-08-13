#include "NeverDLiftFixture.h"

class X86_32_Rotate : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_rotate32.o";
}

TEST_F(X86_32_Rotate, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_rotate32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_Rotate, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_Rotate, RolLifts) {
    verifyLowIRContains(testObj(), "test_rol32", "INT_LEFT");
}

TEST_F(X86_32_Rotate, RorLifts) {
    verifyLowIRContains(testObj(), "test_ror32", "INT_RIGHT");
}

TEST_F(X86_32_Rotate, RclLifts) {
    verifyLowIRContains(testObj(), "test_rcl32", "INT_LEFT");
}

TEST_F(X86_32_Rotate, RcrLifts) {
    verifyLowIRContains(testObj(), "test_rcr32", "INT_RIGHT");
}

TEST_F(X86_32_Rotate, ShldLifts) {
    verifyLowIRContains(testObj(), "test_shld32", "INT_LEFT");
}

TEST_F(X86_32_Rotate, ShrdLifts) {
    verifyLowIRContains(testObj(), "test_shrd32", "INT_RIGHT");
}

TEST_F(X86_32_Rotate, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
