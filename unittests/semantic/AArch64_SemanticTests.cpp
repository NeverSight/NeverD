//===- AArch64_SemanticTests.cpp - AArch64 semantic tests --------*- C++ -*-===//
//
// Migrated from scripts/lift_verifier.py — AArch64 instruction categories:
//   Core, BitShift, Cond, Extend, Carry, Mem, MulDiv, FP, Atomic, CoreExt,
//   MemExt, FPExt, CondSel, NEONScalar, NEONCvt, NEONMinMax, SIMDLdSt
//
//===----------------------------------------------------------------------===//

#include "UnicornSemanticFixture.h"

TEST_P(AArch64Semantic, Verify) { runAArch64(GetParam()); }

// clang-format off

// ============================================================================
// Core: ADD, SUB, AND, ORR, EOR, MOV, MVN, etc.
// ============================================================================
static const std::vector<SemTC> kA64Core = {
  {"mov_imm",       "mov x0, #42",                               {},                                           {"x0"},            "Core", {}},
  {"mov_reg",       "mov x0, x1",                                 {{"x1", 99}},                                 {"x0"},            "Core", {}},
  {"add_imm",       "add x0, x1, #42",                            {{"x1", 100}},                                {"x0"},            "Core", {}},
  {"add_reg",       "add x0, x1, x2",                             {{"x1", 50}, {"x2", 30}},                     {"x0"},            "Core", {}},
  {"sub_imm",       "sub x0, x1, #10",                            {{"x1", 100}},                                {"x0"},            "Core", {}},
  {"sub_reg",       "sub x0, x1, x2",                             {{"x1", 100}, {"x2", 30}},                    {"x0"},            "Core", {}},
  {"and_imm",       "and x0, x1, #0xFF",                          {{"x1", 0x12345678}},                         {"x0"},            "Core", {}},
  {"orr_imm",       "orr x0, x1, #0xFF",                          {{"x1", 0x100}},                              {"x0"},            "Core", {}},
  {"eor_imm",       "eor x0, x1, #0xFF",                          {{"x1", 0xAA}},                               {"x0"},            "Core", {}},
  {"eor_self",      "eor x0, x1, x1",                             {{"x1", 42}},                                 {"x0"},            "Core", {}},
  {"bic",           "bic x0, x1, x2",                             {{"x1", 0xFF}, {"x2", 0x0F}},                 {"x0"},            "Core", {}},
  {"movz",          "movz x0, #0x1234, lsl #16",                  {},                                           {"x0"},            "Core", {}},
  {"movk",          "movz x0, #0x5678; movk x0, #0x1234, lsl #16", {},                                         {"x0"},            "Core", {}},
  {"mul",           "mul x0, x1, x2",                             {{"x1", 6}, {"x2", 7}},                       {"x0"},            "Core", {}},
  {"madd",          "madd x0, x1, x2, x3",                       {{"x1", 6}, {"x2", 7}, {"x3", 10}},           {"x0"},            "Core", {}},
  {"msub",          "msub x0, x1, x2, x3",                       {{"x1", 6}, {"x2", 7}, {"x3", 100}},          {"x0"},            "Core", {}},
  {"udiv",          "udiv x0, x1, x2",                            {{"x1", 100}, {"x2", 7}},                     {"x0"},            "Core", {}},
  {"sdiv",          "sdiv x0, x1, x2",                            {{"x1", 100}, {"x2", 7}},                     {"x0"},            "Core", {}},
  {"neg",           "neg x0, x1",                                 {{"x1", 5}},                                  {"x0"},            "Core", {}},
  {"mvn",           "mvn x0, x1",                                 {{"x1", 0xFF00FF00FF00FF00ULL}},              {"x0"},            "Core", {}},
};

