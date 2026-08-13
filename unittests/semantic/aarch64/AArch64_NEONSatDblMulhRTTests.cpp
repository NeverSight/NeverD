//===- AArch64_NEONSatDblMulhRTTests.cpp - SQDMULH/SQRDMULH edges *- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 NEON saturating doubling multiply returning the high half:
//   SQDMULH  Dst[i] = SignedSat((2*A[i]*B[i]) >> N)
//   SQRDMULH Dst[i] = SignedSat((2*A[i]*B[i] + 2^(N-1)) >> N)   (rounding)
//
// The ONLY input that saturates is A[i]==B[i]==INT_MIN, where the true product
// 2*MIN*MIN = 2^(2N-1) shifts to 2^(N-1) (== INT_MIN as a signed N-bit value)
// and MUST clamp up to INT_MAX.  The lifter special-cases this with a
// `both==MIN -> MAX` SELECT.  Existing SQDMULH/SQRDMULH coverage
// (AArch64_NEONAdvOpsRTTests) only ever used 0x4000_0000 / 0x4000 operands, so
// the INT_MIN saturation arm and the by-element source form had ZERO roundtrip
// coverage — a classic weak-test gap.  These probes drive:
//   * A==B==INT_MIN saturation for .4S and .8H, SQDMULH and SQRDMULH.
//   * the by-element form (`sqrdmulh Vd.4S,Vn.4S,Vm.S[idx]`) which reads the
//     selected lane via operand vector_index and broadcasts it to every lane.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64NEONSatDblMulhRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64NEONSatDblMulhRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {

  // ===== SQDMULH .4S, A==B==INT32_MIN -> saturate lane to INT32_MAX. =====
  // lane0: arg(INT_MIN)*INT_MIN -> sat MAX; lane1: same; lane2: MIN*0x4000_0000
  // (no sat); lane3: 0x4000_0000^2 (no sat).
  {"sqdmulh_4s_intmin",
   "#include <arm_neon.h>\n"
   "long f(long a){\n"
   "  int32x4_t va={(int)a,-0x7fffffff-1,-0x7fffffff-1,0x40000000};\n"
   "  int32x4_t vb={(int)a,-0x7fffffff-1, 0x40000000, 0x40000000};\n"
   "  int32x4_t vr=vqdmulhq_s32(va,vb);\n"
   "  int o[4]; vst1q_s32(o,vr);\n"
   "  return (long)(unsigned)(o[0]*7u+o[1]*13u+o[2]*17u+o[3]*23u);\n"
   "}\n",
   {0x80000000ULL}, "NEONSatDblMulh", 1, "-march=armv8-a+simd"},

  // ===== SQRDMULH .4S, A==B==INT32_MIN (rounding variant). =====
  {"sqrdmulh_4s_intmin",
   "#include <arm_neon.h>\n"
   "long f(long a){\n"
   "  int32x4_t va={(int)a,-0x7fffffff-1,-0x7fffffff-1,0x40000000};\n"
   "  int32x4_t vb={(int)a,-0x7fffffff-1, 0x40000000, 0x40000000};\n"
   "  int32x4_t vr=vqrdmulhq_s32(va,vb);\n"
   "  int o[4]; vst1q_s32(o,vr);\n"
   "  return (long)(unsigned)(o[0]*7u+o[1]*13u+o[2]*17u+o[3]*23u);\n"
   "}\n",
   {0x80000000ULL}, "NEONSatDblMulh", 1, "-march=armv8-a+simd"},

  // ===== SQDMULH .8H, A==B==INT16_MIN -> saturate lane to INT16_MAX. =====
  {"sqdmulh_8h_intmin",
   "#include <arm_neon.h>\n"
   "long f(long a){\n"
   "  int16x8_t va={(short)a,-0x8000,-0x8000,0x4000,100,-100,1,2};\n"
   "  int16x8_t vb={(short)a,-0x8000, 0x4000,0x4000,200, 300,3,4};\n"
   "  int16x8_t vr=vqdmulhq_s16(va,vb);\n"
   "  short o[8]; vst1q_s16(o,vr);\n"
   "  int s=0; for(int i=0;i<8;i++) s=s*31+o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {0x8000ULL}, "NEONSatDblMulh", 1, "-march=armv8-a+simd"},

  // ===== SQRDMULH .8H, A==B==INT16_MIN (rounding variant). =====
  {"sqrdmulh_8h_intmin",
   "#include <arm_neon.h>\n"
   "long f(long a){\n"
   "  int16x8_t va={(short)a,-0x8000,-0x8000,0x4000,100,-100,1,2};\n"
   "  int16x8_t vb={(short)a,-0x8000, 0x4000,0x4000,200, 300,3,4};\n"
   "  int16x8_t vr=vqrdmulhq_s16(va,vb);\n"
   "  short o[8]; vst1q_s16(o,vr);\n"
   "  int s=0; for(int i=0;i<8;i++) s=s*31+o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {0x8000ULL}, "NEONSatDblMulh", 1, "-march=armv8-a+simd"},

  // ===== By-element SQRDMULH .4S, Vm.S[0] broadcast (incl. INT_MIN lane). =====
  // The selected element (lane 0 of vb) multiplies every lane of va.  With vb[0]
  // == INT_MIN and va[2] == INT_MIN, lane2 hits the both-MIN saturation too.
  {"sqrdmulh_4s_byelem0",
   "#include <arm_neon.h>\n"
   "long f(long a){\n"
   "  int32x4_t va={(int)a,0x12345,-0x7fffffff-1,0x7fffffff};\n"
   "  int32x4_t vb={-0x7fffffff-1,2,3,4};\n"
   "  int32x4_t vr=vqrdmulhq_laneq_s32(va,vb,0);\n"
   "  int o[4]; vst1q_s32(o,vr);\n"
   "  return (long)(unsigned)(o[0]*7u+o[1]*13u+o[2]*17u+o[3]*23u);\n"
   "}\n",
   {0x40000000ULL}, "NEONSatDblMulh", 1, "-march=armv8-a+simd"},

  // By-element selecting a NON-zero lane index (idx=2) exercises the lane offset.
  {"sqdmulh_4s_byelem2",
   "#include <arm_neon.h>\n"
   "long f(long a){\n"
   "  int32x4_t va={(int)a,0x12345,-0x40000000,0x7fffffff};\n"
   "  int32x4_t vb={1,2,0x40000000,4};\n"
   "  int32x4_t vr=vqdmulhq_laneq_s32(va,vb,2);\n"
   "  int o[4]; vst1q_s32(o,vr);\n"
   "  return (long)(unsigned)(o[0]*7u+o[1]*13u+o[2]*17u+o[3]*23u);\n"
   "}\n",
   {0x20000000ULL}, "NEONSatDblMulh", 1, "-march=armv8-a+simd"},

  // ===== By-element SQRDMULH .8H, Vm.H[idx]. =====
  {"sqrdmulh_8h_byelem3",
   "#include <arm_neon.h>\n"
   "long f(long a){\n"
   "  int16x8_t va={(short)a,0x1234,-0x8000,0x7fff,100,-100,1,2};\n"
   "  int16x8_t vb={1,2,3,0x4000,5,6,7,8};\n"
   "  int16x8_t vr=vqrdmulhq_laneq_s16(va,vb,3);\n"
   "  short o[8]; vst1q_s16(o,vr);\n"
   "  int s=0; for(int i=0;i<8;i++) s=s*31+o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {0x4000ULL}, "NEONSatDblMulh", 1, "-march=armv8-a+simd"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONSatDblMulh, A64NEONSatDblMulhRT,
                         ::testing::ValuesIn(kA64), rtTCName);
