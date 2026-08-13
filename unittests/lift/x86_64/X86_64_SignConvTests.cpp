#include "NeverDLiftFixture.h"

class X86_64_SignConv : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_sign_conv.o";
}

TEST_F(X86_64_SignConv, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_sign_conv.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_SignConv, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_SignConv, CdqeSignExtends) {
    verifyLowIRContains(testObj(), "test_cdqe", "INT_SEXT");
}

TEST_F(X86_64_SignConv, CdqPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_cdq", "INT_ASHR");
}

TEST_F(X86_64_SignConv, CqoPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_cqo", "INT_ASHR");
}

TEST_F(X86_64_SignConv, Bswap64Lifted) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    bool has_bswap = r.out.find("INTRINSIC") != std::string::npos;
    EXPECT_TRUE(has_bswap) << "Expected BSWAP intrinsic in INTRINSIC form";
}

TEST_F(X86_64_SignConv, Bswap32Lifted) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
}

TEST_F(X86_64_SignConv, XchgPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_xchg", "COPY");
}

TEST_F(X86_64_SignConv, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_64_SignConv, AllModesSucceed) {
    verifyAllModesSucceed(testObj());
}
