#include "NeverDLiftFixture.h"

class X86_32_Stack : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_stack32.o";
}

TEST_F(X86_32_Stack, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_stack32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_Stack, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_Stack, PushPopLifts) {
    verifyLowIRContains(testObj(), "test_push_pop", "STORE");
}

TEST_F(X86_32_Stack, Xchg32Lifts) {
    verifyLowIRContains(testObj(), "test_xchg32", "COPY");
}

TEST_F(X86_32_Stack, Bswap32Lifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("Bswap") != std::string::npos ||
                r.out.find("INTRINSIC") != std::string::npos)
        << "Expected BSWAP intrinsic in LowIR";
}

TEST_F(X86_32_Stack, Cmov32Lifts) {
    verifyLowIRContains(testObj(), "test_cmov32", "INT_SUB");
}

TEST_F(X86_32_Stack, Setcc32Lifts) {
    verifyLowIRContains(testObj(), "test_setcc32", "INT_SUB");
}

TEST_F(X86_32_Stack, Adc32Lifts) {
    verifyLowIRContains(testObj(), "test_adc32", "INT_ADD");
}

TEST_F(X86_32_Stack, Sbb32Lifts) {
    verifyLowIRContains(testObj(), "test_sbb32", "INT_SUB");
}

TEST_F(X86_32_Stack, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
