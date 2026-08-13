#include "NeverDLiftFixture.h"

class AArch64_BitShift : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_bitshift_a64.o";
}

TEST_F(AArch64_BitShift, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_bitshift_a64.o not built";
    verifyAllStages(testObj());
}

TEST_F(AArch64_BitShift, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(AArch64_BitShift, ClzLifts) {
    verifyLowIRContains(testObj(), "test_clz_a64", "LZCOUNT");
}

TEST_F(AArch64_BitShift, RevLifts) {
    verifyLowIRContains(testObj(), "test_rev_a64", "INT_AND");
}

TEST_F(AArch64_BitShift, RorLifts) {
    verifyLowIRContains(testObj(), "test_ror_a64", "INT_RIGHT");
}

TEST_F(AArch64_BitShift, UbfxLifts) {
    verifyLowIRContains(testObj(), "test_bfm_a64", "INT_RIGHT");
}

TEST_F(AArch64_BitShift, SbfxLifts) {
    verifyLowIRContains(testObj(), "test_sbfx_a64", "INT_ASHR");
}

TEST_F(AArch64_BitShift, TstLifts) {
    verifyLowIRContains(testObj(), "test_tst_a64", "INT_AND");
}

TEST_F(AArch64_BitShift, CmnLifts) {
    verifyLowIRContains(testObj(), "test_cmn_a64", "INT_ADD");
}

TEST_F(AArch64_BitShift, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
