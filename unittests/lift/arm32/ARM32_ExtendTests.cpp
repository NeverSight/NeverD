#include "NeverDLiftFixture.h"

class ARM32_Extend : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_extend_arm.o";
}

TEST_F(ARM32_Extend, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_extend_arm.o not built";
    verifyAllStages(testObj());
}

TEST_F(ARM32_Extend, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(ARM32_Extend, SxtbPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_sxtb_arm", "INT_SEXT");
}

TEST_F(ARM32_Extend, SxthPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_sxth_arm", "INT_SEXT");
}

TEST_F(ARM32_Extend, UxtbPreservesSemantics) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    bool has_ext = r.out.find("INT_ZEXT") != std::string::npos ||
                   r.out.find("INT_AND") != std::string::npos;
    EXPECT_TRUE(has_ext) << "Expected zero extension for UXTB";
}

TEST_F(ARM32_Extend, RevLifted) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
}

TEST_F(ARM32_Extend, ClzLifted) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    bool has_clz = r.out.find("INTRINSIC") != std::string::npos ||
                   r.out.find("LZCOUNT") != std::string::npos;
    EXPECT_TRUE(has_clz) << "Expected CLZ handling";
}

TEST_F(ARM32_Extend, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}
