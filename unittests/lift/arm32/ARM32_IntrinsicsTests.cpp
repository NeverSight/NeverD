#include "NeverDLiftFixture.h"

class ARM32_Intrinsics : public NeverDLiftTest {
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

TEST_F(ARM32_Intrinsics, AllStages) {
    verifyAllModesSucceed(obj("test_intrinsics_arm.o"));
}

TEST_F(ARM32_Intrinsics, NoUnreachable) {
    verifyLLVMIRNoUnreachable(obj("test_intrinsics_arm.o"));
}

TEST_F(ARM32_Intrinsics, LLVM_NoNdStubs) {
    auto r = liftToLLVMIR(obj("test_intrinsics_arm.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM IR lift failed: " << r.err;
    EXPECT_TRUE(r.out.find("__nd_") == std::string::npos)
        << "Found __nd_ extern stub in LLVM IR:\n"
        << r.out.substr(0, 3000);
}

TEST_F(ARM32_Intrinsics, Decompile_NoNdStubs) {
    auto r = decompileToHighC(obj("test_intrinsics_arm.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto cfile = tmpFile("decompiled_high.c");
    auto content = std::string{};
    { std::ifstream ifs(cfile); std::ostringstream ss; ss << ifs.rdbuf(); content = ss.str(); }
    EXPECT_FALSE(content.empty()) << "Decompiled C is empty";
    EXPECT_TRUE(content.find("__nd_") == std::string::npos)
        << "Found __nd_ in decompiled C:\n"
        << content.substr(0, 3000);
}

TEST_F(ARM32_Intrinsics, Decompile_NoMsvcAsm) {
    auto r = decompileToHighC(obj("test_intrinsics_arm.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto cfile = tmpFile("decompiled_high.c");
    auto content = std::string{};
    { std::ifstream ifs(cfile); std::ostringstream ss; ss << ifs.rdbuf(); content = ss.str(); }
    EXPECT_TRUE(content.find("__asm {") == std::string::npos)
        << "Found MSVC __asm{} in ARM32 decompile — should use ACLE or GNU __asm__ volatile:\n"
        << content.substr(0, 3000);
}

TEST_F(ARM32_Intrinsics, LlvmC_NoNdStubs) {
    auto r = decompileToC(obj("test_intrinsics_arm.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto cfile = tmpFile("decompiled.c");
    auto content = std::string{};
    { std::ifstream ifs(cfile); std::ostringstream ss; ss << ifs.rdbuf(); content = ss.str(); }
    EXPECT_FALSE(content.empty()) << "LLVM C decompiled output is empty";
    EXPECT_TRUE(content.find("__nd_") == std::string::npos)
        << "Found __nd_ in LLVM C decompile:\n"
        << content.substr(0, 3000);
}

TEST_F(ARM32_Intrinsics, LlvmC_NoMsvcAsm) {
    auto r = decompileToC(obj("test_intrinsics_arm.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto cfile = tmpFile("decompiled.c");
    auto content = std::string{};
    { std::ifstream ifs(cfile); std::ostringstream ss; ss << ifs.rdbuf(); content = ss.str(); }
    EXPECT_TRUE(content.find("__asm {") == std::string::npos)
        << "Found MSVC __asm{} in ARM32 LLVM C decompile — should use GNU __asm__ volatile:\n"
        << content.substr(0, 3000);
}

TEST_F(ARM32_Intrinsics, Decompile_BarriersUseACLE) {
    auto r = decompileToHighC(obj("test_intrinsics_arm.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    EXPECT_TRUE(content.find("__dmb(") != std::string::npos)
        << "Expected __dmb() in ARM32 HighC:\n" << content.substr(0, 3000);
    EXPECT_TRUE(content.find("__dsb(") != std::string::npos)
        << "Expected __dsb() in ARM32 HighC:\n" << content.substr(0, 3000);
    EXPECT_TRUE(content.find("__isb(") != std::string::npos)
        << "Expected __isb() in ARM32 HighC:\n" << content.substr(0, 3000);
}

TEST_F(ARM32_Intrinsics, LlvmC_BarriersUseACLE) {
    auto r = decompileToC(obj("test_intrinsics_arm.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("__dmb(") != std::string::npos)
        << "Expected __dmb() in ARM32 LLVM C:\n" << content.substr(0, 3000);
    EXPECT_TRUE(content.find("__dsb(") != std::string::npos)
        << "Expected __dsb() in ARM32 LLVM C:\n" << content.substr(0, 3000);
    EXPECT_TRUE(content.find("__isb(") != std::string::npos)
        << "Expected __isb() in ARM32 LLVM C:\n" << content.substr(0, 3000);
}

TEST_F(ARM32_Intrinsics, LlvmC_BkptUsesBuiltin) {
    auto r = decompileToC(obj("test_intrinsics_arm.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("__builtin_debugtrap") != std::string::npos)
        << "Expected __builtin_debugtrap() for bkpt:\n" << content.substr(0, 3000);
}

TEST_F(ARM32_Intrinsics, Decompile_BkptUsesBuiltin) {
    auto r = decompileToHighC(obj("test_intrinsics_arm.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    EXPECT_TRUE(content.find("__builtin_debugtrap") != std::string::npos)
        << "Expected __builtin_debugtrap() for bkpt in HighC:\n" << content.substr(0, 3000);
}

TEST_F(ARM32_Intrinsics, Decompile_VoidInference) {
    auto r = decompileToHighC(obj("test_intrinsics_arm.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    for (auto fn : {"test_svc_arm_intrinsic", "test_bkpt_arm_intrinsic",
                    "test_nop_arm_intrinsic", "test_clrex_arm_intrinsic",
                    "test_dmb_arm_intrinsic", "test_dsb_arm_intrinsic",
                    "test_isb_arm_intrinsic"}) {
        auto pos = content.find(fn);
        ASSERT_NE(pos, std::string::npos) << fn << " not found";
        auto prefix = content.substr(pos > 10 ? pos - 10 : 0, 10);
        EXPECT_TRUE(prefix.find("void") != std::string::npos)
            << fn << " should be detected as void:\n"
            << content.substr(pos > 20 ? pos - 20 : 0, 80);
    }
}

TEST_F(ARM32_Intrinsics, LlvmC_VoidInference) {
    auto r = decompileToC(obj("test_intrinsics_arm.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    for (auto fn : {"test_svc_arm_intrinsic", "test_bkpt_arm_intrinsic",
                    "test_nop_arm_intrinsic", "test_clrex_arm_intrinsic",
                    "test_dmb_arm_intrinsic"}) {
        auto pos = content.find(fn);
        ASSERT_NE(pos, std::string::npos) << fn << " not found";
        auto prefix = content.substr(pos > 10 ? pos - 10 : 0, 10);
        EXPECT_TRUE(prefix.find("void") != std::string::npos)
            << fn << " should be detected as void:\n"
            << content.substr(pos > 20 ? pos - 20 : 0, 80);
    }
}

TEST_F(ARM32_Intrinsics, LlvmC_FramePointerReturnAddressIsVoid) {
    auto r = decompileToC(obj("test_frame_pointer_void_arm.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_NE(content.find("void test_frame_pointer_void_arm("),
              std::string::npos)
        << "saved/restored lr should not become an r0 return value:\n"
        << content;
}

TEST_F(ARM32_Intrinsics, LlvmC_ExplicitReturnAddressStaysNonVoid) {
    auto r = decompileToC(obj("test_frame_pointer_void_arm.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_NE(content.find(
                  "uint32_t test_frame_pointer_return_address_arm("),
              std::string::npos)
        << "a return address explicitly loaded into r0 is a real result:\n"
        << content;
}

TEST_F(ARM32_Intrinsics, LlvmC_SelUsesParams) {
    auto r = decompileToC(obj("test_intrinsics_arm.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    auto pos = content.find("test_sel_arm_intrinsic");
    ASSERT_NE(pos, std::string::npos);
    auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
    EXPECT_TRUE(fn_body.find("frame") == std::string::npos)
        << "sel function should have no frame variables:\n" << fn_body;
    EXPECT_TRUE(fn_body.find("arg0") != std::string::npos)
        << "sel function should use arg0 directly:\n" << fn_body;
}

TEST_F(ARM32_Intrinsics, Decompile_ClrexUsesACLE) {
    auto r = decompileToHighC(obj("test_intrinsics_arm.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    EXPECT_TRUE(content.find("__clrex()") != std::string::npos)
        << "Expected __clrex() in ARM32 HighC:\n" << content.substr(0, 3000);
}
