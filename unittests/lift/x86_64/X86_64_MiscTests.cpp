#include "NeverDLiftFixture.h"

class X86_64_Misc : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_misc.o";
}

TEST_F(X86_64_Misc, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_misc.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_Misc, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_Misc, NopPreservesSemantics) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_FALSE(r.out.empty());
}

TEST_F(X86_64_Misc, MovzxByteHasZext) {
    verifyLowIRContains(testObj(), "test_movzx_byte", "INT_ZEXT");
}

TEST_F(X86_64_Misc, MovsxByteHasSext) {
    verifyLowIRContains(testObj(), "test_movsx_byte", "INT_SEXT");
}

TEST_F(X86_64_Misc, MovzxWordHasZext) {
    verifyLowIRContains(testObj(), "test_movzx_word", "INT_ZEXT");
}

TEST_F(X86_64_Misc, MovsxWordHasSext) {
    verifyLowIRContains(testObj(), "test_movsx_word", "INT_SEXT");
}

TEST_F(X86_64_Misc, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}

TEST_F(X86_64_Misc, NoPoisonInFunctions) {
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
