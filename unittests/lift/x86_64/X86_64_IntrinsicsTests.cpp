#include "CSourceText.h"
#include "HighCHostExecution.h"
#include "NeverDLiftFixture.h"

#include <cstring>
#include <string_view>

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

void expectIntrinsicStyle(const std::string &Source, bool LLVMPath = false) {
  // Linux SYSCALL has no equivalent MSVC intrinsic: its established C
  // projection uses GNU constraints to preserve the Linux register ABI.
  // LLVM CPU queries also retain their side-effect and memory ordering
  // contract through constrained assembly. HighC keeps its source intrinsics.
  size_t Search = 0;
  unsigned AsmCount = 0;
  while ((Search = Source.find("__asm__ volatile", Search)) !=
         std::string::npos) {
    const auto Asm = std::string_view(Source).substr(Search);
    // Scalar MSR access keeps owned assembly only as the last
    // NEVERD_X86_SCALAR_SYSTEM choice, after the builtin and the MSVC
    // intrinsic, for a compiler with neither.
    const size_t Line = Source.rfind('\n', Search) + 1;
    const bool ScalarSystemFallback =
        Asm.starts_with("__asm__ volatile(\"wrmsr\"") &&
        Source.find("NEVERD_X86_SCALAR_SYSTEM(__builtin_ia32_wrmsr(", Line) <
            Search;
    EXPECT_TRUE(Asm.starts_with("__asm__ volatile(\"syscall\"") ||
                ScalarSystemFallback ||
                (LLVMPath && (Asm.starts_with("__asm__ volatile(\"cpuid\"") ||
                              Asm.starts_with("__asm__ volatile(\"xgetbv\""))))
        << Source.substr(Search, 200);
    Search += std::strlen("__asm__ volatile");
    ++AsmCount;
  }
  // The fixture has two CPUID occurrences, one XGETBV, one SYSCALL and one
  // WRMSR fallback.
  EXPECT_EQ(AsmCount, LLVMPath ? 5u : 2u) << Source;
  for (const char *Name :
       {"test_rdtsc_intrinsic", "test_pause_intrinsic", "test_mfence_intrinsic",
        "test_lfence_intrinsic", "test_sfence_intrinsic",
        "test_cpuid_intrinsic", "test_cpuid_return_eax",
        "test_clflush_intrinsic", "test_xgetbv_intrinsic",
        "test_rdtscp_intrinsic", "test_wrmsr_intrinsic"}) {
    const auto Body = generatedFunction(Source, Name);
    ASSERT_FALSE(Body.empty()) << Name;
    if (LLVMPath && (std::strcmp(Name, "test_cpuid_intrinsic") == 0 ||
                     std::strcmp(Name, "test_cpuid_return_eax") == 0 ||
                     std::strcmp(Name, "test_xgetbv_intrinsic") == 0)) {
      EXPECT_NE(Body.find("__asm__ volatile"), std::string::npos) << Body;
      EXPECT_NE(Body.find(": \"memory\");"), std::string::npos) << Body;
    } else if (std::strcmp(Name, "test_wrmsr_intrinsic") == 0) {
      EXPECT_NE(Body.find("NEVERD_X86_SCALAR_SYSTEM(__builtin_ia32_wrmsr("),
                std::string::npos)
          << Body;
      EXPECT_NE(Body.find("__writemsr("), std::string::npos) << Body;
    } else
      EXPECT_EQ(Body.find("__asm__ volatile"), std::string::npos) << Body;
  }
}

// The objects are ELF, whose CPUID GCC and Clang spell with <cpuid.h>'s
// macros, which store each register into an lvalue operand.
constexpr const char *CpuidStub = R"C(
static unsigned stub_calls;
static int stub_leaf;
static uint32_t stub_words[4];
static void stub_cpuid(unsigned leaf, unsigned *a, unsigned *b, unsigned *c,
                       unsigned *d) {
    _Static_assert(sizeof(unsigned) == sizeof(uint32_t), "CPUID word width");
    ++stub_calls;
    stub_leaf = (int)leaf;
    *a = stub_words[0];
    *b = stub_words[1];
    *c = stub_words[2];
    *d = stub_words[3];
}
#define __cpuid(leaf, a, b, c, d) stub_cpuid((leaf), &(a), &(b), &(c), &(d))
#define __cpuid_count(leaf, subleaf, a, b, c, d) \
    ((void)(subleaf), stub_cpuid((leaf), &(a), &(b), &(c), &(d)))
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
  expectIntrinsicStyle(content);
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
  expectIntrinsicStyle(content, /*LLVMPath=*/true);
}

