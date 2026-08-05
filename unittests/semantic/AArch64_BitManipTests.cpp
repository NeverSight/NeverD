//===- AArch64_BitManipTests.cpp - AArch64 bit manipulation tests -*- C++ -*-===//
//
// Tests AArch64 CLZ, CTZ, RBIT, REV, EXTR, BFM patterns, and complex
// arithmetic through roundtrip verification.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64BitRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64BitRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64Bit = {
  // --- CLZ ---
  {"a64_clz64",
   "long a64_clz64(long a) {\n"
   "  return a ? __builtin_clzll((unsigned long)a) : 64;\n"
   "}\n",
   {0x100}, "A64Bit"},

  {"a64_clz32",
   "long a64_clz32(long a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  return x ? __builtin_clz(x) : 32;\n"
   "}\n",
   {0x100}, "A64Bit"},

  // --- CTZ ---
  {"a64_ctz64",
   "long a64_ctz64(long a) {\n"
   "  return a ? __builtin_ctzll((unsigned long)a) : 64;\n"
   "}\n",
   {0x100}, "A64Bit"},

  // --- BSWAP ---
  {"a64_bswap16",
   "long a64_bswap16(long a) {\n"
   "  unsigned short x = (unsigned short)a;\n"
   "  return (unsigned short)((x >> 8) | (x << 8));\n"
   "}\n",
   {0x1234}, "A64Bit"},

  // --- Bit reversal ---
  {"a64_rbit32",
   "long a64_rbit32(long a) {\n"
   "  return __builtin_bitreverse32((unsigned int)a);\n"
   "}\n",
   {0x12345678}, "A64Bit"},

  // --- Isolate/reset/mask lowest bit ---
  {"a64_blsi", "long a64_blsi(long a) { return a & (-a); }\n", {0xABCD0000ULL}, "A64Bit"},
  {"a64_blsr", "long a64_blsr(long a) { return a & (a-1); }\n", {0xABCD0000ULL}, "A64Bit"},
  {"a64_blsmsk", "long a64_blsmsk(long a) { return a ^ (a-1); }\n", {0xABCD0000ULL}, "A64Bit"},

  // --- BT/BTS/BTR/BTC ---
  {"a64_bt", "long a64_bt(long a, long b) { return ((unsigned long)a >> (b&63)) & 1; }\n", {0xFF00ULL, 8}, "A64Bit"},
  {"a64_bts", "long a64_bts(long a, long b) { return a | (1UL << (b&63)); }\n", {0, 42}, "A64Bit"},
  {"a64_btr", "long a64_btr(long a, long b) { return a & ~(1UL << (b&63)); }\n", {~0ULL, 42}, "A64Bit"},

  // --- EXTR pattern ---
  {"a64_extr_concat",
   "long a64_extr_concat(long hi, long lo, long shift) {\n"
   "  unsigned long h = (unsigned long)hi;\n"
   "  unsigned long l = (unsigned long)lo;\n"
   "  unsigned int s = (unsigned int)shift & 63;\n"
   "  return (long)((l >> s) | (h << (64 - s)));\n"
   "}\n",
   {0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL, 16}, "A64Bit"},

  // --- Byte extract/pack ---
  {"a64_pack4",
   "long a64_pack4(long a, long b, long c, long d) {\n"
   "  return (a & 0xFF) | ((b & 0xFF) << 8) | ((c & 0xFF) << 16) | ((d & 0xFF) << 24);\n"
   "}\n",
   {0x41, 0x42, 0x43, 0x44}, "A64Bit"},

  {"a64_unpack_b2",
   "long a64_unpack_b2(long a) { return ((unsigned long)a >> 16) & 0xFF; }\n",
   {0x44434241ULL}, "A64Bit"},

  // --- Zigzag encode/decode ---
  {"a64_zigzag_enc", "long a64_zigzag_enc(long n) { return (n << 1) ^ (n >> 63); }\n", {(uint64_t)-100}, "A64Bit"},
  {"a64_zigzag_dec",
   "long a64_zigzag_dec(long n) {\n"
   "  unsigned long u = (unsigned long)n;\n"
   "  return (long)((u >> 1) ^ -(u & 1));\n"
   "}\n",
   {199}, "A64Bit"},

  // --- Popcount (Kernighan) ---
  {"a64_popcount_bk",
   "long a64_popcount_bk(long x) {\n"
   "  unsigned long u = (unsigned long)x;\n"
   "  long c = 0;\n"
   "  while (u) { u &= u-1; ++c; }\n"
   "  return c;\n"
   "}\n",
   {0xDEADBEEFULL}, "A64Bit"},

  // --- Hamming distance ---
  {"a64_hamming",
   "long a64_hamming(long a, long b) {\n"
   "  unsigned long x = (unsigned long)(a ^ b);\n"
   "  long d = 0;\n"
   "  while (x) { d += x & 1; x >>= 1; }\n"
   "  return d;\n"
   "}\n",
   {0xFF00FF00ULL, 0x00FF00FFULL}, "A64Bit"},

  // --- Endian swap 32 via shift+mask ---
  {"a64_endian32",
   "long a64_endian32(long a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  x = ((x >> 8) & 0x00FF00FFU) | ((x << 8) & 0xFF00FF00U);\n"
   "  x = (x >> 16) | (x << 16);\n"
   "  return x;\n"
   "}\n",
   {0x01020304}, "A64Bit"},

  // --- Power of 2 check ---
  {"a64_is_pow2", "long a64_is_pow2(long a) { return a > 0 && (a & (a-1)) == 0; }\n", {256}, "A64Bit"},
  {"a64_next_pow2",
   "long a64_next_pow2(long a) {\n"
   "  unsigned long v = (unsigned long)(a - 1);\n"
   "  v |= v >> 1; v |= v >> 2; v |= v >> 4;\n"
   "  v |= v >> 8; v |= v >> 16; v |= v >> 32;\n"
   "  return (long)(v + 1);\n"
   "}\n",
   {100}, "A64Bit"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(A64Bit, AArch64BitRT,
                         ::testing::ValuesIn(kA64Bit), rtTCName);
