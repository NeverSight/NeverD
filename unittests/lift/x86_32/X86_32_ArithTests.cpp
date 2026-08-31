#include "NeverDLiftFixture.h"

class X86_32_Arith : public NeverDLiftTest {
protected:
    static fs::path testObj() {
        return fs::path(TEST_OBJ_DIR) / "test_arith32.o";
    }
};

namespace {

std::string llvmFunction(const std::string &IR, const std::string &Name) {
    const size_t Symbol = IR.find("@" + Name + "(");
    if (Symbol == std::string::npos)
        return {};
    const size_t Begin = IR.rfind("define ", Symbol);
    const size_t End = IR.find("\n}", Symbol);
    if (Begin == std::string::npos || End == std::string::npos)
        return {};
    return IR.substr(Begin, End + 2 - Begin);
}

} // namespace

TEST_F(X86_32_Arith, AllStagesPass) {
    verifyAllStages(testObj());
}

TEST_F(X86_32_Arith, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_Arith, Add32HasIntAdd) {
    verifyLowIRContains(testObj(), "test_add32", "INT_ADD");
}

TEST_F(X86_32_Arith, Sub32HasIntSub) {
    verifyLowIRContains(testObj(), "test_sub32", "INT_SUB");
}

TEST_F(X86_32_Arith, Imul32HasIntMult) {
    verifyLowIRContains(testObj(), "test_imul32", "INT_MULT");
}

TEST_F(X86_32_Arith, Div32HasIntDiv) {
    verifyLowIRContains(testObj(), "test_div32", "INT_DIV");
}

TEST_F(X86_32_Arith, Shl32HasIntLeft) {
    verifyLowIRContains(testObj(), "test_shl32", "INT_LEFT");
}

TEST_F(X86_32_Arith, Shr32HasIntRight) {
    verifyLowIRContains(testObj(), "test_shr32", "INT_RIGHT");
}

TEST_F(X86_32_Arith, And32HasIntAnd) {
    verifyLowIRContains(testObj(), "test_and32", "INT_AND");
}

TEST_F(X86_32_Arith, Or32HasIntOr) {
    verifyLowIRContains(testObj(), "test_or32", "INT_OR");
}

TEST_F(X86_32_Arith, Xor32HasIntXor) {
    verifyLowIRContains(testObj(), "test_xor32", "INT_XOR");
}

TEST_F(X86_32_Arith, Neg32HasInt2Comp) {
    verifyLowIRContains(testObj(), "test_neg32", "INT_NEG2");
}

TEST_F(X86_32_Arith, Not32HasIntNot) {
    verifyLowIRContains(testObj(), "test_not32", "INT_NOT");
}

TEST_F(X86_32_Arith, Push32Pop32HasStoreLoad) {
    auto r = liftToLowIR(testObj(), "test_push_pop32");
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("STORE") != std::string::npos) << "PUSH32 needs STORE";
    EXPECT_TRUE(r.out.find("LOAD") != std::string::npos) << "POP32 needs LOAD";
}

TEST_F(X86_32_Arith, Lea32HasIntMult) {
    verifyLowIRContains(testObj(), "test_lea32", "INT_MULT");
}

TEST_F(X86_32_Arith, OnlyDiv32HasArchitecturalExceptionUnreachable) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);

    const std::string Div32 = llvmFunction(r.out, "test_div32");
    ASSERT_FALSE(Div32.empty()) << r.out;
    EXPECT_NE(Div32.find("call void @llvm.trap()"), std::string::npos)
        << Div32;
    EXPECT_NE(Div32.find("unreachable"), std::string::npos) << Div32;

    std::string OtherFunctions = r.out;
    OtherFunctions.erase(OtherFunctions.find(Div32), Div32.size());
    EXPECT_EQ(OtherFunctions.find("unreachable"), std::string::npos)
        << "Found unexpected 'unreachable' outside test_div32:\n" << r.out;
}

TEST_F(X86_32_Arith, NoPoisonInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    int poison_count = 0;
    size_t pos = 0;
    while ((pos = r.out.find("poison", pos)) != std::string::npos) {
        size_t line_start = r.out.rfind('\n', pos);
        if (line_start == std::string::npos) line_start = 0;
        std::string line = r.out.substr(line_start, r.out.find('\n', pos) - line_start);
        if (line.find("nocreateundeforpoison") == std::string::npos)
            poison_count++;
        pos += 6;
    }
    // i386 cdecl passes args on stack; argument inference doesn't detect
    // stack parameters yet, so test_div32 appears as 0-arg → div by 0 → poison.
    EXPECT_LE(poison_count, 1)
        << "More than 1 poison (expected only test_div32 cdecl limitation)";
}
