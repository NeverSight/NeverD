#include "NeverDLiftFixture.h"

class X86_64_Intrinsics : public NeverDLiftTest {
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

TEST_F(X86_64_Intrinsics, AllStages) {
    verifyAllModesSucceed(obj("test_intrinsics_system.o"));
}

TEST_F(X86_64_Intrinsics, NoUnreachable) {
    verifyLLVMIRNoUnreachable(obj("test_intrinsics_system.o"));
}

TEST_F(X86_64_Intrinsics, LLVM_NoNdStubs) {
    auto r = liftToLLVMIR(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM IR lift failed: " << r.err;
    EXPECT_TRUE(r.out.find("__nd_") == std::string::npos)
        << "Found __nd_ extern stub in LLVM IR — all intrinsics should be inline:\n"
        << r.out.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, Decompile_NoNdStubs) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    EXPECT_FALSE(content.empty()) << "Decompiled C is empty";
    EXPECT_TRUE(content.find("__nd_") == std::string::npos)
        << "Found __nd_ in decompiled C — should use C intrinsics or __asm{}:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, Decompile_UsesCorrectStyle) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    EXPECT_TRUE(content.find("__asm__ volatile") == std::string::npos)
        << "Found GNU __asm__ volatile in x86 decompile — should use MSVC __asm{} or C intrinsics:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, Decompile_CpuidUsesIntrinsic) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    EXPECT_TRUE(content.find("__cpuid") != std::string::npos)
        << "Expected __cpuid() in decompiled C for cpuid instruction:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, Decompile_RdtscUsesIntrinsic) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    EXPECT_TRUE(content.find("__rdtsc") != std::string::npos)
        << "Expected __rdtsc() in decompiled C for rdtsc instruction:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, Decompile_FenceUsesIntrinsic) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    EXPECT_TRUE(content.find("_mm_pause") != std::string::npos)
        << "Expected _mm_pause() in decompiled C:\n" << content.substr(0, 3000);
    EXPECT_TRUE(content.find("_mm_mfence") != std::string::npos)
        << "Expected _mm_mfence() in decompiled C:\n" << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, LlvmC_NoNdStubs) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_FALSE(content.empty()) << "LLVM C decompiled output is empty";
    EXPECT_TRUE(content.find("__nd_") == std::string::npos)
        << "Found __nd_ in LLVM C decompile — should use C intrinsics or __asm{}:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, LlvmC_UsesCorrectStyle) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("__asm__ volatile") == std::string::npos)
        << "Found GNU __asm__ volatile in x86 LLVM C decompile — should use MSVC __asm{} or C intrinsics:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, LlvmC_CpuidUsesIntrinsic) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("__cpuid") != std::string::npos)
        << "Expected __cpuid(cpuInfo, ...) in LLVM C decompile:\n"
        << content.substr(0, 3000);
    EXPECT_TRUE(content.find("cpuInfo[") != std::string::npos)
        << "Expected cpuInfo array extraction in LLVM C decompile:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, LlvmC_ClflushUsesIntrinsic) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("_mm_clflush") != std::string::npos)
        << "Expected _mm_clflush() in LLVM C decompile (not raw __asm):\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, LlvmC_XgetbvUsesIntrinsic) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("_xgetbv") != std::string::npos)
        << "Expected _xgetbv() in LLVM C decompile:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, LlvmC_RdtscUsesIntrinsic) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("__rdtsc") != std::string::npos)
        << "Expected __rdtsc() in LLVM C decompile:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, LlvmC_FenceUsesIntrinsic) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("_mm_pause") != std::string::npos)
        << "Expected _mm_pause() in LLVM C decompile:\n"
        << content.substr(0, 3000);
    EXPECT_TRUE(content.find("_mm_mfence") != std::string::npos)
        << "Expected _mm_mfence() in LLVM C decompile:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, LlvmC_ReducedVarCount) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    size_t memptr_count = 0;
    size_t pos = 0;
    while ((pos = content.find("memptr", pos)) != std::string::npos) {
        ++memptr_count;
        pos += 6;
    }
    EXPECT_EQ(memptr_count, 0u)
        << "Found intermediate memptr variables — they should be inlined:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, Decompile_CpuidHasOutputArray) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    EXPECT_TRUE(content.find("cpuInfo[") != std::string::npos)
        << "Expected cpuInfo array in HighC cpuid rendering:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, Decompile_XgetbvUsesIntrinsic) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    EXPECT_TRUE(content.find("_xgetbv") != std::string::npos)
        << "Expected _xgetbv() in HighC decompile:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, Decompile_Int3UsesDebugbreak) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    EXPECT_TRUE(content.find("__debugbreak") != std::string::npos)
        << "Expected __debugbreak() in HighC decompile:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, LlvmC_Int3UsesDebugbreak) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("__debugbreak") != std::string::npos)
        << "Expected __debugbreak() in LLVM C decompile:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, LlvmC_StoreForwarding_NoFrameInClflush) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    auto pos = content.find("test_clflush");
    ASSERT_NE(pos, std::string::npos);
    auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
    EXPECT_TRUE(fn_body.find("frame") == std::string::npos)
        << "Store forwarding should eliminate frame array in clflush:\n" << fn_body;
    EXPECT_TRUE(fn_body.find("rsp_init") == std::string::npos)
        << "Store forwarding should eliminate rsp_init in clflush:\n" << fn_body;
}

TEST_F(X86_64_Intrinsics, LlvmC_NoFrameInSimpleFunctions) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    size_t frame_count = 0;
    size_t pos = 0;
    while ((pos = content.find("unsigned char frame", pos)) != std::string::npos) {
        ++frame_count;
        pos += 19;
    }
    EXPECT_EQ(frame_count, 0u)
        << "Found frame array declarations — all should be eliminated by dead store/forwarding:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, Decompile_NoDeadStackStores) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    auto pause_pos = content.find("test_pause_intrinsic");
    ASSERT_NE(pause_pos, std::string::npos);
    auto pause_fn = content.substr(pause_pos, content.find("\n}\n", pause_pos) - pause_pos + 3);
    EXPECT_TRUE(pause_fn.find("*(int") == std::string::npos)
        << "Dead stack stores should be eliminated in pause function:\n" << pause_fn;
}

