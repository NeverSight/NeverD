//===- X64_AdvancedRoundTripTests.cpp - Advanced pattern tests ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests advanced x86_64 patterns: memory ops, struct access, BT/BTS,
// string ops, and more complex ALU patterns through lift pipeline.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64AdvancedRoundTrip : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64AdvancedRoundTrip, AdvancedVerify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64Advanced = {

  // --- BT/BTS/BTR/BTC via C bit-test patterns ---
  {"bt_test",
   "long bt_test(long a, long b) {\n"
   "  return (a >> b) & 1;\n"
   "}\n",
   {0xFF00FF00FF00FF00ULL, 8}, "AdvRT"},

  {"bts_set",
   "long bts_set(long a, long b) {\n"
   "  return a | (1L << b);\n"
   "}\n",
   {0xFF00FF00, 4}, "AdvRT"},

  {"btr_clear",
   "long btr_clear(long a, long b) {\n"
   "  return a & ~(1L << b);\n"
   "}\n",
   {0xFF, 3}, "AdvRT"},

  {"btc_toggle",
   "long btc_toggle(long a, long b) {\n"
   "  return a ^ (1L << b);\n"
   "}\n",
   {0xFF, 4}, "AdvRT"},

  // --- Memory patterns (stack arrays, struct-like access) ---
  {"mem_bubble_sort",
   "long mem_bubble_sort(long a, long b, long c) {\n"
   "  long arr[3];\n"
   "  arr[0] = a; arr[1] = b; arr[2] = c;\n"
   "  for (int i = 0; i < 2; ++i)\n"
   "    for (int j = 0; j < 2 - i; ++j)\n"
   "      if (arr[j] > arr[j+1]) {\n"
   "        long t = arr[j]; arr[j] = arr[j+1]; arr[j+1] = t;\n"
   "      }\n"
   "  return arr[0] * 10000 + arr[1] * 100 + arr[2];\n"
   "}\n",
   {30, 10, 20}, "AdvRT"},

  {"mem_linear_search",
   "long mem_linear_search(long target) {\n"
   "  long arr[5];\n"
   "  arr[0] = 10; arr[1] = 20; arr[2] = 30; arr[3] = 40; arr[4] = 50;\n"
   "  for (int i = 0; i < 5; ++i)\n"
   "    if (arr[i] == target) return i;\n"
   "  return -1;\n"
   "}\n",
   {30}, "AdvRT"},

  {"mem_matrix_2x2_det",
   "long mem_matrix_2x2_det(long a, long b, long c, long d) {\n"
   "  return a * d - b * c;\n"
   "}\n",
   {3, 4, 2, 5}, "AdvRT"},

  // --- XCHG pattern ---
  {"swap_values",
   "long swap_values(long a, long b) {\n"
   "  long tmp = a; a = b; b = tmp;\n"
   "  return a * 1000 + b;\n"
   "}\n",
   {42, 99}, "AdvRT"},

  // --- LEA-heavy patterns ---
  {"lea_3arg",
   "long lea_3arg(long a, long b) {\n"
   "  return a + b * 4 + 16;\n"
   "}\n",
   {10, 5}, "AdvRT"},

  {"lea_scale8",
   "long lea_scale8(long a, long b) {\n"
   "  return a + b * 8;\n"
   "}\n",
   {100, 7}, "AdvRT"},

  // --- Multi-branch patterns (exercises JCC variations) ---
  {"multi_branch",
   "long multi_branch(long a) {\n"
   "  if (a < 0) return a * -2;\n"
   "  if (a < 10) return a + 100;\n"
   "  if (a < 100) return a - 50;\n"
   "  if (a < 1000) return a / 10;\n"
   "  return 0;\n"
   "}\n",
   {42}, "AdvRT"},

  // --- Signed/unsigned comparison mix ---
  {"signed_unsigned_mix",
   "typedef unsigned long ulong;\n"
   "long signed_unsigned_mix(long s, ulong u) {\n"
   "  long r = 0;\n"
   "  if (s < 0) r += 1;\n"
   "  if (u > 0x8000000000000000ULL) r += 2;\n"
   "  if ((ulong)s > u) r += 4;\n"
   "  return r;\n"
   "}\n",
   {(uint64_t)-5, 0x9000000000000000ULL}, "AdvRT"},

  // --- Recursive-like pattern (iterative with stack array) ---
  {"tower_of_hanoi_count",
   "long tower_of_hanoi_count(long n) {\n"
   "  return (1L << n) - 1;\n"
   "}\n",
   {10}, "AdvRT"},

  // --- Bit field extraction ---
  {"extract_byte",
   "long extract_byte(long val, long idx) {\n"
   "  return (val >> (idx * 8)) & 0xFF;\n"
   "}\n",
   {0x0102030405060708ULL, 3}, "AdvRT"},

  {"insert_byte",
   "long insert_byte(long val, long byte, long idx) {\n"
   "  long mask = ~(0xFFUL << (idx * 8));\n"
   "  return (val & mask) | ((byte & 0xFF) << (idx * 8));\n"
   "}\n",
   {0x0102030405060708ULL, 0xAA, 3}, "AdvRT"},

  // --- CMOV-heavy patterns ---
  {"cmov_abs_diff",
   "long cmov_abs_diff(long a, long b) {\n"
   "  long d = a - b;\n"
   "  return d < 0 ? -d : d;\n"
   "}\n",
   {3, 10}, "AdvRT"},

  {"cmov_median3",
   "long cmov_median3(long a, long b, long c) {\n"
   "  if (a > b) { long t = a; a = b; b = t; }\n"
   "  if (b > c) { long t = b; b = c; c = t; }\n"
   "  if (a > b) { long t = a; a = b; b = t; }\n"
   "  return b;\n"
   "}\n",
   {30, 10, 20}, "AdvRT"},

  // --- Unsigned overflow check ---
  {"overflow_check_add",
   "typedef unsigned long ulong;\n"
   "long overflow_check_add(ulong a, ulong b) {\n"
   "  ulong sum = a + b;\n"
   "  return sum < a ? 1 : 0;\n"
   "}\n",
   {0xFFFFFFFFFFFFFFFFULL, 1}, "AdvRT"},

  // --- Rotate via expression ---
  {"rotate_left",
   "typedef unsigned long ulong;\n"
   "long rotate_left(ulong a, long n) {\n"
   "  n &= 63;\n"
   "  return (a << n) | (a >> (64 - n));\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 16}, "AdvRT"},

  {"rotate_right",
   "typedef unsigned long ulong;\n"
   "long rotate_right(ulong a, long n) {\n"
   "  n &= 63;\n"
   "  return (a >> n) | (a << (64 - n));\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 16}, "AdvRT"},

  // --- Hash function patterns (exercises many ops) ---
  {"djb2_hash",
   "typedef unsigned long ulong;\n"
   "long djb2_hash(long seed, long key) {\n"
   "  ulong h = (ulong)seed;\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    unsigned char c = (unsigned char)(key >> (i * 8));\n"
   "    h = h * 33 + c;\n"
   "  }\n"
   "  return (long)h;\n"
   "}\n",
   {5381, 0x48656C6C6F000000ULL}, "AdvRT"},

  // --- Widening multiply ---
  {"widening_mul_lo",
   "typedef unsigned long ulong;\n"
   "long widening_mul_lo(ulong a, ulong b) {\n"
   "  return (long)(a * b);\n"
   "}\n",
   {0x100000007ULL, 0x200000003ULL}, "AdvRT"},

  // --- Conditional increment chain ---
  {"cond_count_bits",
   "long cond_count_bits(long a) {\n"
   "  long c = 0;\n"
   "  if (a & 1) c++;\n"
   "  if (a & 2) c++;\n"
   "  if (a & 4) c++;\n"
   "  if (a & 8) c++;\n"
   "  if (a & 16) c++;\n"
   "  if (a & 32) c++;\n"
   "  if (a & 64) c++;\n"
   "  if (a & 128) c++;\n"
   "  return c;\n"
   "}\n",
   {0xAB}, "AdvRT"},

  // --- Power of 2 operations ---
  {"next_pow2",
   "typedef unsigned long ulong;\n"
   "long next_pow2(ulong v) {\n"
   "  v--;\n"
   "  v |= v >> 1;\n"
   "  v |= v >> 2;\n"
   "  v |= v >> 4;\n"
   "  v |= v >> 8;\n"
   "  v |= v >> 16;\n"
   "  v |= v >> 32;\n"
   "  v++;\n"
   "  return (long)v;\n"
   "}\n",
   {100}, "AdvRT"},

  // --- Mixed-width struct-like access ---
  {"packed_fields",
   "long packed_fields(long packed) {\n"
   "  int lo = (int)(packed & 0xFFFFFFFF);\n"
   "  int hi = (int)((unsigned long)packed >> 32);\n"
   "  return (long)(lo + hi);\n"
   "}\n",
   {0x0000000A00000014ULL}, "AdvRT"},

  // --- Saturating add (no intrinsic) ---
  {"sat_add_u64",
   "typedef unsigned long ulong;\n"
   "long sat_add_u64(ulong a, ulong b) {\n"
   "  ulong s = a + b;\n"
   "  return (long)(s < a ? (ulong)-1 : s);\n"
   "}\n",
   {0xFFFFFFFFFFFFFF00ULL, 0x200}, "AdvRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(AdvRT, X64AdvancedRoundTrip,
                         ::testing::ValuesIn(kX64Advanced), rtTCName);
