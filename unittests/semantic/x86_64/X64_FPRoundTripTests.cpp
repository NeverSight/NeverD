//===- X64_FPRoundTripTests.cpp - FP roundtrip via integer bitcast -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests x86_64 floating-point instructions through the lift pipeline.
// Uses integer parameters with __builtin_memcpy to bitcast, so the function
// ABI stays integer-register based (rdi/rsi -> rax) while exercising FP ops.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FPRoundTrip : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPRoundTrip, FPVerify) { roundTripX64(GetParam()); }

// clang-format off

#define FP_BITCAST_PREAMBLE_D \
  "  double a_f, b_f;\n" \
  "  __builtin_memcpy(&a_f, &a, 8);\n" \
  "  __builtin_memcpy(&b_f, &b, 8);\n"

#define FP_BITCAST_RETURN_D \
  "  long r;\n" \
  "  __builtin_memcpy(&r, &result, 8);\n" \
  "  return r;\n"

#define FP_UNARY_D(name, op) \
  {#name, \
   "long " #name "(long a) {\n" \
   "  double a_f;\n" \
   "  __builtin_memcpy(&a_f, &a, 8);\n" \
   "  double result = " op ";\n" \
   FP_BITCAST_RETURN_D \
   "}\n", \
   {0x4014000000000000ULL}, "FPRT"}

#define FP_BINARY_D(name, op, a_bits, b_bits) \
  {#name, \
   "long " #name "(long a, long b) {\n" \
   FP_BITCAST_PREAMBLE_D \
   "  double result = a_f " op " b_f;\n" \
   FP_BITCAST_RETURN_D \
   "}\n", \
   {a_bits, b_bits}, "FPRT"}

// double bit patterns: 5.0 = 0x4014000000000000, 3.0 = 0x4008000000000000
// 2.0 = 0x4000000000000000, 10.0 = 0x4024000000000000, 1.0 = 0x3FF0000000000000
// -5.0 = 0xC014000000000000

