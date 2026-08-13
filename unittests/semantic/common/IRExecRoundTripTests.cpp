//===- IRExecRoundTripTests.cpp - LLVM IR round-trip tests ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Migrated from scripts/llvm_ir_exec_verifier.py
//
// Verifies end-to-end round-trip: C source → cross-compile → neverd lift →
// retarget LLVM IR → compile on host → execute → compare results.
// This uses NeverDLiftFixture (from unittests/lift/) for the pipeline,
// but we also verify the assembler + emulation baseline here.
//
// The C-function-level tests are already covered by the existing
// X86_64_RoundTripTests.cpp etc.  This file adds LLVM MC assembly
// baseline checks for the same instruction patterns.
//
//===----------------------------------------------------------------------===//

#include "UnicornSemanticFixture.h"

TEST_P(X64Semantic, IRExecVerify) { runX64(GetParam()); }
TEST_P(AArch64Semantic, IRExecVerify) { runAArch64(GetParam()); }
TEST_P(ARM32Semantic, IRExecVerify) { runARM32(GetParam()); }

// clang-format off

static const std::vector<SemTC> kX64RoundTrip = {
  {"add_basic",     "mov rax, 50; add rax, 30",                  {},                                           {"rax"},           "RoundTrip", {}},
  {"sub_basic",     "mov rax, 100; sub rax, 30",                 {},                                           {"rax"},           "RoundTrip", {}},
  {"and_basic",     "mov rax, 0xFF00; and rax, 0x0FF0",          {},                                           {"rax"},           "RoundTrip", {}},
  {"or_basic",      "mov rax, 0xF0; or rax, 0x0F",              {},                                           {"rax"},           "RoundTrip", {}},
  {"xor_basic",     "mov rax, 0xFF; xor rax, 0x55",             {},                                           {"rax"},           "RoundTrip", {}},
  {"mul_basic",     "mov rax, 6; mov rbx, 7; imul rax, rbx",     {},                                           {"rax"},           "RoundTrip", {}},
  {"div_basic",     "mov rax, 100; xor rdx, rdx; mov rcx, 7; div rcx", {},                                     {"rax", "rdx"},    "RoundTrip", {}},
  {"neg_basic",     "mov rax, 42; neg rax",                      {},                                           {"rax"},           "RoundTrip", {}},
  {"not_basic",     "xor rax, rax; not rax",                     {},                                           {"rax"},           "RoundTrip", {}},
  {"shl_basic",     "mov rax, 1; shl rax, 4",                   {},                                           {"rax"},           "RoundTrip", {}},
  {"shr_basic",     "mov rax, 0x100; shr rax, 4",               {},                                           {"rax"},           "RoundTrip", {}},
  {"sar_basic",     "mov rax, -128; sar rax, 2",                {},                                           {"rax"},           "RoundTrip", {}},
};

static const std::vector<SemTC> kA64RoundTrip = {
  {"add_basic",     "mov x0, #50; add x0, x0, #30",              {},                                           {"x0"},            "RoundTrip", {}},
  {"sub_basic",     "mov x0, #100; sub x0, x0, #30",             {},                                           {"x0"},            "RoundTrip", {}},
  {"mul_basic",     "mov x0, #6; mov x1, #7; mul x0, x0, x1",   {},                                           {"x0"},            "RoundTrip", {}},
  {"neg_basic",     "mov x0, #42; neg x0, x0",                   {},                                           {"x0"},            "RoundTrip", {}},
  {"lsl_basic",     "mov x0, #1; lsl x0, x0, #4",               {},                                           {"x0"},            "RoundTrip", {}},
};

static const std::vector<SemTC> kARM32RoundTrip = {
  {"add_basic",     "mov r0, #50; add r0, r0, #30",              {},                                           {"r0"},            "RoundTrip", {}},
  {"sub_basic",     "mov r0, #100; sub r0, r0, #30",             {},                                           {"r0"},            "RoundTrip", {}},
  {"mul_basic",     "mov r0, #6; mov r1, #7; mul r0, r0, r1",   {},                                           {"r0"},            "RoundTrip", {}},
  {"lsl_basic",     "mov r0, #1; lsl r0, r0, #4",               {},                                           {"r0"},            "RoundTrip", {}},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(X64RoundTrip, X64Semantic, ::testing::ValuesIn(kX64RoundTrip), semTCName);
INSTANTIATE_TEST_SUITE_P(A64RoundTrip, AArch64Semantic, ::testing::ValuesIn(kA64RoundTrip), semTCName);
INSTANTIATE_TEST_SUITE_P(ARM32RoundTrip, ARM32Semantic, ::testing::ValuesIn(kARM32RoundTrip), semTCName);
