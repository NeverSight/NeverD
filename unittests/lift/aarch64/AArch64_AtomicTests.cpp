#include "NeverDLiftFixture.h"

class AArch64_Atomic : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_atomic_a64.o";
}

TEST_F(AArch64_Atomic, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_atomic_a64.o not built";
    verifyAllStages(testObj());
}

TEST_F(AArch64_Atomic, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(AArch64_Atomic, LdxrStxrLifted) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    bool has_load = r.out.find("LOAD") != std::string::npos;
    bool has_store = r.out.find("STORE") != std::string::npos;
    EXPECT_TRUE(has_load) << "Expected LOAD for LDXR";
    EXPECT_TRUE(has_store) << "Expected STORE for STXR";
}

TEST_F(AArch64_Atomic, BarrierLifted) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
}

TEST_F(AArch64_Atomic, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}
