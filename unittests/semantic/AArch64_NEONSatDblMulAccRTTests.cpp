//===- AArch64_NEONSatDblMulAccRTTests.cpp - SQDMLAL/SQDMLSL RT -*- C++ -*-===//
//
// AArch64 NEON saturating doubling multiply-accumulate LONG, per widened lane:
//
//   SQDMLAL Vd, Vn, Vm:  Vd[i] = SignedSat( Vd[i] + SignedSat(2 * Vn[i]*Vm[i]) )
//   SQDMLSL Vd, Vn, Vm:  Vd[i] = SignedSat( Vd[i] - SignedSat(2 * Vn[i]*Vm[i]) )
//
// Two saturations stack: the DOUBLED product saturates at the wide width, then
// the accumulate/subtract saturates again.  SQDMULL (the multiply-only sibling)
// has roundtrip coverage; the accumulating SQDMLAL/SQDMLSL — and their high-half
// SQDMLAL2/SQDMLSL2 forms — had none, even though the lifter implements them with
// a dedicated doubling + double-saturation path distinct from plain SMLAL.
//
// The discriminating corners:
//
//   1. Product saturation.  2 * (-0x8000)*(-0x8000) = 0x80000000 overflows i32, so
//      the 4H->4S form must clamp the product to 0x7FFFFFFF *before* accumulating
//      (the 2S->2D form's corner is 2*(-2^31)^2 = +2^63 -> 0x7FFF_FFFF_FFFF_FFFF).
//      A handler that doubles without saturating, or that saturates at the wrong
//      width, diverges here.
//   2. Accumulate saturation.  Vd seeded at INT_MAX with a positive product (or
//      INT_MIN with SQDMLSL) forces the *second* clamp; dropping it overflows.
//   3. SQDMLAL2/SQDMLSL2 read the HIGH half of the Vn/Vm 128-bit sources — a lane
//      that read the low half instead would multiply the wrong elements.
//
// Vectors are seeded from runtime arguments plus the corner constants so clang
// keeps the lanes (no constant-folded result), and every lane is folded into the
// return.  These are base ARMv8-A Advanced SIMD, native on the default Unicorn
// arm64 CPU; the oracle is original-Unicorn vs lifted-Unicorn.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64NEONSatDblMulAccRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64NEONSatDblMulAccRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {

  // ===== SQDMLAL 4S (from 4H): product-sat lane + accumulate-sat lane. =====
  // lane0: acc=a, prod=2*b*100 (no sat).
  // lane1: acc=INT_MAX, prod=sat(2*(-0x8000)*(-0x8000))=INT_MAX -> acc+prod sat.
  // lane2: acc=0, prod=2*(-0x8000)*0x7fff (negative, no sat).
  // lane3: acc=-1000, prod=2*0x7fff*0x7fff (large positive, no sat).
  {"sqdmlal_4s",
   "#include <arm_neon.h>\n"
   "long sqdmlal_4s(long a,long b){\n"
   "  int32x4_t acc={(int)a, 0x7fffffff, 0, -1000};\n"
   "  int16x4_t va={(short)b, -0x8000, -0x8000, 0x7fff};\n"
   "  int16x4_t vb={100, -0x8000, 0x7fff, 0x7fff};\n"
   "  int32x4_t vr=vqdmlal_s16(acc,va,vb);\n"
   "  int o[4]; vst1q_s32(o,vr);\n"
   "  return (long)(unsigned)(o[0]^(o[1]*3)^(o[2]*5)^(o[3]*7));\n"
   "}\n",
   {0x33445566ULL, 0x1234ULL}, "NEONSatDblMulAcc", 1, "-march=armv8-a+simd"},

  // ===== SQDMLSL 4S: subtract, with accumulate-sat at INT_MIN. =====
  // lane1: acc=INT_MIN, prod=sat(2*(-0x8000)*(-0x8000))=INT_MAX -> acc-prod sat low.
  {"sqdmlsl_4s",
   "#include <arm_neon.h>\n"
   "long sqdmlsl_4s(long a,long b){\n"
   "  int32x4_t acc={(int)a, (int)-0x80000000, 0, 1000};\n"
   "  int16x4_t va={(short)b, -0x8000, -0x8000, 0x7fff};\n"
   "  int16x4_t vb={100, -0x8000, 0x7fff, 0x7fff};\n"
   "  int32x4_t vr=vqdmlsl_s16(acc,va,vb);\n"
   "  int o[4]; vst1q_s32(o,vr);\n"
   "  return (long)(unsigned)(o[0]^(o[1]*3)^(o[2]*5)^(o[3]*7));\n"
   "}\n",
   {0x0BADF00DULL, 0x4321ULL}, "NEONSatDblMulAcc", 1, "-march=armv8-a+simd"},

  // ===== SQDMLAL 2D (from 2S): 64-bit product saturation corner. =====
  // lane1: 2*(-2^31)*(-2^31)=+2^63 -> sat 0x7FFF_FFFF_FFFF_FFFF, acc(INT64_MAX) -> sat.
  {"sqdmlal_2d",
   "#include <arm_neon.h>\n"
   "long sqdmlal_2d(long a,long b){\n"
   "  int64x2_t acc={(long)a, 0x7fffffffffffffffL};\n"
   "  int32x2_t va={(int)b, (int)-0x80000000};\n"
   "  int32x2_t vb={3, (int)-0x80000000};\n"
   "  int64x2_t vr=vqdmlal_s32(acc,va,vb);\n"
   "  return (long)(vgetq_lane_s64(vr,0) ^ (vgetq_lane_s64(vr,1)*3));\n"
   "}\n",
   {0x100000ULL, 0x6789AULL}, "NEONSatDblMulAcc", 1, "-march=armv8-a+simd"},

  // ===== SQDMLSL 2D: 64-bit subtract, INT64_MIN accumulate-sat. =====
  {"sqdmlsl_2d",
   "#include <arm_neon.h>\n"
   "long sqdmlsl_2d(long a,long b){\n"
   "  int64x2_t acc={(long)a, (long)0x8000000000000000UL};\n"
   "  int32x2_t va={(int)b, (int)-0x80000000};\n"
   "  int32x2_t vb={7, (int)-0x80000000};\n"
   "  int64x2_t vr=vqdmlsl_s32(acc,va,vb);\n"
   "  return (long)(vgetq_lane_s64(vr,0) ^ (vgetq_lane_s64(vr,1)*3));\n"
   "}\n",
   {0x200000ULL, 0x55555ULL}, "NEONSatDblMulAcc", 1, "-march=armv8-a+simd"},

  // ===== SQDMLAL2 4S: high-half lane selection (uses Vn/Vm lanes 4..7). =====
  {"sqdmlal2_4s",
   "#include <arm_neon.h>\n"
   "long sqdmlal2_4s(long a,long b){\n"
   "  int32x4_t acc={(int)a, 0, 0x7fffffff, -5};\n"
   "  int16x8_t va={1,2,3,4, (short)b, -0x8000, -0x8000, 0x100};\n"
   "  int16x8_t vb={5,6,7,8, 50,    -0x8000, 0x7fff,  0x100};\n"
   "  int32x4_t vr=vqdmlal_high_s16(acc,va,vb);\n"
   "  int o[4]; vst1q_s32(o,vr);\n"
   "  return (long)(unsigned)(o[0]^(o[1]*3)^(o[2]*5)^(o[3]*7));\n"
   "}\n",
   {0x223344ULL, 0x66ULL}, "NEONSatDblMulAcc", 1, "-march=armv8-a+simd"},

  // ===== SQDMLSL2 4S: high-half subtract. =====
  {"sqdmlsl2_4s",
   "#include <arm_neon.h>\n"
   "long sqdmlsl2_4s(long a,long b){\n"
   "  int32x4_t acc={(int)a, (int)-0x80000000, 0x100, -7};\n"
   "  int16x8_t va={9,8,7,6, (short)b, -0x8000, -0x8000, 0x40};\n"
   "  int16x8_t vb={4,3,2,1, 70,    -0x8000, 0x7fff,  0x40};\n"
   "  int32x4_t vr=vqdmlsl_high_s16(acc,va,vb);\n"
   "  int o[4]; vst1q_s32(o,vr);\n"
   "  return (long)(unsigned)(o[0]^(o[1]*3)^(o[2]*5)^(o[3]*7));\n"
   "}\n",
   {0x553311ULL, 0x77ULL}, "NEONSatDblMulAcc", 1, "-march=armv8-a+simd"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONSatDblMulAcc, A64NEONSatDblMulAccRT,
                         ::testing::ValuesIn(kA64), rtTCName);