TEST_F(X86_64_Intrinsics, Decompile_RdtscPreservesCall) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    EXPECT_TRUE(content.find("__rdtsc") != std::string::npos)
        << "Dead store elimination should preserve __rdtsc() call:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, Decompile_XgetbvPreservesCall) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    EXPECT_TRUE(content.find("_xgetbv") != std::string::npos)
        << "Dead store elimination should preserve _xgetbv() call:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, Decompile_VoidInference) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    for (auto fn : {"test_pause_intrinsic", "test_mfence_intrinsic",
                    "test_lfence_intrinsic", "test_sfence_intrinsic",
                    "test_nop_intrinsic", "test_int3_intrinsic"}) {
        auto pos = content.find(fn);
        ASSERT_NE(pos, std::string::npos) << "Function not found: " << fn;
        auto prefix = content.substr(pos > 10 ? pos - 10 : 0, 10);
        EXPECT_TRUE(prefix.find("void") != std::string::npos)
            << fn << " should be detected as void — got:\n"
            << content.substr(pos > 20 ? pos - 20 : 0, 80);
    }
}

TEST_F(X86_64_Intrinsics, LlvmC_VoidInference) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    for (auto fn : {"test_pause_intrinsic", "test_mfence_intrinsic",
                    "test_nop_intrinsic", "test_int3_intrinsic"}) {
        auto pos = content.find(fn);
        ASSERT_NE(pos, std::string::npos) << "Function not found: " << fn;
        auto prefix = content.substr(pos > 10 ? pos - 10 : 0, 10);
        EXPECT_TRUE(prefix.find("void") != std::string::npos)
            << fn << " should be detected as void:\n"
            << content.substr(pos > 20 ? pos - 20 : 0, 80);
    }
}

TEST_F(X86_64_Intrinsics, Decompile_CpuidDeadOutputsRemoved) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    auto pos = content.find("test_cpuid_intrinsic");
    ASSERT_NE(pos, std::string::npos);
    auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
    EXPECT_TRUE(fn_body.find("__cpuid") != std::string::npos)
        << "__cpuid call should be present:\n" << fn_body;
    size_t unused_count = 0;
    for (auto slot : {"cpuInfo[1]", "cpuInfo[2]", "cpuInfo[3]"}) {
        if (fn_body.find(slot) != std::string::npos) ++unused_count;
    }
    EXPECT_EQ(unused_count, 0u)
        << "Unused cpuInfo[1-3] should be removed:\n" << fn_body;
}