TEST_F(X86_64_Intrinsics, LlvmC_CpuidRetainsOrderedRegisterContract) {
  auto r = decompileToC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
  auto content = readDecompiledFile("decompiled.c");
  const auto Body = generatedFunction(content, "test_cpuid_intrinsic");
  ASSERT_FALSE(Body.empty());
  EXPECT_NE(Body.find("__asm__ volatile(\"cpuid\""), std::string::npos) << Body;
  for (const char *Operand :
       {"\"=a\"(cpuInfo[0])", "\"=b\"(cpuInfo[1])", "\"=c\"(cpuInfo[2])",
        "\"=d\"(cpuInfo[3])", "\"a\"((uint32_t)(0))", "\"c\"((uint32_t)(0))",
        ": \"memory\""})
    EXPECT_NE(Body.find(Operand), std::string::npos) << Body;
}

TEST_F(X86_64_Intrinsics, LlvmC_ClflushUsesIntrinsic) {
  auto r = decompileToC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
  auto content = readDecompiledFile("decompiled.c");
  EXPECT_TRUE(content.find("_mm_clflush") != std::string::npos)
      << "Expected _mm_clflush() in LLVM C decompile (not raw __asm):\n"
      << content.substr(0, 3000);
}

TEST_F(X86_64_Intrinsics, LlvmC_XgetbvRetainsOrderedRegisterContract) {
  auto r = decompileToC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
  auto content = readDecompiledFile("decompiled.c");
  const auto Body = generatedFunction(content, "test_xgetbv_intrinsic");
  ASSERT_FALSE(Body.empty());
  for (const char *Operand :
       {"__asm__ volatile(\"xgetbv\"", "\"=a\"(xcr[0])", "\"=d\"(xcr[1])",
        "\"c\"((uint32_t)(0))", ": \"memory\""})
    EXPECT_NE(Body.find(Operand), std::string::npos) << Body;
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

// An ELF object's int3 is the GNU builtin: no header declares Microsoft's
// __debugbreak there.
TEST_F(X86_64_Intrinsics, Decompile_Int3UsesDebugTrapBuiltin) {
  auto r = decompileToHighC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
  auto content = readDecompiledFile("decompiled_high.c");
  EXPECT_TRUE(content.find("__builtin_debugtrap()") != std::string::npos)
      << "Expected __builtin_debugtrap() in HighC decompile:\n"
      << content.substr(0, 3000);
  EXPECT_EQ(content.find("__debugbreak"), std::string::npos);
}

TEST_F(X86_64_Intrinsics, LlvmC_Int3UsesDebugTrapBuiltin) {
  auto r = decompileToC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
  auto content = readDecompiledFile("decompiled.c");
  EXPECT_TRUE(content.find("__builtin_debugtrap()") != std::string::npos)
      << "Expected __builtin_debugtrap() in LLVM C decompile:\n"
      << content.substr(0, 3000);
  EXPECT_EQ(content.find("__debugbreak"), std::string::npos);
}

TEST_F(X86_64_Intrinsics, LlvmC_StoreForwarding_NoFrameInClflush) {
  auto r = decompileToC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
  auto content = readDecompiledFile("decompiled.c");
  auto pos = neverd::test::cDefinitionAt(content, "test_clflush_intrinsic");
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
  auto pos = neverd::test::cDefinitionAt(content, "test_cpuid_return_eax");
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
  auto pos = neverd::test::cDefinitionAt(content, "test_rdtsc_return");
  ASSERT_NE(pos, std::string::npos);
  auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
  EXPECT_TRUE(fn_body.find("<< 0x20") == std::string::npos)
      << "LLVM C: rdtsc hi/lo reconstruction should be collapsed:\n"
      << fn_body;
}

TEST_F(X86_64_Intrinsics, LlvmC_CpuidNoIntermediateVar) {
  auto r = decompileToC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
  auto content = readDecompiledFile("decompiled.c");
  auto pos = neverd::test::cDefinitionAt(content, "test_cpuid_return_eax");
  ASSERT_NE(pos, std::string::npos);
  auto fn_body = content.substr(pos, content.find("\n}\n", pos) - pos + 3);
  EXPECT_TRUE(fn_body.find("cpuInfo[0]") != std::string::npos)
      << "cpuid return should directly use cpuInfo[0]:\n"
      << fn_body;
}

TEST_F(X86_64_Intrinsics, LlvmC_ReturnsWhatTheInstructionLeaves) {
  // rdtsc, cpuid and xgetbv leave their result in EAX, which the function
  // returns: both C backends show the value, as IDA does, where a fence's
  // function returns none.
  auto r = decompileToC(obj("test_intrinsics_system.o"));
  ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
  auto content = readDecompiledFile("decompiled.c");
  for (auto fn : {"test_rdtsc_intrinsic", "test_cpuid_intrinsic",
                  "test_xgetbv_intrinsic"}) {
    auto pos = content.find(fn);
    ASSERT_NE(pos, std::string::npos) << fn << " not found";
    auto prefix = content.substr(pos > 10 ? pos - 10 : 0, 10);
    EXPECT_TRUE(prefix.find("void") == std::string::npos)
        << fn << " returns what the instruction leaves in EAX:\n"
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

class X86_64_HighCIntrinsics : public HighCHostExecutionTest {};

// PADDSB, PSUBUSB, PADDUSW and PSUBSW saturate each lane.  HighC computes a
// lane in int32_t and clamps it to the lane's range.
TEST_F(X86_64_HighCIntrinsics, SaturatingLanesRunLikeTheSource) {
  expectHighCRunsLikeSource(
      {"-target", "x86_64-linux-gnu"}, R"C(
#include <emmintrin.h>
int saturate(int a) {
  __m128i x = _mm_set1_epi32((int)((unsigned)a * 0x01030507u));
  __m128i y = _mm_set1_epi32(a ^ 0x7F80FF01);
  __m128i r = _mm_adds_epi8(x, y);
  r = _mm_xor_si128(r, _mm_subs_epu8(x, y));
  r = _mm_xor_si128(r, _mm_adds_epu16(x, y));
  r = _mm_xor_si128(r, _mm_subs_epi16(x, y));
  return _mm_cvtsi128_si32(r) ^ _mm_extract_epi16(r, 5);
}
)C",
      "saturate", {"saturate"},
      {0, 1, -1, 0x7F, -0x80, 0x12345678, 0x40404040, 0x7FFF7FFF, -0x7FFFFFFF});
}

// CRC32 of each operand width prints as the SSE4.2 intrinsic, the function
// taking the target feature the intrinsic needs.
TEST_F(X86_64_HighCIntrinsics, Crc32RunsLikeTheSource) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
#include <nmmintrin.h>
__attribute__((target("sse4.2"))) int checksum(int a) {
  unsigned c = _mm_crc32_u8((unsigned)a, (unsigned char)(a >> 3));
  c = _mm_crc32_u16(c, (unsigned short)((unsigned)a * 3u));
  c = _mm_crc32_u32(c, (unsigned)a ^ 0xDEADBEEFu);
  return (int)_mm_crc32_u64(c, (unsigned long long)(unsigned)a *
                                   0x9E3779B97F4A7C15ull);
}
)C",
                            "checksum", {"checksum"},
                            {0, 1, -1, 0x7F, 0x12345678, -0x7FFFFFFF});
}

