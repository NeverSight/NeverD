//===- AArch64_NEONAdvOps2RTTests.cpp - NEON advanced ops roundtrip -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip tests for AArch64 NEON operations that historically use
// emitIntrinsic placeholders: halving add, across-lane reduce, byte
// extract, narrow, and reciprocal estimates.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64NEONAdvOps2RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64NEONAdvOps2RT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

// ============================================================================
// Halving add/sub — (a + b) >> 1 (SHADD/UHADD pattern)
// ============================================================================
static const std::vector<RoundTripTC> kHalvingAdd = {
  {"c_uhadd_u8",
   "long c_uhadd_u8(long a, long b) {\n"
   "  unsigned char ua = (unsigned char)a, ub = (unsigned char)b;\n"
   "  return ((unsigned)ua + (unsigned)ub) >> 1;\n"
   "}\n",
   {200, 100}, "HalvingAdd", 1},

  {"c_shadd_s16",
   "long c_shadd_s16(long a, long b) {\n"
   "  short sa = (short)a, sb = (short)b;\n"
   "  return ((int)sa + (int)sb) >> 1;\n"
   "}\n",
   {(uint64_t)(int64_t)-100, 50}, "HalvingAdd", 1},

  {"c_urhadd_u8",
   "long c_urhadd_u8(long a, long b) {\n"
   "  unsigned char ua = (unsigned char)a, ub = (unsigned char)b;\n"
   "  return ((unsigned)ua + (unsigned)ub + 1) >> 1;\n"
   "}\n",
   {201, 100}, "HalvingAdd", 1},
};

// ============================================================================
// Across-lane reduce — max/min/sum of all lanes
// ============================================================================
static const std::vector<RoundTripTC> kAcrossLane = {
  {"c_max_of_4",
   "long c_max_of_4(long a, long b) {\n"
   "  int vals[4] = {(int)a, (int)b, 10, -20};\n"
   "  int m = vals[0];\n"
   "  for (int i = 1; i < 4; i++)\n"
   "    if (vals[i] > m) m = vals[i];\n"
   "  return m;\n"
   "}\n",
   {42, 99}, "AcrossLane", 1},

  {"c_min_of_4",
   "long c_min_of_4(long a, long b) {\n"
   "  int vals[4] = {(int)a, (int)b, 10, -20};\n"
   "  int m = vals[0];\n"
   "  for (int i = 1; i < 4; i++)\n"
   "    if (vals[i] < m) m = vals[i];\n"
   "  return m;\n"
   "}\n",
   {42, 99}, "AcrossLane", 1},

  {"c_sum_of_4",
   "long c_sum_of_4(long a, long b) {\n"
   "  return (int)a + (int)b + 10 + 20;\n"
   "}\n",
   {100, 200}, "AcrossLane", 1},
};

// ============================================================================
// Narrow/Extend — truncate wider to narrower type
// ============================================================================
static const std::vector<RoundTripTC> kNarrowExtend = {
  {"c_narrow_i32_to_i16",
   "long c_narrow_i32_to_i16(long a) {\n"
   "  return (unsigned short)(int)a;\n"
   "}\n",
   {0x12345678ULL}, "NarrowExtend", 1},

  {"c_mask_low32",
   "long c_mask_low32(long a) {\n"
   "  unsigned v = (unsigned)a;\n"
   "  return v + 1;\n"
   "}\n",
   {0x123456789ABCDEF0ULL}, "NarrowExtend", 1},

  {"c_ssat_i32_to_i16",
   "long c_ssat_i32_to_i16(long a) {\n"
   "  int v = (int)a;\n"
   "  if (v > 32767) v = 32767;\n"
   "  if (v < -32768) v = -32768;\n"
   "  return (unsigned short)v;\n"
   "}\n",
   {50000}, "NarrowExtend", 1},
};

// ============================================================================
// Byte-level operations — reverse, count, extract
// ============================================================================
static const std::vector<RoundTripTC> kByteOps = {
  {"c_byte_reverse32",
   "long c_byte_reverse32(long a) {\n"
   "  unsigned v = (unsigned)a;\n"
   "  return __builtin_bswap32(v);\n"
   "}\n",
   {0x12345678ULL}, "ByteOps", 1},

  {"c_byte_reverse64",
   "long c_byte_reverse64(long a) {\n"
   "  return (long)__builtin_bswap64((unsigned long)a);\n"
   "}\n",
   {0x0102030405060708ULL}, "ByteOps", 1},

  {"c_popcount_u32",
   "long c_popcount_u32(long a) {\n"
   "  return __builtin_popcount((unsigned)a);\n"
   "}\n",
   {0xDEADBEEFULL}, "ByteOps", 1},
};

