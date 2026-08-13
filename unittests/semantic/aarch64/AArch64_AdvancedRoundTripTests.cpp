//===- AArch64_AdvancedRoundTripTests.cpp - Advanced pattern tests -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 advanced roundtrip tests: atomic ops, FP patterns, NEON-triggering
// C expressions, bit manipulation, and memory patterns.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64AdvancedRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64AdvancedRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kAArch64Advanced = {
  // --- Volatile load/store (simulates atomic-like access without ldxr/stxr) ---
  {"a64_volatile_rw",
   "long a64_volatile_rw(long a) {\n"
   "  volatile long val = a;\n"
   "  long old = val;\n"
   "  val = old + 10;\n"
   "  return val;\n"
   "}\n",
   {32}, "A64Adv"},

  // --- Bit reversal (rbit instruction) ---
  {"a64_reverse_bits32",
   "long a64_reverse_bits32(long a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  return __builtin_bitreverse32(x);\n"
   "}\n",
   {0x12345678}, "A64Adv"},

  // --- Count leading zeros ---
  {"a64_clz",
   "long a64_clz(long a) {\n"
   "  return __builtin_clzll((unsigned long)a | 1);\n"
   "}\n",
   {0x100}, "A64Adv"},

  // --- Byte swap ---
  {"a64_bswap32",
   "long a64_bswap32(long a) {\n"
   "  return __builtin_bswap32((unsigned int)a);\n"
   "}\n",
   {0x01020304}, "A64Adv"},

  {"a64_bswap64",
   "long a64_bswap64(long a) {\n"
   "  return __builtin_bswap64((unsigned long)a);\n"
   "}\n",
   {0x0102030405060708ULL}, "A64Adv"},

  // --- Struct-like access ---
  {"a64_struct",
   "long a64_struct(long a, long b, long c) {\n"
   "  struct { long x, y, z; } s;\n"
   "  s.x = a; s.y = b; s.z = c;\n"
   "  return s.x * s.y + s.z;\n"
   "}\n",
   {3, 7, 5}, "A64Adv"},

  // --- Array operations ---
  {"a64_array_max",
   "long a64_array_max(long a, long b, long c, long d) {\n"
   "  long arr[4] = {a, b, c, d};\n"
   "  long mx = arr[0];\n"
   "  for (int i = 1; i < 4; ++i)\n"
   "    if (arr[i] > mx) mx = arr[i];\n"
   "  return mx;\n"
   "}\n",
   {5, 12, 3, 9}, "A64Adv"},

  // --- Complex bit manipulation ---
  {"a64_popcount_manual",
   "long a64_popcount_manual(long x) {\n"
   "  long c = 0;\n"
   "  unsigned long u = (unsigned long)x;\n"
   "  while (u) { c += u & 1; u >>= 1; }\n"
   "  return c;\n"
   "}\n",
   {0x0F0F0F0FULL}, "A64Adv"},

  // --- Polynomial evaluation ---
  {"a64_polynomial",
   "long a64_polynomial(long x) {\n"
   "  return x*x*x - 3*x*x + 2*x - 7;\n"
   "}\n",
   {5}, "A64Adv"},

  // --- GCD ---
  {"a64_gcd",
   "long a64_gcd(long a, long b) {\n"
   "  while (b) { long t = b; b = a % b; a = t; }\n"
   "  return a;\n"
   "}\n",
   {48, 18}, "A64Adv"},

  // --- Saturating operations ---
  {"a64_sat_add",
   "long a64_sat_add(long a, long b) {\n"
   "  unsigned long ua = (unsigned long)a, ub = (unsigned long)b;\n"
   "  unsigned long sum = ua + ub;\n"
   "  return sum < ua ? (long)0xFFFFFFFFFFFFFFFFULL : (long)sum;\n"
   "}\n",
   {0xFFFFFFFFFFFFFF00ULL, 0x100}, "A64Adv"},

  // --- Fibonacci ---
  {"a64_fib",
   "long a64_fib(long n) {\n"
   "  long a=0, b=1;\n"
   "  for (long i=0; i<n; ++i) { long t=a+b; a=b; b=t; }\n"
   "  return a;\n"
   "}\n",
   {15}, "A64Adv"},

  // --- 32-bit operations ---
  {"a64_add32", "int a64_add32(int a, int b) { return a + b; }\n", {100, 42}, "A64Adv"},
  {"a64_mul32", "int a64_mul32(int a, int b) { return a * b; }\n", {6, 7}, "A64Adv"},
  {"a64_div32", "int a64_div32(int a, int b) { return a / b; }\n", {100, 7}, "A64Adv"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(A64Adv, AArch64AdvancedRT,
                         ::testing::ValuesIn(kAArch64Advanced), rtTCName);
