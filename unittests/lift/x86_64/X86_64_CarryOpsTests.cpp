#include "NeverDLiftFixture.h"

class X86_64_CarryOps : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_carry_ops.o";
}

TEST_F(X86_64_CarryOps, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_carry_ops.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_CarryOps, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_CarryOps, AdcHasIntCarry) {
    verifyLowIRContains(testObj(), "test_adc", "INT_CARRY");
}

TEST_F(X86_64_CarryOps, SbbHasSBorrow) {
    verifyLowIRContains(testObj(), "test_sbb", "INT_SBOR");
}

TEST_F(X86_64_CarryOps, AdcHasIntAdd) {
    verifyLowIRContains(testObj(), "test_adc", "INT_ADD");
}

TEST_F(X86_64_CarryOps, SbbHasIntSub) {
    verifyLowIRContains(testObj(), "test_sbb", "INT_SUB");
}

TEST_F(X86_64_CarryOps, Adc64HasIntAdd) {
    verifyLowIRContains(testObj(), "test_adc64", "INT_ADD");
}

TEST_F(X86_64_CarryOps, Sbb64HasIntSub) {
    verifyLowIRContains(testObj(), "test_sbb64", "INT_SUB");
}

TEST_F(X86_64_CarryOps, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}