static const std::vector<RoundTripTC> kX64FPRoundTrip = {
  FP_BINARY_D(fp_add,  "+", 0x4014000000000000ULL, 0x4008000000000000ULL),
  FP_BINARY_D(fp_sub,  "-", 0x4024000000000000ULL, 0x4008000000000000ULL),
  FP_BINARY_D(fp_mul,  "*", 0x4014000000000000ULL, 0x4008000000000000ULL),
  FP_BINARY_D(fp_div,  "/", 0x4024000000000000ULL, 0x4000000000000000ULL),

  FP_UNARY_D(fp_neg, "-a_f"),

  // fabs
  {"fp_fabs",
   "long fp_fabs(long a) {\n"
   "  double a_f;\n"
   "  __builtin_memcpy(&a_f, &a, 8);\n"
   "  double result = a_f < 0 ? -a_f : a_f;\n"
   FP_BITCAST_RETURN_D
   "}\n",
   {0xC014000000000000ULL}, "FPRT"},

  // min/max
  {"fp_min",
   "long fp_min(long a, long b) {\n"
   FP_BITCAST_PREAMBLE_D
   "  double result = a_f < b_f ? a_f : b_f;\n"
   FP_BITCAST_RETURN_D
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPRT"},

  {"fp_max",
   "long fp_max(long a, long b) {\n"
   FP_BITCAST_PREAMBLE_D
   "  double result = a_f > b_f ? a_f : b_f;\n"
   FP_BITCAST_RETURN_D
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPRT"},

  // int-to-double conversion (cvtsi2sd)
  {"fp_i64_to_double",
   "long fp_i64_to_double(long a) {\n"
   "  double result = (double)a;\n"
   FP_BITCAST_RETURN_D
   "}\n",
   {42}, "FPRT"},

  // double-to-int conversion (cvttsd2si)
  {"fp_double_to_i64",
   "long fp_double_to_i64(long a) {\n"
   "  double a_f;\n"
   "  __builtin_memcpy(&a_f, &a, 8);\n"
   "  return (long)a_f;\n"
   "}\n",
   {0x4014000000000000ULL}, "FPRT"},

  // float (32-bit) via bitcast through int
  {"fp_float_add",
   "long fp_float_add(long a, long b) {\n"
   "  float a_f, b_f;\n"
   "  int ai = (int)a, bi = (int)b;\n"
   "  __builtin_memcpy(&a_f, &ai, 4);\n"
   "  __builtin_memcpy(&b_f, &bi, 4);\n"
   "  float result = a_f + b_f;\n"
   "  int ri;\n"
   "  __builtin_memcpy(&ri, &result, 4);\n"
   "  return ri;\n"
   "}\n",
   {0x40A00000, 0x40400000}, "FPRT"},

  // float-to-double (cvtss2sd)
  {"fp_f32_to_f64",
   "long fp_f32_to_f64(long a) {\n"
   "  int ai = (int)a;\n"
   "  float a_f;\n"
   "  __builtin_memcpy(&a_f, &ai, 4);\n"
   "  double result = (double)a_f;\n"
   FP_BITCAST_RETURN_D
   "}\n",
   {0x40A00000}, "FPRT"},

  // double-to-float (cvtsd2ss)
  {"fp_f64_to_f32",
   "long fp_f64_to_f32(long a) {\n"
   "  double a_f;\n"
   "  __builtin_memcpy(&a_f, &a, 8);\n"
   "  float result = (float)a_f;\n"
   "  int ri;\n"
   "  __builtin_memcpy(&ri, &result, 4);\n"
   "  return ri;\n"
   "}\n",
   {0x4014000000000000ULL}, "FPRT"},

  // FP comparison (ucomisd/comisd)
  {"fp_cmp_lt",
   "long fp_cmp_lt(long a, long b) {\n"
   FP_BITCAST_PREAMBLE_D
   "  return a_f < b_f;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4014000000000000ULL}, "FPRT"},

  {"fp_cmp_eq",
   "long fp_cmp_eq(long a, long b) {\n"
   FP_BITCAST_PREAMBLE_D
   "  return a_f == b_f;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4014000000000000ULL}, "FPRT"},

  // multiply-add pattern (may generate FMA on supported targets)
  {"fp_muladd",
   "long fp_muladd(long a, long b, long c) {\n"
   "  double a_f, b_f, c_f;\n"
   "  __builtin_memcpy(&a_f, &a, 8);\n"
   "  __builtin_memcpy(&b_f, &b, 8);\n"
   "  __builtin_memcpy(&c_f, &c, 8);\n"
   "  double result = a_f * b_f + c_f;\n"
   FP_BITCAST_RETURN_D
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL, 0x3FF0000000000000ULL}, "FPRT"},

  // int-to-float-to-int roundtrip
  {"fp_i32_roundtrip",
   "long fp_i32_roundtrip(long a) {\n"
   "  int i = (int)a;\n"
   "  float f = (float)i;\n"
   "  return (long)(int)f;\n"
   "}\n",
   {42}, "FPRT"},

  // FP conditional select
  {"fp_select",
   "long fp_select(long cond, long a, long b) {\n"
   "  double a_f, b_f;\n"
   "  __builtin_memcpy(&a_f, &a, 8);\n"
   "  __builtin_memcpy(&b_f, &b, 8);\n"
   "  double result = cond ? a_f : b_f;\n"
   FP_BITCAST_RETURN_D
   "}\n",
   {1, 0x4014000000000000ULL, 0x4008000000000000ULL}, "FPRT"},

  // FP accumulation loop
  {"fp_sum_1_to_n",
   "long fp_sum_1_to_n(long n) {\n"
   "  double sum = 0.0;\n"
   "  for (long i = 1; i <= n; ++i)\n"
   "    sum += (double)i;\n"
   "  double result = sum;\n"
   FP_BITCAST_RETURN_D
   "}\n",
   {10}, "FPRT"},

  // fp_isqrt: Complex FP loop causes recompiled code to access unmapped
  // memory in Unicorn (stack growth beyond mapped region). Not a lift bug.
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(FPRT, X64FPRoundTrip, ::testing::ValuesIn(kX64FPRoundTrip),
                         rtTCName);
