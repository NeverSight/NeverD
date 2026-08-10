//===- X64_ShuffleAtomicRTTests.cpp - Shuffle + atomic RT ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests x86_64 shuffle, byte-swap, bit-test, atomic, and misc patterns.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64ShufAtomRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64ShufAtomRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64ShufAtom = {
  // ========== BSWAP (byte swap) ==========
  {"bswap32",
   "long bswap32(long a) {\n"
   "  unsigned x = (unsigned)a;\n"
   "  return (long)__builtin_bswap32(x);\n"
   "}\n",
   {0x01020304}, "ShufAtom"},

  {"bswap64",
   "long bswap64(long a) {\n"
   "  return (long)__builtin_bswap64((unsigned long)a);\n"
   "}\n",
   {0x0102030405060708ULL}, "ShufAtom"},

  // ========== Rotate (ROL/ROR) ==========
  {"rol32",
   "long rol32(long a, long n) {\n"
   "  unsigned x = (unsigned)a;\n"
   "  unsigned r = (x << (n & 31)) | (x >> (32 - (n & 31)));\n"
   "  return (long)r;\n"
   "}\n",
   {0xDEADBEEF, 8}, "ShufAtom"},

  {"ror32",
   "long ror32(long a, long n) {\n"
   "  unsigned x = (unsigned)a;\n"
   "  unsigned r = (x >> (n & 31)) | (x << (32 - (n & 31)));\n"
   "  return (long)r;\n"
   "}\n",
   {0xDEADBEEF, 8}, "ShufAtom"},

  {"rol64",
   "long rol64(long a, long n) {\n"
   "  unsigned long x = (unsigned long)a;\n"
   "  unsigned long r = (x << (n & 63)) | (x >> (64 - (n & 63)));\n"
   "  return (long)r;\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 16}, "ShufAtom"},

  // ========== Bit test (BT) ==========
  {"bt_set",
   "long bt_set(long a, long bit) {\n"
   "  return (a >> (bit & 63)) & 1;\n"
   "}\n",
   {0xFF00, 8}, "ShufAtom"},

  {"bt_clear",
   "long bt_clear(long a, long bit) {\n"
   "  return (a >> (bit & 63)) & 1;\n"
   "}\n",
   {0xFF00, 3}, "ShufAtom"},

  // ========== BTS (bit test and set) ==========
  {"bts",
   "long bts(long a, long bit) {\n"
   "  return a | (1UL << (bit & 63));\n"
   "}\n",
   {0xFF00, 3}, "ShufAtom"},

  // ========== BTR (bit test and reset) ==========
  {"btr",
   "long btr(long a, long bit) {\n"
   "  return a & ~(1UL << (bit & 63));\n"
   "}\n",
   {0xFF00, 8}, "ShufAtom"},

  // ========== BTC (bit test and complement) ==========
  {"btc",
   "long btc(long a, long bit) {\n"
   "  return a ^ (1UL << (bit & 63));\n"
   "}\n",
   {0xFF00, 8}, "ShufAtom"},

  // ========== XCHG (atomic exchange) ==========
  {"xchg_val",
   "long xchg_val(long a, long b) {\n"
   "  long tmp = a; a = b; b = tmp;\n"
   "  return a ^ b;\n"
   "}\n",
   {42, 100}, "ShufAtom"},

  // ========== Atomic add ==========
  {"atomic_add",
   "long atomic_add(long a, long b) {\n"
   "  long x = a;\n"
   "  x += b;\n"
   "  return x;\n"
   "}\n",
   {42, 100}, "ShufAtom"},

  // ========== POPCNT (population count) ==========
  {"popcount32",
   "long popcount32(long a) {\n"
   "  unsigned x = (unsigned)a;\n"
   "  int c = 0;\n"
   "  while (x) { c += x & 1; x >>= 1; }\n"
   "  return (long)c;\n"
   "}\n",
   {0xFF}, "ShufAtom"},

  // ========== BSF/BSR via builtin (directly generates tzcnt/lzcnt) ==========
  {"bitscan_fwd",
   "long bitscan_fwd(long a) {\n"
   "  if (a == 0) return -1;\n"
   "  return (long)__builtin_ctzll((unsigned long long)a);\n"
   "}\n",
   {0x80}, "ShufAtom"},

  {"bitscan_rev",
   "long bitscan_rev(long a) {\n"
   "  if (a == 0) return -1;\n"
   "  return (long)(63 - __builtin_clzll((unsigned long long)a));\n"
   "}\n",
   {0x100}, "ShufAtom"},

  // ========== LEA with 32-bit dest (regression for #36) ==========
  {"lea32_add",
   "long lea32_add(long a, long b) {\n"
   "  return (long)(unsigned)((unsigned)a + (unsigned)b);\n"
   "}\n",
   {0xFFFFFFFF, 2}, "ShufAtom"},

  {"lea32_scale",
   "long lea32_scale(long a) {\n"
   "  return (long)(unsigned)((unsigned)a * 4 + 100);\n"
   "}\n",
   {0x40000000}, "ShufAtom"},

  // ========== Byte extraction patterns ==========
  {"extract_hi_byte",
   "long extract_hi_byte(long a) {\n"
   "  return (a >> 56) & 0xFF;\n"
   "}\n",
   {0xABCDEF0123456789ULL}, "ShufAtom"},

  {"extract_lo_word",
   "long extract_lo_word(long a) {\n"
   "  return a & 0xFFFF;\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL}, "ShufAtom"},

  // ========== Conditional move patterns ==========
  {"cmov_min",
   "long cmov_min(long a, long b) {\n"
   "  return a < b ? a : b;\n"
   "}\n",
   {42, 100}, "ShufAtom"},

  {"cmov_max",
   "long cmov_max(long a, long b) {\n"
   "  return a > b ? a : b;\n"
   "}\n",
   {42, 100}, "ShufAtom"},

  {"cmov_abs",
   "long cmov_abs(long a) {\n"
   "  return a < 0 ? -a : a;\n"
   "}\n",
   {(uint64_t)(int64_t)-42}, "ShufAtom"},

  // ========== Multi-precision shift (SHLD/SHRD) ==========
  {"shld64",
   "long shld64(long a, long b) {\n"
   "  return (a << 16) | ((unsigned long)b >> 48);\n"
   "}\n",
   {0xDEAD, 0xBEEFCAFE00000000ULL}, "ShufAtom"},

  {"shrd64",
   "long shrd64(long a, long b) {\n"
   "  return ((unsigned long)a >> 16) | (b << 48);\n"
   "}\n",
   {0xDEADBEEF00000000ULL, 0xCAFE}, "ShufAtom"},

  // ========== Zero/sign extend edge cases ==========
  {"zext8_to_64",
   "long zext8_to_64(long a) {\n"
   "  return (unsigned long)(unsigned char)a;\n"
   "}\n",
   {0xAB}, "ShufAtom"},

  {"sext8_to_64",
   "long sext8_to_64(long a) {\n"
   "  return (long)(signed char)a;\n"
   "}\n",
   {0x80}, "ShufAtom"},

  {"sext16_to_64",
   "long sext16_to_64(long a) {\n"
   "  return (long)(short)a;\n"
   "}\n",
   {0x8000}, "ShufAtom"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ShufAtom, X64ShufAtomRT,
                         ::testing::ValuesIn(kX64ShufAtom), rtTCName);