TEST_F(X86_64_Intrinsics, Decompile_ClflushUsesParamName) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    auto pos = content.find("test_clflush_intrinsic");
    ASSERT_NE(pos, std::string::npos);
    auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
    EXPECT_TRUE(fn_body.find("_mm_clflush(arg0)") != std::string::npos)
        << "clflush should use param name arg0, not raw register var:\n" << fn_body;
    EXPECT_TRUE(fn_body.find("v5_0") == std::string::npos)
        << "Raw register variable v5_0 should be aliased to arg0:\n" << fn_body;
}

TEST_F(X86_64_Intrinsics, Decompile_NoStaleMultiOutputDstVars) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    for (auto fn : {"test_rdtsc_intrinsic", "test_cpuid_intrinsic",
                    "test_xgetbv_intrinsic"}) {
        auto pos = content.find(fn);
        ASSERT_NE(pos, std::string::npos) << fn << " not found";
        auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
        size_t unused_vars = 0;
        for (auto prefix : {"int64_t v", "uint64_t v", "int32_t v"}) {
            size_t p = 0;
            while ((p = fn_body.find(prefix, p)) != std::string::npos) {
                auto eol = fn_body.find('\n', p);
                auto decl_line = fn_body.substr(p, eol - p);
                if (decl_line.find('=') == std::string::npos &&
                    decl_line.find(';') != std::string::npos) {
                    auto var = decl_line.substr(strlen(prefix));
                    var = var.substr(0, var.find(';'));
                    bool used_elsewhere = false;
                    size_t search = 0;
                    while ((search = fn_body.find(var, search)) != std::string::npos) {
                        if (search != p + strlen(prefix))
                            used_elsewhere = true;
                        search += var.size();
                    }
                    if (!used_elsewhere) ++unused_vars;
                }
                p = eol;
            }
        }
        EXPECT_EQ(unused_vars, 0u)
            << fn << " has stale destination variable declarations:\n" << fn_body;
    }
}

TEST_F(X86_64_Intrinsics, Decompile_ClflushNoFrame) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    auto pos = content.find("test_clflush_intrinsic");
    ASSERT_NE(pos, std::string::npos);
    auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
    EXPECT_TRUE(fn_body.find("frame") == std::string::npos)
        << "HighC clflush should have no frame variables:\n" << fn_body;
    EXPECT_TRUE(fn_body.find("neverd_mem_") == std::string::npos)
        << "Forwarded clflush argument should not retain memory accesses:\n"
        << fn_body;
    EXPECT_TRUE(fn_body.find("_mm_clflush(arg0)") != std::string::npos &&
                fn_body.find("return;") != std::string::npos)
        << "Clflush should remain a void side-effect on arg0:\n" << fn_body;
    EXPECT_TRUE(fn_body.find("rsp") == std::string::npos &&
                fn_body.find("RSP") == std::string::npos)
        << "HighC clflush should have no stack pointer references:\n" << fn_body;
}

TEST_F(X86_64_Intrinsics, Decompile_CpuidReturnMatchesSignature) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    auto pos = content.find("test_cpuid_return_eax");
    ASSERT_NE(pos, std::string::npos) << "test_cpuid_return_eax not found";
    auto sig_start = pos > 20 ? pos - 20 : 0;
    auto fn_sig = content.substr(sig_start, 60);
    auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
    EXPECT_TRUE(fn_body.find("return (int64_t)") == std::string::npos)
        << "Return should NOT cast to int64_t when function returns int32_t:\n" << fn_body;
}

TEST_F(X86_64_Intrinsics, LlvmC_CpuidReturnMatchesSignature) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    auto pos = content.find("test_cpuid_return_eax");
    ASSERT_NE(pos, std::string::npos) << "test_cpuid_return_eax not found";
    auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
    EXPECT_TRUE(fn_body.find("__cpuid") != std::string::npos ||
                fn_body.find("cpuInfo") != std::string::npos)
        << "cpuid should use __cpuid intrinsic:\n" << fn_body;
}

TEST_F(X86_64_Intrinsics, Decompile_CpuidReturnNoDeadOutputs) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    auto pos = content.find("test_cpuid_return_eax");
    ASSERT_NE(pos, std::string::npos);
    auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
    for (auto pat : {"v0 =", "v7_0", "*(int32_t*)"}) {
        EXPECT_TRUE(fn_body.find(pat) == std::string::npos)
            << "Dead frame/store artifacts should be eliminated: " << pat << "\n" << fn_body;
    }
}

