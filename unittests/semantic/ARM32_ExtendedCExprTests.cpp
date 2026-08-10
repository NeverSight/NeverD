//===- ARM32_ExtendedCExprTests.cpp - Extended ARM32 tests ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// ARM32 additional roundtrip: 8/16-bit ops, sign extension, conditional,
// bitfield, complex arithmetic patterns.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32ExtRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ExtRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32Ext = {
  // --- 8-bit ops ---
  {"arm_add8", "int arm_add8(int a, int b) { return (unsigned char)((unsigned char)a + (unsigned char)b); }\n", {200, 100}, "ARM32Ext"},
  {"arm_sub8", "int arm_sub8(int a, int b) { return (unsigned char)((unsigned char)a - (unsigned char)b); }\n", {200, 50}, "ARM32Ext"},

  // --- 16-bit ops ---
  {"arm_add16", "int arm_add16(int a, int b) { return (unsigned short)((unsigned short)a + (unsigned short)b); }\n", {0xFFF0, 0x20}, "ARM32Ext"},
  {"arm_mul16", "int arm_mul16(int a, int b) { return (short)((short)a * (short)b); }\n", {100, 200}, "ARM32Ext"},

  // --- Sign/zero extension ---
  {"arm_sext8",  "int arm_sext8(int a) { return (signed char)a; }\n", {0x80}, "ARM32Ext"},
  {"arm_sext16", "int arm_sext16(int a) { return (short)a; }\n", {0x8000}, "ARM32Ext"},
  {"arm_zext8",  "int arm_zext8(int a) { return (unsigned char)a; }\n", {0xFF}, "ARM32Ext"},
  {"arm_zext16", "int arm_zext16(int a) { return (unsigned short)a; }\n", {0xFFFF}, "ARM32Ext"},

  // --- Conditional select ---
  {"arm_csel", "int arm_csel(int c, int a, int b) { return c ? a : b; }\n", {1, 42, 99}, "ARM32Ext"},
  {"arm_csinv", "int arm_csinv(int a) { return a >= 0 ? a : ~a; }\n", {(uint64_t)(uint32_t)-5}, "ARM32Ext"},
  {"arm_abs2", "int arm_abs2(int a) { return a >= 0 ? a : -a; }\n", {(uint64_t)(uint32_t)-42}, "ARM32Ext"},

  // --- Multiply-accumulate (mla) ---
  {"arm_mla", "int arm_mla(int a, int b, int c) { return a * b + c; }\n", {3, 7, 5}, "ARM32Ext"},
  {"arm_mls", "int arm_mls(int a, int b, int c) { return c - a * b; }\n", {3, 7, 100}, "ARM32Ext"},

  // --- Bitfield operations ---
  {"arm_ubfx",
   "int arm_ubfx(int val, int off, int w) {\n"
   "  return ((unsigned int)val >> (unsigned int)off) & ((1U << (unsigned int)w) - 1);\n"
   "}\n",
   {0xDEADBEEF, 16, 8}, "ARM32Ext"},

  // --- Divide and modulo ---
  {"arm_divmod",
   "int arm_divmod(int a, int b) {\n"
   "  return (a / b) * 1000 + (a % b);\n"
   "}\n",
   {12345, 67}, "ARM32Ext"},

  // --- Popcount (Brian Kernighan) ---
  {"arm_popcount_bk",
   "int arm_popcount_bk(int x) {\n"
   "  unsigned int u = (unsigned int)x;\n"
   "  int count = 0;\n"
   "  while (u) { u &= u - 1; ++count; }\n"
   "  return count;\n"
   "}\n",
   {0xDEADBEEF}, "ARM32Ext"},

  // --- Dense switch ---
  {"arm_switch",
   "int arm_switch(int a) {\n"
   "  int x = a & 7;\n"
   "  if (x == 0) return 100;\n"
   "  if (x == 1) return 200;\n"
   "  if (x == 2) return 300;\n"
   "  if (x == 3) return 400;\n"
   "  return 999;\n"
   "}\n",
   {3}, "ARM32Ext"},

  // --- Nested ternary ---
  {"arm_classify",
   "int arm_classify2(int a) {\n"
   "  return a < -10 ? -2 : a < 0 ? -1 : a == 0 ? 0 : a < 10 ? 1 : 2;\n"
   "}\n",
   {5}, "ARM32Ext"},

  // --- Power ---
  {"arm_ipow2",
   "int arm_ipow2(int base, int exp) {\n"
   "  int r = 1;\n"
   "  while (exp > 0) {\n"
   "    if (exp & 1) r *= base;\n"
   "    base *= base;\n"
   "    exp >>= 1;\n"
   "  }\n"
   "  return r;\n"
   "}\n",
   {3, 10}, "ARM32Ext"},

  // --- Sort 3 values ---
  {"arm_sort3",
   "int arm_sort3(int a, int b, int c) {\n"
   "  if (a > b) { int t=a; a=b; b=t; }\n"
   "  if (b > c) { int t=b; b=c; c=t; }\n"
   "  if (a > b) { int t=a; a=b; b=t; }\n"
   "  return a*100 + b*10 + c;\n"
   "}\n",
   {7, 2, 5}, "ARM32Ext"},

  // --- Count trailing zeros (manual loop, avoids large constant .rodata) ---
  {"arm_ctz_manual",
   "int arm_ctz_manual(int a) {\n"
   "  if (a == 0) return 32;\n"
   "  unsigned int v = (unsigned int)a;\n"
   "  int n = 0;\n"
   "  while ((v & 1) == 0) { v >>= 1; ++n; }\n"
   "  return n;\n"
   "}\n",
   {0x100}, "ARM32Ext"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ARM32Ext, ARM32ExtRT,
                         ::testing::ValuesIn(kARM32Ext), rtTCName);
