#include "NeverDLiftFixture.h"

class X86_64_BMI : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_bmi.o";
}

TEST_F(X86_64_BMI, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_bmi.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_BMI, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_BMI, AndnLifts) {
    verifyLowIRContains(testObj(), "test_andn", "INT_NOT");
}

TEST_F(X86_64_BMI, BlsiLifts) {
    verifyLowIRContains(testObj(), "test_blsi", "INT_AND");
}

TEST_F(X86_64_BMI, BlsmskLifts) {
    verifyLowIRContains(testObj(), "test_blsmsk", "INT_SUB");
}

TEST_F(X86_64_BMI, BlsrLifts) {
    verifyLowIRContains(testObj(), "test_blsr", "INT_AND");
}

TEST_F(X86_64_BMI, TzcntLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    bool has_op = r.out.find("Tzcnt") != std::string::npos ||
                  r.out.find("INTRINSIC") != std::string::npos ||
                  r.out.find("INT_RIGHT") != std::string::npos;
    EXPECT_TRUE(has_op) << "Expected tzcnt operation in LowIR";
}

TEST_F(X86_64_BMI, LzcntLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    bool has_op = r.out.find("Lzcnt") != std::string::npos ||
                  r.out.find("INTRINSIC") != std::string::npos;
    EXPECT_TRUE(has_op) << "Expected lzcnt operation in LowIR";
}

TEST_F(X86_64_BMI, PopcntLifts) {
    verifyLowIRContains(testObj(), "test_popcnt", "POPCOUNT");
}

TEST_F(X86_64_BMI, ShrxLifts) {
    verifyLowIRContains(testObj(), "test_shrx", "INT_RIGHT");
}

TEST_F(X86_64_BMI, ShlxLifts) {
    verifyLowIRContains(testObj(), "test_shlx", "INT_LEFT");
}

TEST_F(X86_64_BMI, SarxLifts) {
    verifyLowIRContains(testObj(), "test_sarx", "INT_ASHR");
}

TEST_F(X86_64_BMI, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
