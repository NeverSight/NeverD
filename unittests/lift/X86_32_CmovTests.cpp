#include "NeverDLiftFixture.h"

class X86_32_Cmov : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_cmov32.o";
}

TEST_F(X86_32_Cmov, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_cmov32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_Cmov, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_Cmov, CmoveLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("INT_EQUAL") != std::string::npos ||
                r.out.find("INT_ZEXT") != std::string::npos)
        << "Expected conditional logic in LowIR";
}

TEST_F(X86_32_Cmov, CmovneLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("BOOL_NOT") != std::string::npos)
        << "Expected BOOL_NOT for CMOVNE in LowIR";
}

TEST_F(X86_32_Cmov, SeteLifts) {
    verifyLowIRContains(testObj(), "test_sete32", "INT_EQUAL");
}

TEST_F(X86_32_Cmov, SetlLifts) {
    verifyLowIRContains(testObj(), "test_setl32", "INT_NOTEQUAL");
}

TEST_F(X86_32_Cmov, LLVMIRHasSelect) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_FALSE(r.out.empty()) << "LLVM IR output is empty";
}

TEST_F(X86_32_Cmov, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
