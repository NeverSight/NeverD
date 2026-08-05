//===- ARM32_NEONOps2RTTests.cpp - ARM32 NEON/VFP advanced roundtrip ------===//
//
// Roundtrip tests for ARM32 NEON/VFP operations with C expression patterns
// that exercise commonly-used instruction sequences.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32NEONOps2RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NEONOps2RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32Arith = {
  {"arm32_c_mul_acc",
   "long arm32_c_mul_acc(long a, long b) {\n"
   "  return (int)a * (int)b + 42;\n"
   "}\n",
   {7, 13}, "ARM32Arith", 1},

  {"arm32_c_smull",
   "long arm32_c_smull(long a, long b) {\n"
   "  long long r = (long long)(int)a * (long long)(int)b;\n"
   "  return (long)(unsigned)(r >> 32);\n"
   "}\n",
   {100000, 200000}, "ARM32Arith", 1},

  {"arm32_c_clz",
   "long arm32_c_clz(long a) {\n"
   "  unsigned v = (unsigned)a;\n"
   "  if (v == 0) return 32;\n"
   "  return __builtin_clz(v);\n"
   "}\n",
   {0x100ULL}, "ARM32Arith", 1},
};

static const std::vector<RoundTripTC> kARM32FP = {
  {"arm32_c_fadd",
   "long arm32_c_fadd(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  float r = fa + fb;\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40400000ULL, 0x40800000ULL}, "ARM32FP", 1},

  {"arm32_c_fmul",
   "long arm32_c_fmul(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  float r = fa * fb;\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40400000ULL, 0x40800000ULL}, "ARM32FP", 1},

  {"arm32_c_fcmp_lt",
   "long arm32_c_fcmp_lt(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  return fa < fb ? 1 : 0;\n"
   "}\n",
   {0x3F800000ULL, 0x40000000ULL}, "ARM32FP", 1},

  {"arm32_c_fabs",
   "long arm32_c_fabs(long a) {\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  float r = fa < 0 ? -fa : fa;\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0xBF800000ULL}, "ARM32FP", 1},
};

static const std::vector<RoundTripTC> kARM32BitOps = {
  {"arm32_c_bswap32",
   "long arm32_c_bswap32(long a) {\n"
   "  return __builtin_bswap32((unsigned)a);\n"
   "}\n",
   {0x12345678ULL}, "ARM32BitOps", 1},

  {"arm32_c_ctz",
   "long arm32_c_ctz(long a) {\n"
   "  unsigned v = (unsigned)a;\n"
   "  if (v == 0) return 32;\n"
   "  return __builtin_ctz(v);\n"
   "}\n",
   {0x80ULL}, "ARM32BitOps", 1},

  {"arm32_c_bitrev",
   "long arm32_c_bitrev(long a) {\n"
   "  unsigned v = (unsigned)a;\n"
   "  v = ((v >> 1) & 0x55555555) | ((v & 0x55555555) << 1);\n"
   "  v = ((v >> 2) & 0x33333333) | ((v & 0x33333333) << 2);\n"
   "  v = ((v >> 4) & 0x0F0F0F0F) | ((v & 0x0F0F0F0F) << 4);\n"
   "  return __builtin_bswap32(v);\n"
   "}\n",
   {0x12345678ULL}, "ARM32BitOps", 1},
};

static const std::vector<RoundTripTC> kARM32CondOps = {
  {"arm32_c_max",
   "long arm32_c_max(long a, long b) {\n"
   "  int ia = (int)a, ib = (int)b;\n"
   "  return ia > ib ? ia : ib;\n"
   "}\n",
   {42, 99}, "ARM32CondOps", 1},

  {"arm32_c_min",
   "long arm32_c_min(long a, long b) {\n"
   "  int ia = (int)a, ib = (int)b;\n"
   "  return ia < ib ? ia : ib;\n"
   "}\n",
   {42, 99}, "ARM32CondOps", 1},

  {"arm32_c_abs",
   "long arm32_c_abs(long a) {\n"
   "  int v = (int)a;\n"
   "  return (unsigned)(v < 0 ? -v : v);\n"
   "}\n",
   {(uint64_t)(int64_t)-42}, "ARM32CondOps", 1},

  {"arm32_c_clamp",
   "long arm32_c_clamp(long a, long b) {\n"
   "  int v = (int)a, lo = 0, hi = (int)b;\n"
   "  if (v < lo) v = lo;\n"
   "  if (v > hi) v = hi;\n"
   "  return (unsigned)v;\n"
   "}\n",
   {(uint64_t)(int64_t)-5, 100}, "ARM32CondOps", 1},
};

static const std::vector<RoundTripTC> kARM32Shift = {
  {"arm32_c_asr",
   "long arm32_c_asr(long a, long b) {\n"
   "  return (unsigned)((int)a >> ((int)b & 31));\n"
   "}\n",
   {0x80000000ULL, 4}, "ARM32Shift", 1},

  {"arm32_c_ror",
   "long arm32_c_ror(long a, long b) {\n"
   "  unsigned v = (unsigned)a, s = (unsigned)b & 31;\n"
   "  return (v >> s) | (v << (32 - s));\n"
   "}\n",
   {0xDEADBEEFULL, 8}, "ARM32Shift", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ARM32Arith, ARM32NEONOps2RT,
                         ::testing::ValuesIn(kARM32Arith), rtTCName);
INSTANTIATE_TEST_SUITE_P(ARM32FP, ARM32NEONOps2RT,
                         ::testing::ValuesIn(kARM32FP), rtTCName);
INSTANTIATE_TEST_SUITE_P(ARM32BitOps, ARM32NEONOps2RT,
                         ::testing::ValuesIn(kARM32BitOps), rtTCName);
INSTANTIATE_TEST_SUITE_P(ARM32CondOps, ARM32NEONOps2RT,
                         ::testing::ValuesIn(kARM32CondOps), rtTCName);
INSTANTIATE_TEST_SUITE_P(ARM32Shift, ARM32NEONOps2RT,
                         ::testing::ValuesIn(kARM32Shift), rtTCName);