// ============================================================================
// BitShift: LSL, LSR, ASR, ROR, shifts with reg
// ============================================================================
static const std::vector<SemTC> kA64BitShift = {
  {"lsl_imm",       "lsl x0, x1, #3",                            {{"x1", 5}},                                  {"x0"},            "BitShift", {}},
  {"lsr_imm",       "lsr x0, x1, #2",                            {{"x1", 100}},                                {"x0"},            "BitShift", {}},
  {"asr_imm",       "asr x0, x1, #2",                            {{"x1", 0xFFFFFFFFFFFFFF00ULL}},              {"x0"},            "BitShift", {}},
  {"ror_imm",       "ror x0, x1, #4",                            {{"x1", 0xF}},                                {"x0"},            "BitShift", {}},
  {"lsl_reg",       "lsl x0, x1, x2",                            {{"x1", 1}, {"x2", 10}},                      {"x0"},            "BitShift", {}},
  {"lsr_reg",       "lsr x0, x1, x2",                            {{"x1", 1024}, {"x2", 2}},                    {"x0"},            "BitShift", {}},
  {"clz",           "clz x0, x1",                                {{"x1", 0x100}},                              {"x0"},            "BitShift", {}},
  {"cls_zero",      "cls x0, x1",                                {{"x1", 0}},                                  {"x0"},            "BitShift", {}},
  {"rbit",          "rbit x0, x1",                               {{"x1", 1}},                                  {"x0"},            "BitShift", {}},
  {"rev",           "rev x0, x1",                                {{"x1", 0x0102030405060708ULL}},               {"x0"},            "BitShift", {}},
  {"rev16",         "rev16 x0, x1",                              {{"x1", 0x0102030405060708ULL}},               {"x0"},            "BitShift", {}},
  {"rev32",         "rev32 x0, x1",                              {{"x1", 0x0102030405060708ULL}},               {"x0"},            "BitShift", {}},
};

// ============================================================================
// Cond: CMP, CSET, CSEL, CSINC, CSINV, CSNEG, CCMP, CCMN
// ============================================================================
static const std::vector<SemTC> kA64Cond = {
  {"cmp_eq",        "cmp x0, x1; cset x2, eq",                   {{"x0", 42}, {"x1", 42}},                     {"x2"},            "Cond", {}},
  {"cmp_ne",        "cmp x0, x1; cset x2, ne",                   {{"x0", 42}, {"x1", 99}},                     {"x2"},            "Cond", {}},
  {"cmp_lt",        "cmp x0, x1; cset x2, lt",                   {{"x0", 10}, {"x1", 42}},                     {"x2"},            "Cond", {}},
  {"cmp_gt",        "cmp x0, x1; cset x2, gt",                   {{"x0", 42}, {"x1", 10}},                     {"x2"},            "Cond", {}},
  {"cmp_lo",        "cmp x0, x1; cset x2, lo",                   {{"x0", 5}, {"x1", 10}},                      {"x2"},            "Cond", {}},
  {"cmp_hi",        "cmp x0, x1; cset x2, hi",                   {{"x0", 10}, {"x1", 5}},                      {"x2"},            "Cond", {}},
  {"csel_eq",       "cmp x1, x2; csel x0, x3, x4, eq",          {{"x1", 42}, {"x2", 42}, {"x3", 100}, {"x4", 200}}, {"x0"},      "Cond", {}},
  {"csel_ne",       "cmp x1, x2; csel x0, x3, x4, ne",          {{"x1", 42}, {"x2", 42}, {"x3", 100}, {"x4", 200}}, {"x0"},      "Cond", {}},
  {"csinc_eq",      "cmp x1, x2; csinc x0, x3, x4, eq",         {{"x1", 42}, {"x2", 42}, {"x3", 100}, {"x4", 200}}, {"x0"},      "Cond", {}},
  {"csinv_eq",      "cmp x1, x2; csinv x0, x3, x4, eq",         {{"x1", 42}, {"x2", 42}, {"x3", 100}, {"x4", 200}}, {"x0"},      "Cond", {}},
  {"csneg_ne",      "cmp x1, x2; csneg x0, x3, x4, ne",         {{"x1", 10}, {"x2", 20}, {"x3", 100}, {"x4", 200}}, {"x0"},      "Cond", {}},
  {"ccmp_eq",       "cmp x1, x2; ccmp x3, #5, #0, eq",          {{"x1", 42}, {"x2", 42}, {"x3", 5}},          {},                "Cond", {}},
  {"tst_zero",      "tst x0, #0xFF",                             {{"x0", 0x100}},                              {"x0"},            "Cond", {}},
  {"tst_nonzero",   "tst x0, #0xFF",                             {{"x0", 0x42}},                               {"x0"},            "Cond", {}},
};

