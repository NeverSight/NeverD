//===- AArch64_MiscPatternRTTests.cpp - Misc AArch64 RT -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests AArch64 miscellaneous patterns: RBIT, REV, CLZ, CLS, EXTR, BFI/BFXIL,
// and various conditional patterns.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64MiscRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64MiscRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64Misc = {
  // ========== RBIT (reverse bits) ==========
  {"a64_rbit",
   "long a64_rbit(long a) {\n"
   "  unsigned long x = (unsigned long)a, r = 0;\n"
   "  for (int i = 0; i < 64; ++i) {\n"
   "    r |= ((x >> i) & 1) << (63 - i);\n"
   "  }\n"
   "  return (long)r;\n"
   "}\n",
   {0x0102030405060708ULL}, "A64Misc"},

  // ========== REV (reverse bytes) ==========
  {"a64_rev64",
   "long a64_rev64(long a) {\n"
   "  return (long)__builtin_bswap64((unsigned long)a);\n"
   "}\n",
   {0x0102030405060708ULL}, "A64Misc"},

  {"a64_rev32",
   "long a64_rev32(long a) {\n"
   "  return (long)__builtin_bswap32((unsigned)(unsigned long)a);\n"
   "}\n",
   {0x01020304}, "A64Misc"},

  // ========== CLZ (count leading zeros) ==========
  {"a64_clz64",
   "long a64_clz64(long a) {\n"
   "  if (a == 0) return 64;\n"
   "  int n = 0;\n"
   "  unsigned long x = (unsigned long)a;\n"
   "  while (!(x >> 63)) { x <<= 1; n++; }\n"
   "  return n;\n"
   "}\n",
   {0x100}, "A64Misc"},

  // ========== UMULH (unsigned multiply high) ==========
  {"a64_umulh",
   "long a64_umulh(long a, long b) {\n"
   "  unsigned __int128 r = (unsigned __int128)(unsigned long)a * (unsigned long)b;\n"
   "  return (long)(r >> 64);\n"
   "}\n",
   {0xFFFFFFFFFFFFFFFFULL, 2}, "A64Misc", /*OptLevel=*/1},

  // ========== SMULH (signed multiply high) ==========
  {"a64_smulh",
   "long a64_smulh(long a, long b) {\n"
   "  __int128 r = (__int128)a * b;\n"
   "  return (long)(unsigned long)(r >> 64);\n"
   "}\n",
   {(uint64_t)(int64_t)-100, 200}, "A64Misc", 1},

  // ========== EXTR (extract from pair) ==========
  {"a64_extr_16",
   "long a64_extr_16(long a, long b) {\n"
   "  return (a << 48) | ((unsigned long)b >> 16);\n"
   "}\n",
   {0xDEAD, 0xBEEFCAFE00000000ULL}, "A64Misc"},

  // ========== BFI pattern (bit field insert) ==========
  {"a64_bfi",
   "long a64_bfi(long a, long b) {\n"
   "  unsigned long mask = 0xFF00UL;\n"
   "  return (a & ~mask) | ((b << 8) & mask);\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 0x42}, "A64Misc"},

  // ========== UBFX pattern (unsigned bit field extract) ==========
  {"a64_ubfx",
   "long a64_ubfx(long a) {\n"
   "  return (a >> 8) & 0xFF;\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL}, "A64Misc"},

  {"a64_sbfx",
   "long a64_sbfx(long a) {\n"
   "  return ((long)(a << 48)) >> 56;\n"
   "}\n",
   {0x0080}, "A64Misc"},

  // ========== Conditional negate ==========
  {"a64_cneg",
   "long a64_cneg(long a, long b) {\n"
   "  return a > 0 ? b : -b;\n"
   "}\n",
   {1, 42}, "A64Misc"},

  {"a64_cneg_neg",
   "long a64_cneg_neg(long a, long b) {\n"
   "  return a > 0 ? b : -b;\n"
   "}\n",
   {(uint64_t)(int64_t)-1, 42}, "A64Misc"},

  // ========== Ternary select ==========
  {"a64_select3",
   "long a64_select3(long a, long b, long c) {\n"
   "  if (a > 0) return b;\n"
   "  if (a < 0) return c;\n"
   "  return 0;\n"
   "}\n",
   {42, 100, 200}, "A64Misc"},

  // ========== Unsigned min/max ==========
  {"a64_umin",
   "long a64_umin(long a, long b) {\n"
   "  return (unsigned long)a < (unsigned long)b ? a : b;\n"
   "}\n",
   {0xFFFFFFFFFFFFFF00ULL, 0x100}, "A64Misc"},

  {"a64_umax",
   "long a64_umax(long a, long b) {\n"
   "  return (unsigned long)a > (unsigned long)b ? a : b;\n"
   "}\n",
   {0xFFFFFFFFFFFFFF00ULL, 0x100}, "A64Misc"},

  // ========== Multiple return value (divmod) ==========
  {"a64_divmod32",
   "long a64_divmod32(long a, long b) {\n"
   "  int q = (int)a / (int)b;\n"
   "  int r = (int)a % (int)b;\n"
   "  return ((long)(unsigned)q << 32) | (unsigned)r;\n"
   "}\n",
   {123, 10}, "A64Misc"},

  // ========== Widening multiply ==========
  {"a64_umull",
   "long a64_umull(long a, long b) {\n"
   "  return (long)((unsigned long)(unsigned)a * (unsigned)b);\n"
   "}\n",
   {0xFFFFFFFF, 2}, "A64Misc"},

  {"a64_smull",
   "long a64_smull(long a, long b) {\n"
   "  return (long)((long)(int)a * (int)b);\n"
   "}\n",
   {(uint64_t)(int32_t)-100, 200}, "A64Misc"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(A64Misc, A64MiscRT,
                         ::testing::ValuesIn(kA64Misc), rtTCName);
