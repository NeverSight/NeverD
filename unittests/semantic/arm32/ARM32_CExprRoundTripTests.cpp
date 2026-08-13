//===- ARM32_CExprRoundTripTests.cpp - C expression roundtrip ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Pure C expression roundtrip tests for ARM32.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

TEST_P(ARM32RoundTrip, CExprVerify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32CExpr = {
  {"c_add", "int c_add(int a, int b) { return a + b; }\n", {100, 42}, "CExprRT"},
  {"c_sub", "int c_sub(int a, int b) { return a - b; }\n", {100, 30}, "CExprRT"},
  {"c_mul", "int c_mul(int a, int b) { return a * b; }\n", {6, 7}, "CExprRT"},
  {"c_div", "int c_div(int a, int b) { return a / b; }\n", {100, 7}, "CExprRT"},
  {"c_mod", "int c_mod(int a, int b) { return a % b; }\n", {100, 7}, "CExprRT"},
  {"c_neg", "int c_neg(int a) { return -a; }\n", {42}, "CExprRT"},

  {"c_udiv", "typedef unsigned int uint;\nuint c_udiv(uint a, uint b) { return a / b; }\n", {100, 7}, "CExprRT"},
  {"c_umod", "typedef unsigned int uint;\nuint c_umod(uint a, uint b) { return a % b; }\n", {100, 7}, "CExprRT"},

  {"c_and", "int c_and(int a, int b) { return a & b; }\n", {0xFF00, 0x0FF0}, "CExprRT"},
  {"c_or",  "int c_or(int a, int b) { return a | b; }\n", {0xF0, 0x0F}, "CExprRT"},
  {"c_xor", "int c_xor(int a, int b) { return a ^ b; }\n", {0xFF, 0x55}, "CExprRT"},
  {"c_not", "int c_not(int a) { return ~a; }\n", {0xFF}, "CExprRT"},

  {"c_shl", "int c_shl(int a, int b) { return a << b; }\n", {1, 10}, "CExprRT"},
  {"c_shr", "typedef unsigned int uint;\nuint c_shr(uint a, uint b) { return a >> b; }\n", {0x400, 2}, "CExprRT"},
  {"c_sar", "int c_sar(int a, int b) { return a >> b; }\n", {(uint64_t)(uint32_t)-128, 2}, "CExprRT"},

  {"c_eq",  "int c_eq(int a, int b) { return a == b; }\n", {42, 42}, "CExprRT"},
  {"c_ne",  "int c_ne(int a, int b) { return a != b; }\n", {42, 43}, "CExprRT"},
  {"c_lt",  "int c_lt(int a, int b) { return a < b; }\n", {3, 7}, "CExprRT"},
  {"c_gt",  "int c_gt(int a, int b) { return a > b; }\n", {7, 3}, "CExprRT"},
  {"c_min", "int c_min(int a, int b) { return a < b ? a : b; }\n", {3, 7}, "CExprRT"},
  {"c_max", "int c_max(int a, int b) { return a > b ? a : b; }\n", {3, 7}, "CExprRT"},
  {"c_abs", "int c_abs(int a) { return a < 0 ? -a : a; }\n", {(uint64_t)(uint32_t)-42}, "CExprRT"},

  {"c_u8_to_32",  "typedef unsigned char u8;\nint c_u8_to_32(u8 a) { return a; }\n", {0x42}, "CExprRT"},
  {"c_i8_to_32",  "typedef signed char i8;\nint c_i8_to_32(i8 a) { return a; }\n", {0x80}, "CExprRT"},
  {"c_u16_to_32", "typedef unsigned short u16;\nint c_u16_to_32(u16 a) { return a; }\n", {0x1234}, "CExprRT"},
  {"c_i16_to_32", "typedef short i16;\nint c_i16_to_32(i16 a) { return a; }\n", {0x8000}, "CExprRT"},

  {"c_sum_n",   "int c_sum_n(int n) { int s=0; for(int i=1;i<=n;++i) s+=i; return s; }\n", {10}, "CExprRT"},
  {"c_fib",     "int c_fib(int n) { int a=0,b=1; for(int i=0;i<n;++i){int t=a+b;a=b;b=t;} return a; }\n", {10}, "CExprRT"},
  {"c_factorial","int c_factorial(int n) { int r=1; for(int i=2;i<=n;++i) r*=i; return r; }\n", {10}, "CExprRT"},

  {"c_bswap32", "int c_bswap32(int a) { return __builtin_bswap32(a); }\n", {0x01020304}, "CExprRT"},

  // --- Edge cases ---
  {"c_identity", "int c_identity(int a) { return a; }\n", {0xDEADBEEF}, "CExprRT"},
  {"c_shl_zero", "int c_shl_zero(int a) { return a << 0; }\n", {42}, "CExprRT"},
  {"c_add3", "int c_add3(int a, int b, int c) { return a+b+c; }\n", {10, 20, 30}, "CExprRT"},

  // --- Bit manipulation ---
  {"c_isolate_low", "int c_isolate_low(int a) { return a & (-a); }\n", {0xABCD0000UL}, "CExprRT"},
  {"c_clear_low",   "int c_clear_low(int a) { return a & (a-1); }\n", {0xABCD0000UL}, "CExprRT"},
  {"c_sign_bit",    "int c_sign_bit(int a) { return (unsigned int)a >> 31; }\n", {(uint64_t)(uint32_t)-1}, "CExprRT"},

  // --- Complex ---
  {"c_polynomial", "int c_polynomial(int x) { return x*x*x - 3*x*x + 2*x - 7; }\n", {5}, "CExprRT"},

  {"c_popcount_manual",
   "int c_popcount_manual(int x) {\n"
   "  int c = 0;\n"
   "  while (x) { c += x & 1; x = (unsigned int)x >> 1; }\n"
   "  return c;\n"
   "}\n", {0x0F0F0F0F}, "CExprRT"},

  {"c_classify",
   "int c_classify(int a) {\n"
   "  if (a < 0) return -1;\n"
   "  if (a == 0) return 0;\n"
   "  if (a < 10) return 1;\n"
   "  return 2;\n"
   "}\n", {42}, "CExprRT"},

  // --- 8/16-bit conversions ---
  {"c_byte_arith",
   "int c_byte_arith(int a) {\n"
   "  unsigned char b = (unsigned char)a;\n"
   "  return b * b;\n"
   "}\n", {15}, "CExprRT"},

  {"c_word_arith",
   "int c_word_arith(int a, int b) {\n"
   "  unsigned short sa = (unsigned short)a;\n"
   "  unsigned short sb = (unsigned short)b;\n"
   "  return sa + sb;\n"
   "}\n", {300, 200}, "CExprRT"},

  // --- Multiply-accumulate (MLA/MLS patterns) ---
  {"c_mla", "int c_mla(int a, int b, int c) { return a + b * c; }\n", {10, 3, 7}, "CExprRT"},
  {"c_mls", "int c_mls(int a, int b, int c) { return a - b * c; }\n", {100, 3, 7}, "CExprRT"},

  // --- 64-bit multiply (SMULL/UMULL patterns) ---
  {"c_smull",
   "long long c_smull(int a, int b) { return (long long)a * (long long)b; }\n",
   {(uint64_t)(uint32_t)-12345, 6789}, "CExprRT"},
  {"c_umull",
   "unsigned long long c_umull(unsigned int a, unsigned int b) {\n"
   "  return (unsigned long long)a * (unsigned long long)b;\n"
   "}\n", {0xFFFF, 0xFFFF}, "CExprRT"},

  // --- Conditional patterns ---
  {"c_le",    "int c_le(int a, int b) { return a <= b; }\n", {7, 7}, "CExprRT"},
  {"c_ge",    "int c_ge(int a, int b) { return a >= b; }\n", {7, 7}, "CExprRT"},
  {"c_ult",   "int c_ult(unsigned int a, unsigned int b) { return a < b; }\n", {3, 7}, "CExprRT"},
  {"c_clamp", "int c_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }\n", {50, 10, 100}, "CExprRT"},

  // --- Overflow patterns ---
  {"c_overflow_add", "unsigned int c_overflow_add(unsigned int a, unsigned int b) { return a + b; }\n", {0xFFFFFFFF, 1}, "CExprRT"},
  {"c_overflow_mul", "unsigned int c_overflow_mul(unsigned int a, unsigned int b) { return a * b; }\n", {0xFFFF, 0xFFFF}, "CExprRT"},

  // --- Narrow-type stress tests (byte/short level operations) ---
  {"c_reverse_bits8",
   "int c_reverse_bits8(int x) {\n"
   "  unsigned char b = (unsigned char)x;\n"
   "  b = ((b >> 4) & 0x0F) | ((b << 4) & 0xF0);\n"
   "  b = ((b >> 2) & 0x33) | ((b << 2) & 0xCC);\n"
   "  b = ((b >> 1) & 0x55) | ((b << 1) & 0xAA);\n"
   "  return b;\n"
   "}\n", {0xA5}, "CExprRT"},

  {"c_byte_mask_chain",
   "int c_byte_mask_chain(int x) {\n"
   "  unsigned char a = (unsigned char)x;\n"
   "  unsigned char b = a ^ 0xFF;\n"
   "  unsigned char c = (a & 0xF0) | (b & 0x0F);\n"
   "  return c;\n"
   "}\n", {0xAB}, "CExprRT"},

  {"c_nibble_swap",
   "int c_nibble_swap(int x) {\n"
   "  unsigned char b = (unsigned char)x;\n"
   "  return ((b >> 4) | (b << 4)) & 0xFF;\n"
   "}\n", {0xAB}, "CExprRT"},

  {"c_short_mul",
   "int c_short_mul(int a, int b) {\n"
   "  short sa = (short)a;\n"
   "  short sb = (short)b;\n"
   "  return (int)(sa * sb);\n"
   "}\n", {100, 200}, "CExprRT"},

  {"c_byte_rotate",
   "int c_byte_rotate(int x, int n) {\n"
   "  unsigned char b = (unsigned char)x;\n"
   "  n &= 7;\n"
   "  return ((b << n) | (b >> (8 - n))) & 0xFF;\n"
   "}\n", {0xA5, 3}, "CExprRT"},

  // --- Collatz (complex control flow) ---
  {"c_collatz",
   "int c_collatz(int n) {\n"
   "  int steps = 0;\n"
   "  while (n > 1) {\n"
   "    if (n & 1) n = 3*n + 1;\n"
   "    else n = n >> 1;\n"
   "    ++steps;\n"
   "  }\n"
   "  return steps;\n"
   "}\n", {27}, "CExprRT"},

  // --- Multi-arg ---
  {"c_dot2", "int c_dot2(int a, int b, int c, int d) { return a*b + c*d; }\n", {3, 4, 5, 6}, "CExprRT"},

  // --- Stack array ---
  {"c_stack_sum",
   "int c_stack_sum(int a, int b, int c) {\n"
   "  int arr[3];\n"
   "  arr[0] = a; arr[1] = b; arr[2] = c;\n"
   "  int s = 0;\n"
   "  for (int i = 0; i < 3; ++i) s += arr[i];\n"
   "  return s;\n"
   "}\n", {10, 20, 30}, "CExprRT"},

  // --- Rotate ---
  {"c_rotl32",
   "int c_rotl32(int a, int b) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  unsigned int n = (unsigned int)b & 31;\n"
   "  return (int)((x << n) | (x >> (32 - n)));\n"
   "}\n",
   {0xDEADBEEF, 12}, "CExprRT"},

  // --- Power ---
  {"c_ipow",
   "int c_ipow(int base, int exp) {\n"
   "  int result = 1;\n"
   "  while (exp > 0) {\n"
   "    if (exp & 1) result *= base;\n"
   "    base *= base;\n"
   "    exp >>= 1;\n"
   "  }\n"
   "  return result;\n"
   "}\n",
   {3, 10}, "CExprRT"},

  // --- Integer sqrt ---
  {"c_isqrt",
   "int c_isqrt(int n) {\n"
   "  if (n <= 1) return n;\n"
   "  unsigned int lo = 1, hi = (unsigned int)n;\n"
   "  while (lo <= hi) {\n"
   "    unsigned int mid = lo + (hi - lo) / 2;\n"
   "    if (mid <= (unsigned int)n / mid) lo = mid + 1;\n"
   "    else hi = mid - 1;\n"
   "  }\n"
   "  return (int)(lo - 1);\n"
   "}\n",
   {144}, "CExprRT"},

  // --- Parity ---
  {"c_parity",
   "int c_parity(int a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  x ^= x >> 16; x ^= x >> 8;\n"
   "  x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;\n"
   "  return x & 1;\n"
   "}\n",
   {0x0F0F0F0F}, "CExprRT"},

  // --- CRC8 ---
  {"c_crc8",
   "int c_crc8(int data) {\n"
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
   "int c_divmod(int a, int b) {\n"
   "  return (a / b) * 1000 + (a % b);\n"
   "}\n",
   {12345, 67}, "CExprRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ARM32CExprRT, ARM32RoundTrip, ::testing::ValuesIn(kARM32CExpr), rtTCName);
