//===- ARM32_DivFlagRTTests.cpp - ARM32 div + flag patterns ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests ARM32 division, conditional execution, and VFP operations.
// ARM32 is unique in having predicated instructions and IT blocks.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32DivFlagRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32DivFlagRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32DivFlag = {
  // ========== Signed division ==========
  {"arm_sdiv_pos",
   "int arm_sdiv_pos(int a, int b) {\n"
   "  return a / b;\n"
   "}\n",
   {42, 7}, "ARM32DivFlag"},

  {"arm_sdiv_neg",
   "int arm_sdiv_neg(int a, int b) {\n"
   "  return a / b;\n"
   "}\n",
   {(uint64_t)(int32_t)-42, 7}, "ARM32DivFlag"},

  {"arm_smod",
   "int arm_smod(int a, int b) {\n"
   "  return a % b;\n"
   "}\n",
   {43, 7}, "ARM32DivFlag"},

  // ========== Unsigned division ==========
  {"arm_udiv",
   "int arm_udiv(int a, int b) {\n"
   "  return (unsigned)a / (unsigned)b;\n"
   "}\n",
   {100, 7}, "ARM32DivFlag"},

  {"arm_umod",
   "int arm_umod(int a, int b) {\n"
   "  return (unsigned)a % (unsigned)b;\n"
   "}\n",
   {100, 7}, "ARM32DivFlag"},

  // ========== Conditional select (IT block patterns) ==========
  {"arm_csel_lt",
   "int arm_csel_lt(int a, int b) {\n"
   "  return a < b ? a : b;\n"
   "}\n",
   {10, 20}, "ARM32DivFlag"},

  {"arm_csel_gt",
   "int arm_csel_gt(int a, int b) {\n"
   "  return a > b ? a : b;\n"
   "}\n",
   {20, 10}, "ARM32DivFlag"},

  {"arm_csel_eq",
   "int arm_csel_eq(int a, int b) {\n"
   "  return a == 0 ? b : a;\n"
   "}\n",
   {0, 42}, "ARM32DivFlag"},

  // ========== Absolute value ==========
  {"arm_abs",
   "int arm_abs(int a) {\n"
   "  return a < 0 ? -a : a;\n"
   "}\n",
   {(uint64_t)(int32_t)-42}, "ARM32DivFlag"},

  {"arm_abs_pos",
   "int arm_abs_pos(int a) {\n"
   "  return a < 0 ? -a : a;\n"
   "}\n",
   {42}, "ARM32DivFlag"},

  // ========== Sign function ==========
  {"arm_sign",
   "int arm_sign(int a) {\n"
   "  if (a < 0) return -1;\n"
   "  if (a > 0) return 1;\n"
   "  return 0;\n"
   "}\n",
   {(uint64_t)(int32_t)-100}, "ARM32DivFlag"},

  {"arm_sign_pos",
   "int arm_sign_pos(int a) {\n"
   "  if (a < 0) return -1;\n"
   "  if (a > 0) return 1;\n"
   "  return 0;\n"
   "}\n",
   {100}, "ARM32DivFlag"},

  {"arm_sign_zero",
   "int arm_sign_zero(int a) {\n"
   "  if (a < 0) return -1;\n"
   "  if (a > 0) return 1;\n"
   "  return 0;\n"
   "}\n",
   {0}, "ARM32DivFlag"},

  // ========== Compound conditions ==========
  {"arm_and_cond",
   "int arm_and_cond(int a, int b) {\n"
   "  return (a > 0 && b > 0) ? 1 : 0;\n"
   "}\n",
   {10, 20}, "ARM32DivFlag"},

  {"arm_or_cond",
   "int arm_or_cond(int a, int b) {\n"
   "  return (a == 0 || b == 0) ? 1 : 0;\n"
   "}\n",
   {0, 42}, "ARM32DivFlag"},

  // ========== Range check ==========
  {"arm_range",
   "int arm_range(int a) {\n"
   "  return (a >= 10 && a <= 100) ? 1 : 0;\n"
   "}\n",
   {50}, "ARM32DivFlag"},

  {"arm_range_out",
   "int arm_range_out(int a) {\n"
   "  return (a >= 10 && a <= 100) ? 1 : 0;\n"
   "}\n",
   {200}, "ARM32DivFlag"},

  // ========== Saturated add/sub ==========
  {"arm_sat_add_u8",
   "int arm_sat_add_u8(int a, int b) {\n"
   "  unsigned sum = (unsigned char)a + (unsigned char)b;\n"
   "  return sum > 255 ? 255 : (int)sum;\n"
   "}\n",
   {200, 100}, "ARM32DivFlag"},

  {"arm_sat_sub_u8",
   "int arm_sat_sub_u8(int a, int b) {\n"
   "  int diff = (unsigned char)a - (unsigned char)b;\n"
   "  return diff < 0 ? 0 : diff;\n"
   "}\n",
   {50, 100}, "ARM32DivFlag"},

  // ========== Clamp ==========
  {"arm_clamp",
   "int arm_clamp(int val, int lo, int hi) {\n"
   "  if (val < lo) return lo;\n"
   "  if (val > hi) return hi;\n"
   "  return val;\n"
   "}\n",
   {300, 0, 255}, "ARM32DivFlag"},

  {"arm_clamp_in",
   "int arm_clamp_in(int val, int lo, int hi) {\n"
   "  if (val < lo) return lo;\n"
   "  if (val > hi) return hi;\n"
   "  return val;\n"
   "}\n",
   {100, 0, 255}, "ARM32DivFlag"},

  // ========== Multiply-accumulate ==========
  {"arm_mla",
   "int arm_mla(int a, int b, int c) {\n"
   "  return a * b + c;\n"
   "}\n",
   {7, 6, 10}, "ARM32DivFlag"},

  {"arm_mls",
   "int arm_mls(int a, int b, int c) {\n"
   "  return c - a * b;\n"
   "}\n",
   {7, 6, 100}, "ARM32DivFlag"},

  // ========== Bit counting ==========
  {"arm_popcount",
   "int arm_popcount(int a) {\n"
   "  unsigned x = (unsigned)a;\n"
   "  int c = 0;\n"
   "  while (x) { c += x & 1; x >>= 1; }\n"
   "  return c;\n"
   "}\n",
   {0xFF}, "ARM32DivFlag"},

  // ========== FP compare + select ==========
  {"arm_fp_min",
   "int arm_fp_min(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  float r = fa < fb ? fa : fb;\n"
   "  int ret; __builtin_memcpy(&ret, &r, 4); return ret;\n"
   "}\n",
   {0x40A00000, 0x40400000}, "ARM32DivFlag"},  // 5.0f, 3.0f

  {"arm_fp_max",
   "int arm_fp_max(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  float r = fa > fb ? fa : fb;\n"
   "  int ret; __builtin_memcpy(&ret, &r, 4); return ret;\n"
   "}\n",
   {0x40A00000, 0x40400000}, "ARM32DivFlag"},

  // ========== FP abs ==========
  {"arm_fp_abs",
   "int arm_fp_abs(int a) {\n"
   "  float f; __builtin_memcpy(&f, &a, 4);\n"
   "  if (f < 0) f = -f;\n"
   "  int ret; __builtin_memcpy(&ret, &f, 4); return ret;\n"
   "}\n",
   {0xC2280000}, "ARM32DivFlag"},  // -42.0f
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ARM32DivFlag, ARM32DivFlagRT,
                         ::testing::ValuesIn(kARM32DivFlag), rtTCName);