// ============================================================================
// Extend: SXTB, SXTH, SXTW, UXTB, UXTH, UBFX, SBFX, BFI, BFXIL, EXTR
// ============================================================================
static const std::vector<SemTC> kA64Extend = {
  {"sxtb",          "sxtb x0, w1",                               {{"x1", 0x80}},                               {"x0"},            "Extend", {}},
  {"sxth",          "sxth x0, w1",                               {{"x1", 0x8000}},                             {"x0"},            "Extend", {}},
  {"sxtw",          "sxtw x0, w1",                               {{"x1", 0x80000000}},                         {"x0"},            "Extend", {}},
  {"uxtb",          "uxtb w0, w1",                               {{"x1", 0xABCD}},                             {"x0"},            "Extend", {}},
  {"uxth",          "uxth w0, w1",                               {{"x1", 0xABCDEF}},                           {"x0"},            "Extend", {}},
  {"ubfx",          "ubfx x0, x1, #4, #8",                      {{"x1", 0xABCD}},                             {"x0"},            "Extend", {}},
  {"sbfx",          "sbfx x0, x1, #4, #8",                      {{"x1", 0xABCD}},                             {"x0"},            "Extend", {}},
  {"bfi",           "mov x0, #0xFF00; bfi x0, x1, #4, #8",      {{"x1", 0xAB}},                               {"x0"},            "Extend", {}},
  {"extr",          "extr x0, x1, x2, #4",                       {{"x1", 0xF0}, {"x2", 0x0F}},                 {"x0"},            "Extend", {}},
};

// ============================================================================
// Carry: ADDS, ADCS, SUBS, SBCS
// ============================================================================
static const std::vector<SemTC> kA64Carry = {
  {"adds_carry",    "adds x0, x1, x2",                           {{"x1", 0xFFFFFFFFFFFFFFFFULL}, {"x2", 1}},   {"x0"},            "Carry", {}},
  {"adcs",          "adds x0, x1, x2; adcs x3, x4, x5",
   {{"x1", 0xFFFFFFFFFFFFFFFFULL}, {"x2", 1}, {"x4", 10}, {"x5", 20}}, {"x3"},                                                    "Carry", {}},
  {"subs_borrow",   "subs x0, x1, x2",                           {{"x1", 100}, {"x2", 50}},                    {"x0"},            "Carry", {}},
  {"sbcs",          "subs x0, x1, x2; sbcs x3, x4, x5",
   {{"x1", 100}, {"x2", 50}, {"x4", 100}, {"x5", 10}},          {"x3"},                                                           "Carry", {}},
};

// ============================================================================
// Mem: LDR, STR, LDP, STP, LDRB, LDRH, LDRSB, LDRSH, LDRSW, pre/post index
// ============================================================================
static const std::vector<SemTC> kA64Mem = {
  {"ldr_imm",       "ldr x0, [x1]",                              {{"x1", DATA_BASE}},                          {"x0"},            "Mem",
   {{DATA_BASE, packU64(0xDEADBEEFULL)}}},
  {"str_ldr",       "str x0, [x1]; ldr x2, [x1]",                {{"x0", 42}, {"x1", DATA_BASE}},              {"x2"},            "Mem", {}},
  {"ldr_offset",    "ldr x0, [x1, #8]",                           {{"x1", DATA_BASE}},                          {"x0"},            "Mem",
   {{DATA_BASE + 8, packU64(99)}}},
  {"ldrb",          "ldrb w0, [x1]",                              {{"x1", DATA_BASE}},                          {"x0"},            "Mem",
   {{DATA_BASE, {0x42}}}},
  {"ldrh",          "ldrh w0, [x1]",                              {{"x1", DATA_BASE}},                          {"x0"},            "Mem",
   {{DATA_BASE, packU16(0x1234)}}},
  {"ldrsb",         "ldrsb x0, [x1]",                             {{"x1", DATA_BASE}},                          {"x0"},            "Mem",
   {{DATA_BASE, {0x80}}}},
  {"ldrsh",         "ldrsh x0, [x1]",                             {{"x1", DATA_BASE}},                          {"x0"},            "Mem",
   {{DATA_BASE, packU16(0x8000)}}},
  {"ldrsw",         "ldrsw x0, [x1]",                             {{"x1", DATA_BASE}},                          {"x0"},            "Mem",
   {{DATA_BASE, packU32(0x80000000)}}},
  {"ldr_pre",       "ldr x0, [x1, #8]!",                          {{"x1", DATA_BASE}},                          {"x0", "x1"},      "Mem",
   {{DATA_BASE + 8, packU64(77)}}},
  {"ldr_post",      "ldr x0, [x1], #8",                           {{"x1", DATA_BASE}},                          {"x0", "x1"},      "Mem",
   {{DATA_BASE, packU64(88)}}},
  {"ldp_stp",       "stp x0, x1, [x2]; ldp x3, x4, [x2]",        {{"x0", 11}, {"x1", 22}, {"x2", DATA_BASE}},  {"x3", "x4"},      "Mem", {}},
  {"strb_ldrb",     "strb w0, [x1]; ldrb w2, [x1]",               {{"x0", 0xAB}, {"x1", DATA_BASE}},            {"x2"},            "Mem", {}},
};

