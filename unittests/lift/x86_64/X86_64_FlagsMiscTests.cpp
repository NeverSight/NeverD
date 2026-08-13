#include "NeverDLiftFixture.h"

class X86_64_FlagsMisc : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_flags_misc.o";
}

TEST_F(X86_64_FlagsMisc, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_flags_misc.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_FlagsMisc, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_FlagsMisc, LahfLifts) {
    verifyLowIRContains(testObj(), "test_lahf", "INT_OR");
}

TEST_F(X86_64_FlagsMisc, SahfLifts) {
    verifyLowIRContains(testObj(), "test_sahf_then_jcc", "INT_AND");
}

TEST_F(X86_64_FlagsMisc, ClcAdcLifts) {
    verifyLowIRContains(testObj(), "test_clc_adc", "INT_ADD");
}

TEST_F(X86_64_FlagsMisc, StcAdcLifts) {
    verifyLowIRContains(testObj(), "test_stc_adc", "INT_ADD");
}

TEST_F(X86_64_FlagsMisc, CmcLifts) {
    verifyLowIRContains(testObj(), "test_cmc", "BOOL_NOT");
}

TEST_F(X86_64_FlagsMisc, BtLifts) {
    verifyLowIRContains(testObj(), "test_bt", "INT_RIGHT");
}

TEST_F(X86_64_FlagsMisc, BtsLifts) {
    verifyLowIRContains(testObj(), "test_bts", "INT_OR");
}

TEST_F(X86_64_FlagsMisc, BtrLifts) {
    verifyLowIRContains(testObj(), "test_btr", "INT_AND");
}

TEST_F(X86_64_FlagsMisc, BtcLifts) {
    verifyLowIRContains(testObj(), "test_btc", "INT_XOR");
}

TEST_F(X86_64_FlagsMisc, TzcntLifts) {
    verifyLowIRContains(testObj(), "test_tzcnt", "POPCOUNT");
}

TEST_F(X86_64_FlagsMisc, LzcntLifts) {
    verifyLowIRContains(testObj(), "test_lzcnt", "LZCOUNT");
}

TEST_F(X86_64_FlagsMisc, PopcntLifts) {
    verifyLowIRContains(testObj(), "test_popcnt", "POPCOUNT");
}

TEST_F(X86_64_FlagsMisc, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
