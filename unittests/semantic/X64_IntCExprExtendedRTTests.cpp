//===- X64_IntCExprExtendedRTTests.cpp - x64 C expression roundtrip -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "SemanticRoundTripFixture.h"

class X64IntCExprExtRT
    : public SemanticRoundTripFixture,
      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64IntCExprExtRT, Verify) {
  roundTripX64(GetParam());
}

// clang-format off
static const std::vector<RoundTripTC> kX64IntCExprExt = {

  {"c_bitfield_insert",
   "long c_bitfield_insert(long val, long field) {\n"
   "  return (val & ~0xFF00) | ((field & 0xFF) << 8);\n"
   "}\n",
   {0xDEADBEEF, 0x42}, "IntCExpr", 2, ""},

  {"c_bitfield_extract",
   "long c_bitfield_extract(long val) {\n"
   "  return (val >> 16) & 0xFF;\n"
   "}\n",
   {0xDEAD42EF}, "IntCExpr", 2, ""},

  {"c_count_trailing_zeros",
   "long c_count_trailing_zeros(long a) {\n"
   "  return __builtin_ctzll(a | 1);\n"
   "}\n",
   {0x100}, "IntCExpr", 2, "-mbmi"},

  {"c_count_leading_zeros",
   "long c_count_leading_zeros(long a) {\n"
   "  return a ? __builtin_clzll(a) : 64;\n"
   "}\n",
   {0x100}, "IntCExpr", 2, "-mlzcnt"},

  {"c_popcount",
   "long c_popcount(long a) {\n"
   "  return __builtin_popcountll(a);\n"
   "}\n",
   {0xDEADBEEF}, "IntCExpr", 2, "-mpopcnt"},

  {"c_byte_swap",
   "long c_byte_swap(long a) {\n"
   "  return __builtin_bswap64(a);\n"
   "}\n",
   {0x0102030405060708ULL}, "IntCExpr", 2, ""},

  {"c_rotate_left",
   "long c_rotate_left(long val, long n) {\n"
   "  n &= 63;\n"
   "  return (val << n) | ((unsigned long)val >> (64 - n));\n"
   "}\n",
   {0xDEADBEEF, 13}, "IntCExpr", 2, ""},

  {"c_rotate_right",
   "long c_rotate_right(long val, long n) {\n"
   "  n &= 63;\n"
   "  return ((unsigned long)val >> n) | (val << (64 - n));\n"
   "}\n",
   {0xDEADBEEF, 17}, "IntCExpr", 2, ""},

  {"c_abs_diff",
   "long c_abs_diff(long a, long b) {\n"
   "  long d = a - b;\n"
   "  return d < 0 ? -d : d;\n"
   "}\n",
   {30, 50}, "IntCExpr", 2, ""},

  {"c_min_max",
   "long c_min_max(long a, long b, long c) {\n"
   "  long mn = a < b ? a : b;\n"
   "  mn = mn < c ? mn : c;\n"
   "  return mn;\n"
   "}\n",
   {30, 10, 50}, "IntCExpr", 2, ""},

  {"c_clamp",
   "long c_clamp(long val, long lo, long hi) {\n"
   "  if (val < lo) return lo;\n"
   "  if (val > hi) return hi;\n"
   "  return val;\n"
   "}\n",
   {100, 10, 50}, "IntCExpr", 2, ""},

  {"c_sign_extend_8",
   "long c_sign_extend_8(long a) {\n"
   "  return (long)(signed char)a;\n"
   "}\n",
   {0xFE}, "IntCExpr", 2, ""},

  {"c_sign_extend_16",
   "long c_sign_extend_16(long a) {\n"
   "  return (long)(short)a;\n"
   "}\n",
   {0xFFFE}, "IntCExpr", 2, ""},

  {"c_zero_extend_8",
   "long c_zero_extend_8(long a) {\n"
   "  return (long)(unsigned char)a;\n"
   "}\n",
   {0xDEADBEEF}, "IntCExpr", 2, ""},

  {"c_fibonacci_loop",
   "long c_fibonacci_loop(long n) {\n"
   "  long a = 0, b = 1;\n"
   "  for (long i = 0; i < n; ++i) { long t = a + b; a = b; b = t; }\n"
   "  return a;\n"
   "}\n",
   {20}, "IntCExpr", 2, ""},

  {"c_gcd",
   "long c_gcd(long a, long b) {\n"
   "  while (b) { long t = b; b = a % b; a = t; }\n"
   "  return a;\n"
   "}\n",
   {48, 36}, "IntCExpr", 2, ""},

  {"c_power",
   "long c_power(long base, long exp) {\n"
   "  long result = 1;\n"
   "  while (exp > 0) {\n"
   "    if (exp & 1) result *= base;\n"
   "    base *= base;\n"
   "    exp >>= 1;\n"
   "  }\n"
   "  return result;\n"
   "}\n",
   {3, 10}, "IntCExpr", 2, ""},

  {"c_hamming_weight_loop",
   "long c_hamming_weight_loop(long a) {\n"
   "  long count = 0;\n"
   "  while (a) { count++; a &= a - 1; }\n"
   "  return count;\n"
   "}\n",
   {0xDEADBEEF}, "IntCExpr", 2, ""},

  {"c_interleave_bits",
   "long c_interleave_bits(long x, long y) {\n"
   "  long result = 0;\n"
   "  for (int i = 0; i < 16; ++i) {\n"
   "    result |= ((x >> i) & 1) << (2*i);\n"
   "    result |= ((y >> i) & 1) << (2*i + 1);\n"
   "  }\n"
   "  return result;\n"
   "}\n",
   {0xAA, 0x55}, "IntCExpr", 2, ""},

  {"c_reverse_bits",
   "long c_reverse_bits(long x) {\n"
   "  long result = 0;\n"
   "  for (int i = 0; i < 32; ++i) {\n"
   "    result = (result << 1) | (x & 1);\n"
   "    x >>= 1;\n"
   "  }\n"
   "  return result;\n"
   "}\n",
   {0xDEADBEEF}, "IntCExpr", 2, ""},

};
// clang-format on

INSTANTIATE_TEST_SUITE_P(IntCExpr, X64IntCExprExtRT,
                         ::testing::ValuesIn(kX64IntCExprExt),
                         [](const auto &P) { return P.param.Name; });
