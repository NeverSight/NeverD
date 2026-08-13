#include "NeverDLiftFixture.h"

class X86_64_SetccCmov : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_setcc_cmov.o";
}

TEST_F(X86_64_SetccCmov, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_setcc_cmov.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_SetccCmov, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_SetccCmov, SeteHasCopy) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_FALSE(r.out.empty());
}

TEST_F(X86_64_SetccCmov, SetneHasBoolNot) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_FALSE(r.out.empty());
}

TEST_F(X86_64_SetccCmov, CmoveHasConditional) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("COND_BR") != std::string::npos ||
                r.out.find("COPY") != std::string::npos)
        << "Expected conditional logic in LLVM IR";
}

TEST_F(X86_64_SetccCmov, CmovlPreservesSemantics) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_FALSE(r.out.empty());
}

TEST_F(X86_64_SetccCmov, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}

TEST_F(X86_64_SetccCmov, LLVMIRHasSelect) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_FALSE(r.out.empty()) << "LLVM IR output is empty";
}