// ============================================================================
// MulDiv: UMULH, SMULH, UMADDL, SMADDL, MNEG
// ============================================================================
static const std::vector<SemTC> kA64MulDiv = {
  {"umulh",         "umulh x0, x1, x2",                           {{"x1", 0xFFFFFFFFFFFFFFFFULL}, {"x2", 2}},   {"x0"},            "MulDiv", {}},
  {"smulh",         "smulh x0, x1, x2",                           {{"x1", 0xFFFFFFFFFFFFFFFFULL}, {"x2", 2}},   {"x0"},            "MulDiv", {}},
  {"umaddl",        "umaddl x0, w1, w2, x3",                      {{"x1", 0xFFFF}, {"x2", 0xFFFF}, {"x3", 100}},{"x0"},            "MulDiv", {}},
  {"smaddl",        "smaddl x0, w1, w2, x3",                      {{"x1", 0xFFFFFFFF}, {"x2", 2}, {"x3", 100}}, {"x0"},            "MulDiv", {}},
  {"mneg",          "mneg x0, x1, x2",                             {{"x1", 6}, {"x2", 7}},                       {"x0"},            "MulDiv", {}},
};

// ============================================================================
// FP: FMOV, FADD, FSUB, FMUL, FDIV, FNEG, FABS, FCVTZS, SCVTF, UCVTF, FCMP
// ============================================================================
static const std::vector<SemTC> kA64FP = {
  {"fmov_imm",      "fmov d0, #1.0; str d0, [x0]",               {{"x0", DATA_BASE}},                          {},                "FP",
   {{DATA_BASE, zeros(16)}}},
  {"fadd_d",        "ldr d0, [x0]; ldr d1, [x1]; fadd d2, d0, d1; str d2, [x0]",
   {{"x0", DATA_BASE}, {"x1", DATA_BASE + 16}},                   {},                                                              "FP",
   {{DATA_BASE, cat({packF64(1.0), zeros(8), packF64(2.0), zeros(8)})}}},
  {"fsub_d",        "ldr d0, [x0]; ldr d1, [x1]; fsub d2, d0, d1; str d2, [x0]",
   {{"x0", DATA_BASE}, {"x1", DATA_BASE + 16}},                   {},                                                              "FP",
   {{DATA_BASE, cat({packF64(5.0), zeros(8), packF64(2.0), zeros(8)})}}},
  {"fmul_d",        "ldr d0, [x0]; ldr d1, [x1]; fmul d2, d0, d1; str d2, [x0]",
   {{"x0", DATA_BASE}, {"x1", DATA_BASE + 16}},                   {},                                                              "FP",
   {{DATA_BASE, cat({packF64(3.0), zeros(8), packF64(4.0), zeros(8)})}}},
  {"fdiv_d",        "ldr d0, [x0]; ldr d1, [x1]; fdiv d2, d0, d1; str d2, [x0]",
   {{"x0", DATA_BASE}, {"x1", DATA_BASE + 16}},                   {},                                                              "FP",
   {{DATA_BASE, cat({packF64(10.0), zeros(8), packF64(2.0), zeros(8)})}}},
  {"fneg_d",        "ldr d0, [x0]; fneg d1, d0; str d1, [x0]",
   {{"x0", DATA_BASE}},                                           {},                                                              "FP",
   {{DATA_BASE, cat({packF64(5.0), zeros(8)})}}},
  {"fabs_d",        "ldr d0, [x0]; fabs d1, d0; str d1, [x0]",
   {{"x0", DATA_BASE}},                                           {},                                                              "FP",
   {{DATA_BASE, cat({packF64(-5.0), zeros(8)})}}},
  {"fcvtzs",        "ldr d0, [x0]; fcvtzs x1, d0",
   {{"x0", DATA_BASE}},                                           {"x1"},                                                          "FP",
   {{DATA_BASE, cat({packF64(42.9), zeros(8)})}}},
  {"scvtf",         "scvtf d0, x0; str d0, [x1]",
   {{"x0", 42}, {"x1", DATA_BASE}},                               {},                                                              "FP",
   {{DATA_BASE, zeros(16)}}},
  {"ucvtf",         "ucvtf d0, x0; str d0, [x1]",
   {{"x0", 42}, {"x1", DATA_BASE}},                               {},                                                              "FP",
   {{DATA_BASE, zeros(16)}}},
  {"fcmp_eq",       "ldr d0, [x0]; ldr d1, [x1]; fcmp d0, d1; cset x2, eq",
   {{"x0", DATA_BASE}, {"x1", DATA_BASE + 16}},                   {"x2"},                                                          "FP",
   {{DATA_BASE, cat({packF64(3.0), zeros(8), packF64(3.0), zeros(8)})}}},
  {"fsqrt_d",       "ldr d0, [x0]; fsqrt d1, d0; str d1, [x0]",
   {{"x0", DATA_BASE}},                                           {},                                                              "FP",
   {{DATA_BASE, cat({packF64(16.0), zeros(8)})}}},
};

