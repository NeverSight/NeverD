#include "NeverDLiftFixture.h"

class AArch64_Arith : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_arith_a64.o";
}

TEST_F(AArch64_Arith, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_arith_a64.o not built";
    verifyAllStages(testObj());
}

TEST_F(AArch64_Arith, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(AArch64_Arith, AddHasIntAdd) {
    verifyLowIRContains(testObj(), "test_add_a64", "INT_ADD");
}

TEST_F(AArch64_Arith, SubHasIntSub) {
    verifyLowIRContains(testObj(), "test_sub_a64", "INT_SUB");
}

TEST_F(AArch64_Arith, MulHasIntMult) {
    verifyLowIRContains(testObj(), "test_mul_a64", "INT_MULT");
}

TEST_F(AArch64_Arith, AndHasIntAnd) {
    verifyLowIRContains(testObj(), "test_and_a64", "INT_AND");
}

TEST_F(AArch64_Arith, OrrHasIntOr) {
    verifyLowIRContains(testObj(), "test_orr_a64", "INT_OR");
}

TEST_F(AArch64_Arith, EorHasIntXor) {
    verifyLowIRContains(testObj(), "test_eor_a64", "INT_XOR");
}

TEST_F(AArch64_Arith, LslHasIntLeft) {
    verifyLowIRContains(testObj(), "test_lsl_a64", "INT_LEFT");
}

TEST_F(AArch64_Arith, LsrHasIntRight) {
    verifyLowIRContains(testObj(), "test_lsr_a64", "INT_RIGHT");
}

TEST_F(AArch64_Arith, AsrHasIntSright) {
    verifyLowIRContains(testObj(), "test_asr_a64", "INT_ASHR");
}

TEST_F(AArch64_Arith, SdivHasIntSdiv) {
    verifyLowIRContains(testObj(), "test_sdiv_a64", "INT_SDIV");
}

TEST_F(AArch64_Arith, UdivHasIntDiv) {
    verifyLowIRContains(testObj(), "test_udiv_a64", "INT_DIV");
}

TEST_F(AArch64_Arith, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}

TEST_F(AArch64_Arith, NoPoisonInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    // Newer LLVM stamps intrinsics (e.g. llvm.umax) with the attribute
    // `nocreateundeforpoison`, whose substring "poison" is a false positive for
    // a naive search — skip those lines and only flag real poison values.
    size_t pos = 0;
    while ((pos = r.out.find("poison", pos)) != std::string::npos) {
        size_t line_start = r.out.rfind('\n', pos);
        if (line_start == std::string::npos) line_start = 0;
        std::string line = r.out.substr(line_start, r.out.find('\n', pos) - line_start);
        if (line.find("nocreateundeforpoison") == std::string::npos) {
            FAIL() << "Found 'poison' in LLVM IR line: " << line;
        }
        pos += 6;
    }
}
