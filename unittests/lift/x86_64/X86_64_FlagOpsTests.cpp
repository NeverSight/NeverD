#include "NeverDLiftFixture.h"

class X86_64_FlagOps : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_flag_ops.o";
}

TEST_F(X86_64_FlagOps, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_flag_ops.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_FlagOps, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_FlagOps, StcClcLifts) {
    verifyLowIRContains(testObj(), "test_stc_clc", "COPY");
}

TEST_F(X86_64_FlagOps, TestAndJzLifts) {
    verifyLowIRContains(testObj(), "test_test_and_jz", "INT_AND");
}

TEST_F(X86_64_FlagOps, CmpFlagsLifts) {
    verifyLowIRContains(testObj(), "test_cmp_flags", "INT_SUB");
}

TEST_F(X86_64_FlagOps, SetaLifts) {
    verifyLowIRContains(testObj(), "test_seta", "INT_SUB");
}

TEST_F(X86_64_FlagOps, SetgeLifts) {
    verifyLowIRContains(testObj(), "test_setge", "INT_SUB");
}

TEST_F(X86_64_FlagOps, CmovaLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("INT_SUB") != std::string::npos)
        << "Expected CMP-related op in LowIR for cmov";
}

TEST_F(X86_64_FlagOps, CmovlLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("INT_SUB") != std::string::npos)
        << "Expected CMP-related op in LowIR for cmov";
}

TEST_F(X86_64_FlagOps, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
