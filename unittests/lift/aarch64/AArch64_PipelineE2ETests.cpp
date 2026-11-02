//===- AArch64_PipelineE2ETests.cpp - AArch64 pipeline coverage -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check complete AArch64 lifting, source emission and native patch paths.
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace {
#define NEVERD_A64_PIPELINE_FIXTURE_TEXT(Name, Value)                          \
  constexpr char Name[] = Value;
#include "AArch64PipelineFixtures.def"
} // namespace

class AArch64_PipelineE2E : public NeverDLiftTest {};

static fs::path a64obj(const char *name) {
  return fs::path(TEST_OBJ_DIR) / name;
}

static std::string readText(const fs::path &Path) {
  std::ifstream In(Path);
  std::ostringstream Out;
  Out << In.rdbuf();
  return Out.str();
}

// ============================================================================
// Full pipeline verification for every AArch64 test object
// ============================================================================

#define FULL_PIPELINE_TEST(tag, objname)                                       \
  TEST_F(AArch64_PipelineE2E, FullPipeline_##tag) {                            \
    verifyAllModesSucceed(a64obj(objname));                                    \
  }                                                                            \
  TEST_F(AArch64_PipelineE2E, NoUnreachable_##tag) {                           \
    verifyLLVMIRNoUnreachable(a64obj(objname));                                \
  }

FULL_PIPELINE_TEST(Arith, "test_arith_a64.o")
FULL_PIPELINE_TEST(Cond, "test_cond_a64.o")
FULL_PIPELINE_TEST(Mem, "test_mem_a64.o")
FULL_PIPELINE_TEST(BitShift, "test_bitshift_a64.o")
FULL_PIPELINE_TEST(FP, "test_fp_a64.o")
FULL_PIPELINE_TEST(Logic, "test_logic_a64.o")
FULL_PIPELINE_TEST(MulDiv, "test_muldiv_a64.o")
FULL_PIPELINE_TEST(CSel, "test_csel_a64.o")
FULL_PIPELINE_TEST(Carry, "test_carry_a64.o")
FULL_PIPELINE_TEST(Extend, "test_extend_a64.o")
FULL_PIPELINE_TEST(Atomic, "test_atomic_a64.o")

#undef FULL_PIPELINE_TEST

// ============================================================================
// Decompile mode
// ============================================================================

#define DECOMPILE_TEST(tag, objname)                                           \
  TEST_F(AArch64_PipelineE2E, DecompileC_##tag) {                              \
    verifyDecompileProducesOutput(a64obj(objname));                            \
  }

DECOMPILE_TEST(Arith, "test_arith_a64.o")
DECOMPILE_TEST(Cond, "test_cond_a64.o")
DECOMPILE_TEST(Mem, "test_mem_a64.o")
DECOMPILE_TEST(BitShift, "test_bitshift_a64.o")
DECOMPILE_TEST(FP, "test_fp_a64.o")
DECOMPILE_TEST(Logic, "test_logic_a64.o")
DECOMPILE_TEST(MulDiv, "test_muldiv_a64.o")
DECOMPILE_TEST(CSel, "test_csel_a64.o")
DECOMPILE_TEST(Carry, "test_carry_a64.o")
DECOMPILE_TEST(Extend, "test_extend_a64.o")
DECOMPILE_TEST(Atomic, "test_atomic_a64.o")

#undef DECOMPILE_TEST

// ============================================================================
// HighC decompile
// ============================================================================

TEST_F(AArch64_PipelineE2E, DecompileHighC_Arith) {
  auto r = decompileToHighC(a64obj("test_arith_a64.o"));
  ASSERT_EQ(r.exitCode, 0) << "HighC decompile failed: " << r.err;
}

// Every landing pad the call-site table names keeps its code under its own
// label.  The unwinder enters the pad there, even when one jump enters it as
// well, and when a late pass folds the pad's first statement into the next.
TEST_F(AArch64_PipelineE2E, LandingPadsKeepTheirLabels) {
  auto R = decompileToHighC(a64obj("test_landing_pad_chain_a64.so"));
  ASSERT_EQ(R.exitCode, 0) << "HighC decompile failed: " << R.err;
  const std::string Content = readText(tmpFile("decompiled_high.c"));
  // Each function, how many pads its call-site table names, and a call its
  // pads make.
  const struct {
    const char *Name;
    size_t Pads;
    const char *PadCall;
  } Functions[] = {{"pad_chain", 3, "guard_drop("},
                   {"pad_flag", 1, "release("},
                   {"pad_copy", 1, "_Unwind_Resume("}};
  for (const auto &F : Functions) {
    SCOPED_TRACE(F.Name);
    const size_t Start = Content.find(" " + std::string(F.Name) + "(");
    ASSERT_NE(Start, std::string::npos) << Content;
    // The comment before the definition lists the call sites.
    const size_t Header = Content.rfind("neverd.entry", Start);
    ASSERT_NE(Header, std::string::npos) << Content;
    const std::string Comment = Content.substr(Header, Start - Header);
    const std::string Body =
        Content.substr(Start, Content.find("\n}", Start) - Start);
    const std::regex Pad("pad=0x([0-9A-F]+)");
    size_t Pads = 0;
    for (std::sregex_iterator It(Comment.begin(), Comment.end(), Pad), End;
         It != End; ++It, ++Pads)
      EXPECT_NE(Body.find("L_" + (*It)[1].str() + ":"), std::string::npos)
          << (*It)[0] << "\n"
          << Body;
    EXPECT_EQ(Pads, F.Pads) << Comment;
    EXPECT_NE(Body.find(F.PadCall), std::string::npos) << Body;
  }
}

