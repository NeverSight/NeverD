#include "NeverDLiftFixture.h"

class X86_64_StringOps : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_string_ops.o";
}

TEST_F(X86_64_StringOps, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_string_ops.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_StringOps, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_StringOps, MovsbHasStoreLoad) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("STORE") != std::string::npos ||
                r.out.find("LOAD") != std::string::npos)
        << "String ops should have memory operations";
}

TEST_F(X86_64_StringOps, StosbHasStore) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("STORE") != std::string::npos)
        << "STOSB should produce STORE";
}

TEST_F(X86_64_StringOps, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}
