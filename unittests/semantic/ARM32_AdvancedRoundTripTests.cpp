//===- ARM32_AdvancedRoundTripTests.cpp - Advanced pattern tests --*- C++ -*-===//
//
// ARM32 advanced roundtrip tests: atomic ops, VFP patterns, struct access,
// bit manipulation, and loop patterns.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32AdvancedRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32AdvancedRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32Advanced = {
  // --- Atomic exchange ---
  {"arm_atomic_xchg",
   "int arm_atomic_xchg(int a) {\n"
   "  int val = a;\n"
   "  return __atomic_exchange_n(&val, a + 1, __ATOMIC_RELAXED);\n"
   "}\n",
   {42}, "ARM32Adv"},

  // --- Byte swap ---
  {"arm_bswap32",
   "int arm_bswap32(int a) {\n"
   "  return __builtin_bswap32((unsigned int)a);\n"
   "}\n",
   {0x01020304}, "ARM32Adv"},

  // --- Struct access ---
  {"arm_struct",
   "int arm_struct(int a, int b, int c) {\n"
   "  struct { int x, y, z; } s;\n"
   "  s.x = a; s.y = b; s.z = c;\n"
   "  return s.x * s.y + s.z;\n"
   "}\n",
   {3, 7, 5}, "ARM32Adv"},

  // --- Array operations ---
  {"arm_array_sum",
   "int arm_array_sum(int a, int b, int c) {\n"
   "  int arr[4];\n"
   "  arr[0] = a; arr[1] = b; arr[2] = c; arr[3] = a+b;\n"
   "  int s = 0;\n"
   "  for (int i = 0; i < 4; ++i) s += arr[i];\n"
   "  return s;\n"
   "}\n",
   {10, 20, 30}, "ARM32Adv"},

  // --- Popcount manual ---
  {"arm_popcount",
   "int arm_popcount(int x) {\n"
   "  int c = 0;\n"
   "  unsigned int u = (unsigned int)x;\n"
   "  while (u) { c += u & 1; u >>= 1; }\n"
   "  return c;\n"
   "}\n",
   {0x0F0F0F0F}, "ARM32Adv"},

  // --- GCD ---
  {"arm_gcd",
   "int arm_gcd(int a, int b) {\n"
   "  while (b) { int t = b; b = a % b; a = t; }\n"
   "  return a;\n"
   "}\n",
   {48, 18}, "ARM32Adv"},

  // --- Fibonacci ---
  {"arm_fib",
   "int arm_fib(int n) {\n"
   "  int a=0, b=1;\n"
   "  for (int i=0; i<n; ++i) { int t=a+b; a=b; b=t; }\n"
   "  return a;\n"
   "}\n",
   {15}, "ARM32Adv"},

  // --- Factorial ---
  {"arm_factorial",
   "int arm_factorial(int n) {\n"
   "  int r = 1;\n"
   "  for (int i = 2; i <= n; ++i) r *= i;\n"
   "  return r;\n"
   "}\n",
   {10}, "ARM32Adv"},

  // --- Polynomial ---
  {"arm_polynomial",
   "int arm_polynomial(int x) {\n"
   "  return x*x*x - 3*x*x + 2*x - 7;\n"
   "}\n",
   {5}, "ARM32Adv"},

  // --- Bit manipulation ---
  {"arm_isolate_lowest",
   "int arm_isolate_lowest(int a) { return a & (-a); }\n",
   {0xABCD0000}, "ARM32Adv"},

  {"arm_clear_lowest",
   "int arm_clear_lowest(int a) { return a & (a - 1); }\n",
   {0xABCD0000}, "ARM32Adv"},

  // --- Signed/unsigned mix ---
  {"arm_abs",
   "int arm_abs(int a) { return a < 0 ? -a : a; }\n",
   {(uint64_t)(uint32_t)-42}, "ARM32Adv"},

  {"arm_min",
   "int arm_min(int a, int b) { return a < b ? a : b; }\n",
   {3, 7}, "ARM32Adv"},

  {"arm_max",
   "int arm_max(int a, int b) { return a > b ? a : b; }\n",
   {3, 7}, "ARM32Adv"},

  // --- Collatz ---
  {"arm_collatz",
   "int arm_collatz(int n) {\n"
   "  int steps = 0;\n"
   "  while (n > 1) {\n"
   "    if (n & 1) n = 3*n + 1;\n"
   "    else n = n / 2;\n"
   "    ++steps;\n"
   "  }\n"
   "  return steps;\n"
   "}\n",
   {27}, "ARM32Adv"},

  // --- Nibble swap (byte-by-byte to avoid large constant in .rodata) ---
  {"arm_nibble_swap",
   "int arm_nibble_swap(int a) {\n"
   "  unsigned int u = (unsigned int)a;\n"
   "  unsigned int lo = u & 0x0FU, hi = (u >> 4) & 0x0FU;\n"
   "  unsigned int lo2 = (u >> 8) & 0x0FU, hi2 = (u >> 12) & 0x0FU;\n"
   "  unsigned int lo3 = (u >> 16) & 0x0FU, hi3 = (u >> 20) & 0x0FU;\n"
   "  unsigned int lo4 = (u >> 24) & 0x0FU, hi4 = (u >> 28) & 0x0FU;\n"
   "  return (int)((lo << 4) | hi | (lo2 << 12) | (hi2 << 8) |\n"
   "               (lo3 << 20) | (hi3 << 16) | (lo4 << 28) | (hi4 << 24));\n"
   "}\n",
   {0xABCD1234}, "ARM32Adv"},

  // --- Byte extract ---
  {"arm_byte_sum",
   "int arm_byte_sum(int a) {\n"
   "  unsigned int u = (unsigned int)a;\n"
   "  return (u & 0xFF) + ((u >> 8) & 0xFF) + ((u >> 16) & 0xFF) + ((u >> 24) & 0xFF);\n"
   "}\n",
   {0x01020304}, "ARM32Adv"},

  // --- Classify ---
  {"arm_classify",
   "int arm_classify(int a) {\n"
   "  if (a < 0) return -1;\n"
   "  if (a == 0) return 0;\n"
   "  if (a < 10) return 1;\n"
   "  return 2;\n"
   "}\n",
   {42}, "ARM32Adv"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ARM32Adv, ARM32AdvancedRT,
                         ::testing::ValuesIn(kARM32Advanced), rtTCName);
