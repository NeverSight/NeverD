//===- AArch64_NEONPairwiseAbsdRTTests.cpp - pairwise / widening absd ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Probes AArch64 NEON instructions that were previously placeholders or
// half-implemented in the lifter:
//   SABDL/SABDL2/UABDL/UABDL2  (widening absolute difference)
//   SABAL/SABAL2/UABAL/UABAL2  (widening absolute difference accumulate)
//   SMAXP/SMINP/UMAXP/UMINP    (pairwise min/max)
//
// Each test builds varied per-lane inputs and reduces several result lanes so a
// full-width / wrong-lane lift would diverge from the original.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64NEONPairwiseAbsdRT : public SemanticRoundTripFixture,
                                  public ::testing::WithParamInterface<RoundTripTC> {
};
TEST_P(AArch64NEONPairwiseAbsdRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kAbsd = {
  // ===== SABDL / UABDL: widening absolute difference =====
  {"sabdl_s8",
   "#include <arm_neon.h>\n"
   "long sabdl_s8(long a, long b) {\n"
   "  int8x8_t va = {(signed char)a,(signed char)(a+1),(signed char)(a-5),7,-3,100,-100,0};\n"
   "  int8x8_t vb = {(signed char)b,3,(signed char)(b+2),-7,50,-100,100,9};\n"
   "  int16x8_t vr = vabdl_s8(va, vb);\n"
   "  return (long)vgetq_lane_s16(vr,0)+vgetq_lane_s16(vr,1)+vgetq_lane_s16(vr,2)\n"
   "        +vgetq_lane_s16(vr,4)+vgetq_lane_s16(vr,6)+vgetq_lane_s16(vr,7);\n"
   "}\n",
   {7, 200}, "Absd", 1, "-march=armv8-a+simd"},

  {"uabdl_u8",
   "#include <arm_neon.h>\n"
   "long uabdl_u8(long a, long b) {\n"
   "  uint8x8_t va = {(unsigned char)a,(unsigned char)(a+9),3,250,7,1,255,(unsigned char)b};\n"
   "  uint8x8_t vb = {(unsigned char)b,5,(unsigned char)(a+2),9,250,255,1,(unsigned char)a};\n"
   "  uint16x8_t vr = vabdl_u8(va, vb);\n"
   "  return (long)vgetq_lane_u16(vr,0)+vgetq_lane_u16(vr,1)+vgetq_lane_u16(vr,3)\n"
   "        +vgetq_lane_u16(vr,5)+vgetq_lane_u16(vr,6)+vgetq_lane_u16(vr,7);\n"
   "}\n",
   {200, 50}, "Absd", 1, "-march=armv8-a+simd"},

  {"sabdl_s16",
   "#include <arm_neon.h>\n"
   "long sabdl_s16(long a, long b) {\n"
   "  int16x4_t va = {(short)a,(short)(a+100),-300,(short)b};\n"
   "  int16x4_t vb = {(short)b,(short)(b-50),200,(short)a};\n"
   "  int32x4_t vr = vabdl_s16(va, vb);\n"
   "  return (long)vgetq_lane_s32(vr,0)+vgetq_lane_s32(vr,1)+vgetq_lane_s32(vr,2)\n"
   "        +vgetq_lane_s32(vr,3);\n"
   "}\n",
   {1000, 4000}, "Absd", 1, "-march=armv8-a+simd"},

  {"uabdl_u32",
   "#include <arm_neon.h>\n"
   "long uabdl_u32(long a, long b) {\n"
   "  uint32x2_t va = {(unsigned)a,(unsigned)b};\n"
   "  uint32x2_t vb = {(unsigned)b,(unsigned)a};\n"
   "  uint64x2_t vr = vabdl_u32(va, vb);\n"
   "  return (long)vgetq_lane_u64(vr,0)+(long)vgetq_lane_u64(vr,1);\n"
   "}\n",
   {0xF0000000ULL, 5}, "Absd", 1, "-march=armv8-a+simd"},

  // ===== SABDL2 / UABDL2: upper-half widening absolute difference =====
  {"sabdl2_s8",
   "#include <arm_neon.h>\n"
   "long sabdl2_s8(long a, long b) {\n"
   "  int8x16_t va = {0,0,0,0,0,0,0,0,(signed char)a,(signed char)(a+1),-9,7,-3,100,-100,0};\n"
   "  int8x16_t vb = {0,0,0,0,0,0,0,0,(signed char)b,3,(signed char)(b+2),-7,50,-100,100,9};\n"
   "  int16x8_t vr = vabdl_high_s8(va, vb);\n"
   "  return (long)vgetq_lane_s16(vr,0)+vgetq_lane_s16(vr,1)+vgetq_lane_s16(vr,2)\n"
   "        +vgetq_lane_s16(vr,4)+vgetq_lane_s16(vr,7);\n"
   "}\n",
   {12, 90}, "Absd", 1, "-march=armv8-a+simd"},

  {"uabdl2_u16",
   "#include <arm_neon.h>\n"
   "long uabdl2_u16(long a, long b) {\n"
   "  uint16x8_t va = {0,0,0,0,(unsigned short)a,(unsigned short)(a+7),60000,(unsigned short)b};\n"
   "  uint16x8_t vb = {0,0,0,0,(unsigned short)b,5,3,(unsigned short)a};\n"
   "  uint32x4_t vr = vabdl_high_u16(va, vb);\n"
   "  return (long)vgetq_lane_u32(vr,0)+vgetq_lane_u32(vr,1)+vgetq_lane_u32(vr,2)\n"
   "        +vgetq_lane_u32(vr,3);\n"
   "}\n",
   {1234, 60000}, "Absd", 1, "-march=armv8-a+simd"},

  // ===== SABAL / UABAL: widening absolute difference accumulate =====
  {"sabal_s8",
   "#include <arm_neon.h>\n"
   "long sabal_s8(long a, long b) {\n"
   "  int16x8_t acc = {10,-20,30,-40,50,-60,70,-80};\n"
   "  int8x8_t va = {(signed char)a,(signed char)(a+1),(signed char)(a-5),7,-3,100,-100,0};\n"
   "  int8x8_t vb = {(signed char)b,3,(signed char)(b+2),-7,50,-100,100,9};\n"
   "  int16x8_t vr = vabal_s8(acc, va, vb);\n"
   "  return (long)vgetq_lane_s16(vr,0)+vgetq_lane_s16(vr,1)+vgetq_lane_s16(vr,3)\n"
   "        +vgetq_lane_s16(vr,5)+vgetq_lane_s16(vr,7);\n"
   "}\n",
   {7, 200}, "Absd", 1, "-march=armv8-a+simd"},

  {"uabal_u16",
   "#include <arm_neon.h>\n"
   "long uabal_u16(long a, long b) {\n"
   "  uint32x4_t acc = {1000,2000,3000,4000};\n"
   "  uint16x4_t va = {(unsigned short)a,(unsigned short)(a+7),60000,(unsigned short)b};\n"
   "  uint16x4_t vb = {(unsigned short)b,5,3,(unsigned short)a};\n"
   "  uint32x4_t vr = vabal_u16(acc, va, vb);\n"
   "  return (long)vgetq_lane_u32(vr,0)+vgetq_lane_u32(vr,1)+vgetq_lane_u32(vr,2)\n"
   "        +vgetq_lane_u32(vr,3);\n"
   "}\n",
   {1234, 60000}, "Absd", 1, "-march=armv8-a+simd"},

  // ===== SMAXP / SMINP / UMAXP / UMINP: pairwise min/max =====
  {"smaxp_8b",
   "#include <arm_neon.h>\n"
   "long smaxp_8b(long a, long b) {\n"
   "  int8x8_t va = {(signed char)a,(signed char)(a+1),-50,50,7,-7,127,-128};\n"
   "  int8x8_t vb = {(signed char)b,(signed char)(b-3),100,-100,0,9,-1,1};\n"
   "  int8x8_t vr = vpmax_s8(va, vb);\n"
   "  long s=0; signed char t[8]; vst1_s8(t,vr);\n"
   "  for(int i=0;i<8;i++) s+=t[i]; return s;\n"
   "}\n",
   {5, 40}, "Absd", 1, "-march=armv8-a+simd"},

  {"sminp_4h",
   "#include <arm_neon.h>\n"
   "long sminp_4h(long a, long b) {\n"
   "  int16x4_t va = {(short)a,(short)(a+1),-3000,3000};\n"
   "  int16x4_t vb = {(short)b,(short)(b-7),100,-100};\n"
   "  int16x4_t vr = vpmin_s16(va, vb);\n"
   "  long s=0; short t[4]; vst1_s16(t,vr);\n"
   "  for(int i=0;i<4;i++) s+=t[i]; return s;\n"
   "}\n",
   {(uint64_t)(int64_t)-500, 800}, "Absd", 1, "-march=armv8-a+simd"},

  {"umaxp_8b",
   "#include <arm_neon.h>\n"
   "long umaxp_8b(long a, long b) {\n"
   "  uint8x8_t va = {(unsigned char)a,(unsigned char)(a+1),50,200,7,255,1,128};\n"
   "  uint8x8_t vb = {(unsigned char)b,(unsigned char)(b+3),100,9,0,254,2,127};\n"
   "  uint8x8_t vr = vpmax_u8(va, vb);\n"
   "  long s=0; unsigned char t[8]; vst1_u8(t,vr);\n"
   "  for(int i=0;i<8;i++) s+=t[i]; return s;\n"
   "}\n",
   {10, 240}, "Absd", 1, "-march=armv8-a+simd"},

  {"uminp_2s",
   "#include <arm_neon.h>\n"
   "long uminp_2s(long a, long b) {\n"
   "  uint32x2_t va = {(unsigned)a,(unsigned)b};\n"
   "  uint32x2_t vb = {(unsigned)(a+7),(unsigned)(b-3)};\n"
   "  uint32x2_t vr = vpmin_u32(va, vb);\n"
   "  long s=0; unsigned t[2]; vst1_u32(t,vr);\n"
   "  for(int i=0;i<2;i++) s+=t[i]; return s;\n"
   "}\n",
   {0x80000000ULL, 100}, "Absd", 1, "-march=armv8-a+simd"},

  // Q-form pairwise (16B / 8H / 4S inputs).
  {"smaxpq_4s",
   "#include <arm_neon.h>\n"
   "long smaxpq_4s(long a, long b) {\n"
   "  int32x4_t va = {(int)a,(int)(a+1),-100000,100000};\n"
   "  int32x4_t vb = {(int)b,(int)(b-7),7,-7};\n"
   "  int32x4_t vr = vpmaxq_s32(va, vb);\n"
   "  long s=0; int t[4]; vst1q_s32(t,vr);\n"
   "  for(int i=0;i<4;i++) s+=t[i]; return s;\n"
   "}\n",
   {(uint64_t)(int64_t)-9, 1234}, "Absd", 1, "-march=armv8-a+simd"},

  {"uminpq_8h",
   "#include <arm_neon.h>\n"
   "long uminpq_8h(long a, long b) {\n"
   "  uint16x8_t va = {(unsigned short)a,(unsigned short)(a+1),60000,3,7,9,11,13};\n"
   "  uint16x8_t vb = {(unsigned short)b,(unsigned short)(b+5),1,2,4,6,8,10};\n"
   "  uint16x8_t vr = vpminq_u16(va, vb);\n"
   "  long s=0; unsigned short t[8]; vst1q_u16(t,vr);\n"
   "  for(int i=0;i<8;i++) s+=t[i]; return s;\n"
   "}\n",
   {500, 40000}, "Absd", 1, "-march=armv8-a+simd"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONPairwiseAbsd, AArch64NEONPairwiseAbsdRT,
                         ::testing::ValuesIn(kAbsd), rtTCName);
