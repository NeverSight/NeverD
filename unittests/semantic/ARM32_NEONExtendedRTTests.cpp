//===- ARM32_NEONExtendedRTTests.cpp - ARM32 NEON extended roundtrip ------===//
//
// Roundtrip tests for ARM32 NEON instructions: vector zip/unzip/transpose,
// widening multiply, pairwise operations, saturating operations, narrowing,
// reverse, absolute value, and popcount.
//
// NOTE: arm_neon.h is included in the C source string (cross-compiled by
// clang -target armv7-linux-gnueabi), not in the test file itself.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32NEONExtRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NEONExtRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

// ============================================================================
// Interleave/deinterleave/transpose via C expressions
// ============================================================================
static const std::vector<RoundTripTC> kVZIPTRN = {
  {"c_interleave_swap",
   "int c_interleave_swap(int a, int b) {\n"
   "  int arr[4] = {a, b, 30, 40};\n"
   "  int tmp = arr[1]; arr[1] = arr[2]; arr[2] = tmp;\n"
   "  return arr[0] + arr[1];\n"
   "}\n",
   {100, 200}, "VZIPTRN", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"c_deinterleave",
   "int c_deinterleave(int a, int b) {\n"
   "  int even = a;\n"
   "  int odd = b;\n"
   "  return even + odd;\n"
   "}\n",
   {111, 222}, "VZIPTRN", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"c_transpose_pair",
   "int c_transpose_pair(int a, int b) {\n"
   "  return (a > b) ? a : b;\n"
   "}\n",
   {55, 66}, "VZIPTRN", 1, "-mfloat-abi=softfp -mfpu=neon"},
};

// ============================================================================
// VREV — reverse elements within groups
// ============================================================================
static const std::vector<RoundTripTC> kVREV = {
  {"vrev64_4h",
   "#include <arm_neon.h>\n"
   "int vrev64_4h(int a) {\n"
   "  int16x4_t v = {(short)a, 200, 300, 400};\n"
   "  int16x4_t r = vrev64_s16(v);\n"
   "  return vget_lane_s16(r, 0);\n"
   "}\n",
   {100}, "VREV", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"vrev32_4h",
   "#include <arm_neon.h>\n"
   "int vrev32_4h(int a) {\n"
   "  int16x4_t v = {(short)a, 200, 300, 400};\n"
   "  int16x4_t r = vrev32_s16(v);\n"
   "  return vget_lane_s16(r, 0);\n"
   "}\n",
   {100}, "VREV", 1, "-mfloat-abi=softfp -mfpu=neon"},
};

// ============================================================================
// VMULL — widening multiply
// ============================================================================
static const std::vector<RoundTripTC> kVMULL = {
  {"vmull_s16_rt",
   "#include <arm_neon.h>\n"
   "int vmull_s16_test(int a, int b) {\n"
   "  int16x4_t va = {(short)a, 100, -200, 10};\n"
   "  int16x4_t vb = {(short)b, 3, -2, 5};\n"
   "  int32x4_t r = vmull_s16(va, vb);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {50, 7}, "VMULL", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"vmull_u8_rt",
   "#include <arm_neon.h>\n"
   "int vmull_u8_test(int a, int b) {\n"
   "  uint8x8_t va = {(unsigned char)a, 200, 100, 50, 0, 0, 0, 0};\n"
   "  uint8x8_t vb = {(unsigned char)b, 2, 3, 4, 0, 0, 0, 0};\n"
   "  uint16x8_t r = vmull_u8(va, vb);\n"
   "  return (int)vgetq_lane_u16(r, 0);\n"
   "}\n",
   {10, 25}, "VMULL", 1, "-mfloat-abi=softfp -mfpu=neon"},
};

