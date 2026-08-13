//===- AArch64_NEONSatAccWidenRTTests.cpp - SUQADD/USQADD/SHLL ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for AArch64 NEON saturating-accumulate and shift-left-long
// instructions that were lifted as full-width placeholders:
//   SUQADD / USQADD — saturating accumulate of opposite-signedness operand,
//   SHLL / SHLL2    — shift left long by element width (zero-extend + shift).
//
// All probes use boundary values that force saturation / the full shift so a
// non-saturating / non-shifting placeholder yields a different result.  Lanes
// are XOR-reduced for a bit-exact return value.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64SatAccWidenRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64SatAccWidenRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kSatAccWiden = {
  // SUQADD: signed dst + unsigned src, signed-saturating, 4x i32.
  {"suqadd_4s",
   "#include <arm_neon.h>\n"
   "long suqadd_4s(long a) {\n"
   "  int x = (int)a;\n"
   "  int32x4_t vd = {x + 2000000000, x - 2000000000, x, -x};\n"
   "  uint32x4_t vn = {2000000000u, 5u, 2000000000u, 2000000000u};\n"
   "  int32x4_t vr = vuqaddq_s32(vd, vn);\n"
   "  unsigned r = (unsigned)vgetq_lane_s32(vr,0)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,1)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,2)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {100}, "SatAccWiden", 1, "-march=armv8-a+simd"},

  // SUQADD 8x i16.
  {"suqadd_8h",
   "#include <arm_neon.h>\n"
   "long suqadd_8h(long a) {\n"
   "  short x = (short)a;\n"
   "  int16x8_t vd = {(short)(x+30000),(short)(x-30000),x,(short)-x,\n"
   "                  20000,(short)-20000,1,(short)-1};\n"
   "  uint16x8_t vn = {30000,5,30000,30000,30000,2,40000,40000};\n"
   "  int16x8_t vr = vuqaddq_s16(vd, vn);\n"
   "  unsigned r = 0;\n"
   "  for (int i=0;i<8;i++) r = (r<<3) ^ (unsigned short)vr[i];\n"
   "  return (long)r;\n"
   "}\n",
   {7}, "SatAccWiden", 1, "-march=armv8-a+simd"},

  // SUQADD 8x i8 (64-bit vector).
  {"suqadd_8b",
   "#include <arm_neon.h>\n"
   "long suqadd_8b(long a) {\n"
   "  signed char x = (signed char)a;\n"
   "  int8x8_t vd = {(signed char)(x+100),(signed char)(x-100),x,(signed char)-x,\n"
   "                 120,-120,1,-1};\n"
   "  uint8x8_t vn = {100,5,100,200,100,2,200,200};\n"
   "  int8x8_t vr = vuqadd_s8(vd, vn);\n"
   "  unsigned r = 0;\n"
   "  for (int i=0;i<8;i++) r = (r<<4) ^ (unsigned char)vr[i];\n"
   "  return (long)r;\n"
   "}\n",
   {9}, "SatAccWiden", 1, "-march=armv8-a+simd"},

  // USQADD: unsigned dst + signed src, unsigned-saturating, 4x i32.
  {"usqadd_4s",
   "#include <arm_neon.h>\n"
   "long usqadd_4s(long a) {\n"
   "  unsigned x = (unsigned)a;\n"
   "  uint32x4_t vd = {x + 4000000000u, x, 10u, 4000000000u};\n"
   "  int32x4_t vn = {1000000000, -2000000000, -2000000000, 1000000000};\n"
   "  uint32x4_t vr = vsqaddq_u32(vd, vn);\n"
   "  unsigned r = vgetq_lane_u32(vr,0) ^ vgetq_lane_u32(vr,1)\n"
   "             ^ vgetq_lane_u32(vr,2) ^ vgetq_lane_u32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {123}, "SatAccWiden", 1, "-march=armv8-a+simd"},

  // USQADD 16x i8 (full 128-bit vector).
  {"usqadd_16b",
   "#include <arm_neon.h>\n"
   "long usqadd_16b(long a) {\n"
   "  unsigned char x = (unsigned char)a;\n"
   "  uint8x16_t vd = {(unsigned char)(x+200),x,10,250,200,5,0,255,\n"
   "                   (unsigned char)(x+100),x,20,240,100,8,1,254};\n"
   "  int8x16_t vn = {100,-50,-100,50,100,-50,-100,50,\n"
   "                  100,-50,-100,50,100,-50,-100,50};\n"
   "  uint8x16_t vr = vsqaddq_u8(vd, vn);\n"
   "  unsigned r = 0;\n"
   "  for (int i=0;i<16;i++) r = (r*131) ^ vr[i];\n"
   "  return (long)r;\n"
   "}\n",
   {77}, "SatAccWiden", 1, "-march=armv8-a+simd"},

  // SHLL .8h <- .8b, shift #8 (byte << 8 in 16-bit lanes).
  {"shll_8h",
   "#include <arm_neon.h>\n"
   "long shll_8h(long a) {\n"
   "  unsigned char x = (unsigned char)a;\n"
   "  uint8x8_t v = {x,(unsigned char)(x+1),(unsigned char)(x+2),\n"
   "                 (unsigned char)(x+3),(unsigned char)(x+4),\n"
   "                 (unsigned char)(x+5),(unsigned char)(x+6),0xFF};\n"
   "  uint16x8_t r = vshll_n_u8(v, 8);\n"
   "  unsigned s = 0;\n"
   "  for (int i=0;i<8;i++) s = (s<<2) ^ vgetq_lane_u16(r,0) ^ r[i];\n"
   "  return (long)s;\n"
   "}\n",
   {0x41}, "SatAccWiden", 1, "-march=armv8-a+simd"},

  // SHLL .4s <- .4h, shift #16.
  {"shll_4s",
   "#include <arm_neon.h>\n"
   "long shll_4s(long a) {\n"
   "  unsigned short x = (unsigned short)a;\n"
   "  uint16x4_t v = {x,(unsigned short)(x+1),(unsigned short)(x+2),0xFFFF};\n"
   "  uint32x4_t r = vshll_n_u16(v, 16);\n"
   "  unsigned s = vgetq_lane_u32(r,0) ^ vgetq_lane_u32(r,1)\n"
   "             ^ vgetq_lane_u32(r,2) ^ vgetq_lane_u32(r,3);\n"
   "  return (long)s;\n"
   "}\n",
   {0x1234}, "SatAccWiden", 1, "-march=armv8-a+simd"},

  // SHLL .2d <- .2s, shift #32.
  {"shll_2d",
   "#include <arm_neon.h>\n"
   "long shll_2d(long a) {\n"
   "  unsigned x = (unsigned)a;\n"
   "  uint32x2_t v = {x, 0xFFFFFFFFu};\n"
   "  uint64x2_t r = vshll_n_u32(v, 32);\n"
   "  unsigned long s = vgetq_lane_u64(r,0) ^ (vgetq_lane_u64(r,1) >> 32);\n"
   "  return (long)s;\n"
   "}\n",
   {0xABCD}, "SatAccWiden", 1, "-march=armv8-a+simd"},

  // SHLL2 .8h <- high half of .16b, shift #8.
  {"shll2_8h",
   "#include <arm_neon.h>\n"
   "long shll2_8h(long a) {\n"
   "  unsigned char x = (unsigned char)a;\n"
   "  uint8x16_t v = {0,0,0,0,0,0,0,0,\n"
   "                  x,(unsigned char)(x+1),(unsigned char)(x+2),\n"
   "                  (unsigned char)(x+3),(unsigned char)(x+4),\n"
   "                  (unsigned char)(x+5),(unsigned char)(x+6),0xFF};\n"
   "  uint16x8_t r = vshll_high_n_u8(v, 8);\n"
   "  unsigned s = 0;\n"
   "  for (int i=0;i<8;i++) s = (s<<2) ^ r[i];\n"
   "  return (long)s;\n"
   "}\n",
   {0x55}, "SatAccWiden", 1, "-march=armv8-a+simd"},

  // SHSUB: signed halving subtract (a-b)>>1 (arithmetic), per lane.  Negative
  // diffs exercise the arithmetic shift; cross-lane borrow would corrupt a
  // full-width placeholder.
  {"shsub_4s",
   "#include <arm_neon.h>\n"
   "long shsub_4s(long a) {\n"
   "  int x = (int)a;\n"
   "  int32x4_t va = {x, -x, 1000000, -1000000};\n"
   "  int32x4_t vb = {-1000000, 1000000, x, -x};\n"
   "  int32x4_t vr = vhsubq_s32(va, vb);\n"
   "  unsigned r = (unsigned)vgetq_lane_s32(vr,0)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,1)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,2)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {12345}, "SatAccWiden", 1, "-march=armv8-a+simd"},

  // SHSUB signed 8x i8.
  {"shsub_8b",
   "#include <arm_neon.h>\n"
   "long shsub_8b(long a) {\n"
   "  signed char x = (signed char)a;\n"
   "  int8x8_t va = {x,(signed char)-x,100,-100,1,-1,127,-128};\n"
   "  int8x8_t vb = {-100,100,(signed char)x,(signed char)-x,-1,1,-128,127};\n"
   "  int8x8_t vr = vhsub_s8(va, vb);\n"
   "  unsigned r = 0;\n"
   "  for (int i=0;i<8;i++) r = (r<<4) ^ (unsigned char)vr[i];\n"
   "  return (long)r;\n"
   "}\n",
   {37}, "SatAccWiden", 1, "-march=armv8-a+simd"},

  // UHSUB: unsigned halving subtract (a-b)>>1 (logical), per lane.  a<b cases
  // wrap then logical-shift; a full-width placeholder borrows across lanes.
  {"uhsub_16b",
   "#include <arm_neon.h>\n"
   "long uhsub_16b(long a) {\n"
   "  unsigned char x = (unsigned char)a;\n"
   "  uint8x16_t va = {x,0,255,10,200,5,100,1, x,0,255,10,200,5,100,1};\n"
   "  uint8x16_t vb = {0,x,1,255,5,200,1,100, 0,x,1,255,5,200,1,100};\n"
   "  uint8x16_t vr = vhsubq_u8(va, vb);\n"
   "  unsigned r = 0;\n"
   "  for (int i=0;i<16;i++) r = (r*131) ^ vr[i];\n"
   "  return (long)r;\n"
   "}\n",
   {88}, "SatAccWiden", 1, "-march=armv8-a+simd"},

  // UHSUB unsigned 8x i16.
  {"uhsub_8h",
   "#include <arm_neon.h>\n"
   "long uhsub_8h(long a) {\n"
   "  unsigned short x = (unsigned short)a;\n"
   "  uint16x8_t va = {x,0,65535,10,60000,5,1,40000};\n"
   "  uint16x8_t vb = {0,x,1,65535,5,60000,40000,1};\n"
   "  uint16x8_t vr = vhsubq_u16(va, vb);\n"
   "  unsigned r = 0;\n"
   "  for (int i=0;i<8;i++) r = (r<<3) ^ vr[i];\n"
   "  return (long)r;\n"
   "}\n",
   {4321}, "SatAccWiden", 1, "-march=armv8-a+simd"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SatAccWiden, A64SatAccWidenRT,
                         ::testing::ValuesIn(kSatAccWiden), rtTCName);
