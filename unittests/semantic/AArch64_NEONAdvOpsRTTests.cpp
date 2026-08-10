//===- AArch64_NEONAdvOpsRTTests.cpp - NEON advanced ops roundtrip --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip tests for AArch64 NEON instructions not yet covered:
// TBL, EXT, ZIP1/ZIP2, UZP1/UZP2, TRN1/TRN2, CNT, RBIT(vector),
// SADDLP/UADDLP, SQRSHL, XTN/SQXTN, FCVTZS/FCVTZU vector, FRECPE/FRSQRTE.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64NEONAdvOpsRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64NEONAdvOpsRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

// ============================================================================
// ZIP1 / ZIP2 — interleave low/high halves
// ============================================================================
static const std::vector<RoundTripTC> kZipUzp = {
  {"zip1_4s",
   "#include <arm_neon.h>\n"
   "long zip1_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a, 2, 3, 4};\n"
   "  int32x4_t vb = {(int)b, 20, 30, 40};\n"
   "  int32x4_t vr = vzip1q_s32(va, vb);\n"
   "  return (long)(unsigned)vgetq_lane_s32(vr, 0);\n"
   "}\n",
   {10, 100}, "ZipUzp", 1, "-march=armv8-a+simd"},

  {"zip2_4s",
   "#include <arm_neon.h>\n"
   "long zip2_4s(long a, long b) {\n"
   "  int32x4_t va = {1, 2, (int)a, 4};\n"
   "  int32x4_t vb = {10, 20, (int)b, 40};\n"
   "  int32x4_t vr = vzip2q_s32(va, vb);\n"
   "  return (long)(unsigned)vgetq_lane_s32(vr, 0);\n"
   "}\n",
   {77, 88}, "ZipUzp", 1, "-march=armv8-a+simd"},

  {"uzp1_4s",
   "#include <arm_neon.h>\n"
   "long uzp1_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a, 2, 3, 4};\n"
   "  int32x4_t vb = {(int)b, 20, 30, 40};\n"
   "  int32x4_t vr = vuzp1q_s32(va, vb);\n"
   "  return (long)(unsigned)vgetq_lane_s32(vr, 0);\n"
   "}\n",
   {55, 66}, "ZipUzp", 1, "-march=armv8-a+simd"},

  {"uzp2_4s",
   "#include <arm_neon.h>\n"
   "long uzp2_4s(long a, long b) {\n"
   "  int32x4_t va = {1, (int)a, 3, 4};\n"
   "  int32x4_t vb = {10, (int)b, 30, 40};\n"
   "  int32x4_t vr = vuzp2q_s32(va, vb);\n"
   "  return (long)(unsigned)vgetq_lane_s32(vr, 0);\n"
   "}\n",
   {99, 111}, "ZipUzp", 1, "-march=armv8-a+simd"},

  {"trn1_4s",
   "#include <arm_neon.h>\n"
   "long trn1_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a, 2, 3, 4};\n"
   "  int32x4_t vb = {(int)b, 20, 30, 40};\n"
   "  int32x4_t vr = vtrn1q_s32(va, vb);\n"
   "  return (long)(unsigned)vgetq_lane_s32(vr, 0);\n"
   "}\n",
   {15, 25}, "ZipUzp", 1, "-march=armv8-a+simd"},

  {"trn2_4s",
   "#include <arm_neon.h>\n"
   "long trn2_4s(long a, long b) {\n"
   "  int32x4_t va = {1, (int)a, 3, 4};\n"
   "  int32x4_t vb = {10, (int)b, 30, 40};\n"
   "  int32x4_t vr = vtrn2q_s32(va, vb);\n"
   "  return (long)(unsigned)vgetq_lane_s32(vr, 0);\n"
   "}\n",
   {35, 45}, "ZipUzp", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// TBL / TBX — table lookup
// ============================================================================
static const std::vector<RoundTripTC> kTblTbx = {
  {"tbl_16b",
   "long tbl_16b(long a, long b) {\n"
   "  long lo = a & 0xFF;\n"
   "  long hi = b & 0xFF;\n"
   "  long tbl_lo = lo | ((lo+1)<<8) | ((lo+2)<<16) | ((lo+3)<<24)\n"
   "             | ((lo+4)<<32) | ((lo+5)<<40) | ((lo+6)<<48) | ((lo+7)<<56);\n"
   "  long tbl_hi = hi | ((hi+1)<<8) | ((hi+2)<<16) | ((hi+3)<<24)\n"
   "             | ((hi+4)<<32) | ((hi+5)<<40) | ((hi+6)<<48) | ((hi+7)<<56);\n"
   "  long idx_val = 0x0202020202020202LL;\n"
   "  long result;\n"
   "  __asm__ volatile(\n"
   "    \"mov v0.d[0], %1\\n\"\n"
   "    \"mov v0.d[1], %2\\n\"\n"
   "    \"mov v1.d[0], %3\\n\"\n"
   "    \"mov v1.d[1], %3\\n\"\n"
   "    \"tbl v2.16b, {v0.16b}, v1.16b\\n\"\n"
   "    \"umov %w0, v2.b[0]\\n\"\n"
   "    : \"=r\"(result)\n"
   "    : \"r\"(tbl_lo), \"r\"(tbl_hi), \"r\"(idx_val)\n"
   "    : \"v0\", \"v1\", \"v2\"\n"
   "  );\n"
   "  return result;\n"
   "}\n",
   {10, 50}, "TblTbx", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// EXT — extract (byte-level concat + shift)
// ============================================================================
static const std::vector<RoundTripTC> kExt = {
  {"ext_16b_shift4",
   "#include <arm_neon.h>\n"
   "long ext_16b_shift4(long a, long b) {\n"
   "  uint8x16_t va = {(unsigned char)a,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};\n"
   "  uint8x16_t vb = {(unsigned char)b,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31};\n"
   "  uint8x16_t vr = vextq_u8(va, vb, 4);\n"
   "  return (long)vgetq_lane_u8(vr, 0);\n"
   "}\n",
   {0xAA, 0xBB}, "Ext", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// CNT — population count per byte
// ============================================================================
static const std::vector<RoundTripTC> kCnt = {
  {"cnt_8b",
   "#include <arm_neon.h>\n"
   "long cnt_8b(long a) {\n"
   "  uint8x8_t va = {(unsigned char)a, 0xFF, 0, 0x0F, 0, 0, 0, 0};\n"
   "  uint8x8_t vr = vcnt_u8(va);\n"
   "  return (long)vget_lane_u8(vr, 0);\n"
   "}\n",
   {0x55}, "Cnt", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// SADDLP / UADDLP — pairwise add long
// ============================================================================
static const std::vector<RoundTripTC> kPairwiseAdd = {
  {"uaddlp_4s",
   "#include <arm_neon.h>\n"
   "long uaddlp_4s(long a, long b) {\n"
   "  uint32x4_t va = {(unsigned)a, (unsigned)b, 100, 200};\n"
   "  uint64x2_t vr = vpaddlq_u32(va);\n"
   "  return (long)vgetq_lane_u64(vr, 0);\n"
   "}\n",
   {30, 70}, "PairwiseAdd", 1, "-march=armv8-a+simd"},

  {"saddlp_8h",
   "#include <arm_neon.h>\n"
   "long saddlp_8h(long a) {\n"
   "  int16x8_t va = {(short)a, -10, 3, 7, 0, 0, 0, 0};\n"
   "  int32x4_t vr = vpaddlq_s16(va);\n"
   "  return (long)vgetq_lane_s32(vr, 0);\n"
   "}\n",
   {20}, "PairwiseAdd", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// XTN / SQXTN — narrow
// ============================================================================
static const std::vector<RoundTripTC> kNarrow = {
  {"xtn_4s_to_4h",
   "#include <arm_neon.h>\n"
   "long xtn_4s_to_4h(long a) {\n"
   "  int32x4_t va = {(int)a, 100, 200, 300};\n"
   "  int16x4_t vr = vmovn_s32(va);\n"
   "  return (long)(unsigned short)vget_lane_s16(vr, 0);\n"
   "}\n",
   {42}, "Narrow", 1, "-march=armv8-a+simd"},

  {"sqxtn_4s_to_4h",
   "#include <arm_neon.h>\n"
   "long sqxtn_4s_to_4h(long a) {\n"
   "  int32x4_t va = {(int)a, 100, 200, 300};\n"
   "  int16x4_t vr = vqmovn_s32(va);\n"
   "  return (long)(unsigned short)vget_lane_s16(vr, 0);\n"
   "}\n",
   {50000}, "Narrow", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// FCVTZS / FCVTZU — float-to-int vector conversion
// ============================================================================
static const std::vector<RoundTripTC> kFCvt = {
  {"fcvtzs_4s",
   "#include <arm_neon.h>\n"
   "long fcvtzs_4s(long a) {\n"
   "  float fa; __builtin_memcpy(&fa, &a, 4);\n"
   "  float32x4_t va = {fa, 2.7f, -3.9f, 0.0f};\n"
   "  int32x4_t vr = vcvtq_s32_f32(va);\n"
   "  return (long)vgetq_lane_s32(vr, 0);\n"
   "}\n",
   {0x40B33333ULL}, "FCvt", 1, "-march=armv8-a+simd"},

  {"fcvtzu_4s",
   "#include <arm_neon.h>\n"
   "long fcvtzu_4s(long a) {\n"
   "  float fa; __builtin_memcpy(&fa, &a, 4);\n"
   "  float32x4_t va = {fa, 2.7f, 3.9f, 0.0f};\n"
   "  uint32x4_t vr = vcvtq_u32_f32(va);\n"
   "  return (long)vgetq_lane_u32(vr, 0);\n"
   "}\n",
   {0x40B33333ULL}, "FCvt", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// REV64 / REV32 / REV16 (vector) — reverse elements
// ============================================================================
static const std::vector<RoundTripTC> kRev = {
  {"rev64_4s",
   "#include <arm_neon.h>\n"
   "long rev64_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a, (int)b, 3, 4};\n"
   "  int32x4_t vr = vrev64q_s32(va);\n"
   "  return (long)(unsigned)vgetq_lane_s32(vr, 0);\n"
   "}\n",
   {10, 20}, "Rev", 1, "-march=armv8-a+simd"},

  {"rev32_8h",
   "#include <arm_neon.h>\n"
   "long rev32_8h(long a, long b) {\n"
   "  int16x8_t va = {(short)a, (short)b, 3, 4, 5, 6, 7, 8};\n"
   "  int16x8_t vr = vrev32q_s16(va);\n"
   "  return (long)(unsigned short)vgetq_lane_s16(vr, 0);\n"
   "}\n",
   {0x1234, 0x5678}, "Rev", 1, "-march=armv8-a+simd"},
};

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
// FRECPE/FRSQRTE/FRECPS/FRSQRTS/URECPE — reciprocal estimate & Newton step.
// These are architecturally-defined approximations: compare exact result bit
// patterns (XOR'd) so original and recompiled must produce identical bits.
// ============================================================================
static const std::vector<RoundTripTC> kRecip = {
  {"frecpe_4s",
   "#include <arm_neon.h>\n"
   "long frecpe_4s(long a) {\n"
   "  float32x4_t v = {(float)(int)a, 2.0f, 4.0f, 8.0f};\n"
   "  uint32x4_t b = vreinterpretq_u32_f32(vrecpeq_f32(v));\n"
   "  return (long)(unsigned)(vgetq_lane_u32(b,0)^vgetq_lane_u32(b,1)\n"
   "                          ^vgetq_lane_u32(b,2)^vgetq_lane_u32(b,3));\n"
   "}\n",
   {3}, "Recip", 1, "-march=armv8-a+simd"},

  {"frecpe_2d",
   "#include <arm_neon.h>\n"
   "long frecpe_2d(long a) {\n"
   "  float64x2_t v = {(double)(int)a, 4.0};\n"
   "  uint64x2_t b = vreinterpretq_u64_f64(vrecpeq_f64(v));\n"
   "  return (long)(vgetq_lane_u64(b,0)^vgetq_lane_u64(b,1));\n"
   "}\n",
   {7}, "Recip", 1, "-march=armv8-a+simd"},

  {"frecps_4s",
   "#include <arm_neon.h>\n"
   "long frecps_4s(long a) {\n"
   "  float32x4_t v = {(float)(int)a, 2.0f, 4.0f, 8.0f};\n"
   "  float32x4_t e = vrecpeq_f32(v);\n"
   "  uint32x4_t b = vreinterpretq_u32_f32(vrecpsq_f32(v, e));\n"
   "  return (long)(unsigned)(vgetq_lane_u32(b,0)^vgetq_lane_u32(b,3));\n"
   "}\n",
   {5}, "Recip", 1, "-march=armv8-a+simd"},

  {"frsqrte_4s",
   "#include <arm_neon.h>\n"
   "long frsqrte_4s(long a) {\n"
   "  float32x4_t v = {(float)((int)a & 0x7fff)+1.0f, 4.0f, 16.0f, 64.0f};\n"
   "  uint32x4_t b = vreinterpretq_u32_f32(vrsqrteq_f32(v));\n"
   "  return (long)(unsigned)(vgetq_lane_u32(b,0)^vgetq_lane_u32(b,1)\n"
   "                          ^vgetq_lane_u32(b,2)^vgetq_lane_u32(b,3));\n"
   "}\n",
   {9}, "Recip", 1, "-march=armv8-a+simd"},

  {"frsqrts_4s",
   "#include <arm_neon.h>\n"
   "long frsqrts_4s(long a) {\n"
   "  float32x4_t v = {(float)((int)a & 0x7fff)+1.0f, 4.0f, 16.0f, 64.0f};\n"
   "  float32x4_t e = vrsqrteq_f32(v);\n"
   "  uint32x4_t b = vreinterpretq_u32_f32(vrsqrtsq_f32(v, e));\n"
   "  return (long)(unsigned)(vgetq_lane_u32(b,0)^vgetq_lane_u32(b,3));\n"
   "}\n",
   {6}, "Recip", 1, "-march=armv8-a+simd"},

  {"urecpe_2s",
   "#include <arm_neon.h>\n"
   "long urecpe_2s(long a) {\n"
   "  uint32x2_t v = {(unsigned)a | 0x80000000u, 0xC0000000u};\n"
   "  uint32x2_t r = vrecpe_u32(v);\n"
   "  return (long)(unsigned)(vget_lane_u32(r,0)^vget_lane_u32(r,1));\n"
   "}\n",
   {0x12345678}, "Recip", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// PMULL / PMULL2 — polynomial (carry-less) multiply long (p8 and p64).
// ============================================================================
static const std::vector<RoundTripTC> kPmull = {
  {"pmull_p8",
   "#include <arm_neon.h>\n"
   "long pmull_p8(long a) {\n"
   "  poly8x8_t x = {(unsigned char)a, 2, 3, 4, 5, 6, 7, 8};\n"
   "  poly8x8_t y = {9, 10, 11, 12, 13, 14, 15, 16};\n"
   "  uint16x8_t b = vreinterpretq_u16_p16(vmull_p8(x, y));\n"
   "  unsigned short o[8]; vst1q_u16(o, b);\n"
   "  unsigned s = 0;\n"
   "  for (int i=0;i<8;i++) s = s*131 + o[i];\n"
   "  return (long)s;\n"
   "}\n",
   {0x5B}, "Pmull", 1, "-march=armv8-a+simd"},

  {"pmull2_p8",
   "#include <arm_neon.h>\n"
   "long pmull2_p8(long a) {\n"
   "  poly8x16_t x = {(unsigned char)a,2,3,4,5,6,7,8,17,18,19,20,21,22,23,24};\n"
   "  poly8x16_t y = {9,10,11,12,13,14,15,16,25,26,27,28,29,30,31,32};\n"
   "  uint16x8_t b = vreinterpretq_u16_p16(vmull_high_p8(x, y));\n"
   "  unsigned short o[8]; vst1q_u16(o, b);\n"
   "  unsigned s = 0;\n"
   "  for (int i=0;i<8;i++) s = s*131 + o[i];\n"
   "  return (long)s;\n"
   "}\n",
   {0x3C}, "Pmull", 1, "-march=armv8-a+simd"},

  {"pmull_p64",
   "#include <arm_neon.h>\n"
   "long pmull_p64(long a) {\n"
   "  poly64_t x = (poly64_t)(unsigned long)(a | 0x100000001ULL);\n"
   "  poly64_t y = (poly64_t)0xABCD1234ULL;\n"
   "  poly128_t r = vmull_p64(x, y);\n"
   "  uint64x2_t b = vreinterpretq_u64_p128(r);\n"
   "  return (long)(vgetq_lane_u64(b,0) ^ vgetq_lane_u64(b,1));\n"
   "}\n",
   {0x9}, "Pmull", 1, "-march=armv8-a+aes"},
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
INSTANTIATE_TEST_SUITE_P(Pmull, A64NEONAdvOpsRT,
                         ::testing::ValuesIn(kPmull), rtTCName);
INSTANTIATE_TEST_SUITE_P(Recip, A64NEONAdvOpsRT,
                         ::testing::ValuesIn(kRecip), rtTCName);
INSTANTIATE_TEST_SUITE_P(SatNegAbs, A64NEONAdvOpsRT,
                         ::testing::ValuesIn(kSatNegAbs), rtTCName);
INSTANTIATE_TEST_SUITE_P(ZipUzp, A64NEONAdvOpsRT,
                         ::testing::ValuesIn(kZipUzp), rtTCName);
INSTANTIATE_TEST_SUITE_P(TblTbx, A64NEONAdvOpsRT,
                         ::testing::ValuesIn(kTblTbx), rtTCName);
INSTANTIATE_TEST_SUITE_P(Ext, A64NEONAdvOpsRT,
                         ::testing::ValuesIn(kExt), rtTCName);
INSTANTIATE_TEST_SUITE_P(Cnt, A64NEONAdvOpsRT,
                         ::testing::ValuesIn(kCnt), rtTCName);
INSTANTIATE_TEST_SUITE_P(PairwiseAdd, A64NEONAdvOpsRT,
                         ::testing::ValuesIn(kPairwiseAdd), rtTCName);
INSTANTIATE_TEST_SUITE_P(Narrow, A64NEONAdvOpsRT,
                         ::testing::ValuesIn(kNarrow), rtTCName);
INSTANTIATE_TEST_SUITE_P(FCvt, A64NEONAdvOpsRT,
                         ::testing::ValuesIn(kFCvt), rtTCName);
INSTANTIATE_TEST_SUITE_P(Rev, A64NEONAdvOpsRT,
                         ::testing::ValuesIn(kRev), rtTCName);
