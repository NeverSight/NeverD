#include "NeverDLiftFixture.h"

class X86_32_Setcc : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_setcc32.o";
}

TEST_F(X86_32_Setcc, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_setcc32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_Setcc, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_Setcc, SetePreservesSemantics) {
    verifyLowIRContains(testObj(), "test_sete32", "INT_EQUAL");
}

TEST_F(X86_32_Setcc, SetlPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_setl32", "INT_SLESS");
}

TEST_F(X86_32_Setcc, SetgUsesFlags) {
    verifyLowIRContains(testObj(), "test_setg32", "BOOL_NOT");
}

TEST_F(X86_32_Setcc, SetbUsesCarryFlag) {
    verifyLowIRContains(testObj(), "test_setb32", "INT_LESS");
}

TEST_F(X86_32_Setcc, SetsUsesSignFlag) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    bool has_sf = r.out.find("INT_SLESS") != std::string::npos;
    EXPECT_TRUE(has_sf) << "Expected SF computation for SETS";
}

TEST_F(X86_32_Setcc, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_32_Setcc, LLVMIRHasConditionalLogic) {
    verifyLLVMIRHasConditionalLogic(testObj());
}
