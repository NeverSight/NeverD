#include "NeverDLiftFixture.h"

class X86_64_PipelineE2E : public NeverDLiftTest {};

static fs::path obj(const char *name) { return fs::path(TEST_OBJ_DIR) / name; }

// ============================================================================
// Full pipeline: AllStages + Decompile + Verifier + NoBranchTrue +
// NoUnreachable Every test object must pass ALL checks to ensure semantic
// preservation from binary → LowIR → MedIR → HighIR → LLVM IR
// ============================================================================

#define FULL_PIPELINE_TEST(suite, tag, objname)                                \
  TEST_F(suite, FullPipeline_##tag) { verifyAllModesSucceed(obj(objname)); }   \
  TEST_F(suite, NoUnreachable_##tag) {                                         \
    verifyLLVMIRNoUnreachable(obj(objname));                                   \
  }

FULL_PIPELINE_TEST(X86_64_PipelineE2E, Arith, "test_arith.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, MulDiv, "test_muldiv.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, ControlFlow, "test_control_flow.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, StackMov, "test_stack_mov.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, SseFp, "test_sse_fp.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, CarryOps, "test_carry_ops.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, BitOps, "test_bitops.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, SetccCmov, "test_setcc_cmov.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, SignExt, "test_signext.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, StringOps, "test_string_ops.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, RotateShift, "test_rotate_shift.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, Misc, "test_misc.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, SemanticE2E, "test_semantic_e2e.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, ControlSemantics,
                   "test_control_semantics.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, Bmi, "test_bmi.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, AdxMisc, "test_adx_misc.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, SseAvx, "test_sse_avx.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, FlagOps, "test_flag_ops.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, DoubleShift, "test_double_shift.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, FlagsMisc, "test_flags_misc.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, FpConvert, "test_fp_convert.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, RclRcr, "test_rcl_rcr.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, CmpxchgXadd, "test_cmpxchg_xadd.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, SignConv, "test_sign_conv.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, LoopBit, "test_loop.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, FmaMisc, "test_fma_misc.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, X87FPU, "test_x87_fpu.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, LodsMisc, "test_lods_misc.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, SseMisc, "test_sse_misc.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, GlobalVolatile, "test_global_volatile.o")

#undef FULL_PIPELINE_TEST

// This mixed fixture also observes an interior code address. Successful LLVM
// verification is distinct from a portable C representation of that value.
TEST_F(X86_64_PipelineE2E, LiftStages_MemAtomic) {
  verifyAllStages(obj("test_mem_atomic.o"));
  verifyLLVMIRNoVerifierErrors(obj("test_mem_atomic.o"));
  verifyNoConstantTrueBranch(obj("test_mem_atomic.o"));
}

TEST_F(X86_64_PipelineE2E, NoUnreachable_MemAtomic) {
  verifyLLVMIRNoUnreachable(obj("test_mem_atomic.o"));
}

// ============================================================================
// Decompile mode: all objects produce valid C output
// ============================================================================

#define DECOMPILE_TEST(tag, objname)                                           \
  TEST_F(X86_64_PipelineE2E, DecompileC_##tag) {                               \
    verifyDecompileProducesOutput(obj(objname));                               \
  }

DECOMPILE_TEST(Arith, "test_arith.o")
DECOMPILE_TEST(MulDiv, "test_muldiv.o")
DECOMPILE_TEST(ControlFlow, "test_control_flow.o")
DECOMPILE_TEST(StackMov, "test_stack_mov.o")
DECOMPILE_TEST(BitOps, "test_bitops.o")
DECOMPILE_TEST(SemanticE2E, "test_semantic_e2e.o")
DECOMPILE_TEST(ControlSemantics, "test_control_semantics.o")
DECOMPILE_TEST(SseFp, "test_sse_fp.o")
DECOMPILE_TEST(CarryOps, "test_carry_ops.o")
DECOMPILE_TEST(SignExt, "test_signext.o")
DECOMPILE_TEST(SetccCmov, "test_setcc_cmov.o")
DECOMPILE_TEST(FlagOps, "test_flag_ops.o")
DECOMPILE_TEST(FlagsMisc, "test_flags_misc.o")
DECOMPILE_TEST(RotateShift, "test_rotate_shift.o")
DECOMPILE_TEST(DoubleShift, "test_double_shift.o")
DECOMPILE_TEST(Bmi, "test_bmi.o")
DECOMPILE_TEST(CmpxchgXadd, "test_cmpxchg_xadd.o")
DECOMPILE_TEST(FpConvert, "test_fp_convert.o")
DECOMPILE_TEST(RclRcr, "test_rcl_rcr.o")
DECOMPILE_TEST(AdxMisc, "test_adx_misc.o")
DECOMPILE_TEST(SseAvx, "test_sse_avx.o")
DECOMPILE_TEST(Misc, "test_misc.o")
DECOMPILE_TEST(StringOps, "test_string_ops.o")
DECOMPILE_TEST(SignConv, "test_sign_conv.o")
DECOMPILE_TEST(LoopBit, "test_loop.o")
DECOMPILE_TEST(FmaMisc, "test_fma_misc.o")
DECOMPILE_TEST(X87FPU, "test_x87_fpu.o")
DECOMPILE_TEST(LodsMisc, "test_lods_misc.o")
DECOMPILE_TEST(SseMisc, "test_sse_misc.o")
DECOMPILE_TEST(GlobalVolatile, "test_global_volatile.o")

#undef DECOMPILE_TEST

// MinGW-w64's startup code guards its call with a scope that resumes at the
// `nop` the call falls into, and the callee's _fpreset carries x87 state back
// across the call. The x87 context handle must not keep the establisher frame
// from being proved, and the __except arm must not jump to a label that no
// statement carries.
TEST_F(X86_64_PipelineE2E, DecompileC_MinGWStartupSEHWithX87Callee) {
  const auto PE = obj("test_mingw_startup_seh.exe");
  if (!fs::exists(PE))
    GTEST_SKIP() << "MinGW-style SEH fixture not built (lld-link unavailable)";
  const auto Whole = decompileToHighC(PE);
  ASSERT_EQ(Whole.exitCode, 0) << Whole.err;

  const auto Out = tmpFile("mingw_entry.c");
  const auto R = exec(ndBin(), {"decompile", "--func", "mingw_entry", "-o",
                                Out.string(), PE.string()});
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::ifstream Input(Out);
  ASSERT_TRUE(Input.good());
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  EXPECT_NE(C.find("__try {"), std::string::npos) << C;
  EXPECT_NE(C.find("} __except ("), std::string::npos) << C;
  // The handle the x87 state travels in, the temporary the proof admits.
  EXPECT_NE(C.find("neverd_x87_export_context"), std::string::npos) << C;
  const std::regex Goto(R"(goto (L_[0-9A-F]+);)");
  for (auto It = std::sregex_iterator(C.begin(), C.end(), Goto);
       It != std::sregex_iterator(); ++It)
    EXPECT_NE(C.find((*It)[1].str() + ":"), std::string::npos)
        << (*It)[1].str() << " has no label:\n"
        << C;
}

TEST_F(X86_64_PipelineE2E,
       DecompileC_InteriorCodeAddressRefusesWithoutReplacingOutput) {
  for (bool NoOpt : {false, true})
    for (bool Selected : {false, true}) {
      SCOPED_TRACE(NoOpt);
      SCOPED_TRACE(Selected);
      const auto Out = tmpFile("interior-address.c");
      const std::string Sentinel = "existing output must survive refusal\n";
      std::ofstream(Out) << Sentinel;
      std::vector<std::string> Args = {"decompile", "--llvm", "-o",
                                       Out.string(),
                                       obj("test_mem_atomic.o").string()};
      if (NoOpt)
        Args.push_back("--no-opt");
      if (Selected)
        Args.push_back("--func=test_lea_rip_relative");
      auto Result = exec(ndBin(), Args);
      std::ifstream File(Out);
      std::ostringstream Content;
      Content << File.rdbuf();
      if (!Selected) {
        // The whole unit leaves the definition out and names why.
        ASSERT_EQ(Result.exitCode, 0) << Result.err;
        EXPECT_NE(Content.str().find(
                      "/* neverd.not-decompiled: test_lea_rip_relative at "
                      "0x100: c-emission-failed; LLVM C constant is not "
                      "supported: ptr blockaddress(@test_lea_rip_relative"),
                  std::string::npos)
            << Content.str();
        continue;
      }
      ASSERT_NE(Result.exitCode, 0);
      EXPECT_TRUE(Result.errContains("LLVM C constant is not supported"))
          << Result.err;
      EXPECT_TRUE(Result.errContains("blockaddress(@test_lea_rip_relative"))
          << Result.err;
      EXPECT_EQ(Content.str(), Sentinel);
    }
}

TEST_F(X86_64_PipelineE2E, DecompileC_MemAtomicSupportedFunctions) {
  for (const char *Function :
       {"test_xadd", "test_cmpxchg_success", "test_lock_add", "test_lock_inc",
        "test_lock_dec", "test_lea_complex", "test_prefetch", "test_clflush",
        "test_nop_variants", "test_movabs", "_start"})
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(Function);
      SCOPED_TRACE(NoOpt);
      auto Out =
          tmpFile(std::string(Function) + (NoOpt ? "-noopt.c" : "-opt.c"));
      std::vector<std::string> Args = {
          "decompile", "--llvm",     std::string("--func=") + Function,
          "-o",        Out.string(), obj("test_mem_atomic.o").string()};
      if (NoOpt)
        Args.push_back("--no-opt");
      auto Result = exec(ndBin(), Args);
      ASSERT_EQ(Result.exitCode, 0) << Result.err;
      ASSERT_TRUE(fs::exists(Out));
      EXPECT_GT(fs::file_size(Out), 0U);
    }
}