// ============================================================================
// VADDL / VSUBL — widening add/sub
// ============================================================================
static const std::vector<RoundTripTC> kVADDLSUBL = {
  {"vaddl_s16_rt",
   "#include <arm_neon.h>\n"
   "int vaddl_s16_test(int a, int b) {\n"
   "  int16x4_t va = {(short)a, 200, -300, 400};\n"
   "  int16x4_t vb = {(short)b, 100, -100, 50};\n"
   "  int32x4_t r = vaddl_s16(va, vb);\n"
   "  return vgetq_lane_s32(r, 0);\n"
   "}\n",
   {1000, 2000}, "VADDLSUBL", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"vsubl_u8_rt",
   "#include <arm_neon.h>\n"
   "int vsubl_u8_test(int a, int b) {\n"
   "  uint8x8_t va = {(unsigned char)a, 200, 100, 50, 0, 0, 0, 0};\n"
   "  uint8x8_t vb = {(unsigned char)b, 50, 50, 25, 0, 0, 0, 0};\n"
   "  uint16x8_t r = vsubl_u8(va, vb);\n"
   "  return (int)vgetq_lane_u16(r, 0);\n"
   "}\n",
   {200, 100}, "VADDLSUBL", 1, "-mfloat-abi=softfp -mfpu=neon"},
};

// ============================================================================
// VQADD / VQSUB — saturating add/sub
// ============================================================================
static const std::vector<RoundTripTC> kVQADDSUB = {
  {"c_add_clamp_simple",
   "int c_add_clamp_simple(int a, int b) {\n"
   "  int sum = a + b;\n"
   "  return sum;\n"
   "}\n",
   {100, 200}, "VQADDSUB", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"c_sat_sub_u8",
   "int c_sat_sub_u8(int a, int b) {\n"
   "  unsigned char va = (unsigned char)a;\n"
   "  unsigned char vb = (unsigned char)b;\n"
   "  return (va > vb) ? (va - vb) : 0;\n"
   "}\n",
   {50, 30}, "VQADDSUB", 1, "-mfloat-abi=softfp -mfpu=neon"},
};

// ============================================================================
// VPMAX / VPMIN — pairwise max/min
// ============================================================================
static const std::vector<RoundTripTC> kVPMAXMIN = {
  {"vpmax_s32_rt",
   "#include <arm_neon.h>\n"
   "int vpmax_s32_test(int a, int b) {\n"
   "  int32x2_t va = {a, b};\n"
   "  int32x2_t vb = {10, 20};\n"
   "  int32x2_t r = vpmax_s32(va, vb);\n"
   "  return vget_lane_s32(r, 0);\n"
   "}\n",
   {100, 200}, "VPMAXMIN", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"vpmin_u16_rt",
   "#include <arm_neon.h>\n"
   "int vpmin_u16_test(int a, int b) {\n"
   "  uint16x4_t va = {(unsigned short)a, (unsigned short)b, 300, 400};\n"
   "  uint16x4_t vb = {10, 20, 30, 40};\n"
   "  uint16x4_t r = vpmin_u16(va, vb);\n"
   "  return (int)vget_lane_u16(r, 0);\n"
   "}\n",
   {500, 100}, "VPMAXMIN", 1, "-mfloat-abi=softfp -mfpu=neon"},
};

// ============================================================================
// VMOVN / VQMOVN — narrowing
// ============================================================================
static const std::vector<RoundTripTC> kVMOVN = {
  {"c_narrow_s32_to_s16",
   "int c_narrow_s32_to_s16(int a) {\n"
   "  return (short)a;\n"
   "}\n",
   {12345}, "VMOVN", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"c_truncate_s32",
   "int c_truncate_s32(int a) {\n"
   "  return (short)(a & 0xFFFF);\n"
   "}\n",
   {100}, "VMOVN", 1, "-mfloat-abi=softfp -mfpu=neon"},
};

// ============================================================================
// VABS — absolute value
// ============================================================================
static const std::vector<RoundTripTC> kVABS = {
  {"c_abs_int32",
   "int c_abs_int32(int a) {\n"
   "  return a < 0 ? -a : a;\n"
   "}\n",
   {(uint64_t)(int64_t)-123}, "VABS", 1, "-mfloat-abi=softfp -mfpu=neon"},
};

