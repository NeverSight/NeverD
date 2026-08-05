//===- X64_BMIExtTests.cpp - BMI/extension instruction tests ---*- C++ -*-===//
//
// Tests x86_64 BMI, BMI2, LZCNT, TZCNT, POPCNT, and other extension
// instructions through C builtins and bit manipulation patterns.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BMIRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BMIRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64BMI = {
  // --- LZCNT/BSR patterns ---
  {"bmi_lzcnt",
   "long bmi_lzcnt(long a) {\n"
   "  if (a == 0) return 64;\n"
   "  return __builtin_clzll((unsigned long)a);\n"
   "}\n",
   {0x100}, "BMIRT"},

  {"bmi_lzcnt32",
   "long bmi_lzcnt32(long a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  if (x == 0) return 32;\n"
   "  return __builtin_clz(x);\n"
   "}\n",
   {0x100}, "BMIRT"},

  // --- TZCNT/BSF patterns ---
  {"bmi_tzcnt",
   "long bmi_tzcnt(long a) {\n"
   "  if (a == 0) return 64;\n"
   "  return __builtin_ctzll((unsigned long)a);\n"
   "}\n",
   {0x100}, "BMIRT"},

  {"bmi_tzcnt32",
   "long bmi_tzcnt32(long a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  if (x == 0) return 32;\n"
   "  return __builtin_ctz(x);\n"
   "}\n",
   {0x100}, "BMIRT"},

  // --- BLSI (isolate lowest set bit) ---
  {"bmi_blsi",
   "long bmi_blsi(long a) {\n"
   "  return a & (-a);\n"
   "}\n",
   {0xABCD0000ULL}, "BMIRT"},

  // --- BLSR (reset lowest set bit) ---
  {"bmi_blsr",
   "long bmi_blsr(long a) {\n"
   "  return a & (a - 1);\n"
   "}\n",
   {0xABCD0000ULL}, "BMIRT"},

  // --- BLSMSK (get mask up to lowest set bit) ---
  {"bmi_blsmsk",
   "long bmi_blsmsk(long a) {\n"
   "  return a ^ (a - 1);\n"
   "}\n",
   {0xABCD0000ULL}, "BMIRT"},

  // --- ANDN (a & ~b) ---
  {"bmi_andn",
   "long bmi_andn(long a, long b) {\n"
   "  return a & ~b;\n"
   "}\n",
   {0xFF00FF00ULL, 0x0F0F0F0FULL}, "BMIRT"},

  // --- BSWAP ---
  {"bmi_bswap32",
   "long bmi_bswap32(long a) {\n"
   "  return __builtin_bswap32((unsigned int)a);\n"
   "}\n",
   {0x01020304}, "BMIRT"},

  {"bmi_bswap64",
   "long bmi_bswap64(long a) {\n"
   "  return __builtin_bswap64((unsigned long)a);\n"
   "}\n",
   {0x0102030405060708ULL}, "BMIRT"},

  // --- BT/BTS/BTR/BTC C patterns ---
  {"bmi_bt",
   "long bmi_bt(long a, long b) {\n"
   "  return ((unsigned long)a >> (b & 63)) & 1;\n"
   "}\n",
   {0xFF00FF00ULL, 24}, "BMIRT"},

  {"bmi_bts",
   "long bmi_bts(long a, long b) {\n"
   "  return a | (1UL << (b & 63));\n"
   "}\n",
   {0, 42}, "BMIRT"},

  {"bmi_btr",
   "long bmi_btr(long a, long b) {\n"
   "  return a & ~(1UL << (b & 63));\n"
   "}\n",
   {0xFFFFFFFFFFFFFFFFULL, 42}, "BMIRT"},

  {"bmi_btc",
   "long bmi_btc(long a, long b) {\n"
   "  return a ^ (1UL << (b & 63));\n"
   "}\n",
   {0xFF00FF00ULL, 24}, "BMIRT"},

  // --- Multi-bit extract ---
  {"bmi_extract_field",
   "long bmi_extract_field(long val, long start, long len) {\n"
   "  unsigned long mask = len >= 64 ? ~0UL : (1UL << (int)len) - 1;\n"
   "  return (long)(((unsigned long)val >> (int)(start & 63)) & mask);\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 16, 16}, "BMIRT"},

  // --- Deposit field ---
  {"bmi_deposit_field",
   "long bmi_deposit_field(long val, long field, long start, long len) {\n"
   "  unsigned long mask = ((1UL << (int)len) - 1) << (int)(start & 63);\n"
   "  return (long)(((unsigned long)val & ~mask) |\n"
   "    (((unsigned long)field << (int)(start & 63)) & mask));\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 0x1234, 16, 16}, "BMIRT"},

  // --- ROL/ROR via double-shift ---
  {"bmi_rol32",
   "long bmi_rol32(long a, long n) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  unsigned int s = (unsigned int)n & 31;\n"
   "  return (x << s) | (x >> (32 - s));\n"
   "}\n",
   {0xDEADBEEF, 8}, "BMIRT"},

  {"bmi_ror64",
   "long bmi_ror64(long a, long n) {\n"
   "  unsigned long x = (unsigned long)a;\n"
   "  unsigned int s = (unsigned int)n & 63;\n"
   "  return (long)((x >> s) | (x << (64 - s)));\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 20}, "BMIRT"},

  // --- Parallel bit operations ---
  {"bmi_interleave16",
   "long bmi_interleave16(long a, long b) {\n"
   "  unsigned int x = (unsigned int)a & 0xFFFF;\n"
   "  unsigned int y = (unsigned int)b & 0xFFFF;\n"
   "  unsigned int result = 0;\n"
   "  for (int i = 0; i < 16; ++i) {\n"
   "    result |= ((x >> i) & 1) << (2*i);\n"
   "    result |= ((y >> i) & 1) << (2*i+1);\n"
   "  }\n"
   "  return result;\n"
   "}\n",
   {0x5555, 0xAAAA}, "BMIRT"},

  // --- Byte reversal within 32-bit word ---
  {"bmi_rev_bytes32",
   "long bmi_rev_bytes32(long a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) |\n"
   "         ((x << 8) & 0xFF0000) | ((x << 24) & 0xFF000000U);\n"
   "}\n",
   {0x01020304}, "BMIRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(BMIRT, X64BMIRT,
                         ::testing::ValuesIn(kX64BMI), rtTCName);
