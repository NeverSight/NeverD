//===- ARM32_SemanticTests.cpp - ARM32 semantic tests -----------*- C++ -*-===//
//
// Migrated from scripts/lift_verifier.py — ARM32 instruction categories:
//   Core, MulDiv, Shift, Extend, Carry, Mem
//
//===----------------------------------------------------------------------===//

#include "UnicornSemanticFixture.h"

TEST_P(ARM32Semantic, Verify) { runARM32(GetParam()); }

// clang-format off

static const std::vector<SemTC> kARM32Core = {
  {"mov_imm",       "mov r0, #42",                               {},                                           {"r0"},            "Core", {}},
  {"mov_reg",       "mov r0, r1",                                {{"r1", 99}},                                 {"r0"},            "Core", {}},
  {"add_imm",       "add r0, r1, #42",                           {{"r1", 100}},                                {"r0"},            "Core", {}},
  {"add_reg",       "add r0, r1, r2",                            {{"r1", 50}, {"r2", 30}},                     {"r0"},            "Core", {}},
  {"add_shift",     "add r0, r1, r2, lsl #2",                   {{"r1", 10}, {"r2", 5}},                      {"r0"},            "Core", {}},
  {"sub_imm",       "sub r0, r1, #10",                           {{"r1", 100}},                                {"r0"},            "Core", {}},
  {"sub_reg",       "sub r0, r1, r2",                            {{"r1", 100}, {"r2", 30}},                    {"r0"},            "Core", {}},
  {"and_imm",       "and r0, r1, #0xFF",                         {{"r1", 0x12345678}},                         {"r0"},            "Core", {}},
  {"orr_imm",       "orr r0, r1, #0xFF",                         {{"r1", 0x100}},                              {"r0"},            "Core", {}},
  {"eor_imm",       "eor r0, r1, #0xFF",                         {{"r1", 0xAA}},                               {"r0"},            "Core", {}},
  {"eor_self",      "eor r0, r1, r1",                            {{"r1", 42}},                                 {"r0"},            "Core", {}},
  {"mvn",           "mvn r0, r1",                                {{"r1", 0}},                                  {"r0"},            "Core", {}},
  {"rsb",           "rsb r0, r1, #100",                          {{"r1", 30}},                                 {"r0"},            "Core", {}},
  {"bic",           "bic r0, r1, #0xF",                          {{"r1", 0xFF}},                               {"r0"},            "Core", {}},
};

static const std::vector<SemTC> kARM32MulDiv = {
  {"mul_reg",       "mul r0, r1, r2",                            {{"r1", 6}, {"r2", 7}},                       {"r0"},            "MulDiv", {}},
  {"mla",           "mla r0, r1, r2, r3",                       {{"r1", 6}, {"r2", 7}, {"r3", 10}},            {"r0"},            "MulDiv", {}},
  {"mls",           "mls r0, r1, r2, r3",                       {{"r1", 6}, {"r2", 7}, {"r3", 100}},           {"r0"},            "MulDiv", {}},
  {"umull",         "umull r0, r1, r2, r3",                      {{"r2", 0xFFFFFFFF}, {"r3", 2}},               {"r0", "r1"},      "MulDiv", {}},
  {"smull",         "smull r0, r1, r2, r3",                      {{"r2", 0xFFFFFFFF}, {"r3", 2}},               {"r0", "r1"},      "MulDiv", {}},
};

static const std::vector<SemTC> kARM32Shift = {
  {"lsl_imm",       "lsl r0, r1, #3",                           {{"r1", 5}},                                  {"r0"},            "Shift", {}},
  {"lsr_imm",       "lsr r0, r1, #2",                           {{"r1", 100}},                                {"r0"},            "Shift", {}},
  {"asr_imm",       "asr r0, r1, #2",                           {{"r1", 0xFFFFFF00}},                         {"r0"},            "Shift", {}},
  {"ror_imm",       "ror r0, r1, #4",                           {{"r1", 0xF0}},                               {"r0"},            "Shift", {}},
  {"lsl_reg",       "lsl r0, r1, r2",                           {{"r1", 1}, {"r2", 10}},                      {"r0"},            "Shift", {}},
};