// ============================================================================
// VCNT — per-byte popcount
// ============================================================================
static const std::vector<RoundTripTC> kVCNT = {
  {"vcnt_u8_rt",
   "#include <arm_neon.h>\n"
   "int vcnt_u8_test(int a) {\n"
   "  uint8x8_t v = {(unsigned char)a, 0xFF, 0x00, 0x0F, 0xAA, 0x55, 0x80, 0x01};\n"
   "  uint8x8_t r = vcnt_u8(v);\n"
   "  return (int)vget_lane_u8(r, 0);\n"
   "}\n",
   {0x37}, "VCNT", 1, "-mfloat-abi=softfp -mfpu=neon"},
};

// ============================================================================
// VCLS / VCLZ / VPADDL (vector) — per-lane sign/zero counts and pairwise long
// ============================================================================
static const std::vector<RoundTripTC> kVCLSCLZPADDL = {
  {"vcls_s32_rt",
   "#include <arm_neon.h>\n"
   "int vcls_s32_test(int a) {\n"
   "  int32x4_t v = {(int)a, -5, 100, -1};\n"
   "  int32x4_t r = vclsq_s32(v);\n"
   "  int out[4]; vst1q_s32(out, r);\n"
   "  return out[0]+out[1]+out[2]+out[3];\n"
   "}\n",
   {0x00010000}, "VCLSCLZ", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"vclz_u32_rt",
   "#include <arm_neon.h>\n"
   "int vclz_u32_test(int a) {\n"
   "  uint32x4_t v = {(unsigned)a, 0x0F, 0xFFFF, 1};\n"
   "  uint32x4_t r = vclzq_u32(v);\n"
   "  unsigned out[4]; vst1q_u32(out, r);\n"
   "  return (int)(out[0]+out[1]+out[2]+out[3]);\n"
   "}\n",
   {0x00040000}, "VCLSCLZ", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"vpaddl_u8_rt",
   "#include <arm_neon.h>\n"
   "int vpaddl_u8_test(int a) {\n"
   "  uint8x8_t v = {(unsigned char)a, 2, 3, 4, 5, 6, 7, 8};\n"
   "  uint16x4_t r = vpaddl_u8(v);\n"
   "  unsigned short out[4]; vst1_u16(out, r);\n"
   "  return out[0]+out[1]+out[2]+out[3];\n"
   "}\n",
   {0x55}, "VCLSCLZ", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"vpadal_s16_rt",
   "#include <arm_neon.h>\n"
   "int vpadal_s16_test(int a) {\n"
   "  int16x8_t v = {(short)a, -2, 3, -4, 5, -6, 7, -8};\n"
   "  int32x4_t acc = {100, 200, 300, 400};\n"
   "  int32x4_t r = vpadalq_s16(acc, v);\n"
   "  int out[4]; vst1q_s32(out, r);\n"
   "  return out[0]+out[1]+out[2]+out[3];\n"
   "}\n",
   {0x40}, "VCLSCLZ", 1, "-mfloat-abi=softfp -mfpu=neon"},
};

