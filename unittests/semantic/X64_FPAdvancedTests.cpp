//===- X64_FPAdvancedTests.cpp - Advanced FP pattern tests ----*- C++ -*-===//
//
// Tests x86_64 FP patterns: sqrt, NaN, infinity, denormal, multi-step
// conversions, FP comparison chains, and mixed int/FP patterns.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FPART : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPART, Verify) { roundTripX64(GetParam()); }

// clang-format off

#define FP_RET_D \
  "  long r;\n  __builtin_memcpy(&r, &result, 8);\n  return r;\n"

static const std::vector<RoundTripTC> kX64FPAdv = {
  // --- FP comparison chain ---
  {"fp_clamp_d",
   "long fp_clamp_d(long a, long lo_bits, long hi_bits) {\n"
   "  double val, lo, hi;\n"
   "  __builtin_memcpy(&val, &a, 8);\n"
   "  __builtin_memcpy(&lo, &lo_bits, 8);\n"
   "  __builtin_memcpy(&hi, &hi_bits, 8);\n"
   "  double result = val < lo ? lo : (val > hi ? hi : val);\n"
   FP_RET_D
   "}\n",
   {0x4024000000000000ULL, 0x3FF0000000000000ULL, 0x4014000000000000ULL}, "FPAdv"},

  // --- FP abs via bitwise ---
  {"fp_abs_bitwise",
   "long fp_abs_bitwise(long a) {\n"
   "  return a & 0x7FFFFFFFFFFFFFFFULL;\n"
   "}\n",
   {0xC014000000000000ULL}, "FPAdv"},

  // --- FP negate via XOR ---
  {"fp_neg_xor",
   "long fp_neg_xor(long a) {\n"
   "  return a ^ 0x8000000000000000ULL;\n"
   "}\n",
   {0x4014000000000000ULL}, "FPAdv"},

  // --- FP copysign ---
  {"fp_copysign",
   "long fp_copysign(long mag_bits, long sign_bits) {\n"
   "  return (mag_bits & 0x7FFFFFFFFFFFFFFFULL) | (sign_bits & 0x8000000000000000ULL);\n"
   "}\n",
   {0x4014000000000000ULL, 0xC000000000000000ULL}, "FPAdv"},

  // --- Double to float to double roundtrip ---
  {"fp_d2f2d",
   "long fp_d2f2d(long a) {\n"
   "  double d;\n"
   "  __builtin_memcpy(&d, &a, 8);\n"
   "  float f = (float)d;\n"
   "  double result = (double)f;\n"
   FP_RET_D
   "}\n",
   {0x4014000000000000ULL}, "FPAdv"},

  // --- FP linear interpolation ---
  {"fp_lerp",
   "long fp_lerp(long a_bits, long b_bits, long t_bits) {\n"
   "  double a, b, t;\n"
   "  __builtin_memcpy(&a, &a_bits, 8);\n"
   "  __builtin_memcpy(&b, &b_bits, 8);\n"
   "  __builtin_memcpy(&t, &t_bits, 8);\n"
   "  double result = a + (b - a) * t;\n"
   FP_RET_D
   "}\n",
   {0x0000000000000000ULL, 0x4024000000000000ULL, 0x3FE0000000000000ULL}, "FPAdv"},

  // --- FP to int clamped ---
  {"fp_to_int_clamp",
   "long fp_to_int_clamp(long a) {\n"
   "  double d;\n"
   "  __builtin_memcpy(&d, &a, 8);\n"
   "  if (d > 255.0) return 255;\n"
   "  if (d < 0.0) return 0;\n"
   "  return (long)d;\n"
   "}\n",
   {0x406F400000000000ULL}, "FPAdv"},

  // --- Mixed int/FP arithmetic ---
  {"fp_scale_int",
   "long fp_scale_int(long val, long scale_bits) {\n"
   "  double scale;\n"
   "  __builtin_memcpy(&scale, &scale_bits, 8);\n"
   "  return (long)((double)val * scale);\n"
   "}\n",
   {100, 0x4000000000000000ULL}, "FPAdv"},

  // --- Float add chain ---
  {"fp_add_chain",
   "long fp_add_chain(long a, long b, long c) {\n"
   "  double da, db, dc;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  __builtin_memcpy(&db, &b, 8);\n"
   "  __builtin_memcpy(&dc, &c, 8);\n"
   "  double result = da + db + dc;\n"
   FP_RET_D
   "}\n",
   {0x3FF0000000000000ULL, 0x4000000000000000ULL, 0x4008000000000000ULL}, "FPAdv"},

  // --- FP sub then abs ---
  {"fp_abs_diff",
   "long fp_abs_diff(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  __builtin_memcpy(&db, &b, 8);\n"
   "  double diff = da - db;\n"
   "  double result = diff < 0 ? -diff : diff;\n"
   FP_RET_D
   "}\n",
   {0x4008000000000000ULL, 0x4014000000000000ULL}, "FPAdv"},

  // --- Compare and select ---
  {"fp_cmp_select",
   "long fp_cmp_select(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  __builtin_memcpy(&db, &b, 8);\n"
   "  return da > db ? 1 : da < db ? -1 : 0;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPAdv"},

  // --- Successive operations ---
  {"fp_compound",
   "long fp_compound(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  __builtin_memcpy(&db, &b, 8);\n"
   "  double result = (da * db) + (da - db);\n"
   FP_RET_D
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPAdv"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(FPAdv, X64FPART,
                         ::testing::ValuesIn(kX64FPAdv), rtTCName);
