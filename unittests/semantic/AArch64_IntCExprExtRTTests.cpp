//===- AArch64_IntCExprExtRTTests.cpp - AArch64 C expression roundtrip ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "SemanticRoundTripFixture.h"

class AArch64IntCExprExtRT
    : public SemanticRoundTripFixture,
      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64IntCExprExtRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64IntCExprExt = {

  {"c_bitfield_insert_a64",
   "long c_bitfield_insert_a64(long val, long field) {\n"
   "  return (val & ~0xFF00) | ((field & 0xFF) << 8);\n"
   "}\n",
   {0xDEADBEEF, 0x42}, "IntCExpr", 2, ""},

  {"c_ctz_a64",
   "long c_ctz_a64(long a) {\n"
   "  return __builtin_ctzll(a | 1);\n"
   "}\n",
   {0x100}, "IntCExpr", 2, ""},

  {"c_clz_a64",
   "long c_clz_a64(long a) {\n"
   "  return a ? __builtin_clzll(a) : 64;\n"
   "}\n",
   {0x100}, "IntCExpr", 2, ""},

  {"c_popcount_a64",
   "long c_popcount_a64(long a) {\n"
   "  return __builtin_popcountll(a);\n"
   "}\n",
   {0xDEADBEEF}, "IntCExpr", 2, "-march=armv8-a+simd"},

  {"c_bswap64_a64",
   "long c_bswap64_a64(long a) {\n"
   "  return __builtin_bswap64(a);\n"
   "}\n",
   {0x0102030405060708ULL}, "IntCExpr", 2, ""},

  {"c_bswap32_a64",
   "long c_bswap32_a64(long a) {\n"
   "  return (long)__builtin_bswap32((int)a);\n"
   "}\n",
   {0x01020304}, "IntCExpr", 2, ""},

  {"c_abs_diff_a64",
   "long c_abs_diff_a64(long a, long b) {\n"
   "  long d = a - b;\n"
   "  return d < 0 ? -d : d;\n"
   "}\n",
   {30, 50}, "IntCExpr", 2, ""},

  {"c_fibonacci_a64",
   "long c_fibonacci_a64(long n) {\n"
   "  long a = 0, b = 1;\n"
   "  for (long i = 0; i < n; ++i) { long t = a + b; a = b; b = t; }\n"
   "  return a;\n"
   "}\n",
   {20}, "IntCExpr", 2, ""},

  {"c_gcd_a64",
   "long c_gcd_a64(long a, long b) {\n"
   "  while (b) { long t = b; b = a % b; a = t; }\n"
   "  return a;\n"
   "}\n",
   {48, 36}, "IntCExpr", 2, ""},

  {"c_power_a64",
   "long c_power_a64(long base, long exp) {\n"
   "  long result = 1;\n"
   "  while (exp > 0) {\n"
   "    if (exp & 1) result *= base;\n"
   "    base *= base;\n"
   "    exp >>= 1;\n"
   "  }\n"
   "  return result;\n"
   "}\n",
   {3, 10}, "IntCExpr", 2, ""},

  {"c_clamp_a64",
   "long c_clamp_a64(long val, long lo, long hi) {\n"
   "  if (val < lo) return lo;\n"
   "  if (val > hi) return hi;\n"
   "  return val;\n"
   "}\n",
   {100, 10, 50}, "IntCExpr", 2, ""},

  {"c_sext8_a64",
   "long c_sext8_a64(long a) {\n"
   "  return (long)(signed char)a;\n"
   "}\n",
   {0x80}, "IntCExpr", 2, ""},

  {"c_rev_bits_a64",
   "long c_rev_bits_a64(long x) {\n"
   "  long result = 0;\n"
   "  for (int i = 0; i < 32; ++i) {\n"
   "    result = (result << 1) | (x & 1);\n"
   "    x >>= 1;\n"
   "  }\n"
   "  return result;\n"
   "}\n",
   {0xDEADBEEF}, "IntCExpr", 2, ""},

  {"c_conditional_negate",
   "long c_conditional_negate(long val, long cond) {\n"
   "  return cond ? -val : val;\n"
   "}\n",
   {42, 1}, "IntCExpr", 2, ""},

};
// clang-format on

INSTANTIATE_TEST_SUITE_P(IntCExpr, AArch64IntCExprExtRT,
                         ::testing::ValuesIn(kA64IntCExprExt),
                         [](const auto &P) { return P.param.Name; });