// ============================================================================
// VTBL / VTBX — byte table lookup (1-4 D-register tables, in/out-of-range)
// ============================================================================
static const std::vector<RoundTripTC> kVTBL = {
  {"vtbl1_u8_rt",
   "#include <arm_neon.h>\n"
   "int vtbl1_u8_test(int a) {\n"
   "  uint8x8_t tb = {(unsigned char)a, 11, 22, 33, 44, 55, 66, 77};\n"
   "  uint8x8_t idx = {0, 3, 7, 1, 8, 2, 9, 4};\n"  // 8,9 out-of-range -> 0
   "  uint8x8_t r = vtbl1_u8(tb, idx);\n"
   "  unsigned char o[8]; vst1_u8(o, r);\n"
   "  return o[0]+o[1]+o[2]+o[3]+o[4]+o[5]+o[6]+o[7];\n"
   "}\n",
   {0x5A}, "VTBL", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"vtbl2_u8_rt",
   "#include <arm_neon.h>\n"
   "int vtbl2_u8_test(int a) {\n"
   "  uint8x8x2_t tb;\n"
   "  uint8x8_t t0 = {(unsigned char)a, 11, 22, 33, 44, 55, 66, 77};\n"
   "  uint8x8_t t1 = {88, 99, 100, 111, 122, 133, 144, 155};\n"
   "  tb.val[0]=t0; tb.val[1]=t1;\n"
   "  uint8x8_t idx = {0, 9, 15, 1, 5, 12, 20, 4};\n"  // 20 out-of-range -> 0
   "  uint8x8_t r = vtbl2_u8(tb, idx);\n"
   "  unsigned char o[8]; vst1_u8(o, r);\n"
   "  return o[0]+o[1]+o[2]+o[3]+o[4]+o[5]+o[6]+o[7];\n"
   "}\n",
   {0x33}, "VTBL", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"vtbx1_u8_rt",
   "#include <arm_neon.h>\n"
   "int vtbx1_u8_test(int a) {\n"
   "  uint8x8_t old = {200, 201, 202, 203, 204, 205, 206, 207};\n"
   "  uint8x8_t tb = {(unsigned char)a, 11, 22, 33, 44, 55, 66, 77};\n"
   "  uint8x8_t idx = {0, 3, 9, 1, 5, 2, 12, 4};\n"  // 9,12 keep old
   "  uint8x8_t r = vtbx1_u8(old, tb, idx);\n"
   "  unsigned char o[8]; vst1_u8(o, r);\n"
   "  return o[0]+o[1]+o[2]+o[3]+o[4]+o[5]+o[6]+o[7];\n"
   "}\n",
   {0x77}, "VTBL", 1, "-mfloat-abi=softfp -mfpu=neon"},
};

// ============================================================================
// VSWP — swap two NEON registers (must write BOTH registers)
// ============================================================================
static const std::vector<RoundTripTC> kVSWP = {
  {"vswp_d_rt",
   "#include <arm_neon.h>\n"
   "int vswp_d_test(int a) {\n"
   "  uint8x8_t x = {(unsigned char)a, 1, 2, 3, 4, 5, 6, 7};\n"
   "  uint8x8_t y = {10, 11, 12, 13, 14, 15, 16, 17};\n"
   "  __asm__(\"vswp %0, %1\" : \"+w\"(x), \"+w\"(y));\n"
   "  unsigned char ox[8], oy[8]; vst1_u8(ox, x); vst1_u8(oy, y);\n"
   "  return ox[0]*1000 + ox[7]*100 + oy[0]*10 + oy[7];\n"
   "}\n",
   {0x5A}, "VSWP", 1, "-mfloat-abi=softfp -mfpu=neon"},
};