// LOCK OR has no ordered model (X86LegacyAtomic): selecting it fails at the
// instruction instead of printing a plain read-modify-write.
TEST_F(X86_64_PipelineE2E, DecompileC_MemAtomicLockOrIsRefused) {
  for (bool NoOpt : {false, true}) {
    SCOPED_TRACE(NoOpt);
    std::vector<std::string> Args = {"decompile",
                                     "--llvm",
                                     "--func=test_lock_or",
                                     "-o",
                                     tmpFile("test_lock_or.c").string(),
                                     obj("test_mem_atomic.o").string()};
    if (NoOpt)
      Args.push_back("--no-opt");
    const auto Result = exec(ndBin(), Args);
    EXPECT_NE(Result.exitCode, 0);
    EXPECT_TRUE(Result.errContains("unsupported instruction at 0xC2"))
        << Result.err;
  }
}

TEST_F(X86_64_PipelineE2E, GlobalVolatile_LLVMIRValid) {
  verifyLLVMIRNoVerifierErrors(obj("test_global_volatile.o"));
}

TEST_F(X86_64_PipelineE2E, GlobalVolatile_HasStoreAndLoad) {
  auto R = liftToLLVMIR(obj("test_global_volatile.o"));
  ASSERT_EQ(R.exitCode, 0) << "LLVM IR lift failed: " << R.err;
  EXPECT_TRUE(R.out.find("store") != std::string::npos)
      << "Expected store instructions for global variable writes";
  EXPECT_TRUE(R.out.find("load") != std::string::npos)
      << "Expected load instructions for global variable reads";
}