TEST_F(X86_64_Intrinsics, Decompile_CpuidReturnAllVarsDeclared) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    auto pos = content.find("test_cpuid_return_eax");
    ASSERT_NE(pos, std::string::npos);
    auto fn_end = content.find("\n}\n", pos);
    auto fn_body = content.substr(pos, fn_end - pos + 3);
    auto body_start = fn_body.find("{\n");
    ASSERT_NE(body_start, std::string::npos);
    auto body = fn_body.substr(body_start + 2);
    size_t line_start = 0;
    while (line_start < body.size()) {
        auto line_end = body.find('\n', line_start);
        if (line_end == std::string::npos) break;
        auto line = body.substr(line_start, line_end - line_start);
        auto eq_pos = line.find(" = ");
        if (eq_pos != std::string::npos && line.find("*(") == std::string::npos &&
            line.find("cpuInfo") == std::string::npos) {
            auto lhs = line.substr(0, eq_pos);
            while (!lhs.empty() && lhs[0] == ' ') lhs = lhs.substr(1);
            if (!lhs.empty() && lhs != "return") {
                EXPECT_TRUE(fn_body.find(lhs + ";") != std::string::npos ||
                            fn_body.find(lhs + " ") != std::string::npos)
                    << "Variable '" << lhs << "' used but possibly undeclared:\n" << fn_body;
            }
        }
        line_start = line_end + 1;
    }
}

TEST_F(X86_64_Intrinsics, Decompile_RdtscReturnCollapsed) {
    auto r = decompileToHighC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    auto pos = content.find("test_rdtsc_return");
    ASSERT_NE(pos, std::string::npos);
    auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
    EXPECT_TRUE(fn_body.find("return __rdtsc()") != std::string::npos)
        << "rdtsc return should be collapsed to 'return __rdtsc()':\n" << fn_body;
    EXPECT_TRUE(fn_body.find("<< 32") == std::string::npos)
        << "rdtsc hi/lo reconstruction should be eliminated:\n" << fn_body;
}

TEST_F(X86_64_Intrinsics, LlvmC_RdtscReturnCollapsed) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    auto pos = content.find("test_rdtsc_return");
    ASSERT_NE(pos, std::string::npos);
    auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
    EXPECT_TRUE(fn_body.find("<< 32") == std::string::npos)
        << "LLVM C: rdtsc hi/lo reconstruction should be collapsed:\n" << fn_body;
}

TEST_F(X86_64_Intrinsics, LlvmC_CpuidNoIntermediateVar) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    auto pos = content.find("test_cpuid_return_eax");
    ASSERT_NE(pos, std::string::npos);
    auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
    EXPECT_TRUE(fn_body.find("cpuInfo[0]") != std::string::npos)
        << "cpuid return should directly use cpuInfo[0]:\n" << fn_body;
}

TEST_F(X86_64_Intrinsics, LlvmC_VoidInference_SideEffectOnly) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    for (auto fn : {"test_rdtsc_intrinsic", "test_cpuid_intrinsic",
                    "test_xgetbv_intrinsic"}) {
        auto pos = content.find(fn);
        ASSERT_NE(pos, std::string::npos) << fn << " not found";
        auto prefix = content.substr(pos > 10 ? pos - 10 : 0, 10);
        EXPECT_TRUE(prefix.find("void") != std::string::npos)
            << fn << " should be void in LLVM C decompile:\n"
            << content.substr(pos > 20 ? pos - 20 : 0, 80);
    }
}

TEST_F(X86_64_Intrinsics, LlvmC_CpuidReturnNotVoid) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    auto pos = content.find("test_cpuid_return_eax");
    ASSERT_NE(pos, std::string::npos);
    auto prefix = content.substr(pos > 10 ? pos - 10 : 0, 10);
    EXPECT_TRUE(prefix.find("void") == std::string::npos)
        << "test_cpuid_return_eax should NOT be void:\n"
        << content.substr(pos > 20 ? pos - 20 : 0, 80);
}

TEST_F(X86_64_Intrinsics, LlvmC_RdtscpUsesIntrinsic) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("__rdtscp") != std::string::npos ||
                content.find("rdtscp") != std::string::npos)
        << "Expected rdtscp-related output in LLVM C decompile:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, LlvmC_SyscallUsesAsm) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("syscall") != std::string::npos)
        << "Expected syscall-related output in LLVM C decompile:\n"
        << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, LlvmC_WrmsrUsesIntrinsic) {
    auto r = decompileToC(obj("test_intrinsics_system.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("__writemsr") != std::string::npos ||
                content.find("wrmsr") != std::string::npos)
        << "Expected wrmsr-related output in LLVM C decompile:\n"
        << content.substr(0, 3000);
}
