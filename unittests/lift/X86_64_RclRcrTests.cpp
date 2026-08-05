#include "NeverDLiftFixture.h"

class X86_64_RclRcr : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_rcl_rcr.o";
}

TEST_F(X86_64_RclRcr, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_rcl_rcr.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_RclRcr, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_RclRcr, RclLifts) {
    verifyLowIRContains(testObj(), "test_rcl", "INT_LEFT");
}

TEST_F(X86_64_RclRcr, RcrLifts) {
    verifyLowIRContains(testObj(), "test_rcr", "INT_RIGHT");
}

TEST_F(X86_64_RclRcr, Rcl64Lifts) {
    verifyLowIRContains(testObj(), "test_rcl64", "INT_LEFT");
}

TEST_F(X86_64_RclRcr, Rcr64Lifts) {
    verifyLowIRContains(testObj(), "test_rcr64", "INT_RIGHT");
}

TEST_F(X86_64_RclRcr, LoopLifts) {
    verifyLowIRContains(testObj(), "test_loop_sum", "COND_BR");
}

TEST_F(X86_64_RclRcr, EnterLeaveLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("STORE") != std::string::npos)
        << "Expected STORE in LowIR for ENTER/LEAVE";
}

TEST_F(X86_64_RclRcr, SbbPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_sbb_chain", "INT_SUB");
}

TEST_F(X86_64_RclRcr, AdcPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_adc_chain", "INT_ADD");
}

TEST_F(X86_64_RclRcr, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}

TEST_F(X86_64_RclRcr, NoPoisonValues) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    bool has_poison_value = false;
    std::string::size_type pos = 0;
    while ((pos = r.out.find("poison", pos)) != std::string::npos) {
        if (r.out.find("nocreateundeforpoison", pos > 20 ? pos - 20 : 0) == std::string::npos ||
            pos < 20 || r.out.substr(pos - 20, 40).find("nocreateundefor") == std::string::npos) {
            if (pos > 0 && (r.out[pos - 1] == ' ' || r.out[pos - 1] == ',')) {
                has_poison_value = true;
                break;
            }
        }
        pos += 6;
    }
    EXPECT_FALSE(has_poison_value)
        << "Found poison value in LLVM IR:\n" << r.out;
}
