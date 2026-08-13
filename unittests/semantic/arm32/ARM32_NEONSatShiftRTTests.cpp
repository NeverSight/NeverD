//===- ARM32_NEONSatShiftRTTests.cpp - ARM32 NEON sat shift/mul roundtrip --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip tests for ARM32 NEON saturating doubling multiply, saturating /
// rounding shifts, narrowing saturating shifts and shift-insert, using edge
// values that force saturation:
//   VQDMULH/VQRDMULH/VQDMULL, VQSHL/VQSHLU(+imm), VQSHRN/VQRSHRN/VQSHRUN,
//   VSLI/VSRI.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32NEONSatShiftRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NEONSatShiftRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kArmSatShMul = {
  {"vqdmulh_s32",
   "#include <arm_neon.h>\n"
   "long probe_vqdmulh_s32(long a, long b) {\n"
   "  int32x4_t va = {(int)a, 0x40000000, -0x40000000, 0x7fffffff};\n"
   "  int32x4_t vb = {(int)b, 0x40000000, 0x40000000, 0x7fffffff};\n"
   "  int32x4_t vr = vqdmulhq_s32(va, vb);\n"
   "  int o[4]; vst1q_s32(o, vr);\n"
   "  return (long)(unsigned)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x7fffffff, 0x7fffffff}, "ArmSatShMul", 1, ""},

  {"vqrdmulh_s16",
   "#include <arm_neon.h>\n"
   "long probe_vqrdmulh_s16(long a, long b) {\n"
   "  int16x8_t va = {(short)a, 0x4000, -0x4000, 0x7fff, 100, -100, 1, 2};\n"
   "  int16x8_t vb = {(short)b, 0x4000, 0x4000, 0x7fff, 200, 300, 3, 4};\n"
   "  int16x8_t vr = vqrdmulhq_s16(va, vb);\n"
   "  short o[8]; vst1q_s16(o, vr);\n"
   "  int s=0; for(int i=0;i<8;i++) s = s*31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {0x7fff, 0x7fff}, "ArmSatShMul", 1, ""},

  {"vqdmull_s32",
   "#include <arm_neon.h>\n"
   "long probe_vqdmull_s32(long a, long b) {\n"
   "  int32x2_t va = {(int)a, -0x80000000};\n"
   "  int32x2_t vb = {(int)b, -0x80000000};\n"
   "  int64x2_t vr = vqdmull_s32(va, vb);\n"
   "  return (long)(vgetq_lane_s64(vr,0) ^ vgetq_lane_s64(vr,1));\n"
   "}\n",
   {0x12345, 0x6789A}, "ArmSatShMul", 1, ""},

  {"vqshl_s32",
   "#include <arm_neon.h>\n"
   "long probe_vqshl_s32(long a) {\n"
   "  int32x4_t va = {(int)a, 0x10000000, -0x10000000, 1};\n"
   "  int32x4_t vsh = {3, 8, 8, 40};\n"
   "  int32x4_t vr = vqshlq_s32(va, vsh);\n"
   "  int o[4]; vst1q_s32(o, vr);\n"
   "  return (long)(unsigned)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x20000000}, "ArmSatShMul", 1, ""},

  {"vqshl_u32",
   "#include <arm_neon.h>\n"
   "long probe_vqshl_u32(long a) {\n"
   "  uint32x4_t va = {(unsigned)a, 0x10000000u, 0xFF000000u, 1};\n"
   "  int32x4_t vsh = {3, 8, 8, 40};\n"
   "  uint32x4_t vr = vqshlq_u32(va, vsh);\n"
   "  unsigned o[4]; vst1q_u32(o, vr);\n"
   "  return (long)(unsigned)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x20000000}, "ArmSatShMul", 1, ""},

  {"vqshl_n_s32",
   "#include <arm_neon.h>\n"
   "long probe_vqshl_n_s32(long a) {\n"
   "  int32x4_t va = {(int)a, 0x10000000, -0x10000000, 7};\n"
   "  int32x4_t vr = vqshlq_n_s32(va, 5);\n"
   "  int o[4]; vst1q_s32(o, vr);\n"
   "  return (long)(unsigned)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x08000000}, "ArmSatShMul", 1, ""},

  {"vqshlu_n_s32",
   "#include <arm_neon.h>\n"
   "long probe_vqshlu_n_s32(long a) {\n"
   "  int32x4_t va = {(int)a, 0x10000000, -5, 1};\n"
   "  uint32x4_t vr = vqshluq_n_s32(va, 4);\n"
   "  unsigned o[4]; vst1q_u32(o, vr);\n"
   "  return (long)(unsigned)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x08000000}, "ArmSatShMul", 1, ""},

  {"vqshrn_n_s32",
   "#include <arm_neon.h>\n"
   "long probe_vqshrn_n_s32(long a) {\n"
   "  int32x4_t va = {(int)a, 0x7fffffff, -0x80000000, 100000};\n"
   "  int16x4_t vr = vqshrn_n_s32(va, 4);\n"
   "  short o[4]; vst1_s16(o, vr);\n"
   "  return (long)(unsigned short)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x40000}, "ArmSatShMul", 1, ""},

  {"vqrshrn_n_s32",
   "#include <arm_neon.h>\n"
   "long probe_vqrshrn_n_s32(long a) {\n"
   "  int32x4_t va = {(int)a, 0x7fffffff, -0x80000000, 99999};\n"
   "  int16x4_t vr = vqrshrn_n_s32(va, 5);\n"
   "  short o[4]; vst1_s16(o, vr);\n"
   "  return (long)(unsigned short)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x50000}, "ArmSatShMul", 1, ""},

  {"vqshrn_n_u16",
   "#include <arm_neon.h>\n"
   "long probe_vqshrn_n_u16(long a) {\n"
   "  uint16x8_t va = {(unsigned short)a, 0xFFFF, 0x8000, 0x1234, 1, 2, 3, 4};\n"
   "  uint8x8_t vr = vqshrn_n_u16(va, 3);\n"
   "  unsigned char o[8]; vst1_u8(o, vr);\n"
   "  int s=0; for(int i=0;i<8;i++) s = s*31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {0xFF00}, "ArmSatShMul", 1, ""},

  {"vqshrun_n_s32",
   "#include <arm_neon.h>\n"
   "long probe_vqshrun_n_s32(long a) {\n"
   "  int32x4_t va = {(int)a, 0x7fffffff, -0x80000000, 100000};\n"
   "  uint16x4_t vr = vqshrun_n_s32(va, 4);\n"
   "  unsigned short o[4]; vst1_u16(o, vr);\n"
   "  return (long)(unsigned)(o[0]^o[1]^o[2]^o[3]);\n"
   "}\n",
   {0x40000}, "ArmSatShMul", 1, ""},

  // VSLI/VSRI — per-lane shift-left/right and insert, keeping dst bits the
  // shift exposes.  Unblocked by #272c (Phase B3 now rebuilds a whole-Q read
  // from S-lane writes, so the vectors built from scalar args survive).
  {"vsli_n_u32",
   "#include <arm_neon.h>\n"
   "long probe_vsli_n_u32(long a, long b) {\n"
   "  uint32x4_t va = {(unsigned)a, (unsigned)b, (unsigned)(a ^ b), (unsigned)(a + b)};\n"
   "  uint32x4_t vb = {(unsigned)b, (unsigned)a, (unsigned)(a + b), (unsigned)(a ^ b)};\n"
   "  uint32x4_t vr = vsliq_n_u32(va, vb, 5);\n"
   "  unsigned o[4]; vst1q_u32(o, vr);\n"
   "  return (long)(unsigned)(o[0] + o[1]*3u + o[2]*5u + o[3]*7u);\n"
   "}\n",
   {0x12345678, 0x09ABCDEF}, "ArmSatShMul", 1, ""},

  {"vsri_n_u16",
   "#include <arm_neon.h>\n"
   "long probe_vsri_n_u16(long a, long b) {\n"
   "  uint16x8_t va = {(unsigned short)a, (unsigned short)b, 0x1234, 0xABCD, 1, 2, 3, 4};\n"
   "  uint16x8_t vb = {(unsigned short)b, (unsigned short)a, 0xFFFF, 0x8001, 5, 6, 7, 8};\n"
   "  uint16x8_t vr = vsriq_n_u16(va, vb, 6);\n"
   "  unsigned short o[8]; vst1q_u16(o, vr);\n"
   "  int s=0; for(int i=0;i<8;i++) s = s*31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {0xBEEF, 0xCAFE}, "ArmSatShMul", 1, ""},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ArmSatShMul, ARM32NEONSatShiftRT,
                         ::testing::ValuesIn(kArmSatShMul), rtTCName);
