#include "NeverDLiftFixture.h"

namespace {

// Keep the generated declaration and body intact. Only the named intrinsic is
// replaced by a controlled test implementation; no x86 instructions execute.
std::string generatedFunction(const std::string &Source,
                              const std::string &Name) {
  size_t Search = 0;
  while ((Search = Source.find(Name + "(", Search)) != std::string::npos) {
    size_t Start = Source.rfind('\n', Search);
    Start = Start == std::string::npos ? 0 : Start + 1;
    size_t Open = Source.find('{', Search);
    size_t Semicolon = Source.find(';', Search);
    if (Open == std::string::npos ||
        (Semicolon != std::string::npos && Semicolon < Open)) {
      Search += Name.size();
      continue;
    }
    unsigned Depth = 0;
    for (size_t I = Open; I < Source.size(); ++I) {
      if (Source[I] == '{')
        ++Depth;
      else if (Source[I] == '}' && --Depth == 0)
        return Source.substr(Start, I + 1 - Start);
    }
    return {};
  }
  return {};
}

constexpr const char *CpuidStub = R"C(
static unsigned stub_calls;
static int stub_leaf;
static uint32_t stub_words[4];
static void stub_cpuid(int output[4], int leaf) {
    _Static_assert(sizeof(int) == sizeof(uint32_t), "CPUID word width");
    ++stub_calls;
    stub_leaf = leaf;
    memcpy(output, stub_words, sizeof(stub_words));
}
#define __cpuid stub_cpuid
)C";

constexpr const char *ClflushStub = R"C(
static volatile unsigned stub_calls;
static uintptr_t stub_address;
static void stub_clflush(uintptr_t address) {
    ++stub_calls;
    stub_address = address;
}
#define _mm_clflush(address) stub_clflush((uintptr_t)(address))
)C";

} // namespace

class X86_64_Intrinsics : public NeverDLiftTest {
protected:
  std::string readDecompiledFile(const std::string &fname) {
    auto f = tmpFile(fname);
    std::ifstream ifs(f);
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
  }

  void expectGeneratedIntrinsicRuns(const std::string &Source,
                                    const std::string &Name,
                                    const std::string &Support,
                                    const std::string &Driver) {
    const auto Function = generatedFunction(Source, Name);
    ASSERT_FALSE(Function.empty()) << "missing definition of " << Name;
    std::string Helpers;
    size_t Search = 0;
    while ((Search = Source.find("static inline ", Search)) !=
           std::string::npos) {
      const size_t End = Source.find('\n', Search);
      const size_t NameStart = Source.find("neverd_mem_", Search);
      const size_t Open = Source.find('(', Search);
      if (NameStart < End && Open < End && NameStart < Open) {
        const auto Helper = generatedFunction(
            Source, Source.substr(NameStart, Open - NameStart));
        ASSERT_FALSE(Helper.empty());
        Helpers += Helper + "\n";
      }
      Search += 14;
    }

    const fs::path CPath = tmpFile("intrinsic_execution.c");
    std::ofstream Out(CPath);
    Out << "#include <stdint.h>\n#include <stddef.h>\n"
           "#include <string.h>\n#include <stdio.h>\n"
        << Support << Helpers << Function << '\n'
        << Driver;
    Out.close();
    ASSERT_TRUE(Out.good());
    for (const std::string Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      const fs::path Program = tmpFile("intrinsic_execution.exe");
      RunResult Compile =
          exec("clang", {"-std=c11", Optimization, CPath.string(), "-o",
                         Program.string()});
      ASSERT_EQ(Compile.exitCode, 0) << Compile.err << '\n' << Function;
      RunResult Execute = exec(Program.string(), {});
      EXPECT_EQ(Execute.exitCode, 0) << Execute.err << '\n' << Function;
    }
  }
};

static fs::path obj(const char *name) { return fs::path(TEST_OBJ_DIR) / name; }

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
      << "Found __nd_ extern stub in LLVM IR — all intrinsics should be "
         "inline:\n"
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
      << "Found GNU __asm__ volatile in x86 decompile — should use MSVC "
         "__asm{} or C intrinsics:\n"
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
      << "Expected _mm_pause() in decompiled C:\n"
      << content.substr(0, 3000);
  EXPECT_TRUE(content.find("_mm_mfence") != std::string::npos)
      << "Expected _mm_mfence() in decompiled C:\n"
      << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, LlvmC_NoNdStubs) {
  auto r = decompileToC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
  auto content = readDecompiledFile("decompiled.c");
  EXPECT_FALSE(content.empty()) << "LLVM C decompiled output is empty";
  EXPECT_TRUE(content.find("__nd_") == std::string::npos)
      << "Found __nd_ in LLVM C decompile — should use C intrinsics or "
         "__asm{}:\n"
      << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, LlvmC_UsesCorrectStyle) {
  auto r = decompileToC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
  auto content = readDecompiledFile("decompiled.c");
  EXPECT_TRUE(content.find("__asm__ volatile") == std::string::npos)
      << "Found GNU __asm__ volatile in x86 LLVM C decompile — should use MSVC "
         "__asm{} or C intrinsics:\n"
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
      << "Store forwarding should eliminate frame array in clflush:\n"
      << fn_body;
  EXPECT_TRUE(fn_body.find("rsp_init") == std::string::npos)
      << "Store forwarding should eliminate rsp_init in clflush:\n"
      << fn_body;
}

