//===- X64_SSEConversionRTTests.cpp - FP conversion roundtrip --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests x86_64 SSE/AVX type conversion instructions through lift pipeline.
// Covers: CVTSI2SD, CVTSD2SI, CVTSI2SS, CVTSS2SI, CVTSS2SD, CVTSD2SS,
//         truncation variants (CVTTSD2SI etc), packed int<->FP conversions.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSEConvRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSEConvRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64SSEConv = {
  // ========== int -> double (CVTSI2SD) ==========
  {"cvt_i32_to_f64",
   "long cvt_i32_to_f64(long a) {\n"
   "  double d = (double)(int)a;\n"
   "  long r; __builtin_memcpy(&r, &d, 8); return r;\n"
   "}\n",
   {42}, "SSEConvRT"},

  {"cvt_i64_to_f64",
   "long cvt_i64_to_f64(long a) {\n"
   "  double d = (double)a;\n"
   "  long r; __builtin_memcpy(&r, &d, 8); return r;\n"
   "}\n",
   {123456789}, "SSEConvRT"},

  {"cvt_i32_to_f64_neg",
   "long cvt_i32_to_f64_neg(long a) {\n"
   "  double d = (double)(int)a;\n"
   "  long r; __builtin_memcpy(&r, &d, 8); return r;\n"
   "}\n",
   {(uint64_t)(int64_t)-42}, "SSEConvRT"},

  // ========== double -> int (CVTSD2SI / CVTTSD2SI) ==========
  {"cvt_f64_to_i32",
   "long cvt_f64_to_i32(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  return (long)(int)d;\n"
   "}\n",
   {0x4045000000000000ULL}, "SSEConvRT"},  // 42.0

  {"cvt_f64_to_i64",
   "long cvt_f64_to_i64(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  return (long)d;\n"
   "}\n",
   {0x4045000000000000ULL}, "SSEConvRT"},  // 42.0

  {"cvt_f64_to_i32_trunc",
   "long cvt_f64_to_i32_trunc(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  return (long)(int)d;\n"
   "}\n",
   {0x40468CCCCCCCCCCDULL}, "SSEConvRT"},  // 45.1

  {"cvt_f64_to_i32_neg",
   "long cvt_f64_to_i32_neg(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  return (long)(int)d;\n"
   "}\n",
   {0xC045000000000000ULL}, "SSEConvRT"},  // -42.0

  // ========== int -> float (CVTSI2SS) ==========
  {"cvt_i32_to_f32",
   "long cvt_i32_to_f32(long a) {\n"
   "  float f = (float)(int)a;\n"
   "  int r; __builtin_memcpy(&r, &f, 4);\n"
   "  return (long)(unsigned int)r;\n"
   "}\n",
   {42}, "SSEConvRT"},

  {"cvt_i64_to_f32",
   "long cvt_i64_to_f32(long a) {\n"
   "  float f = (float)a;\n"
   "  int r; __builtin_memcpy(&r, &f, 4);\n"
   "  return (long)(unsigned int)r;\n"
   "}\n",
   {1000}, "SSEConvRT"},

  // ========== float -> int (CVTSS2SI / CVTTSS2SI) ==========
  {"cvt_f32_to_i32",
   "long cvt_f32_to_i32(long a) {\n"
   "  int ai = (int)a;\n"
   "  float f; __builtin_memcpy(&f, &ai, 4);\n"
   "  return (long)(int)f;\n"
   "}\n",
   {0x42280000ULL}, "SSEConvRT"},  // 42.0f

  // ========== float <-> double (CVTSS2SD / CVTSD2SS) ==========
  {"cvt_f32_to_f64",
   "long cvt_f32_to_f64(long a) {\n"
   "  int ai = (int)a;\n"
   "  float f; __builtin_memcpy(&f, &ai, 4);\n"
   "  double d = (double)f;\n"
   "  long r; __builtin_memcpy(&r, &d, 8); return r;\n"
   "}\n",
   {0x42280000ULL}, "SSEConvRT"},  // 42.0f

  {"cvt_f64_to_f32",
   "long cvt_f64_to_f32(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  float f = (float)d;\n"
   "  int r; __builtin_memcpy(&r, &f, 4);\n"
   "  return (long)(unsigned int)r;\n"
   "}\n",
   {0x4045000000000000ULL}, "SSEConvRT"},  // 42.0

  // ========== unsigned int conversions ==========
  {"cvt_u32_to_f64",
   "long cvt_u32_to_f64(long a) {\n"
   "  double d = (double)(unsigned int)a;\n"
   "  long r; __builtin_memcpy(&r, &d, 8); return r;\n"
   "}\n",
   {0xFFFFFFFFULL}, "SSEConvRT"},

  {"cvt_u32_to_f32",
   "long cvt_u32_to_f32(long a) {\n"
   "  float f = (float)(unsigned int)a;\n"
   "  int r; __builtin_memcpy(&r, &f, 4);\n"
   "  return (long)(unsigned int)r;\n"
   "}\n",
   {1000000}, "SSEConvRT"},

  // ========== FP comparison patterns (CMPSD/CMPSS/UCOMISD) ==========
  {"fp_cmp_lt",
   "long fp_cmp_lt(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da < db ? 1 : 0;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4014000000000000ULL}, "SSEConvRT"},  // 3.0 < 5.0

  {"fp_cmp_eq",
   "long fp_cmp_eq(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da == db ? 1 : 0;\n"
   "}\n",
   {0x4045000000000000ULL, 0x4045000000000000ULL}, "SSEConvRT"},  // 42.0 == 42.0

  {"fp_cmp_ge",
   "long fp_cmp_ge(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da >= db ? 1 : 0;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "SSEConvRT"},  // 5.0 >= 3.0

  // ========== FP min/max (MINSD/MAXSD) ==========
  {"fp_min_d",
   "long fp_min_d(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da < db ? da : db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "SSEConvRT"},

  {"fp_max_d",
   "long fp_max_d(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da > db ? da : db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "SSEConvRT"},

  // ========== FP abs/negate via bitwise (ANDPD/XORPD patterns) ==========
  {"fp_abs_d",
   "long fp_abs_d(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  if (d < 0) d = -d;\n"
   "  long r; __builtin_memcpy(&r, &d, 8); return r;\n"
   "}\n",
   {0xC045000000000000ULL}, "SSEConvRT"},  // -42.0

  {"fp_negate_d",
   "long fp_negate_d(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  d = -d;\n"
   "  long r; __builtin_memcpy(&r, &d, 8); return r;\n"
   "}\n",
   {0x4045000000000000ULL}, "SSEConvRT"},  // 42.0 -> -42.0

  // ========== FP squared (avoids sqrt library call) ==========
  {"fp_square_d",
   "long fp_square_d(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  double r = d * d;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL}, "SSEConvRT"},  // 10.0^2 = 100.0

  // ========== FP reciprocal ==========
  {"fp_recip_d",
   "long fp_recip_d(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da / db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4000000000000000ULL}, "SSEConvRT"},  // 10.0/2.0=5.0

  // ========== Multi-step FP: a*b+a (no rodata constants) ==========
  {"fp_fma_manual",
   "long fp_fma_manual(long x, long y) {\n"
   "  double a, b;\n"
   "  __builtin_memcpy(&a, &x, 8); __builtin_memcpy(&b, &y, 8);\n"
   "  double r = a * b + a;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4000000000000000ULL}, "SSEConvRT"},

  // ========== VEX-encoded conversions (AVX) ==========
  // VEX scalar vcvtss2sd/vcvtsd2ss must narrow the source to its real FP
  // width; a full XMM source otherwise makes the emitter pick the wrong
  // direction (widen lifted as fptrunc).
  {"vex_f32_to_f64",
   "long vex_f32_to_f64(long a) {\n"
   "  int ai = (int)a; float f; __builtin_memcpy(&f, &ai, 4);\n"
   "  double d = (double)f;\n"
   "  long r; __builtin_memcpy(&r, &d, 8); return r;\n"
   "}\n",
   {0x42280000ULL}, "SSEConvRT", 2, "-mavx"},  // 42.0f

  {"vex_f64_to_f32",
   "long vex_f64_to_f32(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  float f = (float)d;\n"
   "  int r; __builtin_memcpy(&r, &f, 4);\n"
   "  return (long)(unsigned int)r;\n"
   "}\n",
   {0x4045000000000000ULL}, "SSEConvRT", 2, "-mavx"},  // 42.0

  // float<->double chain (vcvtss2sd in a loop), AVX-encoded.
  {"vex_mixconv",
   "long vex_mixconv(long a) {\n"
   "  double acc = 0.0;\n"
   "  for (int i = 0; i < 200; i++) {\n"
   "    float f = (float)((int)(a * (i + 1)) % 333) * 0.125f;\n"
   "    double d = (double)f * 1.5;\n"
   "    float g = (float)d + 0.25f;\n"
   "    acc += (double)g; }\n"
   "  return (long)(int)acc;\n"
   "}\n",
   {0x88990ABULL}, "SSEConvRT", 2, "-mavx"},

  // Packed widen float->double summed back (may emit vcvtps2pd ymm).
  {"vex_ps2pd_sum",
   "long vex_ps2pd_sum(long a) {\n"
   "  float f[8]; for (int i = 0; i < 8; i++) f[i] = (float)((int)(a*(i+1))%97);\n"
   "  double s = 0; for (int i = 0; i < 8; i++) s += (double)f[i];\n"
   "  return (long)(int)(s * 4);\n"
   "}\n",
   {0x123457ULL}, "SSEConvRT", 2, "-mavx"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SSEConvRT, X64SSEConvRT,
                         ::testing::ValuesIn(kX64SSEConv), rtTCName);
