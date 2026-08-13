#include "NeverDLiftFixture.h"

class X86_32_SignExt : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_signext32.o";
}

TEST_F(X86_32_SignExt, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_signext32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_SignExt, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_SignExt, Cbw32HasIntSext) {
    verifyLowIRContains(testObj(), "test_cbw32", "INT_SEXT");
}

TEST_F(X86_32_SignExt, Cwde32HasIntSext) {
    verifyLowIRContains(testObj(), "test_cwde32", "INT_SEXT");
}

TEST_F(X86_32_SignExt, Cdq32HasSright) {
    verifyLowIRContains(testObj(), "test_cdq32", "INT_ASHR");
}

TEST_F(X86_32_SignExt, MovsxByte32HasSext) {
    verifyLowIRContains(testObj(), "test_movsx_byte32", "INT_SEXT");
}

TEST_F(X86_32_SignExt, MovzxByte32HasZext) {
    verifyLowIRContains(testObj(), "test_movzx_byte32", "INT_ZEXT");
}

TEST_F(X86_32_SignExt, MovsxWord32HasSext) {
    verifyLowIRContains(testObj(), "test_movsx_word32", "INT_SEXT");
}

TEST_F(X86_32_SignExt, MovzxWord32HasZext) {
    verifyLowIRContains(testObj(), "test_movzx_word32", "INT_ZEXT");
}

TEST_F(X86_32_SignExt, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}

TEST_F(X86_32_SignExt, NoPoisonInFunctions) {
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
