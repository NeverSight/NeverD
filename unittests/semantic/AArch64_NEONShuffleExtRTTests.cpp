//===- AArch64_NEONShuffleExtRTTests.cpp - NEON shuffle/ext roundtrip -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip tests for AArch64 NEON vector permutation, interleave,
// deinterleave, reverse, narrowing, widening, pairwise, min/max, abs-diff,
// and saturating add/sub instructions.
//
// NOTE: arm_neon.h is included in the C source string, not the test file,
// because clang cross-compiles the C source with -target aarch64-linux-gnu.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64NEONShuffleExtRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64NEONShuffleExtRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

// ============================================================================
// TRN1/TRN2 — transpose
// ============================================================================
static const std::vector<RoundTripTC> kTRN = {
  {"trn1_4s",
   "#include <arm_neon.h>\n"
   "long trn1_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a, 2, (int)b, 4};\n"
   "  int32x4_t vb = {10, 20, 30, 40};\n"
   "  int32x4_t r = vtrn1q_s32(va, vb);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {100, 200}, "TRN", 1, "-march=armv8-a+simd"},

  {"trn2_4s",
   "#include <arm_neon.h>\n"
   "long trn2_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a, 2, (int)b, 4};\n"
   "  int32x4_t vb = {10, 20, 30, 40};\n"
   "  int32x4_t r = vtrn2q_s32(va, vb);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {100, 200}, "TRN", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// ZIP1/ZIP2 — interleave
// ============================================================================
static const std::vector<RoundTripTC> kZIP = {
  {"zip1_4s",
   "#include <arm_neon.h>\n"
   "long zip1_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a, 2, 3, 4};\n"
   "  int32x4_t vb = {(int)b, 20, 30, 40};\n"
   "  int32x4_t r = vzip1q_s32(va, vb);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {100, 200}, "ZIP", 1, "-march=armv8-a+simd"},

  {"zip2_4s",
   "#include <arm_neon.h>\n"
   "long zip2_4s(long a, long b) {\n"
   "  int32x4_t va = {1, 2, (int)a, 4};\n"
   "  int32x4_t vb = {10, 20, (int)b, 40};\n"
   "  int32x4_t r = vzip2q_s32(va, vb);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {300, 400}, "ZIP", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// UZP1/UZP2 — de-interleave
// ============================================================================
static const std::vector<RoundTripTC> kUZP = {
  {"uzp1_4s",
   "#include <arm_neon.h>\n"
   "long uzp1_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a, 99, (int)b, 88};\n"
   "  int32x4_t vb = {10, 77, 20, 66};\n"
   "  int32x4_t r = vuzp1q_s32(va, vb);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {111, 222}, "UZP", 1, "-march=armv8-a+simd"},

  {"uzp2_4s",
   "#include <arm_neon.h>\n"
   "long uzp2_4s(long a, long b) {\n"
   "  int32x4_t va = {99, (int)a, 88, (int)b};\n"
   "  int32x4_t vb = {77, 10, 66, 20};\n"
   "  int32x4_t r = vuzp2q_s32(va, vb);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {111, 222}, "UZP", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// EXT — extract (byte-level concatenate+shift)
// ============================================================================
static const std::vector<RoundTripTC> kEXT = {
  {"ext_16b_shift4",
   "#include <arm_neon.h>\n"
   "long ext_16b_shift4(long a, long b) {\n"
   "  uint8x16_t va = {(unsigned char)a, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};\n"
   "  uint8x16_t vb = {(unsigned char)b, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 0, 0, 0, 0};\n"
   "  uint8x16_t r = vextq_u8(va, vb, 4);\n"
   "  return vgetq_lane_u8(r, 0);\n"
   "}\n",
   {0xAA, 0xBB}, "EXT", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// REV16/REV32/REV64 — reverse elements within groups
// ============================================================================
static const std::vector<RoundTripTC> kREV = {
  {"rev64_4s",
   "#include <arm_neon.h>\n"
   "long rev64_4s(long a, long b) {\n"
   "  int32x4_t v = {(int)a, (int)b, 30, 40};\n"
   "  int32x4_t r = vrev64q_s32(v);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {10, 20}, "REV", 1, "-march=armv8-a+simd"},

  {"rev32_8h",
   "#include <arm_neon.h>\n"
   "long rev32_8h(long a) {\n"
   "  int16x8_t v = {(short)a, 200, 300, 400, 500, 600, 700, 800};\n"
   "  int16x8_t r = vrev32q_s16(v);\n"
   "  return vgetq_lane_s16(r, 0);\n"
   "}\n",
   {100}, "REV", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// XTN / SQXTN — narrowing
// ============================================================================
static const std::vector<RoundTripTC> kXTN = {
  {"xtn_4s_to_4h",
   "#include <arm_neon.h>\n"
   "long xtn_4s_to_4h(long a) {\n"
   "  int32x4_t v = {(int)a, 200, -300, 40000};\n"
   "  int16x4_t r = vmovn_s32(v);\n"
   "  return vget_lane_s16(r, 0);\n"
   "}\n",
   {12345}, "XTN", 1, "-march=armv8-a+simd"},

  {"sqxtn_sat_clamp",
   "#include <arm_neon.h>\n"
   "long sqxtn_sat_clamp(long a) {\n"
   "  int32x4_t v = {(int)a, 50000, -50000, 0};\n"
   "  int16x4_t r = vqmovn_s32(v);\n"
   "  return vget_lane_s16(r, 0);\n"
   "}\n",
   {100}, "XTN", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// SADDL/UADDL — widening add
// ============================================================================
static const std::vector<RoundTripTC> kWideningAdd = {
  {"saddl_4h_to_4s",
   "#include <arm_neon.h>\n"
   "long saddl_4h_to_4s(long a, long b) {\n"
   "  int16x4_t va = {(short)a, 200, -300, 400};\n"
   "  int16x4_t vb = {(short)b, 100, -100, 50};\n"
   "  int32x4_t r = vaddl_s16(va, vb);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {1000, 2000}, "WideningAdd", 1, "-march=armv8-a+simd"},

  {"uaddl_8b_to_8h",
   "#include <arm_neon.h>\n"
   "long uaddl_8b_to_8h(long a, long b) {\n"
   "  uint8x8_t va = {(unsigned char)a, 200, 100, 50, 0, 0, 0, 0};\n"
   "  uint8x8_t vb = {(unsigned char)b, 100, 200, 50, 0, 0, 0, 0};\n"
   "  uint16x8_t r = vaddl_u8(va, vb);\n"
   "  return vgetq_lane_u16(r, 0);\n"
   "}\n",
   {200, 100}, "WideningAdd", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// SMULL/UMULL — widening multiply
// ============================================================================
static const std::vector<RoundTripTC> kWideningMul = {
  {"smull_4h_to_4s",
   "#include <arm_neon.h>\n"
   "long smull_4h_to_4s(long a, long b) {\n"
   "  int16x4_t va = {(short)a, 100, -200, 10};\n"
   "  int16x4_t vb = {(short)b, 3, -2, 5};\n"
   "  int32x4_t r = vmull_s16(va, vb);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {100, 7}, "WideningMul", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// ADDP — pairwise add
// ============================================================================
static const std::vector<RoundTripTC> kPairwise = {
  {"addp_4s",
   "#include <arm_neon.h>\n"
   "long addp_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a, (int)b, 30, 40};\n"
   "  int32x4_t vb = {1, 2, 3, 4};\n"
   "  int32x4_t r = vpaddq_s32(va, vb);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {100, 200}, "Pairwise", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// SMAX/SMIN/UMAX/UMIN per-lane
// ============================================================================
static const std::vector<RoundTripTC> kMinMax = {
  {"smax_4s",
   "#include <arm_neon.h>\n"
   "long smax_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a, -100, 50, 0};\n"
   "  int32x4_t vb = {(int)b, 100, -50, 0};\n"
   "  int32x4_t r = vmaxq_s32(va, vb);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {(uint64_t)(int64_t)-10, 20}, "MinMax", 1, "-march=armv8-a+simd"},

  {"umin_4s",
   "#include <arm_neon.h>\n"
   "long umin_4s(long a, long b) {\n"
   "  uint32x4_t va = {(unsigned)a, 100, 50, 0};\n"
   "  uint32x4_t vb = {(unsigned)b, 200, 25, 0};\n"
   "  uint32x4_t r = vminq_u32(va, vb);\n"
   "  return vgetq_lane_u32(r, 0);\n"
   "}\n",
   {30, 50}, "MinMax", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// SABD — absolute difference
// ============================================================================
static const std::vector<RoundTripTC> kABD = {
  {"sabd_4s",
   "#include <arm_neon.h>\n"
   "long sabd_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a, -10, 50, 0};\n"
   "  int32x4_t vb = {(int)b, 10, -50, 0};\n"
   "  int32x4_t r = vabdq_s32(va, vb);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {100, 30}, "ABD", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// SQADD / UQSUB — saturating add/sub
// ============================================================================
static const std::vector<RoundTripTC> kSatAddSub = {
  {"sqadd_4s",
   "#include <arm_neon.h>\n"
   "long sqadd_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a, 0x7FFFFFF0, -100, 0};\n"
   "  int32x4_t vb = {(int)b, 100, -2147483548, 0};\n"
   "  int32x4_t r = vqaddq_s32(va, vb);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {100, 200}, "SatAddSub", 1, "-march=armv8-a+simd"},

  {"uqsub_4s",
   "#include <arm_neon.h>\n"
   "long uqsub_4s(long a, long b) {\n"
   "  uint32x4_t va = {(unsigned)a, 10, 100, 0};\n"
   "  uint32x4_t vb = {(unsigned)b, 20, 50, 0};\n"
   "  uint32x4_t r = vqsubq_u32(va, vb);\n"
   "  return vgetq_lane_u32(r, 0);\n"
   "}\n",
   {50, 30}, "SatAddSub", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// CNT — per-byte popcount
// ============================================================================
static const std::vector<RoundTripTC> kCNT = {
  {"cnt_8b",
   "#include <arm_neon.h>\n"
   "long cnt_8b(long a) {\n"
   "  uint8x8_t v = {(unsigned char)a, 0xFF, 0x00, 0x0F, 0xAA, 0x55, 0x80, 0x01};\n"
   "  uint8x8_t r = vcnt_u8(v);\n"
   "  return vget_lane_u8(r, 0);\n"
   "}\n",
   {0x37}, "CNT", 1, "-march=armv8-a+simd"},
};

// ============================================================================
// Shift operations: SHL / USHR / SSHR / USRA
// ============================================================================
static const std::vector<RoundTripTC> kShifts = {
  {"ushr_4s",
   "#include <arm_neon.h>\n"
   "long ushr_4s(long a) {\n"
   "  uint32x4_t v = {(unsigned)a, 100, 0xFFFFFFFF, 0};\n"
   "  uint32x4_t r = vshrq_n_u32(v, 4);\n"
   "  return vgetq_lane_u32(r, 0);\n"
   "}\n",
   {0x12345678ULL}, "Shifts", 1, "-march=armv8-a+simd"},

  {"sshr_4s",
   "#include <arm_neon.h>\n"
   "long sshr_4s(long a) {\n"
   "  int32x4_t v = {(int)a, -100, 0x7FFFFFFF, 0};\n"
   "  int32x4_t r = vshrq_n_s32(v, 8);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {(uint64_t)(int64_t)-256}, "Shifts", 1, "-march=armv8-a+simd"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(TRN, A64NEONShuffleExtRT,
                         ::testing::ValuesIn(kTRN), rtTCName);
INSTANTIATE_TEST_SUITE_P(ZIP, A64NEONShuffleExtRT,
                         ::testing::ValuesIn(kZIP), rtTCName);
INSTANTIATE_TEST_SUITE_P(UZP, A64NEONShuffleExtRT,
                         ::testing::ValuesIn(kUZP), rtTCName);
INSTANTIATE_TEST_SUITE_P(EXT, A64NEONShuffleExtRT,
                         ::testing::ValuesIn(kEXT), rtTCName);
INSTANTIATE_TEST_SUITE_P(REV, A64NEONShuffleExtRT,
                         ::testing::ValuesIn(kREV), rtTCName);
INSTANTIATE_TEST_SUITE_P(XTN, A64NEONShuffleExtRT,
                         ::testing::ValuesIn(kXTN), rtTCName);
INSTANTIATE_TEST_SUITE_P(WideningAdd, A64NEONShuffleExtRT,
                         ::testing::ValuesIn(kWideningAdd), rtTCName);
INSTANTIATE_TEST_SUITE_P(WideningMul, A64NEONShuffleExtRT,
                         ::testing::ValuesIn(kWideningMul), rtTCName);
INSTANTIATE_TEST_SUITE_P(Pairwise, A64NEONShuffleExtRT,
                         ::testing::ValuesIn(kPairwise), rtTCName);
INSTANTIATE_TEST_SUITE_P(MinMax, A64NEONShuffleExtRT,
                         ::testing::ValuesIn(kMinMax), rtTCName);
INSTANTIATE_TEST_SUITE_P(ABD, A64NEONShuffleExtRT,
                         ::testing::ValuesIn(kABD), rtTCName);
INSTANTIATE_TEST_SUITE_P(SatAddSub, A64NEONShuffleExtRT,
                         ::testing::ValuesIn(kSatAddSub), rtTCName);
INSTANTIATE_TEST_SUITE_P(CNT, A64NEONShuffleExtRT,
                         ::testing::ValuesIn(kCNT), rtTCName);
INSTANTIATE_TEST_SUITE_P(Shifts, A64NEONShuffleExtRT,
                         ::testing::ValuesIn(kShifts), rtTCName);