TEST_F(X86_64_Intrinsics, LlvmC_NoFrameInSimpleFunctions) {
  auto r = decompileToC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
  auto content = readDecompiledFile("decompiled.c");
  size_t frame_count = 0;
  size_t pos = 0;
  while ((pos = content.find("unsigned char frame", pos)) !=
         std::string::npos) {
    ++frame_count;
    pos += 19;
  }
  EXPECT_EQ(frame_count, 0u) << "Found frame array declarations — all should "
                                "be eliminated by dead store/forwarding:\n"
                             << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, Decompile_NoDeadStackStores) {
  auto r = decompileToHighC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
  auto content = readDecompiledFile("decompiled_high.c");
  auto pause_pos = content.find("test_pause_intrinsic");
  ASSERT_NE(pause_pos, std::string::npos);
  auto pause_fn = content.substr(pause_pos, content.find("\n}\n", pause_pos) -
                                                pause_pos + 3);
  EXPECT_TRUE(pause_fn.find("*(int") == std::string::npos)
      << "Dead stack stores should be eliminated in pause function:\n"
      << pause_fn;
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

TEST_F(X86_64_Intrinsics, Decompile_CpuidExecutesOnceWithRequestedLeaf) {
  auto r = decompileToHighC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
  const auto content = readDecompiledFile("decompiled_high.c");
  expectGeneratedIntrinsicRuns(content, "test_cpuid_intrinsic", CpuidStub, R"C(
int main(void) {
    for (unsigned i = 0; i < 7; ++i) {
        stub_calls = 0;
        stub_leaf = -1;
        for (unsigned j = 0; j < 4; ++j)
            stub_words[j] = UINT32_C(0x80000001) ^ (i * 37 + j * 101);
        (void)test_cpuid_intrinsic();
        if (stub_calls != 1 || stub_leaf != 0) {
            fprintf(stderr, "CPUID side effect: calls=%u leaf=%d\n", stub_calls, stub_leaf);
            return 1;
        }
    }
    return 0;
}
)C");
}

TEST_F(X86_64_Intrinsics, Decompile_ClflushPreservesPointerArgument) {
  auto r = decompileToHighC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
  const auto content = readDecompiledFile("decompiled_high.c");
  expectGeneratedIntrinsicRuns(content, "test_clflush_intrinsic", ClflushStub,
                               R"C(
int main(void) {
    unsigned char memory[65] = {0};
    const unsigned offsets[] = {0, 1, 31, 32, 63, 64};
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); ++i) {
        uintptr_t address = (uintptr_t)(memory + offsets[i]);
        stub_calls = 0;
        stub_address = 0;
        (void)test_clflush_intrinsic((int64_t)address);
        if (stub_calls != 1 || stub_address != address) {
            fprintf(stderr, "CLFLUSH pointer offset=%u calls=%u\n", offsets[i], stub_calls);
            return 1;
        }
    }
    return 0;
}
)C");
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
          if (!used_elsewhere)
            ++unused_vars;
        }
        p = eol;
      }
    }
    EXPECT_EQ(unused_vars, 0u)
        << fn << " has stale destination variable declarations:\n"
        << fn_body;
  }
}

TEST_F(X86_64_Intrinsics, Decompile_ClflushRetainsEffectWithSavedArgument) {
  auto r = decompileToHighC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
  const auto content = readDecompiledFile("decompiled_high.c");
  expectGeneratedIntrinsicRuns(content, "test_clflush_intrinsic", ClflushStub,
                               R"C(
int main(void) {
    unsigned char memory[128];
    unsigned char original[128];
    for (unsigned i = 0; i < sizeof(memory); ++i)
        memory[i] = (unsigned char)(i * 3 + 7);
    memcpy(original, memory, sizeof(memory));
    stub_calls = 0;
    for (unsigned i = 0; i < 17; ++i) {
        uintptr_t address = (uintptr_t)(memory + i * 7);
        (void)test_clflush_intrinsic((int64_t)address);
        if (stub_calls != i + 1 || stub_address != address ||
            memcmp(memory, original, sizeof(memory)) != 0) {
            fprintf(stderr, "CLFLUSH effect or caller memory changed at call %u\n", i);
            return 1;
        }
    }
    return 0;
}
)C");
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
      << "Return should NOT cast to int64_t when function returns int32_t:\n"
      << fn_body;
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
      << "cpuid should use __cpuid intrinsic:\n"
      << fn_body;
}

