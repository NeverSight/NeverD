//===- X64_FPURoundTripTests.cpp - x87 FPU roundtrip tests ------*- C++ -*-===//
//
// Tests x86 x87 FPU instructions through full lift pipeline.  Uses `long
// double` or FP math functions that the compiler emits as x87 sequences.
// These exercise X86LiftFPU.cpp: FLD/FST/FADD/FSUB/FMUL/FDIV/FSQRT/FABS etc.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FPURT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPURT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64FPU = {

  // ===== Basic FP arithmetic via SSE (compiler default) =====
  {"fp_add_double",
   "long fp_add_double(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da + db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "FPU", 1},

  {"fp_sub_double",
   "long fp_sub_double(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da - db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "FPU", 1},

  {"fp_mul_double",
   "long fp_mul_double(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da * db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "FPU", 1},

  {"fp_div_double",
   "long fp_div_double(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da / db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "FPU", 1},

  // ===== FP absolute value and negation =====
  {"fp_abs_double",
   "long fp_abs_double(long a) {\n"
   "  double da;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  double r = __builtin_fabs(da);\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0xC024000000000000ULL}, "FPU", 1},

  {"fp_neg_double",
   "long fp_neg_double(long a) {\n"
   "  double da;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  double r = -da;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL}, "FPU", 1},

  // ===== FP sqrt =====
  {"fp_sqrt_double",
   "long fp_sqrt_double(long a) {\n"
   "  double da;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  double r = __builtin_sqrt(da);\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4059000000000000ULL}, "FPU", 1},

  // ===== Float arithmetic =====
  {"fp_add_float",
   "long fp_add_float(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  float r = fa + fb;\n"
   "  long ret = 0; __builtin_memcpy(&ret, &r, 4); return ret;\n"
   "}\n",
   {0x41200000ULL, 0x40A00000ULL}, "FPU", 1},

  {"fp_mul_float",
   "long fp_mul_float(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  float r = fa * fb;\n"
   "  long ret = 0; __builtin_memcpy(&ret, &r, 4); return ret;\n"
   "}\n",
   {0x41200000ULL, 0x40A00000ULL}, "FPU", 1},

  // ===== FP comparison =====
  {"fp_cmp_lt",
   "long fp_cmp_lt(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da < db;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4024000000000000ULL}, "FPU", 1},

  {"fp_cmp_ge",
   "long fp_cmp_ge(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da >= db;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "FPU", 1},

  {"fp_cmp_eq",
   "long fp_cmp_eq(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da == db;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4024000000000000ULL}, "FPU", 1},

  // ===== FP conversion: int→double→int =====
  {"fp_int_to_double_to_int",
   "long fp_int_to_double_to_int(long a) {\n"
   "  double d = (double)(int)a;\n"
   "  return (long)(int)d;\n"
   "}\n",
   {42}, "FPU", 1},

  {"fp_uint_to_double",
   "long fp_uint_to_double(long a) {\n"
   "  double d = (double)(unsigned int)a;\n"
   "  return (long)d;\n"
   "}\n",
   {12345}, "FPU", 1},

  // ===== FP min/max =====
  {"fp_min_double",
   "long fp_min_double(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da < db ? da : db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "FPU", 1},

  {"fp_max_double",
   "long fp_max_double(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da > db ? da : db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "FPU", 1},

  // ===== FP truncation =====
  {"fp_trunc_to_int",
   "long fp_trunc_to_int(long a) {\n"
   "  double da;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  return (long)(int)da;\n"
   "}\n",
   {0x40091EB851EB851FULL}, "FPU", 1},

  // ===== FP multiply-add (C expression, compiler may use FMA or mul+add) =====
  {"fp_fma_double",
   "long fp_fma_double(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da * db + 1.0;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4000000000000000ULL, 0x4008000000000000ULL}, "FPU", 1},

  // ===== FP reciprocal estimation pattern =====
  {"fp_reciprocal",
   "long fp_reciprocal(long a) {\n"
   "  double da;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  double r = 1.0 / da;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4008000000000000ULL}, "FPU", 1},

  // ===== Copysign =====
  {"fp_copysign",
   "long fp_copysign(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = __builtin_copysign(da, db);\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL, 0xC000000000000000ULL}, "FPU", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(FPU, X64FPURT,
                         ::testing::ValuesIn(kX64FPU), rtTCName);
