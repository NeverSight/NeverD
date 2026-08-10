//===- AArch64_CExprRoundTripTests.cpp - C expression roundtrip -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Pure C expression roundtrip tests for AArch64.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

TEST_P(AArch64RoundTrip, CExprVerify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64CExpr = {
  {"c_add", "long c_add(long a, long b) { return a + b; }\n", {100, 42}, "CExprRT"},
  {"c_sub", "long c_sub(long a, long b) { return a - b; }\n", {100, 30}, "CExprRT"},
  {"c_mul", "long c_mul(long a, long b) { return a * b; }\n", {6, 7}, "CExprRT"},
  {"c_div", "long c_div(long a, long b) { return a / b; }\n", {100, 7}, "CExprRT"},
  {"c_mod", "long c_mod(long a, long b) { return a % b; }\n", {100, 7}, "CExprRT"},
  {"c_neg", "long c_neg(long a) { return -a; }\n", {42}, "CExprRT"},

  {"c_udiv", "typedef unsigned long ulong;\nulong c_udiv(ulong a, ulong b) { return a / b; }\n", {100, 7}, "CExprRT"},
  {"c_umod", "typedef unsigned long ulong;\nulong c_umod(ulong a, ulong b) { return a % b; }\n", {100, 7}, "CExprRT"},

  {"c_and", "long c_and(long a, long b) { return a & b; }\n", {0xFF00, 0x0FF0}, "CExprRT"},
  {"c_or",  "long c_or(long a, long b) { return a | b; }\n", {0xF0, 0x0F}, "CExprRT"},
  {"c_xor", "long c_xor(long a, long b) { return a ^ b; }\n", {0xFF, 0x55}, "CExprRT"},
  {"c_not", "long c_not(long a) { return ~a; }\n", {0xFF}, "CExprRT"},

  {"c_shl", "long c_shl(long a, long b) { return a << b; }\n", {1, 10}, "CExprRT"},
  {"c_shr", "typedef unsigned long ulong;\nulong c_shr(ulong a, ulong b) { return a >> b; }\n", {0x400, 2}, "CExprRT"},
  {"c_sar", "long c_sar(long a, long b) { return a >> b; }\n", {(uint64_t)-128, 2}, "CExprRT"},

  {"c_eq",  "long c_eq(long a, long b) { return a == b; }\n", {42, 42}, "CExprRT"},
  {"c_ne",  "long c_ne(long a, long b) { return a != b; }\n", {42, 43}, "CExprRT"},
  {"c_lt",  "long c_lt(long a, long b) { return a < b; }\n", {3, 7}, "CExprRT"},
  {"c_gt",  "long c_gt(long a, long b) { return a > b; }\n", {7, 3}, "CExprRT"},
  {"c_min", "long c_min(long a, long b) { return a < b ? a : b; }\n", {3, 7}, "CExprRT"},
  {"c_max", "long c_max(long a, long b) { return a > b ? a : b; }\n", {3, 7}, "CExprRT"},
  {"c_abs", "long c_abs(long a) { return a < 0 ? -a : a; }\n", {(uint64_t)-42}, "CExprRT"},

  {"c_add32", "int c_add32(int a, int b) { return a + b; }\n", {100, 42}, "CExprRT"},
  {"c_mul32", "int c_mul32(int a, int b) { return a * b; }\n", {6, 7}, "CExprRT"},
  {"c_div32", "int c_div32(int a, int b) { return a / b; }\n", {100, 7}, "CExprRT"},

  {"c_u8_to_64",  "typedef unsigned char u8;\nlong c_u8_to_64(u8 a) { return a; }\n", {0x42}, "CExprRT"},
  {"c_i8_to_64",  "typedef signed char i8;\nlong c_i8_to_64(i8 a) { return a; }\n", {0x80}, "CExprRT"},
  {"c_u16_to_64", "typedef unsigned short u16;\nlong c_u16_to_64(u16 a) { return a; }\n", {0x1234}, "CExprRT"},
  {"c_i16_to_64", "typedef short i16;\nlong c_i16_to_64(i16 a) { return a; }\n", {0x8000}, "CExprRT"},
  {"c_i32_to_64", "long c_i32_to_64(int a) { return a; }\n", {0xFFFFFFFF}, "CExprRT"},

  {"c_sum_n",   "long c_sum_n(long n) { long s=0; for(long i=1;i<=n;++i) s+=i; return s; }\n", {10}, "CExprRT"},
  {"c_fib",     "long c_fib(long n) { long a=0,b=1; for(long i=0;i<n;++i){long t=a+b;a=b;b=t;} return a; }\n", {10}, "CExprRT"},
  {"c_factorial","long c_factorial(long n) { long r=1; for(long i=2;i<=n;++i) r*=i; return r; }\n", {10}, "CExprRT"},

  {"c_popcount", "long c_popcount(long a) { return __builtin_popcountll(a); }\n", {0xDEADBEEFCAFEBABEULL}, "CExprRT", /*OptLevel=*/2, "-march=armv8-a+simd"},
  {"c_clz64",   "long c_clz64(long a) { return a ? __builtin_clzll(a) : 64; }\n", {0x100}, "CExprRT"},
  {"c_ctz64",   "long c_ctz64(long a) { return a ? __builtin_ctzll(a) : 64; }\n", {0x100}, "CExprRT"},
  {"c_bswap32", "int c_bswap32(int a) { return __builtin_bswap32(a); }\n", {0x01020304}, "CExprRT"},
  {"c_bswap64", "long c_bswap64(long a) { return __builtin_bswap64(a); }\n", {0x0102030405060708ULL}, "CExprRT"},

  {"c_collatz",
   "long c_collatz(long n) {\n"
   "  long steps = 0;\n"
   "  while (n > 1) {\n"
   "    if (n & 1) n = 3*n + 1;\n"
   "    else n = n / 2;\n"
   "    ++steps;\n"
   "  }\n"
   "  return steps;\n"
   "}\n",
   {27}, "CExprRT"},

  // --- Edge cases ---
  {"c_identity", "long c_identity(long a) { return a; }\n", {0xCAFEBABEULL}, "CExprRT"},
  {"c_shl_zero", "long c_shl_zero(long a) { return a << 0; }\n", {42}, "CExprRT"},
  {"c_add3", "long c_add3(long a, long b, long c) { return a+b+c; }\n", {10, 20, 30}, "CExprRT"},
  {"c_dot2", "long c_dot2(long a, long b, long c, long d) { return a*b+c*d; }\n", {3,4,5,6}, "CExprRT"},

  // --- Bit manipulation ---
  {"c_isolate_low", "long c_isolate_low(long a) { return a & (-a); }\n", {0xABCD0000ULL}, "CExprRT"},
  {"c_clear_low",   "long c_clear_low(long a) { return a & (a-1); }\n", {0xABCD0000ULL}, "CExprRT"},
  {"c_sign_bit",    "long c_sign_bit(long a) { return (unsigned long)a >> 63; }\n", {(uint64_t)-1}, "CExprRT"},

  // --- Complex patterns ---
  {"c_polynomial", "long c_polynomial(long x) { return x*x*x - 3*x*x + 2*x - 7; }\n", {5}, "CExprRT"},
  {"c_gcd",
   "long c_gcd(long a, long b) {\n"
   "  while (b) { long t = b; b = a % b; a = t; }\n"
   "  return a;\n"
   "}\n", {48, 18}, "CExprRT"},

  {"c_popcount_manual",
   "long c_popcount_manual(long x) {\n"
   "  long c = 0;\n"
   "  while (x) { c += x & 1; x = (unsigned long)x >> 1; }\n"
   "  return c;\n"
   "}\n", {0x0F0F0F0FULL}, "CExprRT"},

  {"c_classify",
   "long c_classify(long a) {\n"
   "  if (a < 0) return -1;\n"
   "  if (a == 0) return 0;\n"
   "  if (a < 10) return 1;\n"
   "  return 2;\n"
   "}\n", {42}, "CExprRT"},

  // --- 32-bit sub-register patterns ---
  {"c_w_add", "int c_w_add(int a, int b) { return a + b; }\n", {0x7FFFFFFF, 1}, "CExprRT"},
  {"c_w_xor", "int c_w_xor(int a, int b) { return a ^ b; }\n", {0xDEADBEEF, 0x12345678}, "CExprRT"},
  {"c_w_neg", "int c_w_neg(int a) { return -a; }\n", {42}, "CExprRT"},
  {"c_w_shl", "int c_w_shl(int a, int b) { return a << b; }\n", {1, 20}, "CExprRT"},

  // --- Multiply-accumulate (MADD/MSUB patterns) ---
  {"c_madd", "long c_madd(long a, long b, long c) { return a + b * c; }\n", {10, 3, 7}, "CExprRT"},
  {"c_msub", "long c_msub(long a, long b, long c) { return a - b * c; }\n", {100, 3, 7}, "CExprRT"},
  {"c_madd32", "int c_madd32(int a, int b, int c) { return a + b * c; }\n", {10, 3, 7}, "CExprRT"},

  // --- 64-bit div/mod edge cases ---
  {"c_div_neg", "long c_div_neg(long a, long b) { return a / b; }\n", {(uint64_t)-100, 7}, "CExprRT"},
  {"c_mod_neg", "long c_mod_neg(long a, long b) { return a % b; }\n", {(uint64_t)-100, 7}, "CExprRT"},
  {"c_udiv_large", "typedef unsigned long ulong;\nulong c_udiv_large(ulong a, ulong b) { return a / b; }\n", {0xFFFFFFFFFFFFFFFFULL, 3}, "CExprRT"},

  // --- Wide multiply (SMULL/UMULL patterns) ---
  {"c_smull",
   "long c_smull(long a, long b) {\n"
   "  int x = (int)a, y = (int)b;\n"
   "  return (long)x * (long)y;\n"
   "}\n", {(uint64_t)(int)-12345, 6789}, "CExprRT"},
  {"c_umull",
   "typedef unsigned long ulong;\n"
   "ulong c_umull(ulong a, ulong b) {\n"
   "  unsigned int x = (unsigned int)a, y = (unsigned int)b;\n"
   "  return (ulong)x * (ulong)y;\n"
   "}\n", {0xFFFF, 0xFFFF}, "CExprRT"},

  // --- Conditional patterns (CSEL/CSINC/CSINV/CSNEG) ---
  {"c_le",  "long c_le(long a, long b) { return a <= b; }\n", {7, 7}, "CExprRT"},
  {"c_ge",  "long c_ge(long a, long b) { return a >= b; }\n", {7, 7}, "CExprRT"},
  {"c_ult", "typedef unsigned long ulong;\nlong c_ult(ulong a, ulong b) { return a < b; }\n", {3, 7}, "CExprRT"},
  {"c_clamp", "long c_clamp(long v, long lo, long hi) { return v < lo ? lo : (v > hi ? hi : v); }\n", {50, 10, 100}, "CExprRT"},

  // --- Overflow patterns ---
  {"c_overflow_add", "typedef unsigned long ulong;\nulong c_overflow_add(ulong a, ulong b) { return a + b; }\n", {0xFFFFFFFFFFFFFFFFULL, 1}, "CExprRT"},
  {"c_overflow_mul32", "typedef unsigned int u32;\nu32 c_overflow_mul32(u32 a, u32 b) { return a * b; }\n", {0xFFFF, 0xFFFF}, "CExprRT"},

  // --- Bit reversal patterns ---
  {"c_reverse_bits8",
   "long c_reverse_bits8(long x) {\n"
   "  unsigned char b = (unsigned char)x;\n"
   "  b = ((b >> 4) & 0x0F) | ((b << 4) & 0xF0);\n"
   "  b = ((b >> 2) & 0x33) | ((b << 2) & 0xCC);\n"
   "  b = ((b >> 1) & 0x55) | ((b << 1) & 0xAA);\n"
   "  return b;\n"
   "}\n", {0xA5}, "CExprRT"},

  // --- 32-bit bitmix (W-register heavy) ---
  {"c_u32_bitmix",
   "typedef unsigned int u32;\n"
   "long c_u32_bitmix(long a) {\n"
   "  u32 x = (u32)a;\n"
   "  x ^= x >> 16;\n"
   "  x *= 0x45d9f3bU;\n"
   "  x ^= x >> 16;\n"
   "  return x;\n"
   "}\n", {0xDEADBEEF}, "CExprRT"},

  // --- Stack array ---
  {"c_stack_sum",
   "long c_stack_sum(long a, long b, long c) {\n"
   "  long arr[3];\n"
   "  arr[0] = a; arr[1] = b; arr[2] = c;\n"
   "  long s = 0;\n"
   "  for (int i = 0; i < 3; ++i) s += arr[i];\n"
   "  return s;\n"
   "}\n", {10, 20, 30}, "CExprRT"},

  // --- Rotate ---
  {"c_rotl32",
   "long c_rotl32(long a, long b) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  unsigned int n = (unsigned int)b & 31;\n"
   "  return (x << n) | (x >> (32 - n));\n"
   "}\n",
   {0xDEADBEEF, 12}, "CExprRT"},

  // --- Power ---
  {"c_ipow",
   "long c_ipow(long base, long exp) {\n"
   "  long result = 1;\n"
   "  while (exp > 0) {\n"
   "    if (exp & 1) result *= base;\n"
   "    base *= base;\n"
   "    exp >>= 1;\n"
   "  }\n"
   "  return result;\n"
   "}\n",
   {3, 10}, "CExprRT"},

  {"c_isqrt",
   "long c_isqrt(long n) {\n"
   "  if (n <= 1) return n;\n"
   "  unsigned long x = (unsigned long)n;\n"
   "  unsigned long lo = 1, hi = x;\n"
   "  while (lo <= hi) {\n"
   "    unsigned long mid = lo + (hi - lo) / 2;\n"
   "    if (mid <= x / mid) lo = mid + 1;\n"
   "    else hi = mid - 1;\n"
   "  }\n"
   "  return (long)(lo - 1);\n"
   "}\n",
   {144}, "CExprRT"},

  // --- Parity ---
  {"c_parity",
   "long c_parity(long a) {\n"
   "  unsigned long x = (unsigned long)a;\n"
   "  x ^= x >> 32; x ^= x >> 16; x ^= x >> 8;\n"
   "  x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;\n"
   "  return x & 1;\n"
   "}\n",
   {0x0F0F0F0FULL}, "CExprRT"},

  // --- Hash combine ---
  {"c_hash_combine",
   "long c_hash_combine(long a, long b) {\n"
   "  unsigned long h = (unsigned long)a;\n"
   "  h ^= (unsigned long)b + 0x9e3779b9ULL + (h << 6) + (h >> 2);\n"
   "  return (long)h;\n"
   "}\n",
   {42, 100}, "CExprRT"},

  // --- CRC8 ---
  {"c_crc8",
   "long c_crc8(long data) {\n"
   "  unsigned char crc = 0, d = (unsigned char)data;\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    if ((crc ^ d) & 0x80) crc = (crc << 1) ^ 0x07;\n"
   "    else crc <<= 1;\n"
   "    d <<= 1;\n"
   "  }\n"
   "  return crc;\n"
   "}\n",
   {0xA5}, "CExprRT"},

  // --- Divmod ---
  {"c_divmod",
   "long c_divmod(long a, long b) {\n"
   "  return (a / b) * 1000 + (a % b);\n"
   "}\n",
   {12345, 67}, "CExprRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(A64CExprRT, AArch64RoundTrip, ::testing::ValuesIn(kA64CExpr), rtTCName);