// ============================================================================
// FPExt: FABS, FNEG, FSQRT, FCVTZS, SCVTF, UCVTF (fmov-based)
// ============================================================================
static const std::vector<SemTC> kA64FPExt = {
  {"fabs_d",        "fmov d0, #1.0; fneg d0, d0; fabs d0, d0; fmov x0, d0",
   {}, {"x0"}, "FPExt", {}},
  {"fneg_d",        "fmov d0, #2.0; fneg d0, d0; fmov x0, d0",
   {}, {"x0"}, "FPExt", {}},
  {"fsqrt_d",       "fmov d0, #4.0; fsqrt d0, d0; fmov x0, d0",
   {}, {"x0"}, "FPExt", {}},
  {"fcvtzs_d2x",    "fmov d0, #3.0; fcvtzs x0, d0",
   {}, {"x0"}, "FPExt", {}},
  {"scvtf_x2d",     "mov x0, #42; scvtf d0, x0; fmov x1, d0",
   {}, {"x1"}, "FPExt", {}},
  {"ucvtf_x2d",     "mov x0, #42; ucvtf d0, x0; fmov x1, d0",
   {}, {"x1"}, "FPExt", {}},
  {"fcvt_s2d",      "fmov s0, #2.0; fcvt d0, s0; fmov x0, d0",
   {}, {"x0"}, "FPExt", {}},
  {"fmov_imm_d",    "fmov d0, #1.0; fmov x0, d0",
   {}, {"x0"}, "FPExt", {}},
};

// ============================================================================
// Atomic: LDXR, STXR, LDAXR
// ============================================================================
static const std::vector<SemTC> kA64Atomic = {
  {"ldxr_stxr",     "ldxr x0, [x1]; add x0, x0, #1; stxr w2, x0, [x1]",
   {{"x1", DATA_BASE}}, {"x0", "x2"}, "Atomic",
   {{DATA_BASE, packU64(42)}}},
  {"ldaxr",         "ldaxr x0, [x1]",
   {{"x1", DATA_BASE}}, {"x0"}, "Atomic",
   {{DATA_BASE, packU64(99)}}},
};

// ============================================================================
// NEON scalar FP
// ============================================================================
static const std::vector<SemTC> kA64NEONScalar = {
  {"fmov_d_imm",    "fmov d0, #1.0; str d0, [x4]; ldr x0, [x4]",
   {{"x4", DATA_BASE}}, {"x0"}, "NEONScalar", {}},
  {"fneg_d",        "fmov d0, #2.0; fneg d1, d0; str d1, [x4]; ldr x0, [x4]",
   {{"x4", DATA_BASE}}, {"x0"}, "NEONScalar", {}},
  {"fmax_d",        "fmov d0, #1.0; fmov d1, #2.0; fmax d2, d0, d1; str d2, [x4]; ldr x0, [x4]",
   {{"x4", DATA_BASE}}, {"x0"}, "NEONScalar", {}},
  {"fmin_d",        "fmov d0, #1.0; fmov d1, #2.0; fmin d2, d0, d1; str d2, [x4]; ldr x0, [x4]",
   {{"x4", DATA_BASE}}, {"x0"}, "NEONScalar", {}},
  {"fmaxnm_d",      "fmov d0, #1.0; fmov d1, #2.0; fmaxnm d2, d0, d1; str d2, [x4]; ldr x0, [x4]",
   {{"x4", DATA_BASE}}, {"x0"}, "NEONScalar", {}},
  {"fminnm_d",      "fmov d0, #1.0; fmov d1, #2.0; fminnm d2, d0, d1; str d2, [x4]; ldr x0, [x4]",
   {{"x4", DATA_BASE}}, {"x0"}, "NEONScalar", {}},
};

