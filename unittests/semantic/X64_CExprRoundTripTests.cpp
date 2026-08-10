//===- X64_CExprRoundTripTests.cpp - C expression roundtrip tests -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip tests using pure C expressions (no inline asm).
// The compiler naturally selects the correct x86_64 instructions.
// Covers: arithmetic, bitwise, shifts, comparisons, conversions,
//         floating point, 32-bit ops, overflow, abs, min/max patterns.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

TEST_P(X64RoundTrip, CExprVerify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64CExpr = {
  // --- Integer arithmetic ---
  {"c_add", "long c_add(long a, long b) { return a + b; }\n", {100, 42}, "CExprRT"},
  {"c_sub", "long c_sub(long a, long b) { return a - b; }\n", {100, 30}, "CExprRT"},
  {"c_mul", "long c_mul(long a, long b) { return a * b; }\n", {6, 7}, "CExprRT"},
  {"c_div", "long c_div(long a, long b) { return a / b; }\n", {100, 7}, "CExprRT"},
  {"c_mod", "long c_mod(long a, long b) { return a % b; }\n", {100, 7}, "CExprRT"},
  {"c_neg", "long c_neg(long a) { return -a; }\n", {42}, "CExprRT"},

  // --- Unsigned arithmetic ---
  {"c_udiv", "typedef unsigned long ulong;\nulong c_udiv(ulong a, ulong b) { return a / b; }\n", {100, 7}, "CExprRT"},
  {"c_umod", "typedef unsigned long ulong;\nulong c_umod(ulong a, ulong b) { return a % b; }\n", {100, 7}, "CExprRT"},

  // Signed/unsigned 64-bit div/mod stress: exercises CQO+IDIV and XOR-RDX+DIV
  {"c_div_neg", "long c_div_neg(long a, long b) { return a / b; }\n", {(uint64_t)-100, 7}, "CExprRT"},
  {"c_mod_neg", "long c_mod_neg(long a, long b) { return a % b; }\n", {(uint64_t)-100, 7}, "CExprRT"},
  {"c_udiv_large", "typedef unsigned long ulong;\nulong c_udiv_large(ulong a, ulong b) { return a / b; }\n", {0xFFFFFFFFFFFFFFFFULL, 3}, "CExprRT"},
  {"c_umod_large", "typedef unsigned long ulong;\nulong c_umod_large(ulong a, ulong b) { return a % b; }\n", {0xFFFFFFFFFFFFFFFFULL, 3}, "CExprRT"},
  {"c_udiv_gcd",
   "typedef unsigned long ulong;\n"
   "ulong c_udiv_gcd(ulong a, ulong b) {\n"
   "  while (b) { ulong t = b; b = a % b; a = t; }\n"
   "  return a;\n"
   "}\n",
   {48, 18}, "CExprRT"},

  // --- Bitwise ---
  {"c_and", "long c_and(long a, long b) { return a & b; }\n", {0xFF00, 0x0FF0}, "CExprRT"},
  {"c_or",  "long c_or(long a, long b) { return a | b; }\n", {0xF0, 0x0F}, "CExprRT"},
  {"c_xor", "long c_xor(long a, long b) { return a ^ b; }\n", {0xFF, 0x55}, "CExprRT"},
  {"c_not", "long c_not(long a) { return ~a; }\n", {0xFF}, "CExprRT"},

  // --- Shifts ---
  {"c_shl", "long c_shl(long a, long b) { return a << b; }\n", {1, 10}, "CExprRT"},
  {"c_shr", "typedef unsigned long ulong;\nulong c_shr(ulong a, ulong b) { return a >> b; }\n", {0x400, 2}, "CExprRT"},
  {"c_sar", "long c_sar(long a, long b) { return a >> b; }\n", {(uint64_t)-128, 2}, "CExprRT"},

  // --- Comparisons → setcc ---
  {"c_eq",  "long c_eq(long a, long b) { return a == b; }\n", {42, 42}, "CExprRT"},
  {"c_ne",  "long c_ne(long a, long b) { return a != b; }\n", {42, 43}, "CExprRT"},
  {"c_lt",  "long c_lt(long a, long b) { return a < b; }\n", {3, 7}, "CExprRT"},
  {"c_gt",  "long c_gt(long a, long b) { return a > b; }\n", {7, 3}, "CExprRT"},
  {"c_le",  "long c_le(long a, long b) { return a <= b; }\n", {7, 7}, "CExprRT"},
  {"c_ge",  "long c_ge(long a, long b) { return a >= b; }\n", {7, 7}, "CExprRT"},
  {"c_ult", "typedef unsigned long ulong;\nlong c_ult(ulong a, ulong b) { return a < b; }\n", {3, 7}, "CExprRT"},
  {"c_ugt", "typedef unsigned long ulong;\nlong c_ugt(ulong a, ulong b) { return a > b; }\n", {7, 3}, "CExprRT"},

  // --- Conditional / ternary → cmov ---
  {"c_min",   "long c_min(long a, long b) { return a < b ? a : b; }\n", {3, 7}, "CExprRT"},
  {"c_max",   "long c_max(long a, long b) { return a > b ? a : b; }\n", {3, 7}, "CExprRT"},
  {"c_abs",   "long c_abs(long a) { return a < 0 ? -a : a; }\n", {(uint64_t)-42}, "CExprRT"},
  {"c_clamp", "long c_clamp(long v, long lo, long hi) { return v < lo ? lo : (v > hi ? hi : v); }\n", {50, 10, 100}, "CExprRT"},

  // --- 32-bit operations (generates 32-bit x86 ops) ---
  {"c_add32", "int c_add32(int a, int b) { return a + b; }\n", {100, 42}, "CExprRT"},
  {"c_mul32", "int c_mul32(int a, int b) { return a * b; }\n", {6, 7}, "CExprRT"},
  {"c_div32", "int c_div32(int a, int b) { return a / b; }\n", {100, 7}, "CExprRT"},
  {"c_shl32", "int c_shl32(int a, int b) { return a << b; }\n", {1, 10}, "CExprRT"},

  // --- Type conversions (movzx/movsx/movsxd) ---
  {"c_u8_to_64",  "typedef unsigned char u8;\nlong c_u8_to_64(u8 a) { return a; }\n", {0x42}, "CExprRT"},
  {"c_i8_to_64",  "typedef signed char i8;\nlong c_i8_to_64(i8 a) { return a; }\n", {0x80}, "CExprRT"},
  {"c_u16_to_64", "typedef unsigned short u16;\nlong c_u16_to_64(u16 a) { return a; }\n", {0x1234}, "CExprRT"},
  {"c_i16_to_64", "typedef short i16;\nlong c_i16_to_64(i16 a) { return a; }\n", {0x8000}, "CExprRT"},
  {"c_u32_to_64", "typedef unsigned int u32;\nlong c_u32_to_64(u32 a) { return a; }\n", {0x12345678}, "CExprRT"},
  {"c_i32_to_64", "long c_i32_to_64(int a) { return a; }\n", {0xFFFFFFFF}, "CExprRT"},

  // --- Loops (exercises branches) ---
  {"c_sum_n",    "long c_sum_n(long n) { long s=0; for(long i=1;i<=n;++i) s+=i; return s; }\n", {10}, "CExprRT"},
  {"c_fib",      "long c_fib(long n) { long a=0,b=1; for(long i=0;i<n;++i){long t=a+b;a=b;b=t;} return a; }\n", {10}, "CExprRT"},
  {"c_factorial", "long c_factorial(long n) { long r=1; for(long i=2;i<=n;++i) r*=i; return r; }\n", {10}, "CExprRT"},

  // --- Nested control flow ---
  {"c_collatz_steps",
   "long c_collatz_steps(long n) {\n"
   "  long steps = 0;\n"
   "  while (n > 1) {\n"
   "    if (n & 1) n = 3*n + 1;\n"
   "    else n = n / 2;\n"
   "    ++steps;\n"
   "  }\n"
   "  return steps;\n"
   "}\n",
   {27}, "CExprRT"},

  {"c_popcount", "long c_popcount(long a) { return __builtin_popcountll(a); }\n", {0xDEADBEEFCAFEBABEULL}, "CExprRT", /*OptLevel=*/2, "-mpopcnt"},
  {"c_clz64", "long c_clz64(long a) { return a ? __builtin_clzll(a) : 64; }\n", {0x100}, "CExprRT", /*OptLevel=*/2, "-mlzcnt"},
  {"c_ctz64", "long c_ctz64(long a) { return a ? __builtin_ctzll(a) : 64; }\n", {0x100}, "CExprRT", /*OptLevel=*/2, "-mbmi"},

  // --- Byte swap ---
  {"c_bswap32", "int c_bswap32(int a) { return __builtin_bswap32(a); }\n", {0x01020304}, "CExprRT"},
  {"c_bswap64", "long c_bswap64(long a) { return __builtin_bswap64(a); }\n", {0x0102030405060708ULL}, "CExprRT"},

  // --- Edge cases ---
  {"c_shl_zero",  "long c_shl_zero(long a) { return a << 0; }\n", {42}, "CExprRT"},
  {"c_shr_max",   "typedef unsigned long ulong;\nulong c_shr_max(ulong a) { return a >> 63; }\n", {0x8000000000000000ULL}, "CExprRT"},
  {"c_overflow_add", "typedef unsigned long ulong;\nulong c_overflow_add(ulong a, ulong b) { return a + b; }\n", {0xFFFFFFFFFFFFFFFFULL, 1}, "CExprRT"},
  {"c_overflow_mul", "typedef unsigned int u32;\nu32 c_overflow_mul(u32 a, u32 b) { return a * b; }\n", {0xFFFF, 0xFFFF}, "CExprRT"},
  {"c_identity",  "long c_identity(long a) { return a; }\n", {0xDEADBEEFCAFEBABEULL}, "CExprRT"},

  // --- Multi-arg ---
  {"c_add3",  "long c_add3(long a, long b, long c) { return a + b + c; }\n", {10, 20, 30}, "CExprRT"},
  {"c_dot2",  "long c_dot2(long a, long b, long c, long d) { return a*b + c*d; }\n", {3, 4, 5, 6}, "CExprRT"},

  // --- Bit manipulation patterns ---
  {"c_isolate_lowest_set", "long c_isolate_lowest_set(long a) { return a & (-a); }\n", {0xABCD0000ULL}, "CExprRT"},
  {"c_clear_lowest_set",  "long c_clear_lowest_set(long a) { return a & (a - 1); }\n", {0xABCD0000ULL}, "CExprRT"},
  {"c_is_power_of_2",     "long c_is_power_of_2(long a) { return a > 0 && (a & (a-1)) == 0; }\n", {256}, "CExprRT"},
  {"c_sign_bit",          "long c_sign_bit(long a) { return (unsigned long)a >> 63; }\n", {(uint64_t)-1}, "CExprRT"},
  {"c_count_trailing_zeros_mask", "long c_count_trailing_zeros_mask(long a) { return (a & (-a)) - 1; }\n", {0x100}, "CExprRT"},

  // --- Switch-like patterns ---
  {"c_classify",
   "long c_classify(long a) {\n"
   "  if (a < 0) return -1;\n"
   "  if (a == 0) return 0;\n"
   "  if (a < 10) return 1;\n"
   "  if (a < 100) return 2;\n"
   "  return 3;\n"
   "}\n",
   {42}, "CExprRT"},

  // --- Complex nested arithmetic ---
  {"c_polynomial",
   "long c_polynomial(long x) {\n"
   "  return x*x*x - 3*x*x + 2*x - 7;\n"
   "}\n",
   {5}, "CExprRT"},

  {"c_gcd",
   "long c_gcd(long a, long b) {\n"
   "  while (b) {\n"
   "    long t = b;\n"
   "    b = a % b;\n"
   "    a = t;\n"
   "  }\n"
   "  return a;\n"
   "}\n",
   {48, 18}, "CExprRT"},

  {"c_popcount_manual",
   "long c_popcount_manual(long x) {\n"
   "  long count = 0;\n"
   "  while (x) {\n"
   "    count += x & 1;\n"
   "    x = (unsigned long)x >> 1;\n"
   "  }\n"
   "  return count;\n"
   "}\n",
   {0x0F0F0F0FULL}, "CExprRT"},

  {"c_reverse_bits8",
   "long c_reverse_bits8(long x) {\n"
   "  unsigned char b = (unsigned char)x;\n"
   "  b = ((b >> 4) & 0x0F) | ((b << 4) & 0xF0);\n"
   "  b = ((b >> 2) & 0x33) | ((b << 2) & 0xCC);\n"
   "  b = ((b >> 1) & 0x55) | ((b << 1) & 0xAA);\n"
   "  return b;\n"
   "}\n",
   {0xA5}, "CExprRT"},

  // --- Array-like access via pointer arithmetic (stack) ---
  {"c_stack_array_sum",
   "long c_stack_array_sum(long a, long b, long c) {\n"
   "  long arr[3];\n"
   "  arr[0] = a; arr[1] = b; arr[2] = c;\n"
   "  long s = 0;\n"
   "  for (int i = 0; i < 3; ++i) s += arr[i];\n"
   "  return s;\n"
   "}\n",
   {10, 20, 30}, "CExprRT"},

  // --- 32-bit sub-register heavy ---
  {"c_u32_bitmix",
   "typedef unsigned int u32;\n"
   "long c_u32_bitmix(long a) {\n"
   "  u32 x = (u32)a;\n"
   "  x ^= x >> 16;\n"
   "  x *= 0x45d9f3bU;\n"
   "  x ^= x >> 16;\n"
   "  return x;\n"
   "}\n",
   {0xDEADBEEF}, "CExprRT"},

  {"c_u32_murmur_fmix",
   "typedef unsigned int u32;\n"
   "long c_u32_murmur_fmix(long a) {\n"
   "  u32 h = (u32)a;\n"
   "  h ^= h >> 16;\n"
   "  h *= 0x85ebca6bU;\n"
   "  h ^= h >> 13;\n"
   "  h *= 0xc2b2ae35U;\n"
   "  h ^= h >> 16;\n"
   "  return h;\n"
   "}\n",
   {42}, "CExprRT"},

  // --- Rotate via shifts ---
  {"c_rotl32",
   "long c_rotl32(long a, long b) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  unsigned int n = (unsigned int)b & 31;\n"
   "  return (x << n) | (x >> (32 - n));\n"
   "}\n",
   {0xDEADBEEF, 12}, "CExprRT"},

  {"c_rotr64",
   "long c_rotr64(long a, long b) {\n"
   "  unsigned long x = (unsigned long)a;\n"
   "  unsigned int n = (unsigned int)b & 63;\n"
   "  return (long)((x >> n) | (x << (64 - n)));\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 20}, "CExprRT"},

  // --- Multi-operation chains ---
  {"c_hash_combine",
   "long c_hash_combine(long a, long b) {\n"
   "  unsigned long h = (unsigned long)a;\n"
   "  h ^= (unsigned long)b + 0x9e3779b9ULL + (h << 6) + (h >> 2);\n"
   "  return (long)h;\n"
   "}\n",
   {42, 100}, "CExprRT"},

  // --- 16-bit operations ---
  {"c_add16",
   "long c_add16(long a, long b) {\n"
   "  unsigned short x = (unsigned short)a;\n"
   "  unsigned short y = (unsigned short)b;\n"
   "  return (unsigned short)(x + y);\n"
   "}\n",
   {0xFFF0, 0x20}, "CExprRT"},

  {"c_swap16",
   "long c_swap16(long a) {\n"
   "  unsigned short x = (unsigned short)a;\n"
   "  return (unsigned short)((x >> 8) | (x << 8));\n"
   "}\n",
   {0x1234}, "CExprRT"},

  // --- Byte operations ---
  {"c_max_byte",
   "long c_max_byte(long a, long b) {\n"
   "  unsigned char x = (unsigned char)a;\n"
   "  unsigned char y = (unsigned char)b;\n"
   "  return x > y ? x : y;\n"
   "}\n",
   {0x42, 0x99}, "CExprRT"},

  // --- Division and modulo patterns ---
  {"c_divmod_combined",
   "long c_divmod_combined(long a, long b) {\n"
   "  long q = a / b;\n"
   "  long r = a % b;\n"
   "  return q * 1000 + r;\n"
   "}\n",
   {12345, 67}, "CExprRT"},

  // --- Power function (iterative) ---
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

  // --- Binary search pattern ---
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

  // --- CRC-like computation ---
  {"c_crc8",
   "long c_crc8(long data) {\n"
   "  unsigned char crc = 0;\n"
   "  unsigned char d = (unsigned char)data;\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    if ((crc ^ d) & 0x80) crc = (crc << 1) ^ 0x07;\n"
   "    else crc <<= 1;\n"
   "    d <<= 1;\n"
   "  }\n"
   "  return crc;\n"
   "}\n",
   {0xA5}, "CExprRT"},

  // --- Bit counting patterns ---
  {"c_parity",
   "long c_parity(long a) {\n"
   "  unsigned long x = (unsigned long)a;\n"
   "  x ^= x >> 32; x ^= x >> 16; x ^= x >> 8;\n"
   "  x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;\n"
   "  return x & 1;\n"
   "}\n",
   {0x0F0F0F0FULL}, "CExprRT"},

  // --- Conditional without branch (arithmetic trick) ---
  {"c_branchless_min",
   "long c_branchless_min(long a, long b) {\n"
   "  long diff = a - b;\n"
   "  long sign = diff >> 63;\n"
   "  return b + (diff & sign);\n"
   "}\n",
   {42, 17}, "CExprRT"},

  // --- Multi-return via struct ---
  {"c_minmax",
   "long c_minmax(long a, long b) {\n"
   "  long mn = a < b ? a : b;\n"
   "  long mx = a > b ? a : b;\n"
   "  return mn * 1000 + mx;\n"
   "}\n",
   {42, 17}, "CExprRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(CExprRT, X64RoundTrip, ::testing::ValuesIn(kX64CExpr), rtTCName);
