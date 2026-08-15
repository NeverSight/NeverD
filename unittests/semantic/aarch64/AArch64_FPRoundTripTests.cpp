//===- AArch64_FPRoundTripTests.cpp - FP roundtrip via int bitcast -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests AArch64 floating-point and NEON scalar instructions through lift.
// Uses integer bitcast to avoid FP register ABI issues.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64FPRoundTrip : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64FPRoundTrip, FPVerify) { roundTripAArch64(GetParam()); }

// clang-format off

#define A64_FP_RET \
  "  long r;\n" \
  "  __builtin_memcpy(&r, &result, 8);\n" \
  "  return r;\n"

static const std::vector<RoundTripTC> kA64FPRoundTrip = {
  {"fp_add",
   "long fp_add(long a, long b) {\n"
   "  double af, bf;\n"
   "  __builtin_memcpy(&af, &a, 8);\n"
   "  __builtin_memcpy(&bf, &b, 8);\n"
   "  double result = af + bf;\n"
   A64_FP_RET
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPRT"},

  {"fp_sub",
   "long fp_sub(long a, long b) {\n"
   "  double af, bf;\n"
   "  __builtin_memcpy(&af, &a, 8);\n"
   "  __builtin_memcpy(&bf, &b, 8);\n"
   "  double result = af - bf;\n"
   A64_FP_RET
   "}\n",
   {0x4024000000000000ULL, 0x4008000000000000ULL}, "FPRT"},

  {"fp_mul",
   "long fp_mul(long a, long b) {\n"
   "  double af, bf;\n"
   "  __builtin_memcpy(&af, &a, 8);\n"
   "  __builtin_memcpy(&bf, &b, 8);\n"
   "  double result = af * bf;\n"
   A64_FP_RET
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPRT"},

  {"fp_div",
   "long fp_div(long a, long b) {\n"
   "  double af, bf;\n"
   "  __builtin_memcpy(&af, &a, 8);\n"
   "  __builtin_memcpy(&bf, &b, 8);\n"
   "  double result = af / bf;\n"
   A64_FP_RET
   "}\n",
   {0x4024000000000000ULL, 0x4000000000000000ULL}, "FPRT"},

  {"fp_neg",
   "long fp_neg(long a) {\n"
   "  double af;\n"
   "  __builtin_memcpy(&af, &a, 8);\n"
   "  double result = -af;\n"
   A64_FP_RET
   "}\n",
   {0x4014000000000000ULL}, "FPRT"},

  {"fp_abs",
   "long fp_abs(long a) {\n"
   "  double af;\n"
   "  __builtin_memcpy(&af, &a, 8);\n"
   "  double result = af < 0 ? -af : af;\n"
   A64_FP_RET
   "}\n",
   {0xC014000000000000ULL}, "FPRT"},

  {"fp_i64_to_f64",
   "long fp_i64_to_f64(long a) {\n"
   "  double result = (double)a;\n"
   A64_FP_RET
   "}\n",
   {42}, "FPRT"},

  {"fp_f64_to_i64",
   "long fp_f64_to_i64(long a) {\n"
   "  double af;\n"
   "  __builtin_memcpy(&af, &a, 8);\n"
   "  return (long)af;\n"
   "}\n",
   {0x4014000000000000ULL}, "FPRT"},

  {"fp_cmp_lt",
   "long fp_cmp_lt(long a, long b) {\n"
   "  double af, bf;\n"
   "  __builtin_memcpy(&af, &a, 8);\n"
   "  __builtin_memcpy(&bf, &b, 8);\n"
   "  return af < bf;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4014000000000000ULL}, "FPRT"},

  {"fp_cmp_eq",
   "long fp_cmp_eq(long a, long b) {\n"
   "  double af, bf;\n"
   "  __builtin_memcpy(&af, &a, 8);\n"
   "  __builtin_memcpy(&bf, &b, 8);\n"
   "  return af == bf;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4014000000000000ULL}, "FPRT"},

  {"fp_min",
   "long fp_min(long a, long b) {\n"
   "  double af, bf;\n"
   "  __builtin_memcpy(&af, &a, 8);\n"
   "  __builtin_memcpy(&bf, &b, 8);\n"
   "  double result = af < bf ? af : bf;\n"
   A64_FP_RET
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPRT"},

  {"fp_max",
   "long fp_max(long a, long b) {\n"
   "  double af, bf;\n"
   "  __builtin_memcpy(&af, &a, 8);\n"
   "  __builtin_memcpy(&bf, &b, 8);\n"
   "  double result = af > bf ? af : bf;\n"
   A64_FP_RET
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPRT"},

  {"fp_muladd",
   "long fp_muladd(long a, long b, long c) {\n"
   "  double af, bf, cf;\n"
   "  __builtin_memcpy(&af, &a, 8);\n"
   "  __builtin_memcpy(&bf, &b, 8);\n"
   "  __builtin_memcpy(&cf, &c, 8);\n"
   "  double result = af * bf + cf;\n"
   A64_FP_RET
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL, 0x3FF0000000000000ULL}, "FPRT"},

  {"fp_float_add",
   "long fp_float_add(long a, long b) {\n"
   "  int ai = (int)a, bi = (int)b;\n"
   "  float af, bf;\n"
   "  __builtin_memcpy(&af, &ai, 4);\n"
   "  __builtin_memcpy(&bf, &bi, 4);\n"
   "  float result = af + bf;\n"
   "  int ri;\n"
   "  __builtin_memcpy(&ri, &result, 4);\n"
   "  return ri;\n"
   "}\n",
   {0x40A00000, 0x40400000}, "FPRT"},

  // FRECPX(3.0f) returns the reciprocal exponent 1.0f.  FRECPE would instead
  // produce a reciprocal estimate near 1/3, so this input distinguishes the
  // two instructions while the integer ABI keeps the comparison bit-exact.
  {"frecpx_s",
   "long frecpx_s(long a) {\n"
   "  unsigned int result;\n"
   "  __asm__ volatile(\"fmov s0, %w1\\n frecpx s0, s0\\n fmov %w0, s0\"\n"
   "                   : \"=r\"(result) : \"r\"((unsigned int)a) : \"v0\");\n"
   "  return (long)result;\n"
   "}\n",
   {0x40400000}, "FPRT"},

  {"fp_f32_to_f64",
   "long fp_f32_to_f64(long a) {\n"
   "  int ai = (int)a;\n"
   "  float af;\n"
   "  __builtin_memcpy(&af, &ai, 4);\n"
   "  double result = (double)af;\n"
   A64_FP_RET
   "}\n",
   {0x40A00000}, "FPRT"},

  {"fp_sum_loop",
   "long fp_sum_loop(long n) {\n"
   "  double sum = 0.0;\n"
   "  for (long i = 1; i <= n; ++i)\n"
   "    sum += (double)i;\n"
   "  double result = sum;\n"
   A64_FP_RET
   "}\n",
   {10}, "FPRT"},

  {"fp_select",
   "long fp_select(long cond, long a, long b) {\n"
   "  double af, bf;\n"
   "  __builtin_memcpy(&af, &a, 8);\n"
   "  __builtin_memcpy(&bf, &b, 8);\n"
   "  double result = cond ? af : bf;\n"
   A64_FP_RET
   "}\n",
   {1, 0x4014000000000000ULL, 0x4008000000000000ULL}, "FPRT"},

  // fp_isqrt removed: complex FP loop exceeds Unicorn stack mapping
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(FPRT, AArch64FPRoundTrip,
                         ::testing::ValuesIn(kA64FPRoundTrip), rtTCName);
