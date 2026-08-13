#include "NeverDLiftFixture.h"

class X86_64_PipelineE2E : public NeverDLiftTest {};

static fs::path obj(const char* name) {
    return fs::path(TEST_OBJ_DIR) / name;
}

// ============================================================================
// Full pipeline: AllStages + Decompile + Verifier + NoBranchTrue + NoUnreachable
// Every test object must pass ALL checks to ensure semantic preservation
// from binary → LowIR → MedIR → HighIR → LLVM IR
// ============================================================================

#define FULL_PIPELINE_TEST(suite, tag, objname) \
    TEST_F(suite, FullPipeline_##tag) { verifyAllModesSucceed(obj(objname)); } \
    TEST_F(suite, NoUnreachable_##tag) { verifyLLVMIRNoUnreachable(obj(objname)); }

FULL_PIPELINE_TEST(X86_64_PipelineE2E, Arith,           "test_arith.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, MulDiv,          "test_muldiv.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, ControlFlow,     "test_control_flow.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, StackMov,        "test_stack_mov.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, SseFp,           "test_sse_fp.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, CarryOps,        "test_carry_ops.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, BitOps,          "test_bitops.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, SetccCmov,       "test_setcc_cmov.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, SignExt,         "test_signext.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, StringOps,       "test_string_ops.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, RotateShift,     "test_rotate_shift.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, Misc,            "test_misc.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, SemanticE2E,     "test_semantic_e2e.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, ControlSemantics,"test_control_semantics.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, Bmi,             "test_bmi.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, AdxMisc,         "test_adx_misc.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, SseAvx,          "test_sse_avx.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, FlagOps,         "test_flag_ops.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, MemAtomic,       "test_mem_atomic.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, DoubleShift,     "test_double_shift.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, FlagsMisc,       "test_flags_misc.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, FpConvert,       "test_fp_convert.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, RclRcr,          "test_rcl_rcr.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, CmpxchgXadd,     "test_cmpxchg_xadd.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, SignConv,         "test_sign_conv.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, LoopBit,          "test_loop.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, FmaMisc,          "test_fma_misc.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, X87FPU,           "test_x87_fpu.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, LodsMisc,         "test_lods_misc.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, SseMisc,          "test_sse_misc.o")
FULL_PIPELINE_TEST(X86_64_PipelineE2E, GlobalVolatile,   "test_global_volatile.o")

#undef FULL_PIPELINE_TEST

// ============================================================================
// Decompile mode: all objects produce valid C output
// ============================================================================

#define DECOMPILE_TEST(tag, objname) \
    TEST_F(X86_64_PipelineE2E, DecompileC_##tag) { \
        verifyDecompileProducesOutput(obj(objname)); \
    }

DECOMPILE_TEST(Arith,           "test_arith.o")
DECOMPILE_TEST(MulDiv,          "test_muldiv.o")
DECOMPILE_TEST(ControlFlow,     "test_control_flow.o")
DECOMPILE_TEST(StackMov,        "test_stack_mov.o")
DECOMPILE_TEST(BitOps,          "test_bitops.o")
DECOMPILE_TEST(SemanticE2E,     "test_semantic_e2e.o")
DECOMPILE_TEST(ControlSemantics,"test_control_semantics.o")
DECOMPILE_TEST(SseFp,           "test_sse_fp.o")
DECOMPILE_TEST(CarryOps,        "test_carry_ops.o")
DECOMPILE_TEST(SignExt,         "test_signext.o")
DECOMPILE_TEST(SetccCmov,       "test_setcc_cmov.o")
DECOMPILE_TEST(FlagOps,         "test_flag_ops.o")
DECOMPILE_TEST(FlagsMisc,       "test_flags_misc.o")
DECOMPILE_TEST(RotateShift,     "test_rotate_shift.o")
DECOMPILE_TEST(DoubleShift,     "test_double_shift.o")
DECOMPILE_TEST(Bmi,             "test_bmi.o")
DECOMPILE_TEST(CmpxchgXadd,     "test_cmpxchg_xadd.o")
DECOMPILE_TEST(MemAtomic,       "test_mem_atomic.o")
DECOMPILE_TEST(FpConvert,       "test_fp_convert.o")
DECOMPILE_TEST(RclRcr,          "test_rcl_rcr.o")
DECOMPILE_TEST(AdxMisc,         "test_adx_misc.o")
DECOMPILE_TEST(SseAvx,          "test_sse_avx.o")
DECOMPILE_TEST(Misc,            "test_misc.o")
DECOMPILE_TEST(StringOps,       "test_string_ops.o")
DECOMPILE_TEST(SignConv,        "test_sign_conv.o")
DECOMPILE_TEST(LoopBit,         "test_loop.o")
DECOMPILE_TEST(FmaMisc,        "test_fma_misc.o")
DECOMPILE_TEST(X87FPU,         "test_x87_fpu.o")
DECOMPILE_TEST(LodsMisc,       "test_lods_misc.o")
DECOMPILE_TEST(SseMisc,        "test_sse_misc.o")
DECOMPILE_TEST(GlobalVolatile, "test_global_volatile.o")

#undef DECOMPILE_TEST

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
    verifyLLVMIRContains(obj("test_stack_mov.o"), "", "store");
}

TEST_F(X86_64_PipelineE2E, Semantic_LoadExists) {
    verifyLLVMIRContains(obj("test_stack_mov.o"), "", "load");
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
// ============================================================================

TEST_F(X86_64_PipelineE2E, PatchMachO_SimpleAdd) {
    auto src = tmpFile("macho_test.c");
    {
        std::ofstream f(src);
        f << "int add(int a, int b) { return a + b; }\n"
          << "int main(void) { return add(1, 2); }\n";
    }
    auto exe = tmpFile("macho_test");
    auto cr = exec("clang", {"-O0", "-target", "x86_64-apple-macos",
                             "-o", exe.string(), src.string()});
    if (cr.exitCode != 0) GTEST_SKIP() << "Cannot compile x64 Mach-O on this host";
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
    auto cr = exec("clang", {"-O0", "-target", "x86_64-apple-macos",
                             "-o", exe.string(), src.string()});
    if (cr.exitCode != 0) GTEST_SKIP() << "Cannot compile x64 Mach-O";
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
    auto cr = exec("clang", {"-O0", "-target", "x86_64-apple-macos",
                             "-o", exe.string(), src.string()});
    if (cr.exitCode != 0) GTEST_SKIP() << "Cannot compile x64 Mach-O";
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
    auto cr = exec("clang", {"-O0", "-target", "x86_64-apple-macos",
                             "-o", exe.string(), src.string()});
    if (cr.exitCode != 0) GTEST_SKIP() << "Cannot compile x64 Mach-O";
    verifyPatchProducesOutput(exe);
    verifyLLVMIRNoUnreachable(exe);
}
