//===- AArch64_NEONAdvOpsSatRTTests.cpp - NEON saturating ops roundtrip ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip tests for AArch64 NEON saturating arithmetic: vector SQNEG/SQABS,
// scalar saturating add/sub/narrow/negate/abs, saturating doubling multiply,
// saturating and rounding shifts, and shift-insert.
//
// Split out of AArch64_NEONAdvOpsRTTests.cpp.  These cases belong to the same
// gtest suite (A64NEONAdvOpsRT) so that the ctest-visible test names stay
// unchanged, which is why the fixture is redeclared here instead of getting
// its own TEST_P: the TEST_P body may only be defined in one translation unit.
// The declaration below must stay token-identical to the one in
// AArch64_NEONAdvOpsRTTests.cpp.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64NEONAdvOpsRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};

// clang-format off

// ============================================================================
// SQNEG / SQABS (vector) — signed saturating negate/abs (INT_MIN clamps)
// ============================================================================
static const std::vector<RoundTripTC> kSatNegAbs = {
  {"sqneg_4s",
   "#include <arm_neon.h>\n"
   "long sqneg_4s(long a) {\n"
   "  int32x4_t va = {(int)a, -5, 100, -200};\n"
   "  int32x4_t vr = vqnegq_s32(va);\n"
   "  return (long)(unsigned)(vgetq_lane_s32(vr,0)+vgetq_lane_s32(vr,1)\n"
   "                          +vgetq_lane_s32(vr,2)+vgetq_lane_s32(vr,3));\n"
   "}\n",
   {0x80000000ULL}, "SatNegAbs", 1, "-march=armv8-a+simd"},

  {"sqneg_8h",
   "#include <arm_neon.h>\n"
   "long sqneg_8h(long a) {\n"
   "  int16x8_t va = {(short)a, -5, 100, -200, 7, -8, 9, -10};\n"
   "  int16x8_t vr = vqnegq_s16(va);\n"
   "  short out[8]; vst1q_s16(out, vr);\n"
   "  int s=0; for(int i=0;i<8;i++) s += out[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {0x8000ULL}, "SatNegAbs", 1, "-march=armv8-a+simd"},

  {"sqneg_16b",
   "#include <arm_neon.h>\n"
   "long sqneg_16b(long a) {\n"
   "  int8x16_t va = {(signed char)a,-5,12,-20,7,-8,9,-10,11,-12,13,-14,15,-16,17,-18};\n"
   "  int8x16_t vr = vqnegq_s8(va);\n"
   "  signed char out[16]; vst1q_s8(out, vr);\n"
   "  int s=0; for(int i=0;i<16;i++) s += out[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {0x80ULL}, "SatNegAbs", 1, "-march=armv8-a+simd"},

  {"sqabs_4s",
   "#include <arm_neon.h>\n"
   "long sqabs_4s(long a) {\n"
   "  int32x4_t va = {(int)a, -5, 100, -200};\n"
   "  int32x4_t vr = vqabsq_s32(va);\n"
   "  return (long)(unsigned)(vgetq_lane_s32(vr,0)+vgetq_lane_s32(vr,1)\n"
   "                          +vgetq_lane_s32(vr,2)+vgetq_lane_s32(vr,3));\n"
   "}\n",
   {0x80000000ULL}, "SatNegAbs", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// Scalar saturating ops — SQADD/SQSUB/UQADD/UQSUB (b/h/s/d), scalar ADDP,
// scalar SQXTN/UQXTN narrow, scalar SQABS/SQNEG.  These take the LaneSz==0
// (scalar) path in the lifter; previously fell back to unhandled intrinsics
// that silently returned 0.
// ============================================================================
static const std::vector<RoundTripTC> kScalarSat = {
  {"sqadd_d",
   "#include <arm_neon.h>\n"
   "long sqadd_d(long a, long b) {\n"
   "  return (long)vqaddd_s64((int64_t)a, (int64_t)b);\n"
   "}\n",
   {0x7FFFFFFFFFFFFFFFULL, 5}, "ScalarSat", 1, "-march=armv8-a+simd"},

  {"uqadd_d",
   "#include <arm_neon.h>\n"
   "long uqadd_d(long a, long b) {\n"
   "  return (long)vqaddd_u64((uint64_t)a, (uint64_t)b);\n"
   "}\n",
   {0xFFFFFFFFFFFFFFF0ULL, 0x40}, "ScalarSat", 1, "-march=armv8-a+simd"},

  {"sqadd_s",
   "#include <arm_neon.h>\n"
   "long sqadd_s(long a, long b) {\n"
   "  return (long)(unsigned)vqadds_s32((int32_t)a, (int32_t)b);\n"
   "}\n",
   {0x7FFFFF00ULL, 0x400}, "ScalarSat", 1, "-march=armv8-a+simd"},

  {"sqadd_h",
   "#include <arm_neon.h>\n"
   "long sqadd_h(long a, long b) {\n"
   "  return (long)(unsigned short)vqaddh_s16((int16_t)a, (int16_t)b);\n"
   "}\n",
   {0x7F00ULL, 0x400}, "ScalarSat", 1, "-march=armv8-a+simd"},

  {"sqadd_b",
   "#include <arm_neon.h>\n"
   "long sqadd_b(long a, long b) {\n"
   "  return (long)(unsigned char)vqaddb_s8((int8_t)a, (int8_t)b);\n"
   "}\n",
   {0x70ULL, 0x40}, "ScalarSat", 1, "-march=armv8-a+simd"},

  {"sqsub_d",
   "#include <arm_neon.h>\n"
   "long sqsub_d(long a, long b) {\n"
   "  return (long)vqsubd_s64((int64_t)a, (int64_t)b);\n"
   "}\n",
   {0x8000000000000000ULL, 5}, "ScalarSat", 1, "-march=armv8-a+simd"},

  {"uqsub_d",
   "#include <arm_neon.h>\n"
   "long uqsub_d(long a, long b) {\n"
   "  return (long)vqsubd_u64((uint64_t)a, (uint64_t)b);\n"
   "}\n",
   {3, 10}, "ScalarSat", 1, "-march=armv8-a+simd"},

  {"addp_d",
   "#include <arm_neon.h>\n"
   "long addp_d(long a, long b) {\n"
   "  int64x2_t v = {(int64_t)a, (int64_t)b};\n"
   "  return (long)vpaddd_s64(v);\n"
   "}\n",
   {0x111111111ULL, 0x222222222ULL}, "ScalarSat", 1, "-march=armv8-a+simd"},

  {"sqxtn_d_to_s",
   "#include <arm_neon.h>\n"
   "long sqxtn_d_to_s(long a) {\n"
   "  return (long)(unsigned)vqmovnd_s64((int64_t)a);\n"
   "}\n",
   {0x1234567890ULL}, "ScalarSat", 1, "-march=armv8-a+simd"},

  {"sqxtn_s_to_h",
   "#include <arm_neon.h>\n"
   "long sqxtn_s_to_h(long a) {\n"
   "  return (long)(unsigned short)vqmovns_s32((int32_t)a);\n"
   "}\n",
   {0x7FFF0000ULL}, "ScalarSat", 1, "-march=armv8-a+simd"},

  {"sqabs_d",
   "#include <arm_neon.h>\n"
   "long sqabs_d(long a) {\n"
   "  return (long)vqabsd_s64((int64_t)a);\n"
   "}\n",
   {0x8000000000000000ULL}, "ScalarSat", 1, "-march=armv8-a+simd"},

  {"sqneg_d",
   "#include <arm_neon.h>\n"
   "long sqneg_d(long a) {\n"
   "  return (long)vqnegd_s64((int64_t)a);\n"
   "}\n",
   {0x8000000000000000ULL}, "ScalarSat", 1, "-march=armv8-a+simd"},

  {"sqadd_2d",
   "#include <arm_neon.h>\n"
   "long sqadd_2d(long a, long b) {\n"
   "  int64x2_t va = {(int64_t)a, 0x7FFFFFFFFFFFFFF0LL};\n"
   "  int64x2_t vb = {(int64_t)b, 0x40};\n"
   "  int64x2_t vr = vqaddq_s64(va, vb);\n"
   "  return (long)(vgetq_lane_s64(vr,0) ^ vgetq_lane_s64(vr,1));\n"
   "}\n",
   {0x7FFFFFFFFFFFFFF0ULL, 0x40}, "ScalarSat", 1, "-march=armv8-a+simd"},

  {"uqsub_2d",
   "#include <arm_neon.h>\n"
   "long uqsub_2d(long a, long b) {\n"
   "  uint64x2_t va = {(uint64_t)a, 3};\n"
   "  uint64x2_t vb = {(uint64_t)b, 10};\n"
   "  uint64x2_t vr = vqsubq_u64(va, vb);\n"
   "  return (long)(vgetq_lane_u64(vr,0) ^ vgetq_lane_u64(vr,1));\n"
   "}\n",
   {5, 9}, "ScalarSat", 1, "-march=armv8-a+simd"},

  {"uqxtn_d_to_s",
   "#include <arm_neon.h>\n"
   "long uqxtn_d_to_s(long a) {\n"
   "  return (long)(unsigned)vqmovnd_u64((uint64_t)a);\n"
   "}\n",
   {0x1234567890ULL}, "ScalarSat", 1, "-march=armv8-a+simd"},

  {"sqxtun_s_to_h",
   "#include <arm_neon.h>\n"
   "long sqxtun_s_to_h(long a) {\n"
   "  return (long)(unsigned short)vqmovuns_s32((int32_t)a);\n"
   "}\n",
   {0xFFFF8000ULL}, "ScalarSat", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// Saturating doubling multiply / saturating shifts / shift-insert.  Edge
// values force saturation (e.g. INT_MIN*INT_MIN doubling, overflowing shifts).
// ============================================================================
static const std::vector<RoundTripTC> kSatShMul = {
  {"sqdmulh_4s",
   "#include <arm_neon.h>\n"
   "long sqdmulh_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a, 0x40000000, -0x40000000, 0x7fffffff};\n"
   "  int32x4_t vb = {(int)b, 0x40000000, 0x40000000, 0x7fffffff};\n"
   "  int32x4_t vr = vqdmulhq_s32(va, vb);\n"
   "  int o[4]; vst1q_s32(o, vr);\n"
   "  return (long)(unsigned)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x7fffffff, 0x7fffffff}, "SatShMul", 1, "-march=armv8-a+simd"},

  {"sqrdmulh_8h",
   "#include <arm_neon.h>\n"
   "long sqrdmulh_8h(long a, long b) {\n"
   "  int16x8_t va = {(short)a, 0x4000, -0x4000, 0x7fff, 100, -100, 1, 2};\n"
   "  int16x8_t vb = {(short)b, 0x4000, 0x4000, 0x7fff, 200, 300, 3, 4};\n"
   "  int16x8_t vr = vqrdmulhq_s16(va, vb);\n"
   "  short o[8]; vst1q_s16(o, vr);\n"
   "  int s=0; for(int i=0;i<8;i++) s = s*31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {0x7fff, 0x7fff}, "SatShMul", 1, "-march=armv8-a+simd"},

  {"sqdmull_4s",
   "#include <arm_neon.h>\n"
   "long sqdmull_4s(long a, long b) {\n"
   "  int32x2_t va = {(int)a, -0x80000000};\n"
   "  int32x2_t vb = {(int)b, -0x80000000};\n"
   "  int64x2_t vr = vqdmull_s32(va, vb);\n"
   "  return (long)(vgetq_lane_s64(vr,0) ^ vgetq_lane_s64(vr,1));\n"
   "}\n",
   {0x12345, 0x6789A}, "SatShMul", 1, "-march=armv8-a+simd"},

  {"sqshl_4s",
   "#include <arm_neon.h>\n"
   "long sqshl_4s(long a) {\n"
   "  int32x4_t va = {(int)a, 0x10000000, -0x10000000, 1};\n"
   "  int32x4_t vsh = {3, 8, 8, 40};\n"
   "  int32x4_t vr = vqshlq_s32(va, vsh);\n"
   "  int o[4]; vst1q_s32(o, vr);\n"
   "  return (long)(unsigned)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x20000000}, "SatShMul", 1, "-march=armv8-a+simd"},

  {"uqshl_4s",
   "#include <arm_neon.h>\n"
   "long uqshl_4s(long a) {\n"
   "  uint32x4_t va = {(unsigned)a, 0x10000000u, 0xFF000000u, 1};\n"
   "  int32x4_t vsh = {3, 8, 8, 40};\n"
   "  uint32x4_t vr = vqshlq_u32(va, vsh);\n"
   "  unsigned o[4]; vst1q_u32(o, vr);\n"
   "  return (long)(unsigned)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x20000000}, "SatShMul", 1, "-march=armv8-a+simd"},

  {"sqshrn_4s",
   "#include <arm_neon.h>\n"
   "long sqshrn_4s(long a) {\n"
   "  int32x4_t va = {(int)a, 0x7fffffff, -0x80000000, 100000};\n"
   "  int16x4_t vr = vqshrn_n_s32(va, 4);\n"
   "  short o[4]; vst1_s16(o, vr);\n"
   "  return (long)(unsigned short)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x40000}, "SatShMul", 1, "-march=armv8-a+simd"},

  {"sqrshrn_4s",
   "#include <arm_neon.h>\n"
   "long sqrshrn_4s(long a) {\n"
   "  int32x4_t va = {(int)a, 0x7fffffff, -0x80000000, 99999};\n"
   "  int16x4_t vr = vqrshrn_n_s32(va, 5);\n"
   "  short o[4]; vst1_s16(o, vr);\n"
   "  return (long)(unsigned short)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x50000}, "SatShMul", 1, "-march=armv8-a+simd"},

  {"uqshrn_8h",
   "#include <arm_neon.h>\n"
   "long uqshrn_8h(long a) {\n"
   "  uint16x8_t va = {(unsigned short)a, 0xFFFF, 0x8000, 0x1234, 1, 2, 3, 4};\n"
   "  uint8x8_t vr = vqshrn_n_u16(va, 3);\n"
   "  unsigned char o[8]; vst1_u8(o, vr);\n"
   "  int s=0; for(int i=0;i<8;i++) s = s*31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {0xFF00}, "SatShMul", 1, "-march=armv8-a+simd"},

  {"sli_4s",
   "#include <arm_neon.h>\n"
   "long sli_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a, 0x000000FF, 0x12345678, 0};\n"
   "  int32x4_t vb = {(int)b, 0x0000000F, (int)0xFFFFFFFF, 0};\n"
   "  int32x4_t vr = vsliq_n_s32(va, vb, 8);\n"
   "  int o[4]; vst1q_s32(o, vr);\n"
   "  return (long)(unsigned)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x11223344, 0x55667788}, "SatShMul", 1, "-march=armv8-a+simd"},

  {"sri_4s",
   "#include <arm_neon.h>\n"
   "long sri_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a, 0x000000FF, 0x12345678, 0};\n"
   "  int32x4_t vb = {(int)b, (int)0xFF000000, (int)0xFFFFFFFF, 0};\n"
   "  int32x4_t vr = vsriq_n_s32(va, vb, 8);\n"
   "  int o[4]; vst1q_s32(o, vr);\n"
   "  return (long)(unsigned)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x11223344, 0x55667788}, "SatShMul", 1, "-march=armv8-a+simd"},

  {"sqshlu_4s",
   "#include <arm_neon.h>\n"
   "long sqshlu_4s(long a) {\n"
   "  int32x4_t va = {(int)a, 0x10000000, -5, 1};\n"
   "  uint32x4_t vr = vqshluq_n_s32(va, 4);\n"
   "  unsigned o[4]; vst1q_u32(o, vr);\n"
   "  return (long)(unsigned)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x08000000}, "SatShMul", 1, "-march=armv8-a+simd"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ScalarSat, A64NEONAdvOpsRT,
                         ::testing::ValuesIn(kScalarSat), rtTCName);
INSTANTIATE_TEST_SUITE_P(SatShMul, A64NEONAdvOpsRT,
                         ::testing::ValuesIn(kSatShMul), rtTCName);
INSTANTIATE_TEST_SUITE_P(SatNegAbs, A64NEONAdvOpsRT,
                         ::testing::ValuesIn(kSatNegAbs), rtTCName);
