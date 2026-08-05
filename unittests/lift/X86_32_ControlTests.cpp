#include "NeverDLiftFixture.h"

class X86_32_Control : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_control32.o";
}

TEST_F(X86_32_Control, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_control32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_Control, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_Control, Je32HasCbranch) {
    verifyLowIRContains(testObj(), "test_je32", "COND_BR");
}

TEST_F(X86_32_Control, Jl32HasCbranch) {
    verifyLowIRContains(testObj(), "test_jl32", "COND_BR");
}

TEST_F(X86_32_Control, Jb32HasCbranch) {
    verifyLowIRContains(testObj(), "test_jb32", "COND_BR");
}

TEST_F(X86_32_Control, Sete32HasCopy) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_FALSE(r.out.empty());
}

TEST_F(X86_32_Control, Cmove32HasConditional) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("COND_BR") != std::string::npos ||
                r.out.find("COPY") != std::string::npos);
}

TEST_F(X86_32_Control, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}
