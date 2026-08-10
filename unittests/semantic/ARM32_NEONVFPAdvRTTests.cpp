//===- ARM32_NEONVFPAdvRTTests.cpp - ARM32 NEON/VFP advanced roundtrip ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip tests for ARM32 NEON/VFP instructions not yet covered:
// VTBL, VTRN, VZIP, VSWP, VREV32/VREV64, VPMIN/VPMAX, VEXT,
// VRECPE, VCNT, VPADD, VFP advanced operations.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32NEONVFPAdvRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NEONVFPAdvRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

// ============================================================================
// VREV — reverse elements within containers
// ============================================================================
static const std::vector<RoundTripTC> kVRev = {
  {"vrev32_16_basic",
   "#include <arm_neon.h>\n"
   "int vrev32_16_basic(int a, int b) {\n"
   "  int16x4_t va = {(short)a, (short)b, 3, 4};\n"
   "  int16x4_t vr = vrev32_s16(va);\n"
   "  return (int)(unsigned short)vget_lane_s16(vr, 0);\n"
   "}\n",
   {0x1234, 0x5678}, "VRev", 1, "-mfpu=neon -mfloat-abi=softfp"},

  {"vrev64_32_basic",
   "#include <arm_neon.h>\n"
   "int vrev64_32_basic(int a, int b) {\n"
   "  int32x2_t va = {a, b};\n"
   "  int32x2_t vr = vrev64_s32(va);\n"
   "  return (int)vget_lane_s32(vr, 0);\n"
   "}\n",
   {10, 20}, "VRev", 1, "-mfpu=neon -mfloat-abi=softfp"},
};

// ============================================================================
// VEXT — extract (concat + shift)
// ============================================================================
static const std::vector<RoundTripTC> kVExt = {
  {"vext_8b_2",
   "#include <arm_neon.h>\n"
   "int vext_8b_2(int a, int b) {\n"
   "  uint8x8_t va = {(unsigned char)a, 1, 2, 3, 4, 5, 6, 7};\n"
   "  uint8x8_t vb = {(unsigned char)b, 11, 12, 13, 14, 15, 16, 17};\n"
   "  uint8x8_t vr = vext_u8(va, vb, 2);\n"
   "  return (int)vget_lane_u8(vr, 0);\n"
   "}\n",
   {0xAA, 0xBB}, "VExt", 1, "-mfpu=neon -mfloat-abi=softfp"},
};

// ============================================================================
// VPMIN / VPMAX — pairwise min/max
// ============================================================================
static const std::vector<RoundTripTC> kVPMinMax = {
  {"vpmin_s32",
   "#include <arm_neon.h>\n"
   "int test_vpmin_s32(int a, int b) {\n"
   "  int32x2_t va = {a, b};\n"
   "  int32x2_t vb = {100, 200};\n"
   "  int32x2_t vr = vpmin_s32(va, vb);\n"
   "  return (int)vget_lane_s32(vr, 0);\n"
   "}\n",
   {5, 15}, "VPMinMax", 1, "-mfpu=neon -mfloat-abi=softfp"},

  {"vpmax_u32",
   "#include <arm_neon.h>\n"
   "int test_vpmax_u32(int a, int b) {\n"
   "  uint32x2_t va = {(unsigned)a, (unsigned)b};\n"
   "  uint32x2_t vb = {100, 200};\n"
   "  uint32x2_t vr = vpmax_u32(va, vb);\n"
   "  return (int)vget_lane_u32(vr, 0);\n"
   "}\n",
   {30, 70}, "VPMinMax", 1, "-mfpu=neon -mfloat-abi=softfp"},
};

