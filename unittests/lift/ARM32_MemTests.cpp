#include "NeverDLiftFixture.h"

class ARM32_Mem : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_mem_arm.o";
}

TEST_F(ARM32_Mem, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_mem_arm.o not built";
    verifyAllStages(testObj());
}

TEST_F(ARM32_Mem, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(ARM32_Mem, LdrLifts) {
    verifyLowIRContains(testObj(), "test_ldr_str", "LOAD");
}

TEST_F(ARM32_Mem, StrLifts) {
    verifyLowIRContains(testObj(), "test_str", "STORE");
}

TEST_F(ARM32_Mem, LdrhLifts) {
    verifyLowIRContains(testObj(), "test_ldrh", "LOAD");
}

TEST_F(ARM32_Mem, LdrbLifts) {
    verifyLowIRContains(testObj(), "test_ldrb", "LOAD");
}

TEST_F(ARM32_Mem, PushPopLifts) {
    verifyLowIRContains(testObj(), "test_push_pop", "STORE");
}

TEST_F(ARM32_Mem, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
