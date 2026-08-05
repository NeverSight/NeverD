#include "NeverDLiftFixture.h"

class X86_64_StackMov : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_stack_mov.o";
}

TEST_F(X86_64_StackMov, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_stack_mov.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_StackMov, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_StackMov, PushPopPreservesStoreLoad) {
    auto r = liftToLowIR(testObj(), "test_push_pop");
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("STORE") != std::string::npos)
        << "PUSH should produce STORE";
    EXPECT_TRUE(r.out.find("LOAD") != std::string::npos)
        << "POP should produce LOAD";
}

TEST_F(X86_64_StackMov, MovImmPreservesCopy) {
    verifyLowIRContains(testObj(), "test_mov_imm", "COPY");
}

TEST_F(X86_64_StackMov, AdcHasCarry) {
    auto r = liftToLowIR(testObj(), "test_adc");
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("INT_ADD") != std::string::npos)
        << "ADC should have INT_ADD";
}

TEST_F(X86_64_StackMov, SbbHasBorrow) {
    auto r = liftToLowIR(testObj(), "test_sbb");
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("INT_SUB") != std::string::npos)
        << "SBB should have INT_SUB";
}

TEST_F(X86_64_StackMov, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR — semantic loss:\n" << r.out;
}
