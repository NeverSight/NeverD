#include "NeverDLiftFixture.h"

class X86_64_RotateShift : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_rotate_shift.o";
}

TEST_F(X86_64_RotateShift, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_rotate_shift.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_RotateShift, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_RotateShift, RolHasIntLeft) {
    verifyLowIRContains(testObj(), "test_rol", "INT_LEFT");
}

TEST_F(X86_64_RotateShift, RorHasIntRight) {
    verifyLowIRContains(testObj(), "test_ror", "INT_RIGHT");
}

TEST_F(X86_64_RotateShift, ShldPreservesSemantics) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("INT_LEFT") != std::string::npos)
        << "SHLD should produce shift ops";
}

TEST_F(X86_64_RotateShift, ShrdPreservesSemantics) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("INT_RIGHT") != std::string::npos)
        << "SHRD should produce shift ops";
}

TEST_F(X86_64_RotateShift, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}
