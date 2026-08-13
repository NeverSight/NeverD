#include "NeverDLiftFixture.h"

class X86_64_Cmpxchg : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_cmpxchg_xadd.o";
}

TEST_F(X86_64_Cmpxchg, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_cmpxchg_xadd.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_Cmpxchg, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_Cmpxchg, CmpxchgHasCompare) {
    verifyLowIRContains(testObj(), "test_cmpxchg", "INT_EQUAL");
}

TEST_F(X86_64_Cmpxchg, XchgHasCopy) {
    verifyLowIRContains(testObj(), "test_xchg", "COPY");
}

TEST_F(X86_64_Cmpxchg, XaddHasIntAdd) {
    verifyLowIRContains(testObj(), "test_xadd", "INT_ADD");
}

TEST_F(X86_64_Cmpxchg, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}
