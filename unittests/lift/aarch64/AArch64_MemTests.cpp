#include "NeverDLiftFixture.h"

class AArch64_Mem : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_mem_a64.o";
}

TEST_F(AArch64_Mem, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_mem_a64.o not built";
    verifyAllStages(testObj());
}

TEST_F(AArch64_Mem, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(AArch64_Mem, LdrStrHasLoadStore) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("LOAD") != std::string::npos)
        << "LDR should produce LOAD";
    EXPECT_TRUE(r.out.find("STORE") != std::string::npos)
        << "STR should produce STORE";
}

TEST_F(AArch64_Mem, UxtbLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("INT_AND") != std::string::npos ||
                r.out.find("INT_ZEXT") != std::string::npos ||
                r.out.find("SUBBYTES") != std::string::npos)
        << "UXTB should produce masking/extension";
}

TEST_F(AArch64_Mem, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}