// ============================================================================
// HighC decompile mode
// ============================================================================

TEST_F(X86_64_PipelineE2E, DecompileHighC_Arith) {
  auto r = decompileToHighC(obj("test_arith.o"));
  ASSERT_EQ(r.exitCode, 0) << "HighC decompile failed: " << r.err;
}

TEST_F(X86_64_PipelineE2E, DecompileHighC_ControlFlow) {
  auto r = decompileToHighC(obj("test_control_flow.o"));
  ASSERT_EQ(r.exitCode, 0) << "HighC decompile failed: " << r.err;
}

TEST_F(X86_64_PipelineE2E, DecompileHighC_MulDiv) {
  auto r = decompileToHighC(obj("test_muldiv.o"));
  ASSERT_EQ(r.exitCode, 0) << "HighC decompile failed: " << r.err;

  std::ifstream Input(tmpFile("decompiled_high.c"));
  const std::string Content((std::istreambuf_iterator<char>(Input)),
                            std::istreambuf_iterator<char>());
  EXPECT_NE(Content.find("neverd_divisor == 0"), std::string::npos) << Content;
  EXPECT_NE(Content.find("neverd_dividend_high >= neverd_divisor"),
            std::string::npos)
      << Content;
  EXPECT_NE(Content.find("neverd_quotient_limit"), std::string::npos)
      << Content;
  EXPECT_NE(Content.find("__builtin_trap()"), std::string::npos) << Content;
}

