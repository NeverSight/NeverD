//===- KnownBugTests.cpp - Tests for known bugs to be fixed -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// These tests exercise the 5 known bugs listed in the Unicorn unsupported-instructions doc.
// Each test is expected to FAIL until the corresponding bug is fixed.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class KnownBugRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(KnownBugRT, Verify) {
  auto &TC = GetParam();
  if (TC.Category == "KnownBug_x64")
    roundTripX64(TC);
  else if (TC.Category == "KnownBug_a64")
    roundTripAArch64(TC);
  else if (TC.Category == "KnownBug_arm32")
    roundTripARM32(TC);
}

// clang-format off
static const std::vector<RoundTripTC> kKnownBugs = {

  // ========== Bug 1: x86 nested if-else (classify) ==========
  {"nested_if_else_4level",
   "long classify(long x) {\n"
   "  if (x < 0) return -1;\n"
   "  else if (x < 10) return 0;\n"
   "  else if (x < 100) return 1;\n"
   "  else if (x < 1000) return 2;\n"
   "  else return 3;\n"
   "}\n",
   {50}, "KnownBug_x64"},

  {"nested_if_else_5level",
   "long classify5(long x) {\n"
   "  if (x < 0) return -10;\n"
   "  else if (x < 5) return 1;\n"
   "  else if (x < 20) return 2;\n"
   "  else if (x < 50) return 3;\n"
   "  else if (x < 100) return 4;\n"
   "  else return 5;\n"
   "}\n",
   {75}, "KnownBug_x64"},

  {"nested_if_else_neg",
   "long classify_neg(long x) {\n"
   "  if (x < 0) return -1;\n"
   "  else if (x < 10) return 0;\n"
   "  else if (x < 100) return 1;\n"
   "  else return 2;\n"
   "}\n",
   {(uint64_t)(int64_t)-5}, "KnownBug_x64"},

  // ========== Bug 2: x86 clamp_range (3 parameter CMOV) ==========
  {"clamp_range",
   "long clamp_range(long val, long lo, long hi) {\n"
   "  if (val < lo) return lo;\n"
   "  if (val > hi) return hi;\n"
   "  return val;\n"
   "}\n",
   {150, 0, 100}, "KnownBug_x64"},

  {"clamp_range_neg",
   "long clamp_neg(long val, long lo, long hi) {\n"
   "  if (val < lo) return lo;\n"
   "  if (val > hi) return hi;\n"
   "  return val;\n"
   "}\n",
   {(uint64_t)(int64_t)-50, (uint64_t)(int64_t)-100, 100}, "KnownBug_x64"},

  {"clamp_unsigned",
   "typedef unsigned long ulong;\n"
   "long clamp_u(long val, long lo, long hi) {\n"
   "  ulong v = (ulong)val, l = (ulong)lo, h = (ulong)hi;\n"
   "  if (v < l) return (long)l;\n"
   "  if (v > h) return (long)h;\n"
   "  return (long)v;\n"
   "}\n",
   {150, 0, 100}, "KnownBug_x64"},

  // ========== Bug 3: x86 complex loops (collatz/ipow/crc) ==========
  {"collatz",
   "long collatz(long n) {\n"
   "  long steps = 0;\n"
   "  while (n > 1) {\n"
   "    if (n & 1) n = 3 * n + 1;\n"
   "    else n = n / 2;\n"
   "    ++steps;\n"
   "  }\n"
   "  return steps;\n"
   "}\n",
   {27}, "KnownBug_x64"},

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
   {3, 10}, "KnownBug_x64"},

  {"crc8",
   "long crc8(long data) {\n"
   "  unsigned char crc = (unsigned char)data;\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    if (crc & 0x80)\n"
   "      crc = (crc << 1) ^ 0x07;\n"
   "    else\n"
   "      crc <<= 1;\n"
   "  }\n"
   "  return (long)crc;\n"
   "}\n",
   {0xA5}, "KnownBug_x64"},

  // ========== Bug 4: AArch64 8H/16B multi-lane ==========
  {"a64_add_8h",
   "typedef short v8h __attribute__((vector_size(16)));\n"
   "long a64_add_8h(long a, long b) {\n"
   "  v8h va = {(short)a, (short)(a>>16), (short)(a>>32), (short)(a>>48),\n"
   "            (short)a, (short)(a>>16), (short)(a>>32), (short)(a>>48)};\n"
   "  v8h vb = {(short)b, (short)(b>>16), (short)(b>>32), (short)(b>>48),\n"
   "            (short)b, (short)(b>>16), (short)(b>>32), (short)(b>>48)};\n"
   "  v8h vc = va + vb;\n"
   "  return (long)vc[0] + (long)vc[4];\n"
   "}\n",
   {0x0001000200030004ULL, 0x0005000600070008ULL}, "KnownBug_a64", 1},

  {"a64_add_16b",
   "typedef char v16b __attribute__((vector_size(16)));\n"
   "long a64_add_16b(long a, long b) {\n"
   "  v16b va, vb;\n"
   "  __builtin_memcpy(&va, &a, 8);\n"
   "  __builtin_memcpy((char*)&va+8, &a, 8);\n"
   "  __builtin_memcpy(&vb, &b, 8);\n"
   "  __builtin_memcpy((char*)&vb+8, &b, 8);\n"
   "  v16b vc = va + vb;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 16; ++i) sum += vc[i];\n"
   "  return sum;\n"
   "}\n",
   {0x0102030405060708ULL, 0x0102030405060708ULL}, "KnownBug_a64", 1},

  // ========== Bug 5: ARM32 NEON vdupq intrinsic ==========
  {"arm32_vdup_q",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm32_vdup_q(int val) {\n"
   "  v4i v = {val, val, val, val};\n"
   "  return v[0] + v[1] + v[2] + v[3];\n"
   "}\n",
   {42}, "KnownBug_arm32", 1, "-mfpu=neon"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(KnownBug, KnownBugRT,
                         ::testing::ValuesIn(kKnownBugs), rtTCName);