// The 32-bit PEXT and PDEP print as the _u32 intrinsics.  The _u64 ones would
// sign-extend int operands, and PEXT would pack the extra mask bits of a
// negative value into the result.
TEST_F(X86_64_HighCIntrinsics, ThirtyTwoBitPextAndPdepRunLikeTheSource) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
__attribute__((target("bmi2"))) int bits(int a) {
  unsigned m = (unsigned)a * 2654435761u | 0x80000001u;
  return (int)(__builtin_ia32_pext_si((unsigned)a, m) +
               __builtin_ia32_pdep_si((unsigned)a, m));
}
)C",
                            "bits", {"bits"},
                            {0, 1, -1, 0x15, 0x27, 0x3F, 0x12345678});
}

// A gather clears each mask element as it loads it: after the first the
// mask is {0, -1, -1, -1}, a 256-bit constant the C writes as the complement
// of 2^64 - 1.  Printed `~-1`, C complemented an int and loaded only the
// first element.
TEST_F(X86_64_HighCIntrinsics, AGatherKeepsTheMaskOfTheElementsLeft) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
typedef int v4si __attribute__((vector_size(16)));
typedef long long v4di __attribute__((vector_size(32)));
__attribute__((target("avx2"))) int gather(int a) {
  unsigned long long t[64], x = (unsigned)a | 1ull;
  for (int i = 0; i < 64; i++) {
    x = x * 6364136223846793005ull + 1442695040888963407ull;
    t[i] = x;
  }
  v4si vi = {a & 63, (a >> 4) & 63, (a >> 8) & 63, (a >> 12) & 63};
  v4di m = {-1, -1, -1, -1}, s = {0, 0, 0, 0};
  v4di g = __builtin_ia32_gatherd_q256(s, (const long long *)t, vi, m, 8);
  unsigned long long r = 0;
  for (int i = 0; i < 4; i++)
    r += (unsigned long long)g[i];
  return (int)(r ^ (r >> 32));
}
)C",
                            "gather", {"gather"},
                            {0, 1, -1, 0x13579, 0x12345678});
}

