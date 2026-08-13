//===- ARM32_NEONHalvingNarrowRTTests.cpp - VHSUB/VSUBHN ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for ARM32 NEON halving-subtract (VHSUB) and subtract-and-
// narrow-high-half (VSUBHN/VRSUBHN), which were lifted as full-width INT_SUB
// placeholders (no halving / no narrowing / cross-lane borrow).  Mirrors the
// already-correct VHADD / VADDHN handlers.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32HalvingNarrowRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32HalvingNarrowRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kHalvingNarrow = {
  // VHSUB signed 2x i32 (d-reg): negative diffs exercise the arithmetic shift.
  {"vhsub_s32",
   "#include <arm_neon.h>\n"
   "int p_vhsub_s32(int a) {\n"
   "  int x = a;\n"
   "  int32x2_t va = {x, -x};\n"
   "  int32x2_t vb = {-1000000, 1000000};\n"
   "  int32x2_t vr = vhsub_s32(va, vb);\n"
   "  return (int)(vget_lane_s32(vr,0) ^ vget_lane_s32(vr,1));\n"
   "}\n",
   {123456}, "HalvingNarrow", 1, "-mfpu=neon"},

  // VHSUB signed 16x i8 (q-reg).
  {"vhsub_s8q",
   "#include <arm_neon.h>\n"
   "int p_vhsub_s8q(int a) {\n"
   "  signed char x = (signed char)a;\n"
   "  int8x16_t va = {x,(signed char)-x,100,-100,1,-1,127,-128,\n"
   "                  x,(signed char)-x,100,-100,1,-1,127,-128};\n"
   "  int8x16_t vb = {-100,100,(signed char)x,(signed char)-x,-1,1,-128,127,\n"
   "                  -100,100,(signed char)x,(signed char)-x,-1,1,-128,127};\n"
   "  int8x16_t vr = vhsubq_s8(va, vb);\n"
   "  unsigned r = 0;\n"
   "  for (int i=0;i<16;i++) r = (r*131) ^ (unsigned char)vr[i];\n"
   "  return (int)r;\n"
   "}\n",
   {41}, "HalvingNarrow", 1, "-mfpu=neon"},

  // VHSUB unsigned 4x i16 (d-reg): a<b cases wrap then logical-shift.
  {"vhsub_u16",
   "#include <arm_neon.h>\n"
   "int p_vhsub_u16(int a) {\n"
   "  unsigned short x = (unsigned short)a;\n"
   "  uint16x4_t va = {x, 0, 65535, 10};\n"
   "  uint16x4_t vb = {0, x, 1, 65535};\n"
   "  uint16x4_t vr = vhsub_u16(va, vb);\n"
   "  unsigned r = 0;\n"
   "  for (int i=0;i<4;i++) r = (r<<5) ^ vr[i];\n"
   "  return (int)r;\n"
   "}\n",
   {4321}, "HalvingNarrow", 1, "-mfpu=neon"},

  // VSUBHN: subtract i32 lanes, return high i16 (narrowing) — 4 lanes.
  {"vsubhn_s32",
   "#include <arm_neon.h>\n"
   "int p_vsubhn_s32(int a) {\n"
   "  int x = a;\n"
   "  int32x4_t va = {x*1024, -x*1024, 1000000, -1000000};\n"
   "  int32x4_t vb = {-1000000, 1000000, x*1024, -x*1024};\n"
   "  int16x4_t vr = vsubhn_s32(va, vb);\n"
   "  unsigned r = 0;\n"
   "  for (int i=0;i<4;i++) r = (r<<5) ^ (unsigned short)vr[i];\n"
   "  return (int)r;\n"
   "}\n",
   {321}, "HalvingNarrow", 1, "-mfpu=neon"},

  // VRSUBHN: rounding subtract-narrow-high, u16->u8, 8 lanes.
  {"vrsubhn_u16",
   "#include <arm_neon.h>\n"
   "int p_vrsubhn_u16(int a) {\n"
   "  unsigned short x = (unsigned short)a;\n"
   "  uint16x8_t va = {x,(unsigned short)(x+300),60000,128,2,40000,255,1000};\n"
   "  uint16x8_t vb = {128,2,(unsigned short)(x+100),x,40000,2,1000,255};\n"
   "  uint8x8_t vr = vrsubhn_u16(va, vb);\n"
   "  unsigned r = 0;\n"
   "  for (int i=0;i<8;i++) r = (r<<4) ^ vr[i];\n"
   "  return (int)r;\n"
   "}\n",
   {1500}, "HalvingNarrow", 1, "-mfpu=neon"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(HalvingNarrow, ARM32HalvingNarrowRT,
                         ::testing::ValuesIn(kHalvingNarrow), rtTCName);
