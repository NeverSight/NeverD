//===- PipelineSemanticTests.cpp - Pipeline verifier tests -------*- C++ -*-===//
//
// Migrated from scripts/pipeline_verifier.py
// Verifies instruction semantics across NeverD's pipeline stages via Unicorn.
//
//===----------------------------------------------------------------------===//

#include "UnicornSemanticFixture.h"

TEST_P(X64Semantic, PipelineVerify) { runX64(GetParam()); }
TEST_P(AArch64Semantic, PipelineVerify) { runAArch64(GetParam()); }
TEST_P(ARM32Semantic, PipelineVerify) { runARM32(GetParam()); }

// clang-format off

static const std::vector<SemTC> kX64Pipeline = {
  {"add_imm",       "add rax, 42",                               {{"rax", 100}},                               {"rax"},           "ArithPipeline", {}},
  {"sub_reg",       "sub rax, rbx",                              {{"rax", 100}, {"rbx", 30}},                  {"rax"},           "ArithPipeline", {}},
  {"xor_self",      "xor rax, rax",                              {{"rax", 42}},                                {"rax"},           "ArithPipeline", {}},
  {"lea_complex",   "lea rax, [rbx + rcx*4 + 8]",               {{"rbx", 100}, {"rcx", 10}},                  {"rax"},           "ArithPipeline", {}},
  {"adc_chain",     "stc; adc rax, rbx",                         {{"rax", 10}, {"rbx", 20}},                   {"rax"},           "ArithPipeline", {}},
  {"neg_reg",       "neg rax",                                   {{"rax", 42}},                                {"rax"},           "ArithPipeline", {}},
  {"not_reg",       "not rax",                                   {{"rax", 0xFF}},                              {"rax"},           "ArithPipeline", {}},
  {"imul_3op",      "imul rax, rbx, 7",                          {{"rbx", 6}},                                 {"rax"},           "MulPipeline", {}},
  {"div_rdx_rax",   "xor rdx, rdx; mov rax, 100; mov rcx, 7; div rcx", {{"rax", 0}},                          {"rax", "rdx"},    "MulPipeline", {}},
  {"shl_cl",        "shl rax, cl",                               {{"rax", 1}, {"rcx", 10}},                    {"rax"},           "ShiftPipeline", {}},
  {"ror_imm",       "ror rax, 4",                                {{"rax", 0xF0}},                              {"rax"},           "ShiftPipeline", {}},
  {"shrd_imm",      "shrd rax, rbx, 4",                          {{"rax", 0xFF}, {"rbx", 0xA0}},               {"rax"},           "ShiftPipeline", {}},
  {"bsr_nonzero",   "bsr rax, rbx",                              {{"rbx", 0x80}},                              {"rax"},           "BitPipeline", {}},
  {"bsf_nonzero",   "bsf rax, rbx",                              {{"rbx", 0x10}},                              {"rax"},           "BitPipeline", {}},
  {"popcnt",        "popcnt rax, rbx",                           {{"rbx", 0xFF}},                              {"rax"},           "BitPipeline", {}},
  {"tzcnt",         "tzcnt rax, rbx",                            {{"rbx", 0x80}},                              {"rax"},           "BitPipeline", {}},
  {"movzx_byte",    "movzx eax, bl",                             {{"rbx", 0xFF42}},                            {"rax"},           "ExtPipeline", {}},
  {"movsx_word",    "movsx rax, bx",                             {{"rbx", 0x8000}},                            {"rax"},           "ExtPipeline", {}},
  {"cmp_setz",      "cmp rax, rbx; setz cl",                    {{"rax", 42}, {"rbx", 42}},                   {"rcx"},           "FlagPipeline", {}},
  {"cmovz",         "cmp rax, rax; cmovz rbx, rcx",              {{"rax", 42}, {"rbx", 0}, {"rcx", 99}},      {"rbx"},           "ControlPipeline", {}},
};

static const std::vector<SemTC> kA64Pipeline = {
  {"add_imm",       "add x0, x1, #42",                           {{"x1", 100}},                                {"x0"},            "ArithPipeline", {}},
  {"sub_reg",       "sub x0, x1, x2",                            {{"x1", 100}, {"x2", 30}},                    {"x0"},            "ArithPipeline", {}},
  {"eor_self",      "eor x0, x1, x1",                            {{"x1", 42}},                                 {"x0"},            "ArithPipeline", {}},
  {"madd",          "madd x0, x1, x2, x3",                      {{"x1", 6}, {"x2", 7}, {"x3", 10}},           {"x0"},            "MulPipeline", {}},
  {"udiv",          "udiv x0, x1, x2",                           {{"x1", 100}, {"x2", 7}},                     {"x0"},            "MulPipeline", {}},
  {"lsl_imm",       "lsl x0, x1, #4",                            {{"x1", 1}},                                  {"x0"},            "ShiftPipeline", {}},
  {"asr_imm",       "asr x0, x1, #2",                            {{"x1", 0xFFFFFFFFFFFFFF00ULL}},              {"x0"},            "ShiftPipeline", {}},
  {"clz",           "clz x0, x1",                                {{"x1", 0x100}},                              {"x0"},            "BitPipeline", {}},
  {"rbit",          "rbit x0, x1",                               {{"x1", 1}},                                  {"x0"},            "BitPipeline", {}},
  {"sxtb",          "sxtb x0, w1",                               {{"x1", 0x80}},                               {"x0"},            "ExtPipeline", {}},
  {"cmp_cset",      "cmp x0, x1; cset x2, eq",                  {{"x0", 42}, {"x1", 42}},                     {"x2"},            "CondPipeline", {}},
  {"csel_eq",       "cmp x0, x1; csel x2, x3, x4, eq",
   {{"x0", 42}, {"x1", 42}, {"x3", 100}, {"x4", 200}},          {"x2"},                                                          "CondPipeline", {}},
};

static const std::vector<SemTC> kARM32Pipeline = {
  {"add_imm",       "add r0, r1, #42",                           {{"r1", 100}},                                {"r0"},            "ArithPipeline", {}},
  {"sub_reg",       "sub r0, r1, r2",                            {{"r1", 100}, {"r2", 30}},                    {"r0"},            "ArithPipeline", {}},
  {"eor_self",      "eor r0, r1, r1",                            {{"r1", 42}},                                 {"r0"},            "ArithPipeline", {}},
  {"mul_reg",       "mul r0, r1, r2",                            {{"r1", 6}, {"r2", 7}},                       {"r0"},            "MulPipeline", {}},
  {"lsl_imm",       "lsl r0, r1, #3",                            {{"r1", 5}},                                  {"r0"},            "ShiftPipeline", {}},
  {"clz",           "clz r0, r1",                                {{"r1", 0x100}},                              {"r0"},            "BitPipeline", {}},
  {"uxtb",          "uxtb r0, r1",                               {{"r1", 0xABCD}},                             {"r0"},            "ExtPipeline", {}},
  {"sxtb",          "sxtb r0, r1",                               {{"r1", 0x80}},                               {"r0"},            "ExtPipeline", {}},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(X64Pipeline, X64Semantic, ::testing::ValuesIn(kX64Pipeline), semTCName);
INSTANTIATE_TEST_SUITE_P(A64Pipeline, AArch64Semantic, ::testing::ValuesIn(kA64Pipeline), semTCName);
INSTANTIATE_TEST_SUITE_P(ARM32Pipeline, ARM32Semantic, ::testing::ValuesIn(kARM32Pipeline), semTCName);
