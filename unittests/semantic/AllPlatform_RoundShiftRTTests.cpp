//===- AllPlatform_RoundShiftRTTests.cpp - NEON rounding right shift -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for NEON rounding right shift by immediate, which adds a
// `1 << (n-1)` bias before the shift.  Two systematic gaps are covered:
//   * ARM32 VRSHR / VRSRA dropped the rounding bias entirely (treated as the
//     non-rounding VSHR / VSRA).
//   * Both ARM32 and AArch64 must add the bias in precision wider than the
//     lane, matching Unicorn's neon_rshl / handle_shri_with_rndacc, otherwise
//     a max-range lane (0xFFFFFFFF >> 1) overflows the lane and yields 0.
//
// Lanes are combined with a position-weighted fold (r*131+lane) rather than
// XOR so a difference in one lane cannot cancel against another.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32RoundShiftRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32RoundShiftRT, Verify) { roundTripARM32(GetParam()); }

class A64RoundShiftRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64RoundShiftRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kArmRoundShift = {
  // VRSHR.u32 #4 — rounding bias dropped: 0x78 rounds to 8, not 7.
  {"vrshr_u32_round",
   "#include <arm_neon.h>\n"
   "long vrshr_u32_round(long a) {\n"
   "  uint32x4_t vn = {(unsigned)a, 0x78u, 0x1f, 0x88u};\n"
   "  uint32x4_t vr = vrshrq_n_u32(vn, 4);\n"
   "  unsigned o[4]; vst1q_u32(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<4;i++) r=r*131u+o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {0x78}, "ArmRoundShift", 1, ""},

  // VRSHR.u32 #1 — max-range lane: (0xFFFFFFFF+1)>>1 = 0x80000000 in wide
  // precision; lane-width addition would overflow to 0.
  {"vrshr_u32_edge",
   "#include <arm_neon.h>\n"
   "long vrshr_u32_edge(long a) {\n"
   "  uint32x4_t vn = {(unsigned)a, 0x12345679u, 0x80000001u, 0xFFFFFFFEu};\n"
   "  uint32x4_t vr = vrshrq_n_u32(vn, 1);\n"
   "  unsigned o[4]; vst1q_u32(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<4;i++) r=r*131u+o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {0xFFFFFFFF}, "ArmRoundShift", 1, ""},

  // VRSHR.s32 #4 — negative rounds toward nearest: -120 -> -7, not -8.
  {"vrshr_s32_round",
   "#include <arm_neon.h>\n"
   "long vrshr_s32_round(long a) {\n"
   "  int32x4_t vn = {(int)a, -7, 127, -129};\n"
   "  int32x4_t vr = vrshrq_n_s32(vn, 4);\n"
   "  int o[4]; vst1q_s32(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<4;i++) r=r*131u+(unsigned)o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {(uint64_t)-120}, "ArmRoundShift", 1, ""},

  // VRSHR.u16 #3 — 16-bit lane rounding across 8 lanes.
  {"vrshr_u16_round",
   "#include <arm_neon.h>\n"
   "long vrshr_u16_round(long a) {\n"
   "  uint16x8_t vn = {(unsigned short)a, 0x7,0x4,0xFFFF,0x123,0x8,0x80,0xFFFE};\n"
   "  uint16x8_t vr = vrshrq_n_u16(vn, 3);\n"
   "  unsigned short o[8]; vst1q_u16(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<8;i++) r=r*131u+o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {0x7FF}, "ArmRoundShift", 1, ""},

  // VRSHR.u8 #2 — 8-bit lane: 0xFF >> 2 rounding = (0xFF+2)>>2 = 0x40.
  {"vrshr_u8_round",
   "#include <arm_neon.h>\n"
   "long vrshr_u8_round(long a) {\n"
   "  uint8x8_t vn = {(unsigned char)a, 0xFF, 0x7, 0x2, 0x6, 0x80, 0x1, 0xFE};\n"
   "  uint8x8_t vr = vrshr_n_u8(vn, 2);\n"
   "  unsigned char o[8]; vst1_u8(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<8;i++) r=r*131u+o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {0xFF}, "ArmRoundShift", 1, ""},

  // VRSRA.u32 #4 — accumulate: acc += round(src>>4).
  {"vrsra_u32_round",
   "#include <arm_neon.h>\n"
   "long vrsra_u32_round(long a) {\n"
   "  uint32x4_t vacc = {1000, 2000, 3000, 4000};\n"
   "  uint32x4_t vn = {(unsigned)a, 0x78u, 0x1f, 0x88u};\n"
   "  uint32x4_t vr = vrsraq_n_u32(vacc, vn, 4);\n"
   "  unsigned o[4]; vst1q_u32(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<4;i++) r=r*131u+o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {0x78}, "ArmRoundShift", 1, ""},

  // VRSRA.s32 #4 — signed accumulate with negative lanes.
  {"vrsra_s32_round",
   "#include <arm_neon.h>\n"
   "long vrsra_s32_round(long a) {\n"
   "  int32x4_t vacc = {1000, -2000, 3000, -4000};\n"
   "  int32x4_t vn = {(int)a, -7, 127, -129};\n"
   "  int32x4_t vr = vrsraq_n_s32(vacc, vn, 4);\n"
   "  int o[4]; vst1q_s32(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<4;i++) r=r*131u+(unsigned)o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {(uint64_t)-120}, "ArmRoundShift", 1, ""},

  // VRSRA.u32 #1 — accumulate with max-range lane (wide-precision rounding).
  {"vrsra_u32_edge",
   "#include <arm_neon.h>\n"
   "long vrsra_u32_edge(long a) {\n"
   "  uint32x4_t vacc = {1, 2, 3, 4};\n"
   "  uint32x4_t vn = {(unsigned)a, 0x80000001u, 0x12345679u, 0xFFFFFFFEu};\n"
   "  uint32x4_t vr = vrsraq_n_u32(vacc, vn, 1);\n"
   "  unsigned o[4]; vst1q_u32(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<4;i++) r=r*131u+o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {0xFFFFFFFF}, "ArmRoundShift", 1, ""},

  // VRSHL.s32 (register, negative count = rounding shift right) — was lumped
  // with non-rounding VSHL and dropped the bias.  Per-lane-varying counts force
  // a real register `vrshl` (uniform counts get folded to immediate vrshr).
  {"vrshl_s32_round",
   "#include <arm_neon.h>\n"
   "long vrshl_s32_round(long a) {\n"
   "  int32x4_t va = {(int)a, 0x88, 0x1F, -120};\n"
   "  int32x4_t vsh = {-4, -3, -5, -2};\n"
   "  int32x4_t vr = vrshlq_s32(va, vsh);\n"
   "  int o[4]; vst1q_s32(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<4;i++) r=r*131u+(unsigned)o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {0x78}, "ArmRoundShift", 1, ""},

  // VRSHL.u32 (register) negative count rounding right.
  {"vrshl_u32_round",
   "#include <arm_neon.h>\n"
   "long vrshl_u32_round(long a) {\n"
   "  uint32x4_t va = {(unsigned)a, 0x88u, 0x1Fu, 0x78u};\n"
   "  int32x4_t vsh = {-4, -3, -5, -2};\n"
   "  uint32x4_t vr = vrshlq_u32(va, vsh);\n"
   "  unsigned o[4]; vst1q_u32(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<4;i++) r=r*131u+o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {0x78}, "ArmRoundShift", 1, ""},

  // VRSHL.u32 negative count -1 on max-range lane: (0xFFFFFFFF+1)>>1 wide.
  // Non-uniform counts force a real register `vrshl`.
  {"vrshl_u32_edge",
   "#include <arm_neon.h>\n"
   "long vrshl_u32_edge(long a) {\n"
   "  uint32x4_t va = {(unsigned)a, 0x80000001u, 0x12345679u, 0xFFFFFFFEu};\n"
   "  int32x4_t vsh = {-1, -2, -3, -1};\n"
   "  uint32x4_t vr = vrshlq_u32(va, vsh);\n"
   "  unsigned o[4]; vst1q_u32(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<4;i++) r=r*131u+o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {0xFFFFFFFF}, "ArmRoundShift", 1, ""},

  // VRSHL.s32 positive count = plain left shift (control, no rounding).
  {"vrshl_s32_left",
   "#include <arm_neon.h>\n"
   "long vrshl_s32_left(long a) {\n"
   "  int32x4_t va = {(int)a, 0x88, 0x1F, -120};\n"
   "  int32x4_t vsh = {2, 3, 4, 1};\n"
   "  int32x4_t vr = vrshlq_s32(va, vsh);\n"
   "  int o[4]; vst1q_s32(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<4;i++) r=r*131u+(unsigned)o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {0x78}, "ArmRoundShift", 1, ""},
};

static const std::vector<RoundTripTC> kA64RoundShift = {
  // URSHR #1 — max-range lane overflow: must be 0x80000000, not 0 (lane-width
  // overflow) nor 0x7FFFFFFF (non-rounding).
  {"urshr_u32_edge",
   "#include <arm_neon.h>\n"
   "long urshr_u32_edge(long a) {\n"
   "  uint32x4_t vn = {(unsigned)a, 0x80000001u, 0x12345679u, 0xFFFFFFFEu};\n"
   "  uint32x4_t vr = vrshrq_n_u32(vn, 1);\n"
   "  unsigned o[4]; vst1q_u32(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<4;i++) r=r*131u+o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {0xFFFFFFFF}, "A64RoundShift", 1, "-march=armv8-a+simd"},

  // SRSHR #1 — signed max-range lane: INT_MAX/INT_MIN rounding.
  {"srshr_s32_edge",
   "#include <arm_neon.h>\n"
   "long srshr_s32_edge(long a) {\n"
   "  int32x4_t vn = {(int)a, 0x7FFFFFFF, (int)0x80000000, -1};\n"
   "  int32x4_t vr = vrshrq_n_s32(vn, 1);\n"
   "  int o[4]; vst1q_s32(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<4;i++) r=r*131u+(unsigned)o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {0x7FFFFFFF}, "A64RoundShift", 1, "-march=armv8-a+simd"},

  // URSRA #1 — accumulate with max-range lane.
  {"ursra_u32_edge",
   "#include <arm_neon.h>\n"
   "long ursra_u32_edge(long a) {\n"
   "  uint32x4_t vacc = {1, 2, 3, 4};\n"
   "  uint32x4_t vn = {(unsigned)a, 0x80000001u, 0x12345679u, 0xFFFFFFFEu};\n"
   "  uint32x4_t vr = vrsraq_n_u32(vacc, vn, 1);\n"
   "  unsigned o[4]; vst1q_u32(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<4;i++) r=r*131u+o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {0xFFFFFFFF}, "A64RoundShift", 1, "-march=armv8-a+simd"},

  // SRSRA #1 — signed accumulate with max-range lane.
  {"srsra_s32_edge",
   "#include <arm_neon.h>\n"
   "long srsra_s32_edge(long a) {\n"
   "  int32x4_t vacc = {1000, -2000, 3000, -4000};\n"
   "  int32x4_t vn = {(int)a, 0x7FFFFFFF, (int)0x80000000, -1};\n"
   "  int32x4_t vr = vrsraq_n_s32(vacc, vn, 1);\n"
   "  int o[4]; vst1q_s32(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<4;i++) r=r*131u+(unsigned)o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {0x7FFFFFFF}, "A64RoundShift", 1, "-march=armv8-a+simd"},

  // URSHR.u16 #1 — 16-bit max-range lane: (0xFFFF+1)>>1 = 0x8000.
  {"urshr_u16_edge",
   "#include <arm_neon.h>\n"
   "long urshr_u16_edge(long a) {\n"
   "  uint16x8_t vn = {(unsigned short)a,0xFFFE,0x8001,0x1235,0x7,0x4,0x80,0x1};\n"
   "  uint16x8_t vr = vrshrq_n_u16(vn, 1);\n"
   "  unsigned short o[8]; vst1q_u16(o, vr);\n"
   "  unsigned r=0; for(int i=0;i<8;i++) r=r*131u+o[i];\n"
   "  return (long)r;\n"
   "}\n",
   {0xFFFF}, "A64RoundShift", 1, "-march=armv8-a+simd"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ArmRoundShift, ARM32RoundShiftRT,
                         ::testing::ValuesIn(kArmRoundShift),
                         [](const auto &I) { return I.param.Name; });

INSTANTIATE_TEST_SUITE_P(A64RoundShift, A64RoundShiftRT,
                         ::testing::ValuesIn(kA64RoundShift),
                         [](const auto &I) { return I.param.Name; });
