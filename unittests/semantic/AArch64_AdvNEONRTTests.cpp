//===- AArch64_AdvNEONRTTests.cpp - Advanced NEON roundtrip ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests AArch64 advanced NEON patterns: widening/narrowing, saturating,
// pairwise, FP conversion, multiply-accumulate, etc.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64AdvNEONRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64AdvNEONRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64AdvNEON = {
  // ========== Integer conversion / narrowing patterns ==========
  {"a64_narrow_i32_to_i16",
   "long a64_narrow(long a) {\n"
   "  return (long)(unsigned short)(unsigned int)a;\n"
   "}\n",
   {0x12345678}, "A64AdvNEON"},

  {"a64_narrow_i64_to_i32",
   "long a64_narrow64(long a) {\n"
   "  unsigned int lo = (unsigned int)a;\n"
   "  return (long)lo + 0;\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL}, "A64AdvNEON"},

  {"a64_widen_u16_to_u32",
   "long a64_widen(long a) {\n"
   "  unsigned short s = (unsigned short)a;\n"
   "  return (long)(unsigned int)s;\n"
   "}\n",
   {0xABCD}, "A64AdvNEON"},

  {"a64_widen_s8_to_s32",
   "long a64_widen_s8(long a) {\n"
   "  signed char c = (signed char)a;\n"
   "  return (long)(int)c;\n"
   "}\n",
   {0x80}, "A64AdvNEON"},  // -128

  // ========== Saturating patterns ==========
  {"a64_sat_add_u8",
   "long a64_sat_add_u8(long a, long b) {\n"
   "  unsigned int sum = (unsigned char)a + (unsigned char)b;\n"
   "  return sum > 255 ? 255 : sum;\n"
   "}\n",
   {200, 100}, "A64AdvNEON"},

  {"a64_sat_sub_u8",
   "long a64_sat_sub_u8(long a, long b) {\n"
   "  int diff = (unsigned char)a - (unsigned char)b;\n"
   "  return diff < 0 ? 0 : diff;\n"
   "}\n",
   {50, 100}, "A64AdvNEON"},

  {"a64_sat_add_i16",
   "long a64_sat_add_i16(long a, long b) {\n"
   "  int sum = (short)(a & 0xFFFF) + (short)(b & 0xFFFF);\n"
   "  if (sum > 32767) sum = 32767;\n"
   "  if (sum < -32768) sum = -32768;\n"
   "  return (long)(unsigned short)(short)sum;\n"
   "}\n",
   {30000, 10000}, "A64AdvNEON"},

  // ========== Pairwise add pattern ==========
  {"a64_pair_add",
   "long a64_pair_add(long a) {\n"
   "  int lo = (int)(a & 0xFFFFFFFF);\n"
   "  int hi = (int)(a >> 32);\n"
   "  return (long)(lo + hi);\n"
   "}\n",
   {((uint64_t)100 << 32) | 42}, "A64AdvNEON"},

  {"a64_pair_max",
   "long a64_pair_max(long a) {\n"
   "  int lo = (int)(a & 0xFFFFFFFF);\n"
   "  int hi = (int)(a >> 32);\n"
   "  return (long)(lo > hi ? lo : hi);\n"
   "}\n",
   {((uint64_t)42 << 32) | 100}, "A64AdvNEON"},

  // ========== FP conversion ==========
  {"a64_f64_to_i64",
   "long a64_f64_to_i64(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  return (long)d;\n"
   "}\n",
   {0x4045000000000000ULL}, "A64AdvNEON"},  // 42.0

  {"a64_i64_to_f64",
   "long a64_i64_to_f64(long a) {\n"
   "  double d = (double)a;\n"
   "  long r; __builtin_memcpy(&r, &d, 8); return r;\n"
   "}\n",
   {42}, "A64AdvNEON"},

  {"a64_f32_to_i32",
   "long a64_f32_to_i32(long a) {\n"
   "  int ai = (int)a;\n"
   "  float f; __builtin_memcpy(&f, &ai, 4);\n"
   "  return (long)(int)f;\n"
   "}\n",
   {0x42280000ULL}, "A64AdvNEON"},  // 42.0f

  {"a64_f64_to_f32",
   "long a64_f64_to_f32(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  float f = (float)d;\n"
   "  int r; __builtin_memcpy(&r, &f, 4);\n"
   "  return (long)(unsigned int)r;\n"
   "}\n",
   {0x4045000000000000ULL}, "A64AdvNEON"},

  // ========== FP comparison ==========
  {"a64_fcmp_lt",
   "long a64_fcmp_lt(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da < db ? 1 : 0;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4014000000000000ULL}, "A64AdvNEON"},

  {"a64_fcmp_eq",
   "long a64_fcmp_eq(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da == db ? 1 : 0;\n"
   "}\n",
   {0x4045000000000000ULL, 0x4045000000000000ULL}, "A64AdvNEON"},

  // ========== FP abs/neg ==========
  {"a64_fabs",
   "long a64_fabs(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  if (d < 0) d = -d;\n"
   "  long r; __builtin_memcpy(&r, &d, 8); return r;\n"
   "}\n",
   {0xC045000000000000ULL}, "A64AdvNEON"},

  {"a64_fneg",
   "long a64_fneg(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  d = -d;\n"
   "  long r; __builtin_memcpy(&r, &d, 8); return r;\n"
   "}\n",
   {0x4045000000000000ULL}, "A64AdvNEON"},

  // ========== FP sqrt ==========
  {"a64_fsqrt",
   "long a64_fsqrt(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  double r = __builtin_sqrt(d);\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4059000000000000ULL}, "A64AdvNEON", 2, "-fno-math-errno"},  // 100.0

  // ========== Multiply-accumulate ==========
  {"a64_madd",
   "long a64_madd(long a, long b) {\n"
   "  return a * b + 10;\n"
   "}\n",
   {7, 6}, "A64AdvNEON"},

  {"a64_msub",
   "long a64_msub(long a, long b) {\n"
   "  return 100 - a * b;\n"
   "}\n",
   {7, 6}, "A64AdvNEON"},

  // ========== Reverse bits/bytes ==========
  {"a64_rev16",
   "long a64_rev16(long a) {\n"
   "  long r = 0;\n"
   "  for (int i = 0; i < 4; ++i) {\n"
   "    long b0 = (a >> (i*16)) & 0xFF;\n"
   "    long b1 = (a >> (i*16+8)) & 0xFF;\n"
   "    r |= (b0 << (i*16+8)) | (b1 << (i*16));\n"
   "  }\n"
   "  return r;\n"
   "}\n",
   {0x0102030405060708ULL}, "A64AdvNEON"},

  // ========== CSEL/CSINC/CSINV/CSNEG patterns ==========
  {"a64_csel",
   "long a64_csel(long a, long b) {\n"
   "  return a > b ? a : b;\n"
   "}\n",
   {42, 100}, "A64AdvNEON"},

  {"a64_csinc",
   "long a64_csinc(long a, long b) {\n"
   "  return a == 0 ? b + 1 : a;\n"
   "}\n",
   {0, 41}, "A64AdvNEON"},

  {"a64_csinv",
   "long a64_csinv(long a, long b) {\n"
   "  return a != 0 ? a : ~b;\n"
   "}\n",
   {0, 42}, "A64AdvNEON"},

  // ========== Bit manipulation ==========
  {"a64_cls",
   "long a64_cls(long a) {\n"
   "  if (a == 0) return 63;\n"
   "  long n = 0;\n"
   "  long x = a;\n"
   "  long sign = (x >> 63) & 1;\n"
   "  while (((x >> (62 - n)) & 1) == sign && n < 63) ++n;\n"
   "  return n;\n"
   "}\n",
   {0xFF00000000000000ULL}, "A64AdvNEON"},

  {"a64_extr",
   "long a64_extr(long a, long b) {\n"
   "  int shift = 16;\n"
   "  return (a << (64 - shift)) | ((unsigned long long)b >> shift);\n"
   "}\n",
   {0xDEADBEEFULL, 0xCAFEBABEULL}, "A64AdvNEON"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(A64AdvNEON, A64AdvNEONRT,
                         ::testing::ValuesIn(kA64AdvNEON), rtTCName);
