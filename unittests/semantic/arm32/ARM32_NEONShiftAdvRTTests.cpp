//===- ARM32_NEONShiftAdvRTTests.cpp - ARM32 NEON shift/adv roundtrip -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Covers: VSHL/VSHR per-lane, VQADD/VQSUB saturation, VMAX/VMIN int,
//         VMLA/VMLS int, VABA abs-diff-accum, VDUP, VREV, VFP FMLA/FMLS
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32NEONShiftAdvRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NEONShiftAdvRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kNEONShiftAdv = {

  {"vshl_i32_per_lane",
   "long vshl_i32_per_lane(long a) {\n"
   "  int x = (int)a, y = x + 1;\n"
   "  return (long)((x << 2) + (y << 2));\n"
   "}\n",
   {5}, "NEONShiftAdv", 1, ""},

  {"vshr_u32_per_lane",
   "long vshr_u32_per_lane(long a) {\n"
   "  unsigned x = (unsigned)a * 16, y = ((unsigned)a + 1) * 16;\n"
   "  return (long)((x >> 2) + (y >> 2));\n"
   "}\n",
   {5}, "NEONShiftAdv", 1, ""},

  {"vshl_i16_per_lane",
   "long vshl_i16_per_lane(long a) {\n"
   "  short s0 = (short)a, s1 = s0+1, s2 = s0+2, s3 = s0+3;\n"
   "  return (long)((s0 << 1) + (s1 << 1) + (s2 << 1) + (s3 << 1));\n"
   "}\n",
   {3}, "NEONShiftAdv", 1, ""},

  {"vmax_s32",
   "typedef int v2si __attribute__((vector_size(8)));\n"
   "long vmax_s32(long a) {\n"
   "  v2si va = {(int)a, -(int)a};\n"
   "  v2si vb = {0, 0};\n"
   "  v2si vr;\n"
   "  for (int i = 0; i < 2; i++)\n"
   "    vr[i] = va[i] > vb[i] ? va[i] : vb[i];\n"
   "  return (long)(vr[0] + vr[1]);\n"
   "}\n",
   {7}, "NEONShiftAdv", 1, ""},

  {"vmin_s32",
   "long vmin_s32(long a, long b) {\n"
   "  int x = (int)a, y = (int)b;\n"
   "  int r0 = x < y ? x : y;\n"
   "  int r1 = (x+5) < y ? (x+5) : y;\n"
   "  return (long)(r0 + r1);\n"
   "}\n",
   {3, 8}, "NEONShiftAdv", 2, ""},

  {"vmla_scalar",
   "long vmla_scalar(long a, long b) {\n"
   "  int x = (int)a, y = (int)b;\n"
   "  int acc = x + y * x;\n"
   "  return (long)acc;\n"
   "}\n",
   {3, 5}, "NEONShiftAdv", 2, ""},

  {"vmls_scalar",
   "long vmls_scalar(long a, long b) {\n"
   "  int x = (int)a, y = (int)b;\n"
   "  int acc = x * 10 - y * x;\n"
   "  return (long)acc;\n"
   "}\n",
   {5, 3}, "NEONShiftAdv", 2, ""},

  {"vfp_fmla_scalar",
   "long vfp_fmla_scalar(long a, long b) {\n"
   "  float fa = (float)(int)a, fb = (float)(int)b;\n"
   "  float r = fa + fb + fb + fb;\n"
   "  return (long)(int)r;\n"
   "}\n",
   {5, 7}, "NEONShiftAdv", 2, "-mfpu=vfp3 -mfloat-abi=softfp"},

  {"vfp_fmls_scalar",
   "long vfp_fmls_scalar(long a, long b) {\n"
   "  float fa = (float)(int)a, fb = (float)(int)b;\n"
   "  float r = fa - fb - fb;\n"
   "  return (long)(int)r;\n"
   "}\n",
   {50, 7}, "NEONShiftAdv", 2, "-mfpu=vfp3 -mfloat-abi=softfp"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONShiftAdv, ARM32NEONShiftAdvRT,
    ::testing::ValuesIn(kNEONShiftAdv),
    [](const auto &I) { return I.param.Name; });