// ============================================================================
// LLVM IR semantic preservation
// ============================================================================

TEST_F(AArch64_PipelineE2E, Semantic_Add) {
  verifyLLVMIRContains(a64obj("test_arith_a64.o"), "test_add", "add");
}

TEST_F(AArch64_PipelineE2E, Semantic_Sub) {
  verifyLLVMIRContains(a64obj("test_arith_a64.o"), "test_sub", "sub");
}

TEST_F(AArch64_PipelineE2E, Semantic_StoreExists) {
  verifyLLVMIRContains(a64obj("test_mem_a64.o"), "", "store");
}

TEST_F(AArch64_PipelineE2E, Semantic_LoadExists) {
  verifyLLVMIRContains(a64obj("test_mem_a64.o"), "", "load");
}

// ============================================================================
// Conditional branch
// ============================================================================

TEST_F(AArch64_PipelineE2E, CondBranch_Cond) {
  verifyLLVMIRHasConditionalLogic(a64obj("test_cond_a64.o"));
}

// ============================================================================
// Mach-O patch mode (AArch64)
// These smoke fixtures select the implemented DWARF registration path.
// Compact-unwind regeneration remains a separately tracked capability.
// ============================================================================

TEST_F(AArch64_PipelineE2E, PatchMachO_SimpleAdd) {
  auto src = tmpFile("macho_a64.c");
  {
    std::ofstream f(src);
    f << "int add(int a, int b) { return a + b; }\n"
      << "int main(void) { return add(1, 2); }\n";
  }
  auto exe = tmpFile("macho_a64");
  auto cr = exec("clang", {"-O0", "-target", "arm64-apple-macos",
                           "-Wl,-no_compact_unwind", kMachOHeaderPadding, "-o",
                           exe.string(), src.string()});
  if (cr.exitCode != 0)
    GTEST_SKIP() << "Cannot compile arm64 Mach-O on this host";
  verifyPatchProducesOutput(exe);
}

TEST_F(AArch64_PipelineE2E, PatchMachO_Loop) {
  auto src = tmpFile("macho_loop_a64.c");
  {
    std::ofstream f(src);
    f << "int sum(int n) {\n"
      << "    int s = 0;\n"
      << "    for (int i = 0; i < n; i++) s += i;\n"
      << "    return s;\n"
      << "}\n"
      << "int main(void) { return sum(10); }\n";
  }
  auto exe = tmpFile("macho_loop_a64");
  auto cr = exec("clang", {"-O0", "-target", "arm64-apple-macos",
                           "-Wl,-no_compact_unwind", kMachOHeaderPadding, "-o",
                           exe.string(), src.string()});
  if (cr.exitCode != 0)
    GTEST_SKIP() << "Cannot compile arm64 Mach-O";
  verifyPatchProducesOutput(exe);
  verifyLLVMIRHasConditionalLogic(exe);
  verifyNoConstantTrueBranch(exe);
}

TEST_F(AArch64_PipelineE2E, PatchMachO_Recursive) {
  auto src = tmpFile("macho_recur_a64.c");
  {
    std::ofstream f(src);
    f << "int fib(int n) {\n"
      << "    if (n <= 1) return n;\n"
      << "    return fib(n-1) + fib(n-2);\n"
      << "}\n"
      << "int main(void) { return fib(8); }\n";
  }
  auto exe = tmpFile("macho_recur_a64");
  auto cr = exec("clang", {"-O0", "-target", "arm64-apple-macos",
                           "-Wl,-no_compact_unwind", kMachOHeaderPadding, "-o",
                           exe.string(), src.string()});
  if (cr.exitCode != 0)
    GTEST_SKIP() << "Cannot compile arm64 Mach-O";
  verifyPatchProducesOutput(exe);
  verifyLLVMIRHasConditionalLogic(exe);
  verifyLLVMIRNoUnreachable(exe);
}
