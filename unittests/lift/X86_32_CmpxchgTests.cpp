#include "NeverDLiftFixture.h"

class X86_32_Cmpxchg : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_cmpxchg32.o";
}

TEST_F(X86_32_Cmpxchg, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_cmpxchg32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_Cmpxchg, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_Cmpxchg, XchgPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_xchg32", "COPY");
}

TEST_F(X86_32_Cmpxchg, BswapLifted) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0) << "LowIR failed: " << r.err;
    bool has_bswap = r.out.find("INTRINSIC") != std::string::npos ||
                     r.out.find("INT_LEFT") != std::string::npos;
    EXPECT_TRUE(has_bswap) << "Expected BSWAP handling";
}

TEST_F(X86_32_Cmpxchg, CmpxchgLifted) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    bool has_ops = r.out.find("INT_EQUAL") != std::string::npos;
    EXPECT_TRUE(has_ops) << "Expected comparison in CMPXCHG";
}

TEST_F(X86_32_Cmpxchg, XaddLifted) {
    verifyLowIRContains(testObj(), "test_xadd32", "INT_ADD");
}

TEST_F(X86_32_Cmpxchg, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}
