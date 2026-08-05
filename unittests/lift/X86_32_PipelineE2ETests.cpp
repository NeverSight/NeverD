#include "NeverDLiftFixture.h"

class X86_32_PipelineE2E : public NeverDLiftTest {};

static fs::path obj32(const char* name) {
    return fs::path(TEST_OBJ_DIR) / name;
}

// ============================================================================
// Full pipeline verification for every x86_32 test object
// ============================================================================

#define FULL_PIPELINE_TEST(tag, objname) \
    TEST_F(X86_32_PipelineE2E, FullPipeline_##tag) { \
        verifyAllModesSucceed(obj32(objname)); \
    } \
    TEST_F(X86_32_PipelineE2E, NoUnreachable_##tag) { \
        verifyLLVMIRNoUnreachable(obj32(objname)); \
    }

FULL_PIPELINE_TEST(Arith,    "test_arith32.o")
FULL_PIPELINE_TEST(Control,  "test_control32.o")
FULL_PIPELINE_TEST(BitOps,   "test_bitops32.o")
FULL_PIPELINE_TEST(SignExt,  "test_signext32.o")
FULL_PIPELINE_TEST(MulDiv,   "test_muldiv32.o")
FULL_PIPELINE_TEST(Stack,    "test_stack32.o")
FULL_PIPELINE_TEST(Flags,    "test_flags32.o")
FULL_PIPELINE_TEST(Misc,     "test_misc32.o")
FULL_PIPELINE_TEST(FP,       "test_fp32.o")
FULL_PIPELINE_TEST(Cmov,     "test_cmov32.o")
FULL_PIPELINE_TEST(Rotate,   "test_rotate32.o")
FULL_PIPELINE_TEST(Carry,    "test_carry32.o")
FULL_PIPELINE_TEST(Setcc,    "test_setcc32.o")
FULL_PIPELINE_TEST(String,   "test_string32.o")
FULL_PIPELINE_TEST(Cmpxchg,  "test_cmpxchg32.o")
FULL_PIPELINE_TEST(Loop,     "test_loop32.o")
FULL_PIPELINE_TEST(DShift,   "test_dshift32.o")
FULL_PIPELINE_TEST(X87FPU,   "test_x87_fpu32.o")
FULL_PIPELINE_TEST(LodsMisc, "test_lods_misc32.o")

#undef FULL_PIPELINE_TEST

// ============================================================================
// Decompile mode: all objects produce valid C output
// ============================================================================

#define DECOMPILE_TEST(tag, objname) \
    TEST_F(X86_32_PipelineE2E, DecompileC_##tag) { \
        verifyDecompileProducesOutput(obj32(objname)); \
    }

DECOMPILE_TEST(Arith,    "test_arith32.o")
DECOMPILE_TEST(Control,  "test_control32.o")
DECOMPILE_TEST(MulDiv,   "test_muldiv32.o")
DECOMPILE_TEST(Flags,    "test_flags32.o")
DECOMPILE_TEST(BitOps,   "test_bitops32.o")
DECOMPILE_TEST(SignExt,  "test_signext32.o")
DECOMPILE_TEST(Stack,    "test_stack32.o")
DECOMPILE_TEST(Misc,     "test_misc32.o")
DECOMPILE_TEST(FP,       "test_fp32.o")
DECOMPILE_TEST(Cmov,     "test_cmov32.o")
DECOMPILE_TEST(Rotate,   "test_rotate32.o")
DECOMPILE_TEST(Carry,    "test_carry32.o")
DECOMPILE_TEST(Setcc,    "test_setcc32.o")
DECOMPILE_TEST(String,   "test_string32.o")
DECOMPILE_TEST(Cmpxchg,  "test_cmpxchg32.o")
DECOMPILE_TEST(Loop,     "test_loop32.o")
DECOMPILE_TEST(DShift,   "test_dshift32.o")
DECOMPILE_TEST(X87FPU,   "test_x87_fpu32.o")
DECOMPILE_TEST(LodsMisc, "test_lods_misc32.o")

#undef DECOMPILE_TEST

// ============================================================================
// LLVM IR semantic preservation
// ============================================================================

TEST_F(X86_32_PipelineE2E, Semantic_AddI32) {
    verifyLLVMIRContains(obj32("test_arith32.o"), "test_add32", "add i32");
}

TEST_F(X86_32_PipelineE2E, Semantic_SubI32) {
    verifyLLVMIRContains(obj32("test_arith32.o"), "test_sub32", "sub i32");
}

TEST_F(X86_32_PipelineE2E, Semantic_MulI32) {
    verifyLLVMIRContains(obj32("test_muldiv32.o"), "test_imul32", "mul i32");
}

TEST_F(X86_32_PipelineE2E, Semantic_AndI32) {
    verifyLLVMIRContains(obj32("test_arith32.o"), "test_and32", "and i32");
}

TEST_F(X86_32_PipelineE2E, Semantic_OrI32) {
    verifyLLVMIRContains(obj32("test_arith32.o"), "test_or32", "or i32");
}

TEST_F(X86_32_PipelineE2E, Semantic_XorI32) {
    verifyLLVMIRContains(obj32("test_arith32.o"), "test_xor32", "xor i32");
}

TEST_F(X86_32_PipelineE2E, Semantic_StoreExists) {
    verifyLLVMIRContains(obj32("test_stack32.o"), "", "store");
}

TEST_F(X86_32_PipelineE2E, Semantic_LoadExists) {
    verifyLLVMIRContains(obj32("test_stack32.o"), "", "load");
}

TEST_F(X86_32_PipelineE2E, Semantic_IcmpExists) {
    verifyLLVMIRContains(obj32("test_control32.o"), "", "icmp");
}

// ============================================================================
// Conditional branch coverage
// ============================================================================

TEST_F(X86_32_PipelineE2E, CondBranch_Control) {
    verifyLLVMIRHasConditionalLogic(obj32("test_control32.o"));
}

TEST_F(X86_32_PipelineE2E, CondBranch_Cmov) {
    verifyLLVMIRHasConditionalLogic(obj32("test_cmov32.o"));
}

// ============================================================================
// Patch mode (ELF .o not patchable)
// ============================================================================

TEST_F(X86_32_PipelineE2E, PatchELFObjReject) {
    auto r = patchBinary(obj32("test_arith32.o"));
    EXPECT_NE(r.exitCode, 0)
        << "ELF .o should not be directly patchable (needs Mach-O executable)";
}