// ============================================================================
// VPADD — pairwise add
// ============================================================================
static const std::vector<RoundTripTC> kVPadd = {
  {"vpadd_s32",
   "#include <arm_neon.h>\n"
   "int test_vpadd_s32(int a, int b) {\n"
   "  int32x2_t va = {a, b};\n"
   "  int32x2_t vb = {100, 200};\n"
   "  int32x2_t vr = vpadd_s32(va, vb);\n"
   "  return (int)vget_lane_s32(vr, 0);\n"
   "}\n",
   {11, 22}, "VPadd", 1, "-mfpu=neon -mfloat-abi=softfp"},

  {"vpadd_f32",
   "#include <arm_neon.h>\n"
   "int test_vpadd_f32(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  float32x2_t va = {fa, fb};\n"
   "  float32x2_t vr = vpadd_f32(va, va);\n"
   "  float r = vget_lane_f32(vr, 0);\n"
   "  int rv; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40400000, 0x40800000}, "VPadd", 1, "-mfpu=neon -mfloat-abi=softfp"},
};

// ============================================================================
// VCNT — count set bits per byte
// ============================================================================
static const std::vector<RoundTripTC> kVCnt = {
  {"vcnt_8b",
   "#include <arm_neon.h>\n"
   "int vcnt_8b(int a) {\n"
   "  uint8x8_t va = {(unsigned char)a, 0xFF, 0, 0x0F, 0, 0, 0, 0};\n"
   "  uint8x8_t vr = vcnt_u8(va);\n"
   "  return (int)vget_lane_u8(vr, 0);\n"
   "}\n",
   {0x55}, "VCnt", 1, "-mfpu=neon -mfloat-abi=softfp"},
};

// ============================================================================
// VFP advanced: VNMUL, VNMLA, VNMLS
// ============================================================================
static const std::vector<RoundTripTC> kVFPAdv = {
  {"vneg_f32",
   "int vneg_f32(int a) {\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  float r = -fa;\n"
   "  int rv; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40400000}, "VFPAdv", 1, "-mfpu=vfpv3-d16 -mfloat-abi=soft"},

  {"vabs_f32",
   "int vabs_f32(int a) {\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  float r = __builtin_fabsf(fa);\n"
   "  int rv; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0xC0400000}, "VFPAdv", 1, "-mfpu=vfpv3-d16 -mfloat-abi=soft"},

  // fa*fb, fa/fb and sqrtf lower to hardware VFP (vmul/vdiv/vsqrt.f32) only
  // under a hard/softfp float ABI; -mfloat-abi=soft would emit `bl __aeabi_*` /
  // `bl sqrtf` libcalls the bare-metal harness cannot resolve on the original
  // side, making the roundtrip vacuous.  softfp keeps integer arg passing while
  // computing in VFP, so both sides execute the real instruction in Unicorn.
  {"vmul_f32_arm",
   "int vmul_f32_arm(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  float r = fa * fb;\n"
   "  int rv; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40400000, 0x40000000}, "VFPAdv", 1, "-mfpu=vfpv3-d16 -mfloat-abi=softfp"},

  {"vdiv_f32_arm",
   "int vdiv_f32_arm(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  float r = fa / fb;\n"
   "  int rv; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x41200000, 0x40000000}, "VFPAdv", 1, "-mfpu=vfpv3-d16 -mfloat-abi=softfp"},

  {"vsqrt_f32_arm",
   "int vsqrt_f32_arm(int a) {\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  float r = __builtin_sqrtf(fa);\n"
   "  int rv; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x41100000}, "VFPAdv", 1, "-mfpu=vfpv3-d16 -mfloat-abi=softfp"},
};

// ============================================================================
// NEON integer widening: VMULL, VADDL, VSUBL
// ============================================================================
static const std::vector<RoundTripTC> kNEONWiden = {
  {"vmull_s16",
   "#include <arm_neon.h>\n"
   "int test_vmull_s16(int a, int b) {\n"
   "  int16x4_t va = {(short)a, 0, 0, 0};\n"
   "  int16x4_t vb = {(short)b, 0, 0, 0};\n"
   "  int32x4_t vr = vmull_s16(va, vb);\n"
   "  return vgetq_lane_s32(vr, 0);\n"
   "}\n",
   {100, 200}, "NEONWiden", 1, "-mfpu=neon -mfloat-abi=softfp"},

  {"vaddl_u16",
   "#include <arm_neon.h>\n"
   "int test_vaddl_u16(int a, int b) {\n"
   "  uint16x4_t va = {(unsigned short)a, 0, 0, 0};\n"
   "  uint16x4_t vb = {(unsigned short)b, 0, 0, 0};\n"
   "  uint32x4_t vr = vaddl_u16(va, vb);\n"
   "  return (int)vgetq_lane_u32(vr, 0);\n"
   "}\n",
   {60000, 50000}, "NEONWiden", 1, "-mfpu=neon -mfloat-abi=softfp"},
};

// ============================================================================
// VRECPE/VRECPS/VRSQRTE/VRSQRTS — NEON reciprocal & reciprocal-sqrt estimate
// and Newton-Raphson refinement step (the float-divide / rsqrt idiom).  These
// have an architecturally-defined approximation, so the recompiled code must
// execute the *same* NEON instruction (lifted via llvm.arm.neon.*) to match the
// original bit-for-bit under Unicorn.  Exercises both Q (v4f32) and D (v2f32).
// ============================================================================
static const std::vector<RoundTripTC> kVRecip = {
  // Reciprocal estimate only (isolates VRECPE on a Q register).
  {"vrecpe_q_estimate",
   "#include <arm_neon.h>\n"
   "int vrecpe_q_estimate(int a) {\n"
   "  int32x4_t iv = {(a&0x3f)+2, (a&0x3f)+8, (a&0x1f)+16, (a&0xf)+32};\n"
   "  float32x4_t v = vcvtq_f32_s32(iv);\n"
   "  float32x4_t e = vrecpeq_f32(v);\n"
   "  int32x4_t ir = vcvtq_s32_f32(vmulq_f32(vdupq_n_f32(100000.0f), e));\n"
   "  return vgetq_lane_s32(ir,0)+vgetq_lane_s32(ir,1)+vgetq_lane_s32(ir,2)+vgetq_lane_s32(ir,3);\n"
   "}\n",
   {0x35}, "VRecip", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // Full Newton-Raphson reciprocal: VRECPE + 2x VRECPS (binary step).
  {"vrecps_q_nr",
   "#include <arm_neon.h>\n"
   "int vrecps_q_nr(int a, int b) {\n"
   "  int32x4_t iv = {(a&0x3f)+3, (b&0x3f)+5, (a&0x1f)+7, (b&0x1f)+11};\n"
   "  float32x4_t v = vcvtq_f32_s32(iv);\n"
   "  float32x4_t e = vrecpeq_f32(v);\n"
   "  e = vmulq_f32(e, vrecpsq_f32(v, e));\n"
   "  e = vmulq_f32(e, vrecpsq_f32(v, e));\n"
   "  float32x4_t r = vmulq_f32(vdupq_n_f32(1000000.0f), e);\n"
   "  int32x4_t ir = vcvtq_s32_f32(r);\n"
   "  return vgetq_lane_s32(ir,0)+vgetq_lane_s32(ir,1)+vgetq_lane_s32(ir,2)+vgetq_lane_s32(ir,3);\n"
   "}\n",
   {0x2A, 0x57}, "VRecip", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // Reciprocal square root: VRSQRTE + VRSQRTS (binary step).
  {"vrsqrts_q_nr",
   "#include <arm_neon.h>\n"
   "int vrsqrts_q_nr(int a, int b) {\n"
   "  int32x4_t iv = {(a&0x3f)+4, (b&0x3f)+9, (a&0x1f)+16, (b&0x1f)+25};\n"
   "  float32x4_t v = vcvtq_f32_s32(iv);\n"
   "  float32x4_t e = vrsqrteq_f32(v);\n"
   "  e = vmulq_f32(e, vrsqrtsq_f32(vmulq_f32(v, e), e));\n"
   "  float32x4_t r = vmulq_f32(vdupq_n_f32(100000.0f), e);\n"
   "  int32x4_t ir = vcvtq_s32_f32(r);\n"
   "  return vgetq_lane_s32(ir,0)+vgetq_lane_s32(ir,1)+vgetq_lane_s32(ir,2)+vgetq_lane_s32(ir,3);\n"
   "}\n",
   {0x33, 0x4C}, "VRecip", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // D-register (v2f32) reciprocal NR — exercises the 8-byte lane path.
  {"vrecps_d_nr",
   "#include <arm_neon.h>\n"
   "int vrecps_d_nr(int a, int b) {\n"
   "  int32x2_t iv = {(a&0x3f)+3, (b&0x3f)+5};\n"
   "  float32x2_t v = vcvt_f32_s32(iv);\n"
   "  float32x2_t e = vrecpe_f32(v);\n"
   "  e = vmul_f32(e, vrecps_f32(v, e));\n"
   "  e = vmul_f32(e, vrecps_f32(v, e));\n"
   "  float32x2_t r = vmul_f32(vdup_n_f32(1000000.0f), e);\n"
   "  int32x2_t ir = vcvt_s32_f32(r);\n"
   "  return vget_lane_s32(ir,0)+vget_lane_s32(ir,1);\n"
   "}\n",
   {0x11, 0x22}, "VRecip", 1, "-mfpu=neon -mfloat-abi=softfp"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(VRecip, ARM32NEONVFPAdvRT,
                         ::testing::ValuesIn(kVRecip), rtTCName);
INSTANTIATE_TEST_SUITE_P(VRev, ARM32NEONVFPAdvRT,
                         ::testing::ValuesIn(kVRev), rtTCName);
INSTANTIATE_TEST_SUITE_P(VExt, ARM32NEONVFPAdvRT,
                         ::testing::ValuesIn(kVExt), rtTCName);
INSTANTIATE_TEST_SUITE_P(VPMinMax, ARM32NEONVFPAdvRT,
                         ::testing::ValuesIn(kVPMinMax), rtTCName);
INSTANTIATE_TEST_SUITE_P(VPadd, ARM32NEONVFPAdvRT,
                         ::testing::ValuesIn(kVPadd), rtTCName);
INSTANTIATE_TEST_SUITE_P(VCnt, ARM32NEONVFPAdvRT,
                         ::testing::ValuesIn(kVCnt), rtTCName);
INSTANTIATE_TEST_SUITE_P(VFPAdv, ARM32NEONVFPAdvRT,
                         ::testing::ValuesIn(kVFPAdv), rtTCName);
INSTANTIATE_TEST_SUITE_P(NEONWiden, ARM32NEONVFPAdvRT,
                         ::testing::ValuesIn(kNEONWiden), rtTCName);
