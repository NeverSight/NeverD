#include "NeverDLiftFixture.h"

class AArch64_Cond : public NeverDLiftTest {
protected:
    void expectPairedClangSyntax(const fs::path &CFile,
                                 const std::string &Source) {
        auto stringHeader = tmpFile("string.h");
        std::ofstream shim(stringHeader);
        ASSERT_TRUE(shim.good());
        shim << "void *memcpy(void *, const void *, __SIZE_TYPE__);\n";
        shim.close();

        auto syntax = exec(NEVERD_TEST_CLANG,
                           {"-target", "aarch64-none-elf", "-ffreestanding",
                            "-march=armv8.8-a+hbc", "-std=gnu11", "-I",
                            tmp().string(), "-fsyntax-only", CFile.string()});
        EXPECT_EQ(syntax.exitCode, 0) << syntax.err << "\n" << Source;
    }
};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_cond_a64.o";
}

static fs::path hbcObj() {
    return fs::path(TEST_OBJ_DIR) / "test_hbc_a64.o";
}

TEST_F(AArch64_Cond, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_cond_a64.o not built";
    verifyAllStages(testObj());
}

TEST_F(AArch64_Cond, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(AArch64_Cond, CselHasSelectOrCbranch) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("SELECT") != std::string::npos ||
                r.out.find("COND_BR") != std::string::npos)
        << "CSEL should produce SELECT or COND_BR";
}

TEST_F(AArch64_Cond, CsetLifts) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_FALSE(r.out.empty());
}

TEST_F(AArch64_Cond, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}

TEST_F(AArch64_Cond, BcCondUsesNzcvCondition) {
    ASSERT_TRUE(fs::exists(hbcObj())) << "test_hbc_a64.o not built";
    auto low = liftToLowIR(hbcObj());
    ASSERT_EQ(low.exitCode, 0) << low.err;
    EXPECT_NE(low.out.find("COND_BR"), std::string::npos)
        << "BC.eq must branch on the NZCV condition:\n" << low.out;
}

TEST_F(AArch64_Cond, BcCondLLVMDependsOnArgument) {
    auto ir = liftToLLVMIR(hbcObj());
    ASSERT_EQ(ir.exitCode, 0) << ir.err;
    EXPECT_NE(ir.out.find("icmp eq i32 %arg0, 0"), std::string::npos) << ir.out;
    EXPECT_EQ(ir.out.find("ret i64 1"), std::string::npos)
        << "BC.eq must not collapse to an always-taken branch:\n" << ir.out;
}

TEST_F(AArch64_Cond, BcCondHighCBranchesAndCompiles) {
    auto result = decompileToHighC(hbcObj());
    ASSERT_EQ(result.exitCode, 0) << result.err;

    auto cFile = tmpFile("decompiled_high.c");
    ASSERT_TRUE(fs::exists(cFile));
    std::ifstream input(cFile);
    ASSERT_TRUE(input.good());
    std::string source((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
    EXPECT_NE(source.find("if (arg0 == 0)"), std::string::npos) << source;
    expectPairedClangSyntax(cFile, source);
}