static const std::vector<SemTC> kARM32Extend = {
  {"uxtb",          "uxtb r0, r1",                              {{"r1", 0xABCD}},                             {"r0"},            "Extend", {}},
  {"uxth",          "uxth r0, r1",                              {{"r1", 0xABCD}},                             {"r0"},            "Extend", {}},
  {"sxtb",          "sxtb r0, r1",                              {{"r1", 0x80}},                               {"r0"},            "Extend", {}},
  {"sxth",          "sxth r0, r1",                              {{"r1", 0x8000}},                             {"r0"},            "Extend", {}},
  {"rev",           "rev r0, r1",                               {{"r1", 0x01020304}},                         {"r0"},            "Extend", {}},
  {"rev16",         "rev16 r0, r1",                             {{"r1", 0x01020304}},                         {"r0"},            "Extend", {}},
  {"rbit",          "rbit r0, r1",                              {{"r1", 1}},                                  {"r0"},            "Extend", {}},
  {"clz",           "clz r0, r1",                               {{"r1", 0x100}},                              {"r0"},            "Extend", {}},
};

static const std::vector<SemTC> kARM32Carry = {
  {"adds_flags",    "adds r0, r1, r2",                           {{"r1", 0xFFFFFFFF}, {"r2", 1}},              {"r0"},            "Carry", {}},
  {"adc",           "adds r0, r1, r2; adc r3, r4, r5",
   {{"r1", 0xFFFFFFFF}, {"r2", 1}, {"r4", 10}, {"r5", 20}},     {"r3"},                                                          "Carry", {}},
  {"subs_flags",    "subs r0, r1, r2",                           {{"r1", 100}, {"r2", 50}},                    {"r0"},            "Carry", {}},
  {"sbc",           "subs r0, r1, r2; sbc r3, r4, r5",
   {{"r1", 100}, {"r2", 50}, {"r4", 100}, {"r5", 10}},           {"r3"},                                                          "Carry", {}},
};