// ============================================================================
// NEON Convert
// ============================================================================
static const std::vector<SemTC> kA64NEONCvt = {
  {"scvtf_d",       "scvtf d0, x0; str d0, [x4]; ldr x1, [x4]",
   {{"x0", 42}, {"x4", DATA_BASE}}, {"x1"}, "NEONCvt", {}},
  {"ucvtf_d",       "ucvtf d0, x0; str d0, [x4]; ldr x1, [x4]",
   {{"x0", 100}, {"x4", DATA_BASE}}, {"x1"}, "NEONCvt", {}},
  {"fcvtzs_x",      "fmov d0, #1.5; fcvtzs x0, d0",
   {}, {"x0"}, "NEONCvt", {}},
  {"fcvtzu_x",      "fmov d0, #2.5; fcvtzu x0, d0",
   {}, {"x0"}, "NEONCvt", {}},
  {"fcvt_d_s",      "fmov s0, #1.5; fcvt d1, s0; str d1, [x4]; ldr x0, [x4]",
   {{"x4", DATA_BASE}}, {"x0"}, "NEONCvt", {}},
};

// ============================================================================
// SIMD Load/Store
// ============================================================================
static const std::vector<SemTC> kA64SIMDLdSt = {
  {"str_q_ldr_q",   "stp x0, x1, [x4]; ldr q0, [x4]; str q0, [x4, #16]; ldp x2, x3, [x4, #16]",
   {{"x0", 0xDEADBEEFULL}, {"x1", 0xCAFEBABEULL}, {"x4", DATA_BASE}}, {"x2", "x3"}, "SIMDLdSt", {}},
  {"str_d_ldr_d",   "str x0, [x4]; ldr d0, [x4]; str d0, [x4, #8]; ldr x1, [x4, #8]",
   {{"x0", 0x123456789ABCDEF0ULL}, {"x4", DATA_BASE}}, {"x1"}, "SIMDLdSt", {}},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(Core, AArch64Semantic, ::testing::ValuesIn(kA64Core), semTCName);
INSTANTIATE_TEST_SUITE_P(BitShift, AArch64Semantic, ::testing::ValuesIn(kA64BitShift), semTCName);
INSTANTIATE_TEST_SUITE_P(Cond, AArch64Semantic, ::testing::ValuesIn(kA64Cond), semTCName);
INSTANTIATE_TEST_SUITE_P(Extend, AArch64Semantic, ::testing::ValuesIn(kA64Extend), semTCName);
INSTANTIATE_TEST_SUITE_P(Carry, AArch64Semantic, ::testing::ValuesIn(kA64Carry), semTCName);
INSTANTIATE_TEST_SUITE_P(Mem, AArch64Semantic, ::testing::ValuesIn(kA64Mem), semTCName);
INSTANTIATE_TEST_SUITE_P(MulDiv, AArch64Semantic, ::testing::ValuesIn(kA64MulDiv), semTCName);
INSTANTIATE_TEST_SUITE_P(FP, AArch64Semantic, ::testing::ValuesIn(kA64FP), semTCName);
INSTANTIATE_TEST_SUITE_P(FPExt, AArch64Semantic, ::testing::ValuesIn(kA64FPExt), semTCName);
// ============================================================================
// CoreExt: NEG, NEGS, MVN, CLS, CLZ, RBIT, REV, TST, CMN, ADR
// ============================================================================
static const std::vector<SemTC> kA64CoreExt = {
  {"negs",          "negs x0, x1",                                {{"x1", 5}},                                  {"x0"},            "CoreExt", {}},
  {"cls_neg",       "cls x0, x1",                                 {{"x1", 0xFFFFFFFF00000000ULL}},              {"x0"},            "CoreExt", {}},
  {"clz_nonzero",   "clz x0, x1",                                {{"x1", 0x00000100}},                         {"x0"},            "CoreExt", {}},
  {"cmn_regs",      "cmn x0, x1",                                {{"x0", 5}, {"x1", 3}},                       {"x0"},            "CoreExt", {}},
  {"adr_offset",    "adr x0, #0x10",                             {},                                           {"x0"},            "CoreExt", {}},
};

// ============================================================================
// MemExt: LDR/STR pre/post index, LDP, register offset/shifted
// ============================================================================
static const std::vector<SemTC> kA64MemExt = {
  {"ldr_pre_idx",   "ldr x0, [x1, #8]!",                         {{"x1", DATA_BASE}},                          {"x0", "x1"},      "MemExt",
   {{DATA_BASE + 8, packU64(0xDEADBEEFULL)}}},
  {"ldr_post_idx",  "ldr x0, [x1], #8",                           {{"x1", DATA_BASE}},                          {"x0", "x1"},      "MemExt",
   {{DATA_BASE, packU64(0xCAFEBABEULL)}}},
  {"ldp_pair",      "ldp x0, x1, [x2]",                           {{"x2", DATA_BASE}},                          {"x0", "x1"},      "MemExt",
   {{DATA_BASE, cat({packU64(100), packU64(200)})}}},
  {"stp_pair",      "mov x0, #10; mov x1, #20; stp x0, x1, [x2]", {{"x2", DATA_BASE}},                         {},                "MemExt", {}},
  {"ldr_reg_offset","ldr x0, [x1, x2]",                           {{"x1", DATA_BASE}, {"x2", 8}},               {"x0"},            "MemExt",
   {{DATA_BASE + 8, packU64(0x12345678ULL)}}},
  {"ldr_reg_shifted","ldr x0, [x1, x2, lsl #3]",                  {{"x1", DATA_BASE}, {"x2", 1}},               {"x0"},            "MemExt",
   {{DATA_BASE + 8, packU64(0xABCDEFULL)}}},
  {"ldrb_load",     "ldrb w0, [x1]",                              {{"x1", DATA_BASE}},                          {"x0"},            "MemExt",
   {{DATA_BASE, {0xAB}}}},
  {"ldrh_load",     "ldrh w0, [x1]",                              {{"x1", DATA_BASE}},                          {"x0"},            "MemExt",
   {{DATA_BASE, packU16(0xABCD)}}},
  {"strb_store",    "mov w0, #0x42; strb w0, [x1]",               {{"x1", DATA_BASE}},                          {"x0"},            "MemExt", {}},
  {"strh_store",    "mov w0, #0x1234; strh w0, [x1]",             {{"x1", DATA_BASE}},                          {"x0"},            "MemExt", {}},
};

// ============================================================================
// A64 32-bit ops (W-register instructions)
// ============================================================================
static const std::vector<SemTC> kA64Op32 = {
  {"add_w",         "add w0, w1, #42",                            {{"x1", 100}},                                {"x0"},            "A64Op32", {}},
  {"sub_w",         "sub w0, w1, #10",                            {{"x1", 100}},                                {"x0"},            "A64Op32", {}},
  {"and_w",         "and w0, w1, #0xFF",                          {{"x1", 0x12345678}},                         {"x0"},            "A64Op32", {}},
  {"lsl_w",         "lsl w0, w1, #3",                             {{"x1", 5}},                                  {"x0"},            "A64Op32", {}},
  {"lsr_w",         "lsr w0, w1, #2",                             {{"x1", 100}},                                {"x0"},            "A64Op32", {}},
  {"mul_w",         "mul w0, w1, w2",                             {{"x1", 6}, {"x2", 7}},                       {"x0"},            "A64Op32", {}},
  {"udiv_w",        "udiv w0, w1, w2",                            {{"x1", 100}, {"x2", 7}},                     {"x0"},            "A64Op32", {}},
  {"sdiv_w",        "sdiv w0, w1, w2",                            {{"x1", 100}, {"x2", 7}},                     {"x0"},            "A64Op32", {}},
};

// ============================================================================
// NEON FP MinMax
// ============================================================================
static const std::vector<SemTC> kA64NEONMinMax = {
  {"fmaxnm_d",      "fmov d0, #1.0; fmov d1, #2.0; fmaxnm d2, d0, d1; str d2, [x4]; ldr x0, [x4]",
   {{"x4", DATA_BASE}}, {"x0"}, "NEONMinMax", {}},
  {"fminnm_d",      "fmov d0, #1.0; fmov d1, #2.0; fminnm d2, d0, d1; str d2, [x4]; ldr x0, [x4]",
   {{"x4", DATA_BASE}}, {"x0"}, "NEONMinMax", {}},
  {"fmax_s",        "fmov s0, #1.0; fmov s1, #2.0; fmax s2, s0, s1; str s2, [x4]; ldr w0, [x4]",
   {{"x4", DATA_BASE}}, {"x0"}, "NEONMinMax", {}},
  {"fmin_s",        "fmov s0, #1.0; fmov s1, #2.0; fmin s2, s0, s1; str s2, [x4]; ldr w0, [x4]",
   {{"x4", DATA_BASE}}, {"x0"}, "NEONMinMax", {}},
};

// ============================================================================
// NEON Tbl / FP moves
// ============================================================================
static const std::vector<SemTC> kA64NEONTbl = {
  {"fmov_v_imm",    "fmov d0, #1.0; str d0, [x4]; ldr x0, [x4]",
   {{"x4", DATA_BASE}}, {"x0"}, "NEONTbl", {}},
  {"fmov_v_reg",    "fmov d0, x0; str d0, [x4]; ldr x1, [x4]",
   {{"x0", 0x3FF0000000000000ULL}, {"x4", DATA_BASE}}, {"x1"}, "NEONTbl", {}},
  {"fmov_gp_from_v","fmov d0, x0; fmov x1, d0",
   {{"x0", 0xDEADBEEFULL}}, {"x1"}, "NEONTbl", {}},
};

// ============================================================================
// Mem pair (byte/half signed extensions)
// ============================================================================
static const std::vector<SemTC> kA64MemPair = {
  {"ldrb_strb",     "strb w0, [x4]; ldrb w1, [x4]",
   {{"x0", 0xFF}, {"x4", DATA_BASE}}, {"x1"}, "MemPair", {}},
  {"ldrh_strh",     "strh w0, [x4]; ldrh w1, [x4]",
   {{"x0", 0xABCD}, {"x4", DATA_BASE}}, {"x1"}, "MemPair", {}},
  {"ldrsb",         "strb w0, [x4]; ldrsb x1, [x4]",
   {{"x0", 0x80}, {"x4", DATA_BASE}}, {"x1"}, "MemPair", {}},
  {"ldrsh",         "strh w0, [x4]; ldrsh x1, [x4]",
   {{"x0", 0x8000}, {"x4", DATA_BASE}}, {"x1"}, "MemPair", {}},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(Atomic, AArch64Semantic, ::testing::ValuesIn(kA64Atomic), semTCName);
INSTANTIATE_TEST_SUITE_P(NEONScalar, AArch64Semantic, ::testing::ValuesIn(kA64NEONScalar), semTCName);
INSTANTIATE_TEST_SUITE_P(NEONCvt, AArch64Semantic, ::testing::ValuesIn(kA64NEONCvt), semTCName);
INSTANTIATE_TEST_SUITE_P(SIMDLdSt, AArch64Semantic, ::testing::ValuesIn(kA64SIMDLdSt), semTCName);
INSTANTIATE_TEST_SUITE_P(CoreExt, AArch64Semantic, ::testing::ValuesIn(kA64CoreExt), semTCName);
INSTANTIATE_TEST_SUITE_P(MemExt, AArch64Semantic, ::testing::ValuesIn(kA64MemExt), semTCName);
INSTANTIATE_TEST_SUITE_P(A64Op32, AArch64Semantic, ::testing::ValuesIn(kA64Op32), semTCName);
INSTANTIATE_TEST_SUITE_P(NEONMinMax, AArch64Semantic, ::testing::ValuesIn(kA64NEONMinMax), semTCName);
INSTANTIATE_TEST_SUITE_P(NEONTbl, AArch64Semantic, ::testing::ValuesIn(kA64NEONTbl), semTCName);
INSTANTIATE_TEST_SUITE_P(MemPair, AArch64Semantic, ::testing::ValuesIn(kA64MemPair), semTCName);