// VPERMD names its indices before its data, and the lifter keeps that order;
// _mm256_permutevar8x32_epi32 takes the data first.
TEST_F(X86_64_HighCIntrinsics, APermuteByIndexTakesTheDataFirst) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
typedef int v8si __attribute__((vector_size(32)));
__attribute__((target("avx2"))) int permd(int a) {
  v8si b = {a, a, a, a, a, a, a, a};
  v8si io = {0, 1, 2, 3, 4, 5, 6, 7};
  v8si x = b * 7 + io, idx = (b + io) & 7;
  v8si z = __builtin_ia32_permvarsi256(x, idx);
  return z[0] + z[1] * 2 + z[2] * 3 + z[3] * 4 + z[4] * 5 + z[5] * 6 +
         z[6] * 7 + z[7] * 8;
}
)C",
                            "permd", {"permd"}, {0, 1, -1, 0x4d, 77, 0x12345});
}

// `xor edx, edx; div rsi` divides the zero-extended rax.  The guard that
// traps where the quotient does not fit took the dividend's signed source,
// so a dividend with its top bit set extended by its sign and trapped.
TEST_F(X86_64_HighCIntrinsics, AnUnsignedDivisionGuardZeroExtends) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
long udivide(long a, long b) __attribute__((noinline));
int quotient(int a) {
  return (int)udivide((long)(0xFFFFFFFF00000000ul ^ (unsigned)a),
                      (long)((a & 0xFF) + 1));
}
long udivide(long a, long b) {
  unsigned long ua = (unsigned long)a, ub = (unsigned long)b;
  if (ub == 0)
    return 0;
  return (long)(ua / ub);
}
)C",
                            "quotient", {"quotient", "udivide"},
                            {0, 1, -1, 0x7F, 0x12345678});
}

// MULSD and ADDSS read their constant-pool operand through the address they
// take, so each constant is the double or float the instruction reads.
TEST_F(X86_64_HighCIntrinsics, ConstantPoolOperandsKeepTheirWidth) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
int scale(int a) {
  double x = (double)a * 1e-4;
  float y = (float)a + 2.5f;
  unsigned long long b;
  unsigned c;
  __builtin_memcpy(&b, &x, 8);
  __builtin_memcpy(&c, &y, 4);
  return (int)(b ^ (b >> 32) ^ c);
}
)C",
                            "scale", {"scale"},
                            {0, 1, -1, 31416, 0x12345, -0x7FFFFFFF});
}

// An instruction past the x86-64 baseline prints as its intrinsic in a
// function that takes the intrinsic's target feature, so the C compiles with
// no -m option.
TEST_F(X86_64_Intrinsics, Decompile_IntrinsicsTakeTheirTargetFeature) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "assembling x86-64 requires Clang";
  const auto Source = tmpFile("beyond_baseline.s");
  const auto Object = tmpFile("beyond_baseline.o");
  std::ofstream(Source) << R"S(