TEST_F(X86_64_PipelineE2E, DecompileHighC_BitOps) {
  auto r = decompileToHighC(obj("test_bitops.o"));
  ASSERT_EQ(r.exitCode, 0) << "HighC decompile failed: " << r.err;
}

// ============================================================================
// LLVM IR semantic preservation: key operations in output
// ============================================================================

TEST_F(X86_64_PipelineE2E, Semantic_AddI32) {
  verifyLLVMIRContains(obj("test_arith.o"), "test_add", "add i32");
}

TEST_F(X86_64_PipelineE2E, Semantic_SubI32) {
  verifyLLVMIRContains(obj("test_arith.o"), "test_sub", "sub i32");
}

TEST_F(X86_64_PipelineE2E, Semantic_AndI32) {
  verifyLLVMIRContains(obj("test_arith.o"), "test_and", "and i32");
}

TEST_F(X86_64_PipelineE2E, Semantic_OrI32) {
  verifyLLVMIRContains(obj("test_arith.o"), "test_or", "or i32");
}

TEST_F(X86_64_PipelineE2E, Semantic_XorI32) {
  verifyLLVMIRContains(obj("test_arith.o"), "test_xor", "xor i32");
}

TEST_F(X86_64_PipelineE2E, Semantic_ShlI32) {
  verifyLLVMIRContains(obj("test_arith.o"), "test_shl", "shl i32");
}

TEST_F(X86_64_PipelineE2E, Semantic_MulI32) {
  verifyLLVMIRContains(obj("test_muldiv.o"), "test_imul", "mul i32");
}

TEST_F(X86_64_PipelineE2E, Semantic_UDivI32) {
  verifyLLVMIRContains(obj("test_muldiv.o"), "test_div", "udiv i32");
}

TEST_F(X86_64_PipelineE2E, Semantic_AddI64) {
  verifyLLVMIRContains(obj("test_arith.o"), "test_add64", "add i64");
}

TEST_F(X86_64_PipelineE2E, Semantic_SubI64) {
  verifyLLVMIRContains(obj("test_arith.o"), "test_sub64", "sub i64");
}

TEST_F(X86_64_PipelineE2E, Semantic_StoreExists) {
  verifyLLVMIRContains(obj("test_stack_mov.o"), "", "store",
                       /*NoOptimization=*/true);
}

TEST_F(X86_64_PipelineE2E, Semantic_LoadExists) {
  verifyLLVMIRContains(obj("test_stack_mov.o"), "", "load",
                       /*NoOptimization=*/true);
}

TEST_F(X86_64_PipelineE2E, Semantic_IcmpExists) {
  verifyLLVMIRContains(obj("test_control_flow.o"), "", "icmp");
}

TEST_F(X86_64_PipelineE2E, Semantic_CallExists) {
  verifyLLVMIRContains(obj("test_control_semantics.o"), "", "call");
}

// ============================================================================
// Conditional branch coverage
// ============================================================================

TEST_F(X86_64_PipelineE2E, CondBranch_ControlFlow) {
  verifyLLVMIRHasConditionalLogic(obj("test_control_flow.o"));
}

TEST_F(X86_64_PipelineE2E, CondBranch_ControlSemantics) {
  verifyLLVMIRHasConditionalLogic(obj("test_control_semantics.o"));
}

TEST_F(X86_64_PipelineE2E, CondBranch_SetccCmov) {
  verifyLLVMIRHasConditionalLogic(obj("test_setcc_cmov.o"));
}

// ============================================================================
// Patch mode (ELF .o not patchable)
// ============================================================================

TEST_F(X86_64_PipelineE2E, PatchELFObjReject) {
  auto r = patchBinary(obj("test_arith.o"));
  EXPECT_NE(r.exitCode, 0)
      << "ELF .o should not be directly patchable (needs Mach-O executable)";
}

// ============================================================================
// Mach-O patch mode with real executables
// These smoke fixtures select the implemented DWARF registration path.
// Compact-unwind regeneration remains a separately tracked capability.
// ============================================================================

