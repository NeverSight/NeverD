//===- ARM32_AdvancedNEONRTTests.cpp - ARM32 advanced NEON RT --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests ARM32 advanced NEON/VFP patterns: widening, narrowing, FP conversion,
// multiply-accumulate, saturating arithmetic, etc.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32AdvNEONRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32AdvNEONRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32AdvNEON = {
  // ========== Integer narrowing/widening ==========
  {"arm_narrow_i32_i16",
   "int arm_narrow(int a) { return (int)(unsigned short)(unsigned int)a; }\n",
   {0x12345678ULL}, "ARM32AdvNEON"},

  {"arm_widen_u16_u32",
   "int arm_widen(int a) { return (int)(unsigned int)(unsigned short)a; }\n",
   {0xABCD}, "ARM32AdvNEON"},

  {"arm_widen_s8_s32",
   "int arm_widen_s8(int a) { return (int)(signed char)a; }\n",
   {0x80}, "ARM32AdvNEON"},  // -128

  // ========== Saturating patterns ==========
  {"arm_sat_add_u8",
   "int arm_sat_add_u8(int a, int b) {\n"
   "  unsigned int sum = (unsigned char)a + (unsigned char)b;\n"
   "  return sum > 255 ? 255 : (int)sum;\n"
   "}\n",
   {200, 100}, "ARM32AdvNEON"},

  {"arm_sat_sub_u8",
   "int arm_sat_sub_u8(int a, int b) {\n"
   "  int diff = (unsigned char)a - (unsigned char)b;\n"
   "  return diff < 0 ? 0 : diff;\n"
   "}\n",
   {50, 100}, "ARM32AdvNEON"},

  {"arm_sat_add_i16",
   "int arm_sat_add_i16(int a, int b) {\n"
   "  int sum = (short)(a & 0xFFFF) + (short)(b & 0xFFFF);\n"
   "  if (sum > 32767) sum = 32767;\n"
   "  if (sum < -32768) sum = -32768;\n"
   "  return (int)(unsigned short)(short)sum;\n"
   "}\n",
   {30000, 10000}, "ARM32AdvNEON"},

  // ========== FP conversion ==========
  {"arm_f32_to_i32",
   "int arm_f32_to_i32(int a) {\n"
   "  float f; __builtin_memcpy(&f, &a, 4);\n"
   "  return (int)f;\n"
   "}\n",
   {0x42280000ULL}, "ARM32AdvNEON"},  // 42.0f

  {"arm_i32_to_f32",
   "int arm_i32_to_f32(int a) {\n"
   "  float f = (float)a;\n"
   "  int r; __builtin_memcpy(&r, &f, 4); return r;\n"
   "}\n",
   {42}, "ARM32AdvNEON"},

  {"arm_f32_to_i32_neg",
   "int arm_f32_to_i32_neg(int a) {\n"
   "  float f; __builtin_memcpy(&f, &a, 4);\n"
   "  return (int)f;\n"
   "}\n",
   {0xC2280000ULL}, "ARM32AdvNEON"},  // -42.0f

  // ========== FP comparison ==========
  {"arm_fcmp_lt",
   "int arm_fcmp_lt(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  return fa < fb ? 1 : 0;\n"
   "}\n",
   {0x40400000ULL, 0x40A00000ULL}, "ARM32AdvNEON"},  // 3.0f < 5.0f

  {"arm_fcmp_eq",
   "int arm_fcmp_eq(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  return fa == fb ? 1 : 0;\n"
   "}\n",
   {0x42280000ULL, 0x42280000ULL}, "ARM32AdvNEON"},

  // ========== FP abs/neg ==========
  {"arm_fabs",
   "int arm_fabs(int a) {\n"
   "  float f; __builtin_memcpy(&f, &a, 4);\n"
   "  if (f < 0) f = -f;\n"
   "  int r; __builtin_memcpy(&r, &f, 4); return r;\n"
   "}\n",
   {0xC2280000ULL}, "ARM32AdvNEON"},  // -42.0f

  {"arm_fneg",
   "int arm_fneg(int a) {\n"
   "  float f; __builtin_memcpy(&f, &a, 4);\n"
   "  f = -f;\n"
   "  int r; __builtin_memcpy(&r, &f, 4); return r;\n"
   "}\n",
   {0x42280000ULL}, "ARM32AdvNEON"},

  // ========== Multiply-accumulate ==========
  {"arm_mla",
   "int arm_mla(int a, int b) { return a * b + 10; }\n",
   {7, 6}, "ARM32AdvNEON"},

  {"arm_mls",
   "int arm_mls(int a, int b) { return 100 - a * b; }\n",
   {7, 6}, "ARM32AdvNEON"},

  // ========== UMULL/SMULL patterns ==========
  {"arm_umull",
   "int arm_umull(int a, int b) {\n"
   "  unsigned long long r = (unsigned long long)(unsigned int)a *\n"
   "                         (unsigned long long)(unsigned int)b;\n"
   "  return (int)(r >> 32);\n"
   "}\n",
   {0x80000000ULL, 3}, "ARM32AdvNEON"},

  {"arm_smull",
   "int arm_smull(int a, int b) {\n"
   "  long long r = (long long)a * (long long)b;\n"
   "  return (int)(r >> 32);\n"
   "}\n",
   {(uint64_t)(uint32_t)(int32_t)-1000000, 1000000}, "ARM32AdvNEON"},

  // ========== Conditional patterns ==========
  {"arm_cond_max",
   "int arm_cond_max(int a, int b) {\n"
   "  return a > b ? a : b;\n"
   "}\n",
   {42, 100}, "ARM32AdvNEON"},

  {"arm_cond_abs",
   "int arm_cond_abs(int a) {\n"
   "  return a < 0 ? -a : a;\n"
   "}\n",
   {(uint64_t)(uint32_t)(int32_t)-42}, "ARM32AdvNEON"},

  // ========== Bit manipulation ==========
  {"arm_rev",
   "int arm_rev(int a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  return (int)(((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) |\n"
   "              ((x << 8) & 0xFF0000) | ((x << 24) & 0xFF000000));\n"
   "}\n",
   {0x12345678ULL}, "ARM32AdvNEON"},

  {"arm_rev16",
   "int arm_rev16(int a) {\n"
   "  unsigned x = (unsigned)a;\n"
   "  return (int)(((x & 0xFF00FF00u) >> 8) | ((x & 0x00FF00FFu) << 8));\n"
   "}\n",
   {0x12345678ULL}, "ARM32AdvNEON", /*OptLevel=*/1},

  {"arm_rev_byte",
   "int arm_rev_byte(int a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  return (int)(((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) |\n"
   "              ((x << 8) & 0xFF0000) | ((x << 24)));\n"
   "}\n",
   {0x12345678ULL}, "ARM32AdvNEON"},

  // ========== BFC/BFI patterns ==========
  {"arm_bfi_sim",
   "int arm_bfi(int val, int field) {\n"
   "  return (val & ~0xF0) | ((field & 0xF) << 4);\n"
   "}\n",
   {0xFF00, 0xA}, "ARM32AdvNEON"},

  {"arm_bfc_sim",
   "int arm_bfc(int val) {\n"
   "  return val & ~0xFF0;\n"
   "}\n",
   {0xDEADBEEFULL}, "ARM32AdvNEON"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ARM32AdvNEON, ARM32AdvNEONRT,
                         ::testing::ValuesIn(kARM32AdvNEON), rtTCName);
