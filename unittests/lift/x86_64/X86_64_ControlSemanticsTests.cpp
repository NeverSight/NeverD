#include "NeverDLiftFixture.h"

class X86_64_ControlSemantics : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_control_semantics.o";
}

TEST_F(X86_64_ControlSemantics, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_control_semantics.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_ControlSemantics, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_ControlSemantics, NoUnreachable) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}

TEST_F(X86_64_ControlSemantics, NestedIfElse_HasBranches) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("icmp") != std::string::npos)
        << "Expected comparison instructions in LLVM IR";
}

TEST_F(X86_64_ControlSemantics, NestedIfElse_PreservesAllPaths) {
    auto r = liftToHighIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("-1") != std::string::npos)
        << "Expected -1 (negative case) in HighIR for nested_if_else";
}

TEST_F(X86_64_ControlSemantics, Loop_HasPhiOrWhile) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    bool has_loop_structure = r.out.find("phi") != std::string::npos ||
                               r.out.find("while") != std::string::npos ||
                               r.out.find("br label") != std::string::npos;
    bool loop_const_folded = r.out.find("test_loop_accumulate") != std::string::npos &&
                              r.out.find("ret i32") != std::string::npos;
    EXPECT_TRUE(has_loop_structure || loop_const_folded)
        << "Expected loop structure or constant-folded result in LLVM IR";
}

TEST_F(X86_64_ControlSemantics, ConditionalMove_HasSelect) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("select") != std::string::npos)
        << "Expected 'select' for CMOV in LLVM IR";
}

TEST_F(X86_64_ControlSemantics, Bswap_LowIR) {
    verifyLowIRContains(testObj(), "test_bswap", "INTRINSIC");
}

TEST_F(X86_64_ControlSemantics, BT_LowIR) {
    verifyLowIRContains(testObj(), "test_bt", "INT_RIGHT");
}

TEST_F(X86_64_ControlSemantics, BSF_LowIR) {
    verifyLowIRContains(testObj(), "test_bsf", "INTRINSIC");
}

TEST_F(X86_64_ControlSemantics, PopCount_LowIR) {
    verifyLowIRContains(testObj(), "test_popcnt", "POPCOUNT");
}

TEST_F(X86_64_ControlSemantics, ADCChain_HasCarry) {
    verifyLowIRContains(testObj(), "test_adc_chain", "INT_CARRY");
}
