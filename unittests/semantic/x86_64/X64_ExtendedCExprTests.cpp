//===- X64_ExtendedCExprTests.cpp - Extended C expression tests --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Additional x86_64 roundtrip tests via C expressions covering:
// - 8/16-bit ops (movzx/movsx/cwde/cbw patterns)
// - LEA multi-form addressing
// - Conditional moves (cmov variants)
// - Unsigned comparison patterns
// - 128-bit multiply
// - Flag-heavy patterns
// - More control flow
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64ExtRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64ExtRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64Ext = {
  // --- 8-bit arithmetic ---
  {"c_add8",
   "long c_add8(long a, long b) {\n"
   "  unsigned char x = (unsigned char)a;\n"
   "  unsigned char y = (unsigned char)b;\n"
   "  return (unsigned char)(x + y);\n"
   "}\n",
   {200, 100}, "ExtRT"},

  {"c_sub8",
   "long c_sub8(long a, long b) {\n"
   "  unsigned char x = (unsigned char)a;\n"
   "  unsigned char y = (unsigned char)b;\n"
   "  return (unsigned char)(x - y);\n"
   "}\n",
   {200, 50}, "ExtRT"},

  {"c_mul8",
   "long c_mul8(long a, long b) {\n"
   "  unsigned char x = (unsigned char)a;\n"
   "  unsigned char y = (unsigned char)b;\n"
   "  return (unsigned char)(x * y);\n"
   "}\n",
   {7, 9}, "ExtRT"},

  // --- 16-bit arithmetic ---
  {"c_add16_overflow",
   "long c_add16_overflow(long a, long b) {\n"
   "  unsigned short x = (unsigned short)a;\n"
   "  unsigned short y = (unsigned short)b;\n"
   "  return (unsigned short)(x + y);\n"
   "}\n",
   {0xFFF0, 0x20}, "ExtRT"},

  {"c_mul16",
   "long c_mul16(long a, long b) {\n"
   "  short x = (short)a;\n"
   "  short y = (short)b;\n"
   "  return (short)(x * y);\n"
   "}\n",
   {100, 200}, "ExtRT"},

  // --- LEA patterns (exercises SIB addressing) ---
  {"c_lea_2",
   "long c_lea_2(long a) {\n"
   "  return a * 2 + 7;\n"
   "}\n",
   {42}, "ExtRT"},

  {"c_lea_3",
   "long c_lea_3(long a) {\n"
   "  return a * 3;\n"
   "}\n",
   {42}, "ExtRT"},

  {"c_lea_5",
   "long c_lea_5(long a) {\n"
   "  return a * 5;\n"
   "}\n",
   {42}, "ExtRT"},

  {"c_lea_9",
   "long c_lea_9(long a) {\n"
   "  return a * 9;\n"
   "}\n",
   {42}, "ExtRT"},

  {"c_lea_add",
   "long c_lea_add(long a, long b) {\n"
   "  return a + b * 4 + 16;\n"
   "}\n",
   {100, 20}, "ExtRT"},

  // --- cmov variants ---
  {"c_cmove",
   "long c_cmove(long a, long b, long c) {\n"
   "  return a == 0 ? b : c;\n"
   "}\n",
   {0, 42, 99}, "ExtRT"},

  {"c_cmovne",
   "long c_cmovne(long a, long b, long c) {\n"
   "  return a != 0 ? b : c;\n"
   "}\n",
   {1, 42, 99}, "ExtRT"},

  {"c_cmovs",
   "long c_cmovs(long a, long b, long c) {\n"
   "  return a < 0 ? b : c;\n"
   "}\n",
   {(uint64_t)-5, 42, 99}, "ExtRT"},

  {"c_cmovg",
   "long c_cmovg(long a, long b, long c) {\n"
   "  return a > 0 ? b : c;\n"
   "}\n",
   {5, 42, 99}, "ExtRT"},

  {"c_cmova",
   "long c_cmova(long a, long b, long c) {\n"
   "  unsigned long ua = (unsigned long)a;\n"
   "  return ua > 100 ? b : c;\n"
   "}\n",
   {200, 42, 99}, "ExtRT"},

  // --- Unsigned comparison ---
  {"c_unsigned_cmp",
   "long c_unsigned_cmp(long a, long b) {\n"
   "  unsigned long ua = (unsigned long)a;\n"
   "  unsigned long ub = (unsigned long)b;\n"
   "  if (ua < ub) return -1;\n"
   "  if (ua > ub) return 1;\n"
   "  return 0;\n"
   "}\n",
   {0xFFFFFFFFFFFFFFFFULL, 42}, "ExtRT"},

  // --- Sign extension chains ---
  {"c_sext_chain",
   "long c_sext_chain(long a) {\n"
   "  signed char b = (signed char)a;\n"
   "  short c = b;\n"
   "  int d = c;\n"
   "  return d;\n"
   "}\n",
   {0x80}, "ExtRT"},

  {"c_zext_chain",
   "long c_zext_chain(long a) {\n"
   "  unsigned char b = (unsigned char)a;\n"
   "  unsigned short c = b;\n"
   "  unsigned int d = c;\n"
   "  return d;\n"
   "}\n",
   {0xFF}, "ExtRT"},

  // --- Widening multiply ---
  {"c_widen_mul_signed",
   "long c_widen_mul_signed(long a, long b) {\n"
   "  int x = (int)a;\n"
   "  int y = (int)b;\n"
   "  return (long)x * y;\n"
   "}\n",
   {0xFFFFFFFF, 0xFFFFFFFF}, "ExtRT"},

  // --- Nested ternary ---
  {"c_nested_ternary",
   "long c_nested_ternary(long a) {\n"
   "  return a < -10 ? -2 : a < 0 ? -1 : a == 0 ? 0 : a < 10 ? 1 : 2;\n"
   "}\n",
   {5}, "ExtRT"},

  // --- Flag patterns ---
  {"c_carry_chain",
   "long c_carry_chain(long a, long b, long c) {\n"
   "  unsigned long r1 = (unsigned long)a + (unsigned long)b;\n"
   "  int carry = r1 < (unsigned long)a;\n"
   "  unsigned long r2 = r1 + (unsigned long)c;\n"
   "  carry += r2 < r1;\n"
   "  return (long)r2 + carry;\n"
   "}\n",
   {0xFFFFFFFFFFFFFFFFULL, 1, 1}, "ExtRT"},

  // --- Switch-like computed goto ---
  {"c_switch_dense",
   "long c_switch_dense(long a) {\n"
   "  int x = (int)(a & 7);\n"
   "  long r = 0;\n"
   "  if (x == 0) r = 100;\n"
   "  else if (x == 1) r = 200;\n"
   "  else if (x == 2) r = 300;\n"
   "  else if (x == 3) r = 400;\n"
   "  else if (x == 4) r = 500;\n"
   "  else r = 999;\n"
   "  return r;\n"
   "}\n",
   {3}, "ExtRT"},

  // --- Bit field extract/insert ---
  {"c_bitfield_extract",
   "long c_bitfield_extract(long val, long offset, long width) {\n"
   "  return ((unsigned long)val >> (unsigned int)offset) & ((1UL << (unsigned int)width) - 1);\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 16, 8}, "ExtRT"},

  {"c_bitfield_insert",
   "long c_bitfield_insert(long val, long field, long offset, long width) {\n"
   "  unsigned long mask = ((1UL << (unsigned int)width) - 1) << (unsigned int)offset;\n"
   "  return (long)(((unsigned long)val & ~mask) | (((unsigned long)field << (unsigned int)offset) & mask));\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 0x42, 16, 8}, "ExtRT"},

  // --- Population count (Brian Kernighan) ---
  {"c_popcount_bk",
   "long c_popcount_bk(long x) {\n"
   "  unsigned long u = (unsigned long)x;\n"
   "  long count = 0;\n"
   "  while (u) { u &= u - 1; ++count; }\n"
   "  return count;\n"
   "}\n",
   {0xDEADBEEFULL}, "ExtRT"},

  // --- Recursive-like pattern (towers of Hanoi count) ---
  {"c_hanoi",
   "long c_hanoi(long n) {\n"
   "  return (1L << (int)n) - 1;\n"
   "}\n",
   {10}, "ExtRT"},

  // --- Mixed 32/64-bit ops ---
  {"c_mix32_64",
   "long c_mix32_64(long a, long b) {\n"
   "  int x = (int)a * (int)b;\n"
   "  unsigned long y = (unsigned long)(unsigned int)x;\n"
   "  return (long)(y * 3 + 7);\n"
   "}\n",
   {12345, 67890}, "ExtRT"},

  // --- De Bruijn sequence ---
  {"c_debruijn_ctz",
   "long c_debruijn_ctz(long a) {\n"
   "  if (a == 0) return 64;\n"
   "  unsigned long v = (unsigned long)a & -(unsigned long)a;\n"
   "  return (long)((v * 0x022fdd63cc95386dULL) >> 58);\n"
   "}\n",
   {0x100}, "ExtRT"},

  // --- Array bubble sort (memory-intensive) ---
  {"c_sort4",
   "long c_sort4(long a, long b, long c, long d) {\n"
   "  long arr[4] = {a, b, c, d};\n"
   "  for (int i = 0; i < 3; ++i)\n"
   "    for (int j = 0; j < 3 - i; ++j)\n"
   "      if (arr[j] > arr[j+1]) {\n"
   "        long t = arr[j]; arr[j] = arr[j+1]; arr[j+1] = t;\n"
   "      }\n"
   "  return arr[0]*1000 + arr[1]*100 + arr[2]*10 + arr[3];\n"
   "}\n",
   {4, 2, 7, 1}, "ExtRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ExtRT, X64ExtRT,
                         ::testing::ValuesIn(kX64Ext), rtTCName);
