#include "NeverDLiftFixture.h"

class AArch64_Cond : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_cond_a64.o";
}

TEST_F(AArch64_Cond, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_cond_a64.o not built";
    verifyAllStages(testObj());
}

TEST_F(AArch64_Cond, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(AArch64_Cond, CselHasSelectOrCbranch) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("SELECT") != std::string::npos ||
                r.out.find("COND_BR") != std::string::npos)
        << "CSEL should produce SELECT or COND_BR";
}

TEST_F(AArch64_Cond, CsetLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_FALSE(r.out.empty());
}

TEST_F(AArch64_Cond, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}