TEST_F(X86_64_PipelineE2E, PatchMachO_SimpleAdd) {
  auto src = tmpFile("macho_test.c");
  {
    std::ofstream f(src);
    f << "int add(int a, int b) { return a + b; }\n"
      << "int main(void) { return add(1, 2); }\n";
  }
  auto exe = tmpFile("macho_test");
  auto cr = exec("clang",
                 {"-O0", "-target", "x86_64-apple-macos",
                  "-Wl,-no_compact_unwind", "-o", exe.string(), src.string()});
  if (cr.exitCode != 0)
    GTEST_SKIP() << "Cannot compile x64 Mach-O on this host";
  verifyPatchProducesOutput(exe);
}

TEST_F(X86_64_PipelineE2E, PatchMachO_Loop) {
  auto src = tmpFile("macho_loop.c");
  {
    std::ofstream f(src);
    f << "int factorial(int n) {\n"
      << "    int r = 1;\n"
      << "    for (int i = 2; i <= n; i++) r *= i;\n"
      << "    return r;\n"
      << "}\n"
      << "int main(void) { return factorial(5); }\n";
  }
  auto exe = tmpFile("macho_loop");
  auto cr = exec("clang",
                 {"-O0", "-target", "x86_64-apple-macos",
                  "-Wl,-no_compact_unwind", "-o", exe.string(), src.string()});
  if (cr.exitCode != 0)
    GTEST_SKIP() << "Cannot compile x64 Mach-O";
  verifyPatchProducesOutput(exe);
  verifyLLVMIRHasConditionalLogic(exe);
  verifyNoConstantTrueBranch(exe);
}

TEST_F(X86_64_PipelineE2E, PatchMachO_Recursive) {
  auto src = tmpFile("macho_recur.c");
  {
    std::ofstream f(src);
    f << "int fib(int n) {\n"
      << "    if (n <= 1) return n;\n"
      << "    return fib(n-1) + fib(n-2);\n"
      << "}\n"
      << "int main(void) { return fib(10); }\n";
  }
  auto exe = tmpFile("macho_recur");
  auto cr = exec("clang",
                 {"-O0", "-target", "x86_64-apple-macos",
                  "-Wl,-no_compact_unwind", "-o", exe.string(), src.string()});
  if (cr.exitCode != 0)
    GTEST_SKIP() << "Cannot compile x64 Mach-O";
  verifyPatchProducesOutput(exe);
  verifyLLVMIRHasConditionalLogic(exe);
  verifyLLVMIRNoUnreachable(exe);
}

TEST_F(X86_64_PipelineE2E, PatchMachO_Switch) {
  auto src = tmpFile("macho_switch.c");
  {
    std::ofstream f(src);
    f << "int classify(int x) {\n"
      << "    switch(x) {\n"
      << "        case 0: return 10;\n"
      << "        case 1: return 20;\n"
      << "        case 2: return 30;\n"
      << "        case 3: return 40;\n"
      << "        default: return -1;\n"
      << "    }\n"
      << "}\n"
      << "int main(void) { return classify(2); }\n";
  }
  auto exe = tmpFile("macho_switch");
  auto cr = exec("clang",
                 {"-O0", "-target", "x86_64-apple-macos",
                  "-Wl,-no_compact_unwind", "-o", exe.string(), src.string()});
  if (cr.exitCode != 0)
    GTEST_SKIP() << "Cannot compile x64 Mach-O";
  verifyPatchProducesOutput(exe);
  // Check recovery before LLVM can fold affine switch cases into arithmetic.
  auto LLVM = liftToLLVMIRUnopt(exe);
  ASSERT_EQ(LLVM.exitCode, 0) << "LLVM IR lift failed: " << LLVM.err;
  // Recovery may retain a 64-bit table key before optimization narrows it.
  EXPECT_NE(LLVM.out.find("  switch "), std::string::npos)
      << "Expected the recovered classify switch:\n"
      << LLVM.out;
  // A proof-complete set of cases may still use an explicit trap for an
  // unmodelled/default selector value.  That `unreachable` is intentional;
  // the regression here is losing the dispatch itself, not the presence of a
  // fail-closed default block.
}