// ============================================================================
// FP rounding — floor/ceil/trunc/round
// ============================================================================
static const std::vector<RoundTripTC> kFPRounding = {
  {"c_floor_f64",
   "long c_floor_f64(long a) {\n"
   "  double da; __builtin_memcpy(&da,&a,8);\n"
   "  double r = __builtin_floor(da);\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4002666666666666ULL}, "FPRounding", 1},

  {"c_ceil_f64",
   "long c_ceil_f64(long a) {\n"
   "  double da; __builtin_memcpy(&da,&a,8);\n"
   "  double r = __builtin_ceil(da);\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4002666666666666ULL}, "FPRounding", 1},

  {"c_trunc_f64",
   "long c_trunc_f64(long a) {\n"
   "  double da; __builtin_memcpy(&da,&a,8);\n"
   "  double r = __builtin_trunc(da);\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0xC002666666666666ULL}, "FPRounding", 1},

  {"c_round_f64",
   "long c_round_f64(long a) {\n"
   "  double da; __builtin_memcpy(&da,&a,8);\n"
   "  double r = __builtin_round(da);\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4006666666666666ULL}, "FPRounding", 1},
};

// ============================================================================
// Multiply-accumulate patterns (common in DSP/ML)
// ============================================================================
static const std::vector<RoundTripTC> kMulAcc = {
  {"c_madd_i64",
   "long c_madd_i64(long a, long b) {\n"
   "  return a * b + 100;\n"
   "}\n",
   {7, 13}, "MulAcc", 1},

  {"c_msub_i64",
   "long c_msub_i64(long a, long b) {\n"
   "  return 1000 - a * b;\n"
   "}\n",
   {7, 13}, "MulAcc", 1},

  {"c_smull_i32",
   "long c_smull_i32(long a, long b) {\n"
   "  return (long)(int)a * (long)(int)b;\n"
   "}\n",
   {(uint64_t)(int64_t)-12345, 6789}, "MulAcc", 1},
};

// ============================================================================
// Conditional select patterns (CSEL/CSINC/CSINV/CSNEG)
// ============================================================================
static const std::vector<RoundTripTC> kCondSel = {
  {"c_csel_gt",
   "long c_csel_gt(long a, long b) {\n"
   "  return a > b ? a : b;\n"
   "}\n",
   {42, 99}, "CondSel", 1},

  {"c_csinc",
   "long c_csinc(long a, long b) {\n"
   "  return a == b ? a : b + 1;\n"
   "}\n",
   {42, 42}, "CondSel", 1},

  {"c_csinv",
   "long c_csinv(long a, long b) {\n"
   "  return a < b ? a : ~b;\n"
   "}\n",
   {10, 20}, "CondSel", 1},

  {"c_csneg",
   "long c_csneg(long a, long b) {\n"
   "  return a >= b ? a : -b;\n"
   "}\n",
   {5, 10}, "CondSel", 1},
};

// ============================================================================
// Bit manipulation — BFI/UBFX/SBFX patterns
// ============================================================================
static const std::vector<RoundTripTC> kBitField = {
  {"c_extract_bits",
   "long c_extract_bits(long a) {\n"
   "  return (a >> 4) & 0xFF;\n"
   "}\n",
   {0xABCDEF12ULL}, "BitField", 1},

  {"c_insert_bits",
   "long c_insert_bits(long a, long b) {\n"
   "  unsigned long mask = 0xFF00UL;\n"
   "  return (a & ~mask) | ((b & 0xFF) << 8);\n"
   "}\n",
   {0x12345678ULL, 0xABULL}, "BitField", 1},

  {"c_sign_extend_byte",
   "long c_sign_extend_byte(long a) {\n"
   "  return (long)(signed char)a;\n"
   "}\n",
   {0x80ULL}, "BitField", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(HalvingAdd, A64NEONAdvOps2RT,
                         ::testing::ValuesIn(kHalvingAdd), rtTCName);
INSTANTIATE_TEST_SUITE_P(AcrossLane, A64NEONAdvOps2RT,
                         ::testing::ValuesIn(kAcrossLane), rtTCName);
INSTANTIATE_TEST_SUITE_P(NarrowExtend, A64NEONAdvOps2RT,
                         ::testing::ValuesIn(kNarrowExtend), rtTCName);
INSTANTIATE_TEST_SUITE_P(ByteOps, A64NEONAdvOps2RT,
                         ::testing::ValuesIn(kByteOps), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPRounding, A64NEONAdvOps2RT,
                         ::testing::ValuesIn(kFPRounding), rtTCName);
INSTANTIATE_TEST_SUITE_P(MulAcc, A64NEONAdvOps2RT,
                         ::testing::ValuesIn(kMulAcc), rtTCName);
INSTANTIATE_TEST_SUITE_P(CondSel, A64NEONAdvOps2RT,
                         ::testing::ValuesIn(kCondSel), rtTCName);
INSTANTIATE_TEST_SUITE_P(BitField, A64NEONAdvOps2RT,
                         ::testing::ValuesIn(kBitField), rtTCName);
