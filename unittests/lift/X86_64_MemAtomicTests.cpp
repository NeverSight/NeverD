#include "NeverDLiftFixture.h"

class X86_64_MemAtomic : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_mem_atomic.o";
}

TEST_F(X86_64_MemAtomic, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_mem_atomic.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_MemAtomic, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_MemAtomic, XaddLifts) {
    verifyLowIRContains(testObj(), "test_xadd", "INT_ADD");
}

TEST_F(X86_64_MemAtomic, CmpxchgLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("Cmpxchg") != std::string::npos ||
                r.out.find("INTRINSIC") != std::string::npos ||
                r.out.find("INT_EQUAL") != std::string::npos)
        << "Expected CMPXCHG operation in LowIR";
}

TEST_F(X86_64_MemAtomic, LockAddLifts) {
    verifyLowIRContains(testObj(), "test_lock_add", "INT_ADD");
}

TEST_F(X86_64_MemAtomic, LeaComplexLifts) {
    verifyLowIRContains(testObj(), "test_lea_complex", "INT_MULT");
}

TEST_F(X86_64_MemAtomic, MovabsLifts) {
    verifyLowIRContains(testObj(), "test_movabs", "COPY");
}

TEST_F(X86_64_MemAtomic, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
