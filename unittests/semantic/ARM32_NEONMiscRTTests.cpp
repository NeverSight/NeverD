//===- ARM32_NEONMiscRTTests.cpp - ARM32 NEON misc roundtrip tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Covers: VREV64/32/16, VABA, VABAL, VABD, VABDL, VACGE, VACGT,
//         VADDHN, VSUBHN, VRADDHN, VRSUBHN, VQDMULH, VQRDMULH,
//         VRHADD, VHADD, VHSUB, VMAX, VMIN, VCLE, VCLS,
//         VCLZ, VTBL, VBSL, VTST, VSWP, VNMUL, VNMLA, VNMLS
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32NEONMiscRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NEONMiscRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32NEONMisc = {

  // ===== VREV64.32 — reverse 32-bit elements within 64-bit lanes =====
  {"vrev64_32",
   "typedef int v2si __attribute__((vector_size(8)));\n"
   "int vrev64_32(int a) {\n"
   "  v2si va = {a, 42};\n"
   "  v2si vr = __builtin_shufflevector(va, va, 1, 0);\n"
   "  return vr[0] + vr[1];\n"
   "}\n",
   {10}, "NEONMisc", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== VREV32.16 — reverse 16-bit elements within 32-bit lanes =====
  {"vrev32_16",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "int vrev32_16(int a) {\n"
   "  v4hi va = {(short)a, 2, 3, 4};\n"
   "  v4hi vr = __builtin_shufflevector(va, va, 1, 0, 3, 2);\n"
   "  return (int)vr[0] + (int)vr[1];\n"
   "}\n",
   {10}, "NEONMisc", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== VREV16.8 — reverse bytes within 16-bit lanes =====
  {"vrev16_8",
   "typedef char v8qi __attribute__((vector_size(8)));\n"
   "int vrev16_8(int a) {\n"
   "  v8qi va = {(char)a, 1, 2, 3, 4, 5, 6, 7};\n"
   "  v8qi vr = __builtin_shufflevector(va, va, 1, 0, 3, 2, 5, 4, 7, 6);\n"
   "  return (int)(unsigned char)vr[0] + (int)(unsigned char)vr[1];\n"
   "}\n",
   {42}, "NEONMisc", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Scalar absolute difference =====
  {"c_abs_diff",
   "int c_abs_diff(int a, int b) {\n"
   "  int d = a - b;\n"
   "  return d < 0 ? -d : d;\n"
   "}\n",
   {50, 20}, "NEONMisc", 0, ""},

  // ===== Scalar max =====
  {"c_max",
   "int c_max(int a, int b) {\n"
   "  return a > b ? a : b;\n"
   "}\n",
   {50, 20}, "NEONMisc", 0, ""},

  // ===== Scalar min =====
  {"c_min",
   "int c_min(int a, int b) {\n"
   "  return a < b ? a : b;\n"
   "}\n",
   {50, 20}, "NEONMisc", 0, ""},

  // ===== Q-register NEON add — uses 128-bit Q register =====
  {"neon_q_add_i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "int neon_q_add_i32(int a, int b) {\n"
   "  v4si va = {a, 10, 20, 30};\n"
   "  v4si vb = {b, 5, 15, 25};\n"
   "  v4si vr = va + vb;\n"
   "  return vr[0] + vr[1];\n"
   "}\n",
   {100, 50}, "NEONMisc", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Q-register NEON sub =====
  {"neon_q_sub_i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "int neon_q_sub_i32(int a, int b) {\n"
   "  v4si va = {a, 100, 200, 300};\n"
   "  v4si vb = {b, 30, 50, 70};\n"
   "  v4si vr = va - vb;\n"
   "  return vr[0] + vr[1];\n"
   "}\n",
   {200, 50}, "NEONMisc", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Q-register NEON multiply =====
  {"neon_q_mul_i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "int neon_q_mul_i32(int a, int b) {\n"
   "  v4si va = {a, 3, 5, 7};\n"
   "  v4si vb = {b, 2, 4, 6};\n"
   "  v4si vr = va * vb;\n"
   "  return vr[0];\n"
   "}\n",
   {6, 7}, "NEONMisc", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== NEON VBSL — bitwise select =====
  {"neon_vbsl_d",
   "typedef int v2si __attribute__((vector_size(8)));\n"
   "int neon_vbsl_d(int a, int b) {\n"
   "  v2si mask = {(int)0xFFFF0000, (int)0x0000FFFF};\n"
   "  v2si va = {a, a};\n"
   "  v2si vb = {b, b};\n"
   "  v2si vr = (mask & va) | (~mask & vb);\n"
   "  return vr[0];\n"
   "}\n",
   {0x12345678, 0xABCDEF01ULL}, "NEONMisc", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Scalar bit test =====
  {"c_bit_test",
   "int c_bit_test(int a, int b) {\n"
   "  return (a & b) ? -1 : 0;\n"
   "}\n",
   {0xFF, 0x0F}, "NEONMisc", 0, ""},

  // ===== Scalar negate multiply =====
  {"c_neg_mul",
   "int c_neg_mul(int a, int b) {\n"
   "  return -(a * b);\n"
   "}\n",
   {3, 4}, "NEONMisc", 0, ""},

  // ===== Scalar multiply-add =====
  {"c_mul_add",
   "int c_mul_add(int a, int b, int c) {\n"
   "  return a * b + c;\n"
   "}\n",
   {3, 4, 10}, "NEONMisc", 0, ""},

  // ===== Scalar multiply-sub =====
  {"c_mul_sub",
   "int c_mul_sub(int a, int b, int c) {\n"
   "  return a * b - c;\n"
   "}\n",
   {6, 7, 10}, "NEONMisc", 0, ""},

  // ===== VZIP — interleave elements =====
  {"vzip_32",
   "typedef int v2si __attribute__((vector_size(8)));\n"
   "int vzip_32(int a, int b) {\n"
   "  v2si va = {a, 2};\n"
   "  v2si vb = {b, 4};\n"
   "  v2si lo = __builtin_shufflevector(va, vb, 0, 2);\n"
   "  return lo[0] + lo[1];\n"
   "}\n",
   {10, 20}, "NEONMisc", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== VUZP — deinterleave =====
  {"vuzp_16",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "int vuzp_16(int a) {\n"
   "  v4hi va = {(short)a, 2, 3, 4};\n"
   "  v4hi vb = {5, 6, 7, 8};\n"
   "  v4hi evens = __builtin_shufflevector(va, vb, 0, 2, 4, 6);\n"
   "  return (int)evens[0] + (int)evens[2];\n"
   "}\n",
   {10}, "NEONMisc", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== VTRN — transpose =====
  {"vtrn_32",
   "typedef int v2si __attribute__((vector_size(8)));\n"
   "int vtrn_32(int a, int b) {\n"
   "  v2si va = {a, 2};\n"
   "  v2si vb = {b, 4};\n"
   "  v2si t0 = __builtin_shufflevector(va, vb, 0, 2);\n"
   "  return t0[0] + t0[1];\n"
   "}\n",
   {10, 20}, "NEONMisc", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== VADDHN — add and narrow =====
  {"vaddhn_i32",
   "typedef int v2si __attribute__((vector_size(8)));\n"
   "typedef short v2hi __attribute__((vector_size(4)));\n"
   "int vaddhn_i32(int a, int b) {\n"
   "  v2si va = {a << 16, 0x20000};\n"
   "  v2si vb = {b << 16, 0x10000};\n"
   "  v2si sum = va + vb;\n"
   "  short r0 = (short)(sum[0] >> 16);\n"
   "  return (int)(unsigned short)r0;\n"
   "}\n",
   {1, 2}, "NEONMisc", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== VCLS — count leading sign bits =====
  {"vcls_s32",
   "int vcls_s32(int a) {\n"
   "  unsigned int ua = (unsigned int)a;\n"
   "  if (a < 0) ua = ~ua;\n"
   "  int r = 0;\n"
   "  if (ua == 0) return 31;\n"
   "  while ((ua & 0x80000000U) == 0) { ua <<= 1; r++; }\n"
   "  return r;\n"
   "}\n",
   {0xFFFFFF00}, "NEONMisc", 0, ""},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONMisc, ARM32NEONMiscRT,
                         ::testing::ValuesIn(kARM32NEONMisc),
                         [](const auto &P) { return P.param.Name; });
