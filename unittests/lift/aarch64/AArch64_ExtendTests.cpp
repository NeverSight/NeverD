#include "NeverDLiftFixture.h"

class AArch64_Extend : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_extend_a64.o";
}

TEST_F(AArch64_Extend, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_extend_a64.o not built";
    verifyAllStages(testObj());
}

TEST_F(AArch64_Extend, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(AArch64_Extend, SxtbPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_sxtb_a64", "INT_SEXT");
}

TEST_F(AArch64_Extend, SxthPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_sxth_a64", "INT_SEXT");
}

TEST_F(AArch64_Extend, SxtwPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_sxtw_a64", "INT_SEXT");
}

TEST_F(AArch64_Extend, UxtbPreservesSemantics) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    bool has_ext = r.out.find("INT_ZEXT") != std::string::npos ||
                   r.out.find("INT_AND") != std::string::npos;
    EXPECT_TRUE(has_ext) << "Expected zero extension for UXTB";
}

TEST_F(AArch64_Extend, RevLifted) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
}

TEST_F(AArch64_Extend, ClzLifted) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    bool has_clz = r.out.find("INTRINSIC") != std::string::npos ||
                   r.out.find("LZCOUNT") != std::string::npos;
    EXPECT_TRUE(has_clz) << "Expected CLZ handling";
}

TEST_F(AArch64_Extend, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}
