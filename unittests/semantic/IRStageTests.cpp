//===- IRStageTests.cpp - IR stage verification tests -----------*- C++ -*-===//
//
// Migrated from scripts/ir_stage_verifier.py
// Validates instruction encoding via LLVM MC + Unicorn emulation as ground
// truth, then lifts through each NeverD pipeline stage to verify
// structural correctness.
//
// This file uses UnicornSemanticFixture to verify the assembly→emulate
// baseline. Full pipeline-stage checking (low/med/high/llvm-ir) uses
// NeverDLiftFixture from unittests/lift/ and is already covered there.
//
//===----------------------------------------------------------------------===//

#include "UnicornSemanticFixture.h"

TEST_P(X64Semantic, IRStageVerify) { runX64(GetParam()); }
TEST_P(AArch64Semantic, IRStageVerify) { runAArch64(GetParam()); }
TEST_P(ARM32Semantic, IRStageVerify) { runARM32(GetParam()); }

// clang-format off

static const std::vector<SemTC> kX64IRStage = {
  {"add_imm",       "add rax, 42",                               {{"rax", 100}},                               {"rax"},           "IRStage", {}},
  {"sub_reg",       "sub rax, rbx",                              {{"rax", 100}, {"rbx", 30}},                  {"rax"},           "IRStage", {}},
  {"imul_3op",      "imul rax, rbx, 7",                          {{"rbx", 6}},                                 {"rax"},           "IRStage", {}},
  {"shl_cl",        "shl rax, cl",                               {{"rax", 1}, {"rcx", 10}},                    {"rax"},           "IRStage", {}},
  {"movzx_byte",    "movzx eax, bl",                             {{"rbx", 0xFF42}},                            {"rax"},           "IRStage", {}},
  {"cmp_setz",      "cmp rax, rbx; setz cl",                    {{"rax", 42}, {"rbx", 42}},                   {"rcx"},           "IRStage", {}},
  {"cmovz",         "cmp rax, rax; cmovz rbx, rcx",              {{"rax", 42}, {"rbx", 0}, {"rcx", 99}},      {"rbx"},           "IRStage", {}},
  {"push_pop",      "push rax; pop rbx",                         {{"rax", 0x42}},                              {"rbx"},           "IRStage", {}},
  {"xchg",          "xchg rax, rbx",                             {{"rax", 1}, {"rbx", 2}},                     {"rax", "rbx"},    "IRStage", {}},
  {"bsr",           "bsr rax, rbx",                              {{"rbx", 0x80}},                              {"rax"},           "IRStage", {}},
  {"popcnt",        "popcnt rax, rbx",                           {{"rbx", 0xFF}},                              {"rax"},           "IRStage", {}},
  {"lea_sib",       "lea rax, [rbx + rcx*4 + 8]",               {{"rbx", 100}, {"rcx", 10}},                  {"rax"},           "IRStage", {}},
};

static const std::vector<SemTC> kA64IRStage = {
  {"add_imm",       "add x0, x1, #42",                           {{"x1", 100}},                                {"x0"},            "IRStage", {}},
  {"sub_reg",       "sub x0, x1, x2",                            {{"x1", 100}, {"x2", 30}},                    {"x0"},            "IRStage", {}},
  {"mul",           "mul x0, x1, x2",                            {{"x1", 6}, {"x2", 7}},                       {"x0"},            "IRStage", {}},
  {"lsl_imm",       "lsl x0, x1, #4",                            {{"x1", 1}},                                  {"x0"},            "IRStage", {}},
  {"sxtb",          "sxtb x0, w1",                               {{"x1", 0x80}},                               {"x0"},            "IRStage", {}},
  {"cmp_cset",      "cmp x0, x1; cset x2, eq",                  {{"x0", 42}, {"x1", 42}},                     {"x2"},            "IRStage", {}},
};

static const std::vector<SemTC> kARM32IRStage = {
  {"add_imm",       "add r0, r1, #42",                           {{"r1", 100}},                                {"r0"},            "IRStage", {}},
  {"sub_reg",       "sub r0, r1, r2",                            {{"r1", 100}, {"r2", 30}},                    {"r0"},            "IRStage", {}},
  {"mul_reg",       "mul r0, r1, r2",                            {{"r1", 6}, {"r2", 7}},                       {"r0"},            "IRStage", {}},
  {"lsl_imm",       "lsl r0, r1, #3",                            {{"r1", 5}},                                  {"r0"},            "IRStage", {}},
  {"uxtb",          "uxtb r0, r1",                               {{"r1", 0xABCD}},                             {"r0"},            "IRStage", {}},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(X64IRStage, X64Semantic, ::testing::ValuesIn(kX64IRStage), semTCName);
INSTANTIATE_TEST_SUITE_P(A64IRStage, AArch64Semantic, ::testing::ValuesIn(kA64IRStage), semTCName);
INSTANTIATE_TEST_SUITE_P(ARM32IRStage, ARM32Semantic, ::testing::ValuesIn(kARM32IRStage), semTCName);
