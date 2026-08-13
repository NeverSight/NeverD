#include "NeverDLiftFixture.h"

class AArch64_Intrinsics : public NeverDLiftTest {
protected:
    std::string readDecompiledFile(const std::string& fname) {
        auto f = tmpFile(fname);
        std::ifstream ifs(f);
        std::ostringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    }
};

static fs::path obj(const char* name) {
    return fs::path(TEST_OBJ_DIR) / name;
}

TEST_F(AArch64_Intrinsics, AllStages) {
    verifyAllModesSucceed(obj("test_intrinsics_a64.o"));
}

TEST_F(AArch64_Intrinsics, OnlyBrkIsUnreachable) {
    auto r = liftToLLVMIR(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM IR lift failed: " << r.err;

    auto functionStart = r.out.find("@test_brk_intrinsic");
    ASSERT_NE(functionStart, std::string::npos) << r.out.substr(0, 3000);
    functionStart = r.out.rfind("define ", functionStart);
    ASSERT_NE(functionStart, std::string::npos);
    auto functionEnd = r.out.find("\n}", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    functionEnd += 2;

    const std::string brkFunction =
        r.out.substr(functionStart, functionEnd - functionStart);
    EXPECT_NE(brkFunction.find("call void @llvm.trap()"), std::string::npos)
        << brkFunction;
    EXPECT_NE(brkFunction.find("unreachable"), std::string::npos)
        << brkFunction;

    std::string otherFunctions = r.out;
    otherFunctions.erase(functionStart, functionEnd - functionStart);
    EXPECT_EQ(otherFunctions.find("unreachable"), std::string::npos)
        << "Only architectural traps may be unreachable:\n"
        << otherFunctions.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, LLVM_NoNdStubs) {
    auto r = liftToLLVMIR(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM IR lift failed: " << r.err;
    EXPECT_TRUE(r.out.find("__nd_") == std::string::npos)
        << "Found __nd_ extern stub in LLVM IR:\n"
        << r.out.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, Decompile_NoNdStubs) {
    auto r = decompileToHighC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto cfile = tmpFile("decompiled_high.c");
    auto content = std::string{};
    { std::ifstream ifs(cfile); std::ostringstream ss; ss << ifs.rdbuf(); content = ss.str(); }
    EXPECT_FALSE(content.empty()) << "Decompiled C is empty";
    EXPECT_TRUE(content.find("__nd_") == std::string::npos)
        << "Found __nd_ in decompiled C:\n"
        << content.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, Decompile_NoMsvcAsm) {
    auto r = decompileToHighC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto cfile = tmpFile("decompiled_high.c");
    auto content = std::string{};
    { std::ifstream ifs(cfile); std::ostringstream ss; ss << ifs.rdbuf(); content = ss.str(); }
    EXPECT_TRUE(content.find("__asm {") == std::string::npos)
        << "Found MSVC __asm{} in AArch64 decompile — should use ACLE or GNU __asm__ volatile:\n"
        << content.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, LlvmC_NoNdStubs) {
    auto r = decompileToC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto cfile = tmpFile("decompiled.c");
    auto content = std::string{};
    { std::ifstream ifs(cfile); std::ostringstream ss; ss << ifs.rdbuf(); content = ss.str(); }
    EXPECT_FALSE(content.empty()) << "LLVM C decompiled output is empty";
    EXPECT_TRUE(content.find("__nd_") == std::string::npos)
        << "Found __nd_ in LLVM C decompile:\n"
        << content.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, LlvmC_NoMsvcAsm) {
    auto r = decompileToC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("__asm {") == std::string::npos)
        << "Found MSVC __asm{} in AArch64 LLVM C decompile — should use GNU __asm__ volatile:\n"
        << content.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, Decompile_DmbUsesACLE) {
    auto r = decompileToHighC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    EXPECT_TRUE(content.find("__dmb(") != std::string::npos)
        << "Expected __dmb() ACLE intrinsic in AArch64 decompile:\n"
        << content.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, LlvmC_BarriersUseACLE) {
    auto r = decompileToC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("__dmb(") != std::string::npos)
        << "Expected __dmb() in LLVM C:\n" << content.substr(0, 3000);
    EXPECT_TRUE(content.find("__dsb(") != std::string::npos)
        << "Expected __dsb() in LLVM C:\n" << content.substr(0, 3000);
    EXPECT_TRUE(content.find("__isb(") != std::string::npos)
        << "Expected __isb() in LLVM C:\n" << content.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, LlvmC_HintsUseACLE) {
    auto r = decompileToC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("__yield()") != std::string::npos)
        << "Expected __yield() in LLVM C:\n" << content.substr(0, 3000);
    EXPECT_TRUE(content.find("__clrex()") != std::string::npos)
        << "Expected __clrex() in LLVM C:\n" << content.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, LlvmC_CleanOutput) {
    auto r = decompileToC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    size_t frame_count = 0;
    size_t pos = 0;
    while ((pos = content.find("frame", pos)) != std::string::npos) {
        ++frame_count; pos += 5;
    }
    EXPECT_EQ(frame_count, 0u)
        << "Found frame variables in clean ARM intrinsic functions:\n"
        << content.substr(0, 3000);
}
