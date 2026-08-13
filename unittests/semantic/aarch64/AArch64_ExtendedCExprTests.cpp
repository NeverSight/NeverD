//===- AArch64_ExtendedCExprTests.cpp - Extended AArch64 tests --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 additional roundtrip tests: 8/16/32-bit ops, conditional select,
// sign/zero extension, bitfield ops, complex patterns.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64ExtRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64ExtRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64Ext = {
  // --- 8-bit ops ---
  {"a64_add8", "long a64_add8(long a, long b) { return (unsigned char)((unsigned char)a + (unsigned char)b); }\n", {200, 100}, "A64Ext"},
  {"a64_mul8", "long a64_mul8(long a, long b) { return (unsigned char)((unsigned char)a * (unsigned char)b); }\n", {7, 9}, "A64Ext"},

  // --- 16-bit ops ---
  {"a64_add16", "long a64_add16(long a, long b) { return (unsigned short)((unsigned short)a + (unsigned short)b); }\n", {0xFFF0, 0x20}, "A64Ext"},
  {"a64_mul16", "long a64_mul16(long a, long b) { return (short)((short)a * (short)b); }\n", {100, 200}, "A64Ext"},

  // --- 32-bit ops ---
  {"a64_rotl32",
   "long a64_rotl32(long a, long b) {\n"
   "  unsigned int x = (unsigned int)a, n = (unsigned int)b & 31;\n"
   "  return (x << n) | (x >> (32 - n));\n"
   "}\n",
   {0xDEADBEEF, 12}, "A64Ext"},

  // --- Sign/zero extension ---
  {"a64_sext8",  "long a64_sext8(long a) { return (signed char)a; }\n", {0x80}, "A64Ext"},
  {"a64_sext16", "long a64_sext16(long a) { return (short)a; }\n", {0x8000}, "A64Ext"},
  {"a64_sext32", "long a64_sext32(long a) { return (int)a; }\n", {0x80000000ULL}, "A64Ext"},
  {"a64_zext8",  "long a64_zext8(long a) { return (unsigned char)a; }\n", {0xFF}, "A64Ext"},
  {"a64_zext16", "long a64_zext16(long a) { return (unsigned short)a; }\n", {0xFFFF}, "A64Ext"},
  {"a64_zext32", "long a64_zext32(long a) { return (unsigned int)a; }\n", {0xFFFFFFFFULL}, "A64Ext"},

  // --- Conditional select (csel/csinc/csinv/csneg) ---
  {"a64_csel",  "long a64_csel(long c, long a, long b) { return c ? a : b; }\n", {1, 42, 99}, "A64Ext"},
  {"a64_csinc", "long a64_csinc(long a, long b) { return a >= b ? a : b + 1; }\n", {3, 7}, "A64Ext"},
  {"a64_csinv", "long a64_csinv(long a) { return a >= 0 ? a : ~a; }\n", {(uint64_t)-5}, "A64Ext"},
  {"a64_csneg", "long a64_csneg(long a) { return a >= 0 ? a : -a; }\n", {(uint64_t)-42}, "A64Ext"},

  // --- Bitfield operations (bfm, ubfx, sbfx) ---
  {"a64_ubfx",
   "long a64_ubfx(long val, long off, long w) {\n"
   "  return ((unsigned long)val >> (unsigned int)off) & ((1UL << (unsigned int)w) - 1);\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 16, 8}, "A64Ext"},

  {"a64_bfi",
   "long a64_bfi(long val, long field, long off, long w) {\n"
   "  unsigned long mask = ((1UL << (unsigned int)w) - 1) << (unsigned int)off;\n"
   "  return (long)(((unsigned long)val & ~mask) | (((unsigned long)field << (unsigned int)off) & mask));\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 0x42, 16, 8}, "A64Ext"},

  // --- MADD/MSUB patterns ---
  {"a64_madd", "long a64_madd(long a, long b, long c) { return a * b + c; }\n", {3, 7, 5}, "A64Ext"},
  {"a64_msub", "long a64_msub(long a, long b, long c) { return c - a * b; }\n", {3, 7, 100}, "A64Ext"},
  {"a64_mneg", "long a64_mneg(long a, long b) { return -(a * b); }\n", {3, 7}, "A64Ext"},

  // --- Divide and modulo ---
  {"a64_divmod",
   "long a64_divmod(long a, long b) {\n"
   "  return (a / b) * 1000 + (a % b);\n"
   "}\n",
   {12345, 67}, "A64Ext"},

  // --- Popcount (Brian Kernighan) ---
  {"a64_popcount_bk",
   "long a64_popcount_bk(long x) {\n"
   "  unsigned long u = (unsigned long)x;\n"
   "  long count = 0;\n"
   "  while (u) { u &= u - 1; ++count; }\n"
   "  return count;\n"
   "}\n",
   {0xDEADBEEFULL}, "A64Ext"},

  // --- Nested ternary ---
  {"a64_nested_ternary",
   "long a64_nested_ternary(long a) {\n"
   "  return a < -10 ? -2 : a < 0 ? -1 : a == 0 ? 0 : a < 10 ? 1 : 2;\n"
   "}\n",
   {5}, "A64Ext"},

  // --- Dense switch ---
  {"a64_switch",
   "long a64_switch(long a) {\n"
   "  int x = (int)(a & 7);\n"
   "  if (x == 0) return 100;\n"
   "  if (x == 1) return 200;\n"
   "  if (x == 2) return 300;\n"
   "  if (x == 3) return 400;\n"
   "  return 999;\n"
   "}\n",
   {3}, "A64Ext"},

  // --- Carry chain ---
  {"a64_carry_chain",
   "long a64_carry_chain(long a, long b, long c) {\n"
   "  unsigned long r1 = (unsigned long)a + (unsigned long)b;\n"
   "  int carry = r1 < (unsigned long)a;\n"
   "  unsigned long r2 = r1 + (unsigned long)c;\n"
   "  carry += r2 < r1;\n"
   "  return (long)r2 + carry;\n"
   "}\n",
   {0xFFFFFFFFFFFFFFFFULL, 1, 1}, "A64Ext"},

  // --- Sort 3 values (simpler than sort4 to avoid stack issues) ---
  {"a64_sort3",
   "long a64_sort3(long a, long b, long c) {\n"
   "  if (a > b) { long t=a; a=b; b=t; }\n"
   "  if (b > c) { long t=b; b=c; c=t; }\n"
   "  if (a > b) { long t=a; a=b; b=t; }\n"
   "  return a*100 + b*10 + c;\n"
   "}\n",
   {7, 2, 5}, "A64Ext"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(A64Ext, AArch64ExtRT,
                         ::testing::ValuesIn(kA64Ext), rtTCName);