.text
.globl beyond_baseline
.type beyond_baseline,@function
beyond_baseline:
  pdep %rsi, %rdi, %rax
  pext %rsi, %rax, %rax
  movq %rax, %xmm0
  movq %rsi, %xmm1
  pshufb %xmm1, %xmm0
  aesenc %xmm1, %xmm0
  movq %xmm0, %rax
  ret
.size beyond_baseline,.-beyond_baseline
)S";
  const auto Assembled =
      exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-c",
                               Source.string(), "-o", Object.string()});
  ASSERT_EQ(Assembled.exitCode, 0) << Assembled.err;
  auto r = decompileToHighC(Object);
  ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
  const std::string C = readDecompiledFile("decompiled_high.c");
  for (const char *Name :
       {"_pdep_u64(", "_pext_u64(", "_mm_shuffle_epi8(", "_mm_aesenc_si128("})
    EXPECT_NE(C.find(Name), std::string::npos) << Name << "\n" << C;
  EXPECT_EQ(C.find("unknown"), std::string::npos) << C;
  const auto CFile = tmpFile("beyond_baseline_high.c");
  std::ofstream(CFile) << C;
  const auto Compiled = checkHighCClangCompile(
      CFile, {"-target", "x86_64-linux-gnu", "-ffreestanding", "-std=gnu11"});
  EXPECT_EQ(Compiled.exitCode, 0) << Compiled.err << "\n" << C;
}

// CMPXCHG16B faults on an address that is not 16-byte aligned; the C traps
// there before the exchange.
TEST_F(X86_64_Intrinsics, Decompile_AlignmentPreconditionTraps) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "assembling x86-64 requires Clang";
  const auto Source = tmpFile("cas16.s");
  const auto Object = tmpFile("cas16.o");
  std::ofstream(Source) << R"S(
.text
.globl cas16
.type cas16,@function
cas16:
  pushq %rbx
  movq %rsi, %rax
  movq %rdx, %rbx
  xorl %edx, %edx
  xorl %ecx, %ecx
  lock cmpxchg16b (%rdi)
  sete %al
  movzbl %al, %eax
  popq %rbx
  ret
.size cas16,.-cas16
)S";
  const auto Assembled =
      exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-c",
                               Source.string(), "-o", Object.string()});
  ASSERT_EQ(Assembled.exitCode, 0) << Assembled.err;
  auto r = decompileToHighC(Object);
  ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
  const std::string C = readDecompiledFile("decompiled_high.c");
  EXPECT_NE(C.find("& 15) != 0)\n"), std::string::npos) << C;
  EXPECT_NE(C.find("__builtin_trap();"), std::string::npos) << C;
  EXPECT_EQ(C.find("neverd_require_aligned"), std::string::npos) << C;
  const auto CFile = tmpFile("cas16_high.c");
  std::ofstream(CFile) << C;
  const auto Compiled =
      checkHighCClangCompile(CFile, {"-target", "x86_64-linux-gnu", "-mcx16",
                                     "-ffreestanding", "-std=gnu11"});
  EXPECT_EQ(Compiled.exitCode, 0) << Compiled.err << "\n" << C;
}

// A function enters at 8 mod 16, so `movaps` to a slot 24 bytes below the
// entry is aligned: the C frame storage gives the slot that alignment, and
// its check, which would always pass, is not printed.  A slot 16 bytes below
// the entry is misaligned, and its check stays.
TEST_F(X86_64_Intrinsics, Decompile_AnAlignedFrameSlotNeedsNoCheck) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "assembling x86-64 requires Clang";
  const auto Source = tmpFile("spill16.s");
  const auto Object = tmpFile("spill16.o");
  std::ofstream(Source) << R"S(
.text
.globl spill16
.type spill16,@function
spill16:
  subq $24, %rsp
  movd %edi, %xmm0
  pshufd $0, %xmm0, %xmm0
  movaps %xmm0, (%rsp)
  movl 4(%rsp), %eax
  addl (%rsp), %eax
  addq $24, %rsp
  ret
.size spill16,.-spill16
.globl spill8
.type spill8,@function
spill8:
  subq $16, %rsp
  movd %edi, %xmm0
  pshufd $0, %xmm0, %xmm0
  movaps %xmm0, (%rsp)
  movl 4(%rsp), %eax
  addl (%rsp), %eax
  addq $16, %rsp
  ret
