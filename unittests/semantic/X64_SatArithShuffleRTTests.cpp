//===- X64_SatArithShuffleRTTests.cpp - Sat arith + shuffle RT --*- C++ -*-===//
//
// Tests x86_64 SSE saturating arithmetic, pack/extract, and shuffle patterns.
// Covers: PADDSB/PADDUSB/PSUBSB/PSUBUSB, PACKSSWB/PACKUSWB,
//         PUNPCKL/PUNPCKH, PMADDWD, PEXTRW/PINSRW, PALIGNR, etc.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SatShufRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SatShufRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

#define V16C_S "typedef char v16c __attribute__((vector_size(16)));\n"
#define V16UC_S "typedef unsigned char v16uc __attribute__((vector_size(16)));\n"
#define V8S_S "typedef short v8s __attribute__((vector_size(16)));\n"
#define V8US_S "typedef unsigned short v8us __attribute__((vector_size(16)));\n"
#define V4I_S "typedef int v4i __attribute__((vector_size(16)));\n"
#define V2Q_S "typedef long long v2q __attribute__((vector_size(16)));\n"

static const std::vector<RoundTripTC> kX64SatShuf = {
  // ========== Saturating add signed bytes (PADDSB) ==========
  {"sat_add_sb",
   V16C_S
   "long sat_add_sb(long a) {\n"
   "  signed char x = 100, y = 50;\n"
   "  signed char r = x + y;\n"
   "  if (r < x) r = 127;\n"
   "  return (long)(unsigned char)r;\n"
   "}\n",
   {0}, "SatShufRT"},

  // ========== Saturating add unsigned bytes (PADDUSB) ==========
  {"sat_add_ub",
   V16UC_S
   "long sat_add_ub(long a) {\n"
   "  unsigned char x = 200, y = 100;\n"
   "  unsigned int sum = (unsigned int)x + (unsigned int)y;\n"
   "  return sum > 255 ? 255 : sum;\n"
   "}\n",
   {0}, "SatShufRT"},

  // ========== Saturating sub signed (PSUBSB pattern) ==========
  {"sat_sub_sb",
   "long sat_sub_sb(long a, long b) {\n"
   "  signed char x = (signed char)a, y = (signed char)b;\n"
   "  int diff = (int)x - (int)y;\n"
   "  if (diff < -128) diff = -128;\n"
   "  if (diff > 127) diff = 127;\n"
   "  return (long)(unsigned char)(signed char)diff;\n"
   "}\n",
   {100, 200}, "SatShufRT"},

  // ========== Clamp to byte (PACKUSWB-like pattern) ==========
  {"clamp_to_byte",
   "long clamp_to_byte(long a) {\n"
   "  int x = (int)a;\n"
   "  if (x < 0) x = 0;\n"
   "  if (x > 255) x = 255;\n"
   "  return (long)x;\n"
   "}\n",
   {300}, "SatShufRT"},

  {"clamp_to_byte_neg",
   "long clamp_to_byte_neg(long a) {\n"
   "  int x = (int)a;\n"
   "  if (x < 0) x = 0;\n"
   "  if (x > 255) x = 255;\n"
   "  return (long)x;\n"
   "}\n",
   {(uint64_t)(int64_t)-50}, "SatShufRT"},

  // ========== Multiply-accumulate 16-bit (PMADDWD pattern) ==========
  {"madd_16x2",
   "long madd_16x2(long a, long b) {\n"
   "  short a0 = (short)(a & 0xFFFF);\n"
   "  short a1 = (short)((a >> 16) & 0xFFFF);\n"
   "  short b0 = (short)(b & 0xFFFF);\n"
   "  short b1 = (short)((b >> 16) & 0xFFFF);\n"
   "  return (long)((int)a0 * (int)b0 + (int)a1 * (int)b1);\n"
   "}\n",
   {(3 | (4ULL << 16)), (5 | (6ULL << 16))}, "SatShufRT"},

  // ========== Byte interleave (PUNPCKLBW/PUNPCKHBW patterns) ==========
  {"interleave_bytes_low",
   "long interleave_bytes_low(long a, long b) {\n"
   "  long r = 0;\n"
   "  for (int i = 0; i < 4; ++i) {\n"
   "    r |= ((a >> (i*8)) & 0xFF) << (i*16);\n"
   "    r |= ((b >> (i*8)) & 0xFF) << (i*16+8);\n"
   "  }\n"
   "  return r;\n"
   "}\n",
   {0x01020304ULL, 0x0A0B0C0DULL}, "SatShufRT"},

  // ========== Byte extract (PEXTRB/PEXTRW pattern) ==========
  {"extract_byte",
   "long extract_byte(long val, long idx) {\n"
   "  return (val >> ((idx & 7) * 8)) & 0xFF;\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 5}, "SatShufRT"},

  {"extract_word",
   "long extract_word(long val, long idx) {\n"
   "  return (val >> ((idx & 3) * 16)) & 0xFFFF;\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 2}, "SatShufRT"},

  // ========== Byte insert (PINSRB/PINSRW pattern) ==========
  {"insert_byte",
   "long insert_byte(long val, long byte_idx) {\n"
   "  long byte_val = 0xAA;\n"
   "  long shift = (byte_idx & 7) * 8;\n"
   "  return (val & ~(0xFFULL << shift)) | (byte_val << shift);\n"
   "}\n",
   {0x0102030405060708ULL, 3}, "SatShufRT"},

  // ========== Shuffle bytes (PSHUFB-like pattern) ==========
  {"shuffle_bytes_rev",
   "long shuffle_bytes_rev(long a) {\n"
   "  long r = 0;\n"
   "  for (int i = 0; i < 8; ++i)\n"
   "    r |= ((a >> (i*8)) & 0xFF) << ((7-i)*8);\n"
   "  return r;\n"
   "}\n",
   {0x0102030405060708ULL}, "SatShufRT"},

  // ========== PSADBW (sum of absolute differences) ==========
  {"sad_bytes",
   "long sad_bytes(long a, long b) {\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    int va = (a >> (i*8)) & 0xFF;\n"
   "    int vb = (b >> (i*8)) & 0xFF;\n"
   "    int d = va - vb;\n"
   "    sum += d < 0 ? -d : d;\n"
   "  }\n"
   "  return sum;\n"
   "}\n",
   {0x0102030405060708ULL, 0x0807060504030201ULL}, "SatShufRT"},

  // ========== PAVGB (byte average) ==========
  {"avg_bytes",
   "long avg_bytes(long a, long b) {\n"
   "  long r = 0;\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    unsigned va = (a >> (i*8)) & 0xFF;\n"
   "    unsigned vb = (b >> (i*8)) & 0xFF;\n"
   "    r |= (long)((va + vb + 1) >> 1) << (i*8);\n"
   "  }\n"
   "  return r;\n"
   "}\n",
   {0x10203040ULL, 0x20304050ULL}, "SatShufRT"},

  // ========== PMULHW/PMULLW pattern ==========
  {"mul_hi_16",
   "long mul_hi_16(long a, long b) {\n"
   "  short sa = (short)(a & 0xFFFF);\n"
   "  short sb = (short)(b & 0xFFFF);\n"
   "  int prod = (int)sa * (int)sb;\n"
   "  return (long)(unsigned short)(prod >> 16);\n"
   "}\n",
   {30000, 20000}, "SatShufRT"},

  {"mul_lo_16",
   "long mul_lo_16(long a, long b) {\n"
   "  short sa = (short)(a & 0xFFFF);\n"
   "  short sb = (short)(b & 0xFFFF);\n"
   "  return (long)(unsigned short)(sa * sb);\n"
   "}\n",
   {100, 200}, "SatShufRT"},

  // ========== MOVMSKPS/MOVMSKPD pattern ==========
  {"sign_mask",
   "long sign_mask(long a) {\n"
   "  long mask = 0;\n"
   "  for (int i = 0; i < 8; ++i)\n"
   "    if ((a >> (i*8)) & 0x80) mask |= (1 << i);\n"
   "  return mask;\n"
   "}\n",
   {0x80FF007F80008000ULL}, "SatShufRT"},

  // ========== Conditional swap (min/max pair) ==========
  {"cond_swap",
   "long cond_swap(long a, long b) {\n"
   "  long lo = a < b ? a : b;\n"
   "  long hi = a > b ? a : b;\n"
   "  return lo ^ hi;\n"
   "}\n",
   {42, 100}, "SatShufRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SatShufRT, X64SatShufRT,
                         ::testing::ValuesIn(kX64SatShuf), rtTCName);
