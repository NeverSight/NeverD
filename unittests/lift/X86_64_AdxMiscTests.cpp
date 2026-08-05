#include "NeverDLiftFixture.h"

class X86_64_AdxMisc : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_adx_misc.o";
}

TEST_F(X86_64_AdxMisc, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_adx_misc.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_AdxMisc, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_AdxMisc, BswapLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("Bswap") != std::string::npos ||
                r.out.find("INTRINSIC") != std::string::npos)
        << "Expected BSWAP intrinsic in LowIR";
}

TEST_F(X86_64_AdxMisc, XchgLifts) {
    verifyLowIRContains(testObj(), "test_xchg", "COPY");
}

TEST_F(X86_64_AdxMisc, BtLifts) {
    verifyLowIRContains(testObj(), "test_bt", "INT_RIGHT");
}

TEST_F(X86_64_AdxMisc, BtsLifts) {
    verifyLowIRContains(testObj(), "test_bts", "INT_OR");
}

TEST_F(X86_64_AdxMisc, BtrLifts) {
    verifyLowIRContains(testObj(), "test_btr", "INT_AND");
}

TEST_F(X86_64_AdxMisc, BsfLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("Bsf") != std::string::npos ||
                r.out.find("INTRINSIC") != std::string::npos)
        << "Expected BSF in LowIR";
}

TEST_F(X86_64_AdxMisc, BsrLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("Bsr") != std::string::npos ||
                r.out.find("INTRINSIC") != std::string::npos)
        << "Expected BSR in LowIR";
}

TEST_F(X86_64_AdxMisc, RdtscLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("Rdtsc") != std::string::npos ||
                r.out.find("INTRINSIC") != std::string::npos)
        << "Expected RDTSC in LowIR";
}

TEST_F(X86_64_AdxMisc, CpuidLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("Cpuid") != std::string::npos ||
                r.out.find("INTRINSIC") != std::string::npos)
        << "Expected CPUID in LowIR";
}

TEST_F(X86_64_AdxMisc, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}
