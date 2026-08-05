#include "NeverDLiftFixture.h"

class X86_64_SignExt : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_signext.o";
}

TEST_F(X86_64_SignExt, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_signext.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_SignExt, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_SignExt, CbwHasIntSext) {
    verifyLowIRContains(testObj(), "test_cbw", "INT_SEXT");
}

TEST_F(X86_64_SignExt, CwdeHasIntSext) {
    verifyLowIRContains(testObj(), "test_cwde", "INT_SEXT");
}

TEST_F(X86_64_SignExt, CdqeHasIntSext) {
    verifyLowIRContains(testObj(), "test_cdqe", "INT_SEXT");
}

TEST_F(X86_64_SignExt, CdqHasSright) {
    verifyLowIRContains(testObj(), "test_cdq", "INT_ASHR");
}

TEST_F(X86_64_SignExt, CqoHasSright) {
    verifyLowIRContains(testObj(), "test_cqo", "INT_ASHR");
}

TEST_F(X86_64_SignExt, MovsxdHasIntSext) {
    verifyLowIRContains(testObj(), "test_movsxd", "INT_SEXT");
}

TEST_F(X86_64_SignExt, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}
