#include "NeverDLiftFixture.h"

class X86_64_Arith : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_arith.o";
}

TEST_F(X86_64_Arith, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_arith.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_Arith, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_Arith, AddPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_add", "INT_ADD");
}

TEST_F(X86_64_Arith, SubPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_sub", "INT_SUB");
}

TEST_F(X86_64_Arith, AndPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_and", "INT_AND");
}

TEST_F(X86_64_Arith, OrPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_or", "INT_OR");
}

TEST_F(X86_64_Arith, XorPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_xor", "INT_XOR");
}

TEST_F(X86_64_Arith, NegPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_neg", "INT_NEG2");
}

TEST_F(X86_64_Arith, NotPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_not", "INT_NOT");
}

TEST_F(X86_64_Arith, IncPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_inc", "INT_ADD");
}

TEST_F(X86_64_Arith, DecPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_dec", "INT_SUB");
}

TEST_F(X86_64_Arith, ShlPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_shl", "INT_LEFT");
}

TEST_F(X86_64_Arith, ShrPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_shr", "INT_RIGHT");
}

TEST_F(X86_64_Arith, SarPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_sar", "INT_ASHR");
}

TEST_F(X86_64_Arith, Add64PreservesSemantics) {
    verifyLowIRContains(testObj(), "test_add64", "INT_ADD");
}

TEST_F(X86_64_Arith, Sub64PreservesSemantics) {
    verifyLowIRContains(testObj(), "test_sub64", "INT_SUB");
}

TEST_F(X86_64_Arith, LeaSimplePreservesSemantics) {
    verifyLowIRContains(testObj(), "test_lea_simple", "INT_ADD");
}

TEST_F(X86_64_Arith, LeaScaledPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_lea_scaled", "INT_MULT");
}

TEST_F(X86_64_Arith, AddLLVMIR_HasAdd) {
    verifyLLVMIRContains(testObj(), "test_add", "add ");
}

TEST_F(X86_64_Arith, SubLLVMIR_HasSub) {
    verifyLLVMIRContains(testObj(), "test_sub", "sub ");
}

TEST_F(X86_64_Arith, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR — semantic loss:\n" << r.out;
}
