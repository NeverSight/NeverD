//===- X64_MemStringRTTests.cpp - Memory + string pattern RT ---*- C++ -*-===//
//
// Tests x86_64 memory access patterns, string-like operations, and
// complex address mode calculations through lift pipeline.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64MemStrRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64MemStrRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64MemStr = {
  // ========== Memory copy pattern (MOV-based) ==========
  {"memcpy_8",
   "long memcpy_8(long a) {\n"
   "  long tmp;\n"
   "  __builtin_memcpy(&tmp, &a, 8);\n"
   "  return tmp;\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL}, "MemStr"},

  // ========== Byte memory pattern ==========
  {"byte_store_load",
   "long byte_store_load(long a) {\n"
   "  unsigned char buf[8];\n"
   "  buf[0] = (unsigned char)a;\n"
   "  buf[1] = (unsigned char)(a >> 8);\n"
   "  buf[2] = (unsigned char)(a >> 16);\n"
   "  buf[3] = (unsigned char)(a >> 24);\n"
   "  return (unsigned long)buf[0] | ((unsigned long)buf[1] << 8)\n"
   "       | ((unsigned long)buf[2] << 16) | ((unsigned long)buf[3] << 24);\n"
   "}\n",
   {0x12345678}, "MemStr"},

  // ========== Stack-based swap ==========
  {"swap_via_stack",
   "long swap_via_stack(long a, long b) {\n"
   "  long tmp = a;\n"
   "  a = b;\n"
   "  b = tmp;\n"
   "  return a + b;\n"
   "}\n",
   {42, 100}, "MemStr"},

  // ========== Multi-return via struct-like ==========
  {"multi_return",
   "long multi_return(long a, long b) {\n"
   "  long sum = a + b;\n"
   "  long diff = a - b;\n"
   "  return sum ^ diff;\n"
   "}\n",
   {100, 42}, "MemStr"},

  // ========== Fibonacci (stack + loop) ==========
  {"fib_iter",
   "long fib_iter(long n) {\n"
   "  if (n <= 1) return n;\n"
   "  long a = 0, b = 1;\n"
   "  for (long i = 2; i <= n; ++i) {\n"
   "    long t = a + b;\n"
   "    a = b;\n"
   "    b = t;\n"
   "  }\n"
   "  return b;\n"
   "}\n",
   {10}, "MemStr"},

  // ========== GCD (Euclidean algorithm) ==========
  {"gcd",
   "long gcd(long a, long b) {\n"
   "  while (b != 0) {\n"
   "    long t = b;\n"
   "    b = a % b;\n"
   "    a = t;\n"
   "  }\n"
   "  return a;\n"
   "}\n",
   {48, 18}, "MemStr"},

  // ========== Power (iterative) ==========
  {"ipow",
   "long ipow(long base, long exp) {\n"
   "  long result = 1;\n"
   "  while (exp > 0) {\n"
   "    if (exp & 1) result *= base;\n"
   "    base *= base;\n"
   "    exp >>= 1;\n"
   "  }\n"
   "  return result;\n"
   "}\n",
   {2, 10}, "MemStr"},

  // ========== Collatz length ==========
  {"collatz",
   "long collatz(long n) {\n"
   "  long count = 0;\n"
   "  while (n != 1) {\n"
   "    if (n & 1) n = 3 * n + 1;\n"
   "    else n >>= 1;\n"
   "    count++;\n"
   "  }\n"
   "  return count;\n"
   "}\n",
   {27}, "MemStr"},

  // ========== Integer sqrt (Heron's method) ==========
  {"isqrt",
   "long isqrt(long n) {\n"
   "  if (n < 2) return n;\n"
   "  long x = n;\n"
   "  long y = (x + 1) / 2;\n"
   "  while (y < x) {\n"
   "    x = y;\n"
   "    y = (x + n / x) / 2;\n"
   "  }\n"
   "  return x;\n"
   "}\n",
   {144}, "MemStr"},

  // ========== Hamming distance ==========
  {"hamming",
   "long hamming(long a, long b) {\n"
   "  unsigned long x = (unsigned long)(a ^ b);\n"
   "  long count = 0;\n"
   "  while (x) {\n"
   "    count += x & 1;\n"
   "    x >>= 1;\n"
   "  }\n"
   "  return count;\n"
   "}\n",
   {0xFF00FF, 0x00FF00}, "MemStr"},

  // ========== Bit reverse 32 ==========
  {"bitrev32",
   "long bitrev32(long a) {\n"
   "  unsigned x = (unsigned)a;\n"
   "  unsigned r = 0;\n"
   "  for (int i = 0; i < 32; ++i) {\n"
   "    r = (r << 1) | (x & 1);\n"
   "    x >>= 1;\n"
   "  }\n"
   "  return (long)r;\n"
   "}\n",
   {0x12345678}, "MemStr"},

  // ========== CRC-like pattern ==========
  {"crc_step",
   "long crc_step(long crc, long data) {\n"
   "  unsigned c = (unsigned)crc;\n"
   "  unsigned d = (unsigned)data;\n"
   "  c ^= d;\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    if (c & 1) c = (c >> 1) ^ 0xEDB88320u;\n"
   "    else c >>= 1;\n"
   "  }\n"
   "  return (long)c;\n"
   "}\n",
   {0xFFFFFFFF, 0x41}, "MemStr"},

  // ========== LEA addressing mode combinations ==========
  {"lea_base_idx",
   "long lea_base_idx(long a, long b) {\n"
   "  return a + b * 4;\n"
   "}\n",
   {100, 10}, "MemStr"},

  {"lea_base_idx_disp",
   "long lea_base_idx_disp(long a, long b) {\n"
   "  return a + b * 8 + 42;\n"
   "}\n",
   {100, 10}, "MemStr"},

  // ========== Nested conditional ==========
  {"nested_cond",
   "long nested_cond(long a, long b, long c) {\n"
   "  if (a > 0) {\n"
   "    if (b > 0) return a + b;\n"
   "    else return a - b;\n"
   "  } else {\n"
   "    if (c > 0) return b + c;\n"
   "    else return 0;\n"
   "  }\n"
   "}\n",
   {10, (uint64_t)(int64_t)-5, 20}, "MemStr"},

  // ========== Ternary chain ==========
  {"ternary_chain",
   "long ternary_chain(long a) {\n"
   "  return a > 100 ? 3 : a > 50 ? 2 : a > 0 ? 1 : 0;\n"
   "}\n",
   {75}, "MemStr"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(MemStr, X64MemStrRT,
                         ::testing::ValuesIn(kX64MemStr), rtTCName);