static const std::vector<SemTC> kARM32Mem = {
  {"ldr_imm",       "ldr r0, [r1]",                              {{"r1", DATA_BASE}},                          {"r0"},            "Mem",
   {{DATA_BASE, packU32(0xDEADBEEF)}}},
  {"str_ldr",       "str r0, [r1]; ldr r2, [r1]",               {{"r0", 42}, {"r1", DATA_BASE}},               {"r2"},            "Mem", {}},
  {"ldr_offset",    "ldr r0, [r1, #4]",                          {{"r1", DATA_BASE}},                          {"r0"},            "Mem",
   {{DATA_BASE + 4, packU32(99)}}},
  {"ldrb",          "ldrb r0, [r1]",                             {{"r1", DATA_BASE}},                          {"r0"},            "Mem",
   {{DATA_BASE, {0x42}}}},
  {"ldrh",          "ldrh r0, [r1]",                             {{"r1", DATA_BASE}},                          {"r0"},            "Mem",
   {{DATA_BASE, packU16(0x1234)}}},
  {"ldrsb",         "ldrsb r0, [r1]",                            {{"r1", DATA_BASE}},                          {"r0"},            "Mem",
   {{DATA_BASE, {0x80}}}},
  {"ldrsh",         "ldrsh r0, [r1]",                            {{"r1", DATA_BASE}},                          {"r0"},            "Mem",
   {{DATA_BASE, packU16(0x8000)}}},
  {"push_pop",      "push {r0}; pop {r1}",                       {{"r0", 42}},                                 {"r1"},            "Mem", {}},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(Core, ARM32Semantic, ::testing::ValuesIn(kARM32Core), semTCName);
INSTANTIATE_TEST_SUITE_P(MulDiv, ARM32Semantic, ::testing::ValuesIn(kARM32MulDiv), semTCName);
// ============================================================================
// CoreExt: ADC, SBC, RSC, MRS/MSR patterns, conditional
// ============================================================================
static const std::vector<SemTC> kARM32CoreExt = {
  {"adc_simple",    "adds r0, r1, r2; adc r3, r4, #0",
   {{"r1", 0xFFFFFFFF}, {"r2", 1}, {"r4", 0}},                   {"r3"},            "CoreExt", {}},
  {"neg",           "rsb r0, r1, #0",                            {{"r1", 42}},                                 {"r0"},            "CoreExt", {}},
  {"mvn_val",       "mvn r0, #0xFF",                             {},                                           {"r0"},            "CoreExt", {}},
  {"orr_reg",       "orr r0, r1, r2",                            {{"r1", 0xF0}, {"r2", 0x0F}},                 {"r0"},            "CoreExt", {}},
  {"eor_reg",       "eor r0, r1, r2",                            {{"r1", 0xFF}, {"r2", 0x55}},                 {"r0"},            "CoreExt", {}},
  {"bic_reg",       "bic r0, r1, r2",                            {{"r1", 0xFF}, {"r2", 0x0F}},                 {"r0"},            "CoreExt", {}},
  {"cmp_eq",        "cmp r0, r1; moveq r2, #1; movne r2, #0",
   {{"r0", 42}, {"r1", 42}},                                     {"r2"},            "CoreExt", {}},
  {"cmp_lt",        "cmp r0, r1; movlt r2, #1; movge r2, #0",
   {{"r0", 10}, {"r1", 42}},                                     {"r2"},            "CoreExt", {}},
  {"tst",           "tst r0, #0xFF",                             {{"r0", 0x100}},                              {},                "CoreExt", {}},
};

// ============================================================================
// VFP: vmov, vadd, vsub, vmul, vdiv, vneg, vabs, vsqrt, vcmp, vcvt
// ============================================================================
static const std::vector<SemTC> kARM32VFP = {
  {"vadd_f32",      "vmov s0, r0; vmov s1, r1; vadd.f32 s2, s0, s1; vmov r2, s2",
   {{"r0", f32bits(1.0f)}, {"r1", f32bits(2.0f)}},                {"r2"},            "VFP", {}},
  {"vsub_f32",      "vmov s0, r0; vmov s1, r1; vsub.f32 s2, s0, s1; vmov r2, s2",
   {{"r0", f32bits(5.0f)}, {"r1", f32bits(2.0f)}},                {"r2"},            "VFP", {}},
  {"vmul_f32",      "vmov s0, r0; vmov s1, r1; vmul.f32 s2, s0, s1; vmov r2, s2",
   {{"r0", f32bits(3.0f)}, {"r1", f32bits(4.0f)}},                {"r2"},            "VFP", {}},
  {"vdiv_f32",      "vmov s0, r0; vmov s1, r1; vdiv.f32 s2, s0, s1; vmov r2, s2",
   {{"r0", f32bits(10.0f)}, {"r1", f32bits(2.0f)}},               {"r2"},            "VFP", {}},
  {"vneg_f32",      "vmov s0, r0; vneg.f32 s1, s0; vmov r1, s1",
   {{"r0", f32bits(3.0f)}},                                       {"r1"},            "VFP", {}},
  {"vabs_f32",      "vmov s0, r0; vabs.f32 s1, s0; vmov r1, s1",
   {{"r0", f32bits(-3.0f)}},                                      {"r1"},            "VFP", {}},
  {"vsqrt_f32",     "vmov s0, r0; vsqrt.f32 s1, s0; vmov r1, s1",
   {{"r0", f32bits(16.0f)}},                                      {"r1"},            "VFP", {}},
  {"vcvt_f32_u32",  "vmov s0, r0; vcvt.f32.u32 s1, s0; vmov r1, s1",
   {{"r0", 100}},                                                 {"r1"},            "VFP", {}},
  {"vcvt_u32_f32",  "vmov s0, r0; vcvt.u32.f32 s1, s0; vmov r1, s1",
   {{"r0", f32bits(42.9f)}},                                      {"r1"},            "VFP", {}},
  {"vcmp_f32",      "vmov s0, r0; vmov s1, r1; vcmp.f32 s0, s1; vmrs APSR_nzcv, fpscr",
   {{"r0", f32bits(1.0f)}, {"r1", f32bits(2.0f)}},                {},                "VFP", {}},
  {"vmla_f32",      "vmov s0, r0; vmov s1, r1; vmov s2, r2; vmla.f32 s0, s1, s2; vmov r3, s0",
   {{"r0", f32bits(10.0f)}, {"r1", f32bits(2.0f)}, {"r2", f32bits(3.0f)}}, {"r3"},   "VFP", {}},
};

// ============================================================================
// NEON: basic integer vector ops
// ============================================================================
static const std::vector<SemTC> kARM32NEON = {
  {"vdup_32",       "vdup.32 d0, r0; vmov r1, r2, d0",
   {{"r0", 0x12345678}},                                          {"r1", "r2"},      "NEON", {}},
  {"vmov_i32",      "vmov.i32 d0, #0; vmov r0, r1, d0",
   {},                                                             {"r0", "r1"},      "NEON", {}},
  {"vmvn_i32",      "vmov.i32 d0, #0; vmvn.i32 d1, d0; vmov r0, r1, d1",
   {},                                                             {"r0", "r1"},      "NEON", {}},
  {"vcvt_f64_f32",  "vmov s0, r0; vcvt.f64.f32 d1, s0; vmov r1, r2, d1",
   {{"r0", f32bits(1.5f)}},                                       {"r1", "r2"},      "NEON", {}},
};

// ============================================================================
// Mem Extra: LDR pre/post, LDRSH, LDRSB, STM/LDM
// ============================================================================
static const std::vector<SemTC> kARM32MemExt = {
  {"ldr_pre",       "ldr r0, [r1, #4]!",                         {{"r1", DATA_BASE}},                          {"r0", "r1"},      "MemExt",
   {{DATA_BASE + 4, packU32(0xDEADBEEF)}}},
  {"ldr_post",      "ldr r0, [r1], #4",                          {{"r1", DATA_BASE}},                          {"r0", "r1"},      "MemExt",
   {{DATA_BASE, packU32(0xCAFEBABE)}}},
  {"ldrsh",         "ldrsh r0, [r1]",                             {{"r1", DATA_BASE}},                          {"r0"},            "MemExt",
   {{DATA_BASE, packU16(0x8000)}}},
  {"strb_ldrb",     "strb r0, [r1]; ldrb r2, [r1]",              {{"r0", 0xAB}, {"r1", DATA_BASE}},             {"r2"},            "MemExt", {}},
  {"strh_ldrh",     "strh r0, [r1]; ldrh r2, [r1]",              {{"r0", 0xABCD}, {"r1", DATA_BASE}},           {"r2"},            "MemExt", {}},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(Shift, ARM32Semantic, ::testing::ValuesIn(kARM32Shift), semTCName);
INSTANTIATE_TEST_SUITE_P(Extend, ARM32Semantic, ::testing::ValuesIn(kARM32Extend), semTCName);
INSTANTIATE_TEST_SUITE_P(Carry, ARM32Semantic, ::testing::ValuesIn(kARM32Carry), semTCName);
INSTANTIATE_TEST_SUITE_P(Mem, ARM32Semantic, ::testing::ValuesIn(kARM32Mem), semTCName);
INSTANTIATE_TEST_SUITE_P(CoreExt, ARM32Semantic, ::testing::ValuesIn(kARM32CoreExt), semTCName);
INSTANTIATE_TEST_SUITE_P(VFP, ARM32Semantic, ::testing::ValuesIn(kARM32VFP), semTCName);
INSTANTIATE_TEST_SUITE_P(NEON, ARM32Semantic, ::testing::ValuesIn(kARM32NEON), semTCName);
INSTANTIATE_TEST_SUITE_P(MemExt, ARM32Semantic, ::testing::ValuesIn(kARM32MemExt), semTCName);