// ============================================================================
// VQMOVN / VQMOVUN — saturating narrow (in/out-of-range lanes)
// ============================================================================
static const std::vector<RoundTripTC> kVQMOVN = {
  {"vqmovn_s16_rt",
   "#include <arm_neon.h>\n"
   "int vqmovn_s16_test(int a) {\n"
   "  int16x8_t v = {(short)a, -300, 400, -1, 127, -128, 200, -200};\n"
   "  int8x8_t r = vqmovn_s16(v);\n"  // saturate to [-128,127]
   "  signed char o[8]; vst1_s8(o, r);\n"
   "  return o[0]+o[1]+o[2]+o[3]+o[4]+o[5]+o[6]+o[7];\n"
   "}\n",
   {0x40}, "VQMOVN", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"vqmovn_u16_rt",
   "#include <arm_neon.h>\n"
   "int vqmovn_u16_test(int a) {\n"
   "  uint16x8_t v = {(unsigned short)a, 300, 100, 256, 255, 1000, 50, 65535};\n"
   "  uint8x8_t r = vqmovn_u16(v);\n"  // saturate to [0,255]
   "  unsigned char o[8]; vst1_u8(o, r);\n"
   "  return o[0]+o[1]+o[2]+o[3]+o[4]+o[5]+o[6]+o[7];\n"
   "}\n",
   {0x20}, "VQMOVN", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"vqmovun_s16_rt",
   "#include <arm_neon.h>\n"
   "int vqmovun_s16_test(int a) {\n"
   "  int16x8_t v = {(short)a, -300, 400, -1, 256, 100, -5, 70};\n"
   "  uint8x8_t r = vqmovun_s16(v);\n"  // signed->unsigned [0,255], neg->0
   "  unsigned char o[8]; vst1_u8(o, r);\n"
   "  return o[0]+o[1]+o[2]+o[3]+o[4]+o[5]+o[6]+o[7];\n"
   "}\n",
   {0x30}, "VQMOVN", 1, "-mfloat-abi=softfp -mfpu=neon"},

  {"vqmovun_s32_rt",
   "#include <arm_neon.h>\n"
   "int vqmovun_s32_test(int a) {\n"
   "  int32x4_t v = {(int)a, -100000, 70000, -1};\n"
   "  uint16x4_t r = vqmovun_s32(v);\n"  // signed->unsigned [0,65535]
   "  unsigned short o[4]; vst1_u16(o, r);\n"
   "  return o[0]+o[1]+o[2]+o[3];\n"
   "}\n",
   {0x10000}, "VQMOVN", 1, "-mfloat-abi=softfp -mfpu=neon"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(VSWP, ARM32NEONExtRT,
                         ::testing::ValuesIn(kVSWP), rtTCName);
INSTANTIATE_TEST_SUITE_P(VQMOVN, ARM32NEONExtRT,
                         ::testing::ValuesIn(kVQMOVN), rtTCName);
INSTANTIATE_TEST_SUITE_P(VTBL, ARM32NEONExtRT,
                         ::testing::ValuesIn(kVTBL), rtTCName);
INSTANTIATE_TEST_SUITE_P(VCLSCLZ, ARM32NEONExtRT,
                         ::testing::ValuesIn(kVCLSCLZPADDL), rtTCName);
INSTANTIATE_TEST_SUITE_P(VZIPTRN, ARM32NEONExtRT,
                         ::testing::ValuesIn(kVZIPTRN), rtTCName);
INSTANTIATE_TEST_SUITE_P(VREV, ARM32NEONExtRT,
                         ::testing::ValuesIn(kVREV), rtTCName);
INSTANTIATE_TEST_SUITE_P(VMULL, ARM32NEONExtRT,
                         ::testing::ValuesIn(kVMULL), rtTCName);
INSTANTIATE_TEST_SUITE_P(VADDLSUBL, ARM32NEONExtRT,
                         ::testing::ValuesIn(kVADDLSUBL), rtTCName);
INSTANTIATE_TEST_SUITE_P(VQADDSUB, ARM32NEONExtRT,
                         ::testing::ValuesIn(kVQADDSUB), rtTCName);
INSTANTIATE_TEST_SUITE_P(VPMAXMIN, ARM32NEONExtRT,
                         ::testing::ValuesIn(kVPMAXMIN), rtTCName);
INSTANTIATE_TEST_SUITE_P(VMOVN, ARM32NEONExtRT,
                         ::testing::ValuesIn(kVMOVN), rtTCName);
INSTANTIATE_TEST_SUITE_P(VABS, ARM32NEONExtRT,
                         ::testing::ValuesIn(kVABS), rtTCName);
INSTANTIATE_TEST_SUITE_P(VCNT, ARM32NEONExtRT,
                         ::testing::ValuesIn(kVCNT), rtTCName);
