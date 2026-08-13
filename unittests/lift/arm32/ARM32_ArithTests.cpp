#include "NeverDLiftFixture.h"

class ARM32_Arith : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_arith_arm.o";
}

TEST_F(ARM32_Arith, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_arith_arm.o not built";
    verifyAllStages(testObj());
}

TEST_F(ARM32_Arith, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(ARM32_Arith, AddLifts) {
    verifyLowIRContains(testObj(), "test_add_arm", "INT_ADD");
}

TEST_F(ARM32_Arith, SubLifts) {
    verifyLowIRContains(testObj(), "test_sub_arm", "INT_SUB");
}

TEST_F(ARM32_Arith, AndLifts) {
    verifyLowIRContains(testObj(), "test_and_arm", "INT_AND");
}

TEST_F(ARM32_Arith, OrrLifts) {
    verifyLowIRContains(testObj(), "test_orr_arm", "INT_OR");
}

TEST_F(ARM32_Arith, EorLifts) {
    verifyLowIRContains(testObj(), "test_eor_arm", "INT_XOR");
}

TEST_F(ARM32_Arith, MvnLifts) {
    verifyLowIRContains(testObj(), "test_mvn_arm", "INT_NOT");
}

TEST_F(ARM32_Arith, LslLifts) {
    verifyLowIRContains(testObj(), "test_lsl_arm", "INT_LEFT");
}

TEST_F(ARM32_Arith, LsrLifts) {
    verifyLowIRContains(testObj(), "test_lsr_arm", "INT_RIGHT");
}

TEST_F(ARM32_Arith, AsrLifts) {
    verifyLowIRContains(testObj(), "test_asr_arm", "INT_ASHR");
}

TEST_F(ARM32_Arith, MulLifts) {
    verifyLowIRContains(testObj(), "test_mul_arm", "INT_MULT");
}

TEST_F(ARM32_Arith, MlaLifts) {
    verifyLowIRContains(testObj(), "test_mla_arm", "INT_ADD");
}

TEST_F(ARM32_Arith, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
