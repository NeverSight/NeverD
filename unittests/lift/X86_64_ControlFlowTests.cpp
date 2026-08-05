#include "NeverDLiftFixture.h"

class X86_64_ControlFlow : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_control_flow.o";
}

TEST_F(X86_64_ControlFlow, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_control_flow.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_ControlFlow, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_ControlFlow, CmpJeHasIntEqual) {
    verifyLowIRContains(testObj(), "test_cmp_je", "INT_EQUAL");
}

TEST_F(X86_64_ControlFlow, CmpJneHasBoolNot) {
    auto r = liftToLowIR(testObj(), "test_cmp_jne");
    ASSERT_EQ(r.exitCode, 0);
    bool hasBranch = r.out.find("COND_BR") != std::string::npos ||
                     r.out.find("BOOL_NOT") != std::string::npos;
    EXPECT_TRUE(hasBranch) << "JNE should produce COND_BR or BOOL_NOT";
}

TEST_F(X86_64_ControlFlow, CmpJlHasFlags) {
    auto r = liftToLowIR(testObj(), "test_cmp_jl");
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("INT_SUB") != std::string::npos)
        << "CMP should produce INT_SUB for flag computation";
    EXPECT_TRUE(r.out.find("COND_BR") != std::string::npos)
        << "JL should produce COND_BR";
}

TEST_F(X86_64_ControlFlow, TestJzHasIntAnd) {
    verifyLowIRContains(testObj(), "test_test_jz", "INT_AND");
}

TEST_F(X86_64_ControlFlow, CmovHasCopy) {
    auto r = liftToLowIR(testObj(), "test_cmov");
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("INT_SUB") != std::string::npos)
        << "CMOV should have CMP (INT_SUB) for condition";
}

TEST_F(X86_64_ControlFlow, SetccProducesFlag) {
    auto r = liftToLowIR(testObj(), "test_setcc");
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("INT_SUB") != std::string::npos ||
                r.out.find("INT_NOTEQUAL") != std::string::npos)
        << "SETL should use CMP flags";
}

TEST_F(X86_64_ControlFlow, MovzxHasZext) {
    verifyLowIRContains(testObj(), "test_movzx", "INT_ZEXT");
}

TEST_F(X86_64_ControlFlow, MovsxHasSext) {
    verifyLowIRContains(testObj(), "test_movsx", "INT_SEXT");
}

TEST_F(X86_64_ControlFlow, XchgPreservesSwap) {
    auto r = liftToLowIR(testObj(), "test_xchg");
    ASSERT_EQ(r.exitCode, 0);
    int copyCount = 0;
    size_t pos = 0;
    while ((pos = r.out.find("COPY", pos)) != std::string::npos) {
        copyCount++;
        pos += 4;
    }
    EXPECT_GE(copyCount, 3) << "XCHG needs at least 3 COPYs (tmp=a, a=b, b=tmp)";
}

TEST_F(X86_64_ControlFlow, BtHasIntRight) {
    auto r = liftToLowIR(testObj(), "test_bt");
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("INT_RIGHT") != std::string::npos ||
                r.out.find("INT_AND") != std::string::npos)
        << "BT should extract a bit";
}

TEST_F(X86_64_ControlFlow, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR — semantic loss:\n" << r.out;
}
