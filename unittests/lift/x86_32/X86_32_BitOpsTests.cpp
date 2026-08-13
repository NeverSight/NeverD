#include "NeverDLiftFixture.h"

class X86_32_BitOps : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_bitops32.o";
}

TEST_F(X86_32_BitOps, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_bitops32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_BitOps, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_BitOps, Adc32HasIntAdd) {
    verifyLowIRContains(testObj(), "test_adc32", "INT_ADD");
}

TEST_F(X86_32_BitOps, Sbb32HasIntSub) {
    verifyLowIRContains(testObj(), "test_sbb32", "INT_SUB");
}

TEST_F(X86_32_BitOps, Bt32HasIntAnd) {
    verifyLowIRContains(testObj(), "test_bt32", "INT_AND");
}

TEST_F(X86_32_BitOps, Bsf32HasBitScan) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_FALSE(r.out.empty());
}

TEST_F(X86_32_BitOps, Bswap32Lifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_FALSE(r.out.empty());
}

TEST_F(X86_32_BitOps, Xchg32HasCopy) {
    verifyLowIRContains(testObj(), "test_xchg32", "COPY");
}

TEST_F(X86_32_BitOps, Rol32HasIntLeft) {
    verifyLowIRContains(testObj(), "test_rol32", "INT_LEFT");
}

TEST_F(X86_32_BitOps, Ror32HasIntRight) {
    verifyLowIRContains(testObj(), "test_ror32", "INT_RIGHT");
}

TEST_F(X86_32_BitOps, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}