.size spill8,.-spill8
)S";
  const auto Assembled =
      exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-c",
                               Source.string(), "-o", Object.string()});
  ASSERT_EQ(Assembled.exitCode, 0) << Assembled.err;
  auto r = decompileToHighC(Object);
  ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
  const std::string C = readDecompiledFile("decompiled_high.c");
  const size_t Aligned = C.find("spill16(int32_t arg0) {");
  const size_t Misaligned = C.find("spill8(int32_t arg0) {");
  ASSERT_NE(Aligned, std::string::npos) << C;
  ASSERT_NE(Misaligned, std::string::npos) << C;
  const auto Body = [&](size_t Begin) {
    return C.substr(Begin, C.find("\n}\n", Begin) - Begin);
  };
  EXPECT_EQ(Body(Aligned).find("& 15) != 0"), std::string::npos) << C;
  EXPECT_NE(Body(Misaligned).find("& 15) != 0"), std::string::npos) << C;
  const auto CFile = tmpFile("spill16_high.c");
  std::ofstream(CFile) << C;
  const auto Compiled = checkHighCClangCompile(
      CFile, {"-target", "x86_64-linux-gnu", "-ffreestanding", "-std=gnu11"});
  EXPECT_EQ(Compiled.exitCode, 0) << Compiled.err << "\n" << C;
}

// The magnitude mask of an fabs is a double constant that `vmovddup` loads
// from memory and broadcasts, which HighC folds to its eight bytes: the
// intrinsic reads only those, and they widen to its vector operand.
TEST_F(X86_64_HighCIntrinsics, ABroadcastConstantWidensToItsVector) {
  HostFlags = {"-mfma"};
  expectHighCRunsLikeSource(
      {"-target", "x86_64-linux-gnu", "-mfma", "-fno-math-errno"}, R"C(
int fma_chain(int a) {
  unsigned h = (unsigned)a, acc = 0;
  double s = 1.0;
  for (int i = 0; i < 64; i++) {
    h = h * 1103515245u + 12345u;
    double x = (double)(int)(h & 0x1FFFF) * 0.0000131 - 3.0;
    double y = (double)(int)((h >> 8) & 0x1FFFF) * 0.0000071 + 1.0;
    s = __builtin_fma(x, y, s);
    if (s > 1e9 || s < -1e9)
      s *= 0.0009765625;
    unsigned long long bb;
    __builtin_memcpy(&bb, &s, 8);
    acc = acc * 131u + (unsigned)bb + (unsigned)(bb >> 32);
  }
  return (int)acc;
}
)C",
      "fma_chain", {"fma_chain"}, {0, 1, -1, 0x4567, 77});
}

namespace {

class X86_64_SystemRegister : public HighCHostExecutionTest {};

// `str %eax` stores the task register's selector, zero extended, in the
// register.  The C runs the instruction itself on a value of that width
// through GNU inline asm and returns it.  Nothing executes here: a CPU with
// UMIP refuses STR in user mode.
TEST_F(X86_64_SystemRegister, AStoredSelectorIsTheValueReturned) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const auto Source = tmpFile("kernel.c");
  const auto Object = tmpFile("kernel.o");
  std::ofstream(Source) << "unsigned long str32(unsigned long x) {\n"
                           "  __asm__ volatile(\"str %k0\" : \"+r\"(x));\n"
                           "  return x;\n"
                           "}\n";
  const auto Compiled =
      exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-O1", "-c",
                               Source.string(), "-o", Object.string()});
  ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;
  const auto Decompiled = decompileToHighC(Object);
  ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  EXPECT_NE(C.find("__asm__ volatile(\"str %0\" : \"=r\"("), std::string::npos)
      << C;
  EXPECT_EQ(C.find("void str32"), std::string::npos) << C;
  const auto HostSource = tmpFile("host.c");
  std::ofstream(HostSource) << C;
  const auto Built =
      exec(NEVERD_TEST_CLANG, {"-std=gnu11", "-c", HostSource.string(), "-o",
                               tmpFile("host.o").string()});
  EXPECT_EQ(Built.exitCode, 0) << Built.err << "\n" << C;
}

} // namespace