TEST_F(X86_64_Intrinsics, Decompile_CpuidReturnPreservesEaxBitPattern) {
  auto r = decompileToHighC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
  const auto content = readDecompiledFile("decompiled_high.c");
  expectGeneratedIntrinsicRuns(content, "test_cpuid_return_eax", CpuidStub, R"C(
int main(void) {
    const uint32_t inputs[] = {0, 1, UINT32_C(0x7fffffff), UINT32_C(0x80000000),
                               UINT32_C(0x89abcdef), UINT32_C(0xffffffff)};
    for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
        stub_calls = 0;
        stub_leaf = -1;
        stub_words[0] = inputs[i];
        stub_words[1] = inputs[i] ^ UINT32_C(0xa5a5a5a5);
        stub_words[2] = inputs[i] ^ UINT32_C(0x5a5a5a5a);
        stub_words[3] = ~inputs[i];
        uint32_t result = (uint32_t)test_cpuid_return_eax();
        if (stub_calls != 1 || stub_leaf != 0 || result != inputs[i]) {
            fprintf(stderr, "CPUID input=%08x result=%08x calls=%u leaf=%d\n",
                    inputs[i], result, stub_calls, stub_leaf);
            return 1;
        }
    }
    return 0;
}
)C");
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
    if (line_end == std::string::npos)
      break;
    auto line = body.substr(line_start, line_end - line_start);
    auto eq_pos = line.find(" = ");
    if (eq_pos != std::string::npos && line.find("*(") == std::string::npos &&
        line.find("cpuInfo") == std::string::npos) {
      auto lhs = line.substr(0, eq_pos);
      while (!lhs.empty() && lhs[0] == ' ')
        lhs = lhs.substr(1);
      if (!lhs.empty() && lhs != "return") {
        EXPECT_TRUE(fn_body.find(lhs + ";") != std::string::npos ||
                    fn_body.find(lhs + " ") != std::string::npos)
            << "Variable '" << lhs << "' used but possibly undeclared:\n"
            << fn_body;
      }
    }
    line_start = line_end + 1;
  }
}

TEST_F(X86_64_Intrinsics, Decompile_RdtscReturnPreservesAllBitsOnce) {
  auto r = decompileToHighC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
  const auto content = readDecompiledFile("decompiled_high.c");
  expectGeneratedIntrinsicRuns(content, "test_rdtsc_return", R"C(
static uint64_t stub_timestamp;
static unsigned stub_calls;
static uint64_t stub_rdtsc(void) {
    ++stub_calls;
    return stub_timestamp;
}
#define __rdtsc stub_rdtsc
)C",
                               R"C(
int main(void) {
    const uint64_t inputs[] = {
        0, 1, UINT64_C(0x112233447fffffff), UINT64_C(0x1122334480000001),
        UINT64_C(0xffffffff00000000), UINT64_C(0x8000000000000000),
        UINT64_C(0xffffffffffffffff)
    };
    for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
        stub_timestamp = inputs[i];
        stub_calls = 0;
        uint64_t result = (uint64_t)test_rdtsc_return();
        if (stub_calls != 1 || result != inputs[i]) {
            fprintf(stderr, "RDTSC input=%016llx result=%016llx calls=%u\n",
                    (unsigned long long)inputs[i], (unsigned long long)result, stub_calls);
            return 1;
        }
    }
    return 0;
}
)C");
}

TEST_F(X86_64_Intrinsics, LlvmC_RdtscReturnCollapsed) {
  auto r = decompileToC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
  auto content = readDecompiledFile("decompiled.c");
  auto pos = content.find("test_rdtsc_return");
  ASSERT_NE(pos, std::string::npos);
  auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
  EXPECT_TRUE(fn_body.find("<< 32") == std::string::npos)
      << "LLVM C: rdtsc hi/lo reconstruction should be collapsed:\n"
      << fn_body;
}

TEST_F(X86_64_Intrinsics, LlvmC_CpuidNoIntermediateVar) {
  auto r = decompileToC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
  auto content = readDecompiledFile("decompiled.c");
  auto pos = content.find("test_cpuid_return_eax");
  ASSERT_NE(pos, std::string::npos);
  auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
  EXPECT_TRUE(fn_body.find("cpuInfo[0]") != std::string::npos)
      << "cpuid return should directly use cpuInfo[0]:\n"
      << fn_body;
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
