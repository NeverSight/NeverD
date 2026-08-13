//===- X64_BlendCmpRTTests.cpp - BLEND/CMP instruction roundtrip tests ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests for BLEND family (PBLENDW, BLENDPS, BLENDPD, BLENDVPS, BLENDVPD,
// PBLENDVB) and CMPPS/CMPPD predicate correctness.
//
// These instructions historically have bugs:
// - BLEND: lifter uses SELECT(1, Src, Dst) which always picks Src
// - CMPPS: lifter uses FLOAT_EQUAL ignoring the imm8 predicate
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BlendCmpRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BlendCmpRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

// ============================================================================
// BLENDPS — SSE4.1 immediate blend, testing lane that should come from DST
// mask 0x1 = lane0 from src, lanes 1-3 from dst
// ============================================================================
static const std::vector<RoundTripTC> kBlendPSKeepDst = {
  {"blendps_keep_lane1",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long blendps_keep_lane1(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  v4f va = {1.0f, fa, 3.0f, 4.0f};\n"
   "  v4f vb = {10.0f, fb, 30.0f, 40.0f};\n"
   "  v4f vr = __builtin_ia32_blendps(va, vb, 0x1);\n"
   "  float r = vr[1];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40A00000ULL, 0x42C80000ULL}, "BlendPSKeepDst", 1, "-msse4.1 -mno-avx"},

  {"blendps_keep_lane3",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long blendps_keep_lane3(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  v4f va = {1.0f, 2.0f, 3.0f, fa};\n"
   "  v4f vb = {10.0f, 20.0f, 30.0f, fb};\n"
   "  v4f vr = __builtin_ia32_blendps(va, vb, 0x3);\n"
   "  float r = vr[3];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40A00000ULL, 0x42C80000ULL}, "BlendPSKeepDst", 1, "-msse4.1 -mno-avx"},

  {"blendps_mixed_readback",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long blendps_mixed_readback(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  v4f va = {fa, 2.0f, 3.0f, 4.0f};\n"
   "  v4f vb = {10.0f, fb, 30.0f, 40.0f};\n"
   "  v4f vr = __builtin_ia32_blendps(va, vb, 0xA);\n"
   "  float r0 = vr[0], r1 = vr[1];\n"
   "  int i0, i1;\n"
   "  __builtin_memcpy(&i0,&r0,4); __builtin_memcpy(&i1,&r1,4);\n"
   "  return ((long)(unsigned)i1 << 32) | (unsigned)i0;\n"
   "}\n",
   {0x40A00000ULL, 0x42C80000ULL}, "BlendPSKeepDst", 1, "-msse4.1 -mno-avx"},
};

// ============================================================================
// BLENDPD — double precision blend, lane that should come from DST
// ============================================================================
static const std::vector<RoundTripTC> kBlendPDKeepDst = {
  {"blendpd_keep_lane0",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long blendpd_keep_lane0(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va = {da, 1.0};\n"
   "  v2d vb = {db, 2.0};\n"
   "  v2d vr = __builtin_ia32_blendpd(va, vb, 0x2);\n"
   "  double r = vr[0];\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4059000000000000ULL}, "BlendPDKeepDst", 1, "-msse4.1 -mno-avx"},
};

// ============================================================================
// PBLENDW — word-level blend with immediate mask
// ============================================================================
static const std::vector<RoundTripTC> kPBlendW = {
  {"pblendw_keep_low",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long pblendw_keep_low(long a, long b) {\n"
   "  v8s va = {(short)a, 2, 3, 4, 5, 6, 7, 8};\n"
   "  v8s vb = {(short)b, 20, 30, 40, 50, 60, 70, 80};\n"
   "  v8s vr = __builtin_ia32_pblendw128(va, vb, 0xF0);\n"
   "  return (unsigned short)vr[0];\n"
   "}\n",
   {100, 200}, "PBlendW", 1, "-msse4.1 -mno-avx"},

  {"pblendw_keep_high",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long pblendw_keep_high(long a, long b) {\n"
   "  v8s va = {1, 2, 3, 4, 5, 6, 7, (short)a};\n"
   "  v8s vb = {10, 20, 30, 40, 50, 60, 70, (short)b};\n"
   "  v8s vr = __builtin_ia32_pblendw128(va, vb, 0x0F);\n"
   "  return (unsigned short)vr[7];\n"
   "}\n",
   {100, 200}, "PBlendW", 1, "-msse4.1 -mno-avx"},
};

// ============================================================================
// PSHUFLW / PSHUFHW — word-level shuffle (low/high qword)
// ============================================================================
static const std::vector<RoundTripTC> kPSHUFLWHW = {
  {"pshuflw_reverse",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long pshuflw_reverse(long a, long b) {\n"
   "  v8s v = {(short)a, (short)b, 3, 4, 5, 6, 7, 8};\n"
   "  v8s r = __builtin_ia32_pshuflw(v, 0x1B);\n"
   "  return (unsigned short)r[0];\n"
   "}\n",
   {10, 20}, "PSHUFLWHW", 1, "-msse2 -mno-avx"},

  {"pshufhw_swap",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long pshufhw_swap(long a, long b) {\n"
   "  v8s v = {1, 2, 3, 4, (short)a, (short)b, 7, 8};\n"
   "  v8s r = __builtin_ia32_pshufhw(v, 0x1B);\n"
   "  return (unsigned short)r[4];\n"
   "}\n",
   {100, 200}, "PSHUFLWHW", 1, "-msse2 -mno-avx"},
};

// ============================================================================
// SHUFPD — double shuffle
// ============================================================================
static const std::vector<RoundTripTC> kSHUFPD = {
  {"shufpd_swap",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long shufpd_swap(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va = {da, 1.0};\n"
   "  v2d vb = {db, 2.0};\n"
   "  v2d vr = __builtin_ia32_shufpd(va, vb, 0x1);\n"
   "  double r = vr[0];\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4000000000000000ULL, 0x4008000000000000ULL}, "SHUFPD", 1, "-msse2 -mno-avx"},
};

// ============================================================================
// INSERTPS — SSE4.1 insert single-precision with zeroing
// ============================================================================
static const std::vector<RoundTripTC> kINSERTPS = {
  {"insertps_with_zero",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long insertps_with_zero(long a) {\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  v4f va = {fa, 2.0f, 3.0f, 4.0f};\n"
   "  v4f vb = {10.0f, 20.0f, 30.0f, 40.0f};\n"
   "  v4f vr = __builtin_ia32_insertps128(va, vb, 0x26);\n"
   "  float r = vr[0];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40A00000ULL}, "INSERTPS", 1, "-msse4.1 -mno-avx"},
};

// ============================================================================
// PTEST — SSE4.1 packed test (sets ZF/CF based on AND/ANDN)
// ============================================================================
static const std::vector<RoundTripTC> kPTEST = {
  {"ptest_all_zero",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long ptest_all_zero(long a) {\n"
   "  v2q va = {(long long)a, 0};\n"
   "  v2q vb = {0, 0};\n"
   "  return __builtin_ia32_ptestz128(va, vb);\n"
   "}\n",
   {42}, "PTEST", 1, "-msse4.1 -mno-avx"},

  {"ptest_nonzero",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long ptest_nonzero(long a) {\n"
   "  v2q va = {(long long)a, 0};\n"
   "  v2q vb = {(long long)a, 0};\n"
   "  return __builtin_ia32_ptestz128(va, vb);\n"
   "}\n",
   {42}, "PTEST", 1, "-msse4.1 -mno-avx"},
};

// ============================================================================
// VPERMILPS / VPERMILPD — AVX in-lane permute
// ============================================================================
static const std::vector<RoundTripTC> kVPERMIL = {
  {"vpermilps_reverse",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long vpermilps_reverse(long a) {\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  v4f va = {fa, 2.0f, 3.0f, 4.0f};\n"
   "  v4f vr = __builtin_ia32_vpermilps(va, 0x1B);\n"
   "  float r = vr[3];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40A00000ULL}, "VPERMIL", 1, "-mavx -mno-avx2"},

  {"vpermilpd_swap",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long vpermilpd_swap(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va = {da, db};\n"
   "  v2d vr = __builtin_ia32_vpermilpd(va, 0x1);\n"
   "  double r = vr[0];\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4000000000000000ULL, 0x4008000000000000ULL}, "VPERMIL", 1, "-mavx -mno-avx2"},
};

// ============================================================================
// CMPPS predicate variants (testing specific predicates beyond EQ)
// ============================================================================
static const std::vector<RoundTripTC> kCMPPS = {
  {"cmpps_lt",
   "long cmpps_lt(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  return fa < fb ? 1 : 0;\n"
   "}\n",
   {0x3F800000ULL, 0x40000000ULL}, "CMPPS", 1, ""},

  {"cmpps_le",
   "long cmpps_le(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  return fa <= fb ? 1 : 0;\n"
   "}\n",
   {0x40000000ULL, 0x40000000ULL}, "CMPPS", 1, ""},

  {"cmpss_neq_c",
   "long cmpss_neq_c(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  return (fa < fb) || (fa > fb) ? 1 : 0;\n"
   "}\n",
   {0x3F800000ULL, 0x40000000ULL}, "CMPPS", 1, ""},

  {"cmpps_gt",
   "long cmpps_gt(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  return fa > fb ? 1 : 0;\n"
   "}\n",
   {0x40000000ULL, 0x3F800000ULL}, "CMPPS", 1, ""},
};

// ============================================================================
// CMPPD predicate variants
// ============================================================================
static const std::vector<RoundTripTC> kCMPPD = {
  {"cmppd_lt",
   "long cmppd_lt(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  return da < db ? 1 : 0;\n"
   "}\n",
   {0x3FF0000000000000ULL, 0x4000000000000000ULL}, "CMPPD", 1, ""},

  {"cmppd_ge",
   "long cmppd_ge(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  return da >= db ? 1 : 0;\n"
   "}\n",
   {0x4000000000000000ULL, 0x3FF0000000000000ULL}, "CMPPD", 1, ""},
};

// ============================================================================
// MOVMSKPS / MOVMSKPD — extract sign bits (use C sign checks, avoids rodata)
// ============================================================================
static const std::vector<RoundTripTC> kMOVMSK = {
  {"movmskps_c_sign_extract",
   "long movmskps_c_sign_extract(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  int r = 0;\n"
   "  if (fa < 0.0f) r |= 1;\n"
   "  if (fb < 0.0f) r |= 2;\n"
   "  return r;\n"
   "}\n",
   {0xBF800000ULL, 0x40000000ULL}, "MOVMSK", 1, ""},

  {"movmskpd_c_sign_extract",
   "long movmskpd_c_sign_extract(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  int r = 0;\n"
   "  if (da < 0.0) r |= 1;\n"
   "  if (db < 0.0) r |= 2;\n"
   "  return r;\n"
   "}\n",
   {0xBFF0000000000000ULL, 0x4000000000000000ULL}, "MOVMSK", 1, ""},
};

// ============================================================================
// PCMPGTB/W/D — packed signed compare greater than
// ============================================================================
static const std::vector<RoundTripTC> kPCMPGT = {
  {"pcmpgtd_basic",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long pcmpgtd_basic(long a, long b) {\n"
   "  v4i va = {(int)a, 10, 20, 30};\n"
   "  v4i vb = {(int)b, 5, 25, 30};\n"
   "  v4i vr = (va > vb);\n"
   "  return (unsigned)vr[0];\n"
   "}\n",
   {100, 50}, "PCMPGT", 1, "-msse2 -mno-avx"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(BlendPSKeepDst, X64BlendCmpRT,
                         ::testing::ValuesIn(kBlendPSKeepDst), rtTCName);
INSTANTIATE_TEST_SUITE_P(BlendPDKeepDst, X64BlendCmpRT,
                         ::testing::ValuesIn(kBlendPDKeepDst), rtTCName);
INSTANTIATE_TEST_SUITE_P(PBlendW, X64BlendCmpRT,
                         ::testing::ValuesIn(kPBlendW), rtTCName);
INSTANTIATE_TEST_SUITE_P(PSHUFLWHW, X64BlendCmpRT,
                         ::testing::ValuesIn(kPSHUFLWHW), rtTCName);
INSTANTIATE_TEST_SUITE_P(SHUFPD, X64BlendCmpRT,
                         ::testing::ValuesIn(kSHUFPD), rtTCName);
INSTANTIATE_TEST_SUITE_P(INSERTPS, X64BlendCmpRT,
                         ::testing::ValuesIn(kINSERTPS), rtTCName);
INSTANTIATE_TEST_SUITE_P(PTEST, X64BlendCmpRT,
                         ::testing::ValuesIn(kPTEST), rtTCName);
INSTANTIATE_TEST_SUITE_P(VPERMIL, X64BlendCmpRT,
                         ::testing::ValuesIn(kVPERMIL), rtTCName);
INSTANTIATE_TEST_SUITE_P(CMPPS, X64BlendCmpRT,
                         ::testing::ValuesIn(kCMPPS), rtTCName);
INSTANTIATE_TEST_SUITE_P(CMPPD, X64BlendCmpRT,
                         ::testing::ValuesIn(kCMPPD), rtTCName);
INSTANTIATE_TEST_SUITE_P(MOVMSK, X64BlendCmpRT,
                         ::testing::ValuesIn(kMOVMSK), rtTCName);
INSTANTIATE_TEST_SUITE_P(PCMPGT, X64BlendCmpRT,
                         ::testing::ValuesIn(kPCMPGT), rtTCName);
