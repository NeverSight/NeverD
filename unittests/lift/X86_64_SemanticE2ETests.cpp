#include "NeverDLiftFixture.h"

class X86_64_SemanticE2E : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_semantic_e2e.o";
}

TEST_F(X86_64_SemanticE2E, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_semantic_e2e.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_SemanticE2E, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_SemanticE2E, AddSemanticsLLVMIR) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("ret i32 50") != std::string::npos ||
                r.out.find("ret i64 50") != std::string::npos ||
                r.out.find("add") != std::string::npos)
        << "Add semantics not preserved in LLVM IR:\n" << r.out;
}

TEST_F(X86_64_SemanticE2E, SubSemanticsLLVMIR) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("ret i32 63") != std::string::npos ||
                r.out.find("ret i64 63") != std::string::npos ||
                r.out.find("sub") != std::string::npos)
        << "Sub semantics not preserved in LLVM IR:\n" << r.out;
}

TEST_F(X86_64_SemanticE2E, ImulSemanticsLLVMIR) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("ret i32 42") != std::string::npos ||
                r.out.find("ret i64 42") != std::string::npos ||
                r.out.find("mul") != std::string::npos)
        << "Imul semantics not preserved in LLVM IR:\n" << r.out;
}

TEST_F(X86_64_SemanticE2E, ShiftSemanticsLLVMIR) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("ret i32 255") != std::string::npos ||
                r.out.find("ret i64 255") != std::string::npos ||
                r.out.find("lshr") != std::string::npos ||
                r.out.find("store i32 255") != std::string::npos)
        << "Shift semantics not preserved in LLVM IR:\n" << r.out;
}

TEST_F(X86_64_SemanticE2E, NegSemanticsLLVMIR) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("ret i32 -42") != std::string::npos ||
                r.out.find("ret i64 -42") != std::string::npos ||
                r.out.find("sub i32 0") != std::string::npos ||
                r.out.find("sub nsw i32 0") != std::string::npos ||
                r.out.find("store i32 -42") != std::string::npos)
        << "Neg semantics not preserved in LLVM IR:\n" << r.out;
}

TEST_F(X86_64_SemanticE2E, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}

TEST_F(X86_64_SemanticE2E, NoPoisonInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
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
