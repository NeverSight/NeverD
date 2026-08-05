//===- AArch64_CoreNEON2RTTests.cpp - Batch coverage for uncovered NEON --*-===//
//
// Roundtrip probes targeting AArch64 CoreNEON instructions that have zero
// roundtrip coverage.  Each probe uses NEON intrinsics to exercise a specific
// instruction, XOR-reducing lanes to a single return value for bit-exact
// comparison.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64CoreNEON2RT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CoreNEON2RT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kCoreNEON2 = {
  // --- SSUBW / SSUBW2 (signed widening subtract) ---
  {"ssubw_s32",
   "#include <arm_neon.h>\n"
   "long ssubw_s32(long a) {\n"
   "  int64x2_t va = {(long)a + 100000, -(long)a};\n"
   "  int32x2_t vb = {(int)a, (int)(-a + 1)};\n"
   "  int64x2_t vr = vsubw_s32(va, vb);\n"
   "  return (long)(vgetq_lane_s64(vr,0) ^ vgetq_lane_s64(vr,1));\n"
   "}\n",
   {12345}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  // USUBW (unsigned widening subtract)
  {"usubw_u16",
   "#include <arm_neon.h>\n"
   "long usubw_u16(long a) {\n"
   "  uint32x4_t va = {(unsigned)a+1000, (unsigned)a+2000,\n"
   "                   (unsigned)a+3000, (unsigned)a+4000};\n"
   "  uint16x4_t vb = {100, 200, 300, 400};\n"
   "  uint32x4_t vr = vsubw_u16(va, vb);\n"
   "  unsigned r = vgetq_lane_u32(vr,0) ^ vgetq_lane_u32(vr,1)\n"
   "             ^ vgetq_lane_u32(vr,2) ^ vgetq_lane_u32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {5000}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  // --- RADDHN / RADDHN2 (rounding add returning high narrow) ---
  {"raddhn_s32",
   "#include <arm_neon.h>\n"
   "long raddhn_s32(long a) {\n"
   "  int32x4_t va = {(int)a * 65536, (int)(-a) * 65536,\n"
   "                  (int)a * 32768, (int)(-a) * 32768};\n"
   "  int32x4_t vb = {(int)a * 65536 + 0x8000, (int)a * 65536,\n"
   "                  (int)a * 32768 + 0x8000, (int)(-a) * 32768};\n"
   "  int16x4_t vr = vraddhn_s32(va, vb);\n"
   "  unsigned r = (unsigned short)vget_lane_s16(vr,0)\n"
   "             ^ (unsigned short)vget_lane_s16(vr,1)\n"
   "             ^ (unsigned short)vget_lane_s16(vr,2)\n"
   "             ^ (unsigned short)vget_lane_s16(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {7}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  // --- RSUBHN (rounding subtract returning high narrow) ---
  {"rsubhn_s32",
   "#include <arm_neon.h>\n"
   "long rsubhn_s32(long a) {\n"
   "  int32x4_t va = {(int)a * 131072, (int)(-a) * 131072,\n"
   "                  (int)a * 131072, (int)a * 131072};\n"
   "  int32x4_t vb = {(int)a * 65536, (int)(-a) * 65536,\n"
   "                  (int)a * 65536 + 0x8000, (int)(-a) * 65536};\n"
   "  int16x4_t vr = vrsubhn_s32(va, vb);\n"
   "  unsigned r = (unsigned short)vget_lane_s16(vr,0)\n"
   "             ^ (unsigned short)vget_lane_s16(vr,1)\n"
   "             ^ (unsigned short)vget_lane_s16(vr,2)\n"
   "             ^ (unsigned short)vget_lane_s16(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {3}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  // --- BIF (bit insert if false): vd = (vd & mask) | (vn & ~mask) ---
  {"bif_u8",
   "#include <arm_neon.h>\n"
   "long bif_u8(long a) {\n"
   "  uint8x16_t vd = vdupq_n_u8((unsigned char)a);\n"
   "  uint8x16_t vn = vdupq_n_u8((unsigned char)(a >> 8));\n"
   "  uint8x16_t vm = vdupq_n_u8(0xF0);\n"
   "  asm(\"bif %0.16b, %1.16b, %2.16b\" : \"+w\"(vd) : \"w\"(vn), \"w\"(vm));\n"
   "  return (long)vgetq_lane_u8(vd, 0);\n"
   "}\n",
   {0xAB12}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  // --- FACGE / FACGT (FP absolute compare >= / >) ---
  {"facge_f32",
   "#include <arm_neon.h>\n"
   "long facge_f32(long a) {\n"
   "  float x = (float)(int)a;\n"
   "  float32x4_t va = {x, -x, x*2.0f, -1.0f};\n"
   "  float32x4_t vb = {-x, x, x, x*3.0f};\n"
   "  uint32x4_t vr = vcageq_f32(va, vb);\n"
   "  unsigned r = vgetq_lane_u32(vr,0) ^ vgetq_lane_u32(vr,1)\n"
   "             ^ vgetq_lane_u32(vr,2) ^ vgetq_lane_u32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {5}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  {"facgt_f32",
   "#include <arm_neon.h>\n"
   "long facgt_f32(long a) {\n"
   "  float x = (float)(int)a;\n"
   "  float32x4_t va = {x, -x, x*2.0f, -1.0f};\n"
   "  float32x4_t vb = {-x, x, x, x*3.0f};\n"
   "  uint32x4_t vr = vcagtq_f32(va, vb);\n"
   "  unsigned r = vgetq_lane_u32(vr,0) ^ vgetq_lane_u32(vr,1)\n"
   "             ^ vgetq_lane_u32(vr,2) ^ vgetq_lane_u32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {5}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  // --- FNMADD / FNMSUB (negated fused multiply-add/sub) ---
  {"fnmadd_f64",
   "#include <arm_neon.h>\n"
   "long fnmadd_f64(long a) {\n"
   "  double x = (double)(int)a;\n"
   "  double r;\n"
   "  asm(\"fnmadd %d0, %d1, %d2, %d3\" : \"=w\"(r) : \"w\"(x), \"w\"(2.0), \"w\"(10.0));\n"
   "  return (long)r;\n"
   "}\n",
   {7}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  {"fnmsub_f64",
   "#include <arm_neon.h>\n"
   "long fnmsub_f64(long a) {\n"
   "  double x = (double)(int)a;\n"
   "  double r;\n"
   "  asm(\"fnmsub %d0, %d1, %d2, %d3\" : \"=w\"(r) : \"w\"(x), \"w\"(3.0), \"w\"(5.0));\n"
   "  return (long)r;\n"
   "}\n",
   {4}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  // --- SADALP / UADALP (signed/unsigned add and accumulate long pairwise) ---
  {"sadalp_s16",
   "#include <arm_neon.h>\n"
   "long sadalp_s16(long a) {\n"
   "  int16x8_t vn = {(short)a, (short)-a, 100, -50, 200, -100, 1, -1};\n"
   "  int32x4_t vd = {1000, 2000, 3000, 4000};\n"
   "  int32x4_t vr = vpadalq_s16(vd, vn);\n"
   "  unsigned r = (unsigned)vgetq_lane_s32(vr,0)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,1)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,2)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {42}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  {"uadalp_u8",
   "#include <arm_neon.h>\n"
   "long uadalp_u8(long a) {\n"
   "  uint8x16_t vn = {(unsigned char)a, 10, 20, 30, 40, 50, 60, 70,\n"
   "                   80, 90, 100, 110, 120, 130, 140, 150};\n"
   "  uint16x8_t vd = {1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000};\n"
   "  uint16x8_t vr = vpadalq_u8(vd, vn);\n"
   "  unsigned r = vgetq_lane_u16(vr,0) ^ vgetq_lane_u16(vr,1)\n"
   "             ^ vgetq_lane_u16(vr,2) ^ vgetq_lane_u16(vr,3)\n"
   "             ^ vgetq_lane_u16(vr,4) ^ vgetq_lane_u16(vr,5)\n"
   "             ^ vgetq_lane_u16(vr,6) ^ vgetq_lane_u16(vr,7);\n"
   "  return (long)r;\n"
   "}\n",
   {25}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  // --- TBX (table lookup extension — out-of-range indices keep default) ---
  {"tbx_u8",
   "#include <arm_neon.h>\n"
   "long tbx_u8(long a) {\n"
   "  uint8x16_t tbl = {10,20,30,40,50,60,70,80,90,100,110,120,130,140,150,160};\n"
   "  uint8x8_t idx = {0, 3, 7, 15, 1, 5, 2, 4};\n"
   "  uint8x8_t def = vdup_n_u8(0xFF);\n"
   "  uint8x8_t vr;\n"
   "  asm(\"tbx %0.8b, {%1.16b}, %2.8b\" : \"=w\"(vr) : \"w\"(tbl), \"w\"(idx));\n"
   "  unsigned r = vget_lane_u8(vr,0) ^ vget_lane_u8(vr,1)\n"
   "             ^ vget_lane_u8(vr,2) ^ vget_lane_u8(vr,3)\n"
   "             ^ vget_lane_u8(vr,4) ^ vget_lane_u8(vr,5)\n"
   "             ^ vget_lane_u8(vr,6) ^ vget_lane_u8(vr,7);\n"
   "  return (long)r;\n"
   "}\n",
   {0}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  // --- SMLSL / UMLSL (widening multiply-subtract long) ---
  {"smlsl_s16",
   "#include <arm_neon.h>\n"
   "long smlsl_s16(long a) {\n"
   "  int32x4_t vacc = {100000, 200000, 300000, 400000};\n"
   "  int16x4_t vn = {(short)a, 100, -50, 200};\n"
   "  int16x4_t vm = {10, 20, 30, 40};\n"
   "  int32x4_t vr = vmlsl_s16(vacc, vn, vm);\n"
   "  unsigned r = (unsigned)vgetq_lane_s32(vr,0)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,1)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,2)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {7}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  {"umlsl_u16",
   "#include <arm_neon.h>\n"
   "long umlsl_u16(long a) {\n"
   "  uint32x4_t vacc = {500000, 600000, 700000, 800000};\n"
   "  uint16x4_t vn = {(unsigned short)a, 100, 200, 300};\n"
   "  uint16x4_t vm = {10, 20, 30, 40};\n"
   "  uint32x4_t vr = vmlsl_u16(vacc, vn, vm);\n"
   "  unsigned r = vgetq_lane_u32(vr,0) ^ vgetq_lane_u32(vr,1)\n"
   "             ^ vgetq_lane_u32(vr,2) ^ vgetq_lane_u32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {50}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  // --- FCVTL (FP convert narrow to wider, bottom half) via inline asm ---
  {"fcvtl_f16_f32",
   "#include <arm_neon.h>\n"
   "long fcvtl_f16_f32(long a) {\n"
   "  float x = (float)(int)a;\n"
   "  __fp16 h1 = (__fp16)x, h2 = (__fp16)(-x);\n"
   "  float16x4_t vin = {h1, h2, (__fp16)0.0f, h1};\n"
   "  float32x4_t vr;\n"
   "  asm(\"fcvtl %0.4s, %1.4h\" : \"=w\"(vr) : \"w\"(vin));\n"
   "  union { float f; unsigned u; } c0,c1,c2,c3;\n"
   "  c0.f = vgetq_lane_f32(vr,0); c1.f = vgetq_lane_f32(vr,1);\n"
   "  c2.f = vgetq_lane_f32(vr,2); c3.f = vgetq_lane_f32(vr,3);\n"
   "  return (long)(c0.u ^ c1.u ^ c2.u ^ c3.u);\n"
   "}\n",
   {7}, "CoreNEON2", 1, "-march=armv8-a+simd+fp16"},

  // --- SADDLV / UADDLV (across-vector add long) ---
  {"saddlv_s16",
   "#include <arm_neon.h>\n"
   "long saddlv_s16(long a) {\n"
   "  int16x8_t v = {(short)a, -100, 200, -300, 400, -500, 600, -700};\n"
   "  int32_t r = vaddlvq_s16(v);\n"
   "  return (long)r;\n"
   "}\n",
   {42}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  {"uaddlv_u8",
   "#include <arm_neon.h>\n"
   "long uaddlv_u8(long a) {\n"
   "  uint8x16_t v = {(unsigned char)a,10,20,30,40,50,60,70,\n"
   "                  80,90,100,110,120,130,140,150};\n"
   "  uint16_t r = vaddlvq_u8(v);\n"
   "  return (long)r;\n"
   "}\n",
   {25}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  // --- FCMLE / FCMLT zero (FP compare <=0 / <0) ---
  {"fcmle_zero_f32",
   "#include <arm_neon.h>\n"
   "long fcmle_zero_f32(long a) {\n"
   "  float x = (float)(int)a;\n"
   "  float32x4_t v = {x, -x, 0.0f, x - 100.0f};\n"
   "  uint32x4_t vr = vcleq_f32(v, vdupq_n_f32(0.0f));\n"
   "  unsigned r = vgetq_lane_u32(vr,0) ^ vgetq_lane_u32(vr,1)\n"
   "             ^ vgetq_lane_u32(vr,2) ^ vgetq_lane_u32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {50}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  {"fcmlt_zero_f32",
   "#include <arm_neon.h>\n"
   "long fcmlt_zero_f32(long a) {\n"
   "  float x = (float)(int)a;\n"
   "  float32x4_t v = {x, -x, 0.0f, x - 100.0f};\n"
   "  uint32x4_t vr = vcltq_f32(v, vdupq_n_f32(0.0f));\n"
   "  unsigned r = vgetq_lane_u32(vr,0) ^ vgetq_lane_u32(vr,1)\n"
   "             ^ vgetq_lane_u32(vr,2) ^ vgetq_lane_u32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {50}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  // --- SRSRA / USRA / SSRA / URSRA (shift right and accumulate) ---
  {"ssra_s32",
   "#include <arm_neon.h>\n"
   "long ssra_s32(long a) {\n"
   "  int32x4_t vacc = {1000, 2000, 3000, 4000};\n"
   "  int32x4_t vn = {(int)a * 16, (int)(-a) * 16, 256, -512};\n"
   "  int32x4_t vr = vsraq_n_s32(vacc, vn, 4);\n"
   "  unsigned r = (unsigned)vgetq_lane_s32(vr,0)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,1)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,2)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {7}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  {"usra_u32",
   "#include <arm_neon.h>\n"
   "long usra_u32(long a) {\n"
   "  uint32x4_t vacc = {1000, 2000, 3000, 4000};\n"
   "  uint32x4_t vn = {(unsigned)a * 16, 256, 512, 1024};\n"
   "  uint32x4_t vr = vsraq_n_u32(vacc, vn, 4);\n"
   "  unsigned r = vgetq_lane_u32(vr,0) ^ vgetq_lane_u32(vr,1)\n"
   "             ^ vgetq_lane_u32(vr,2) ^ vgetq_lane_u32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {7}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  {"srsra_s32",
   "#include <arm_neon.h>\n"
   "long srsra_s32(long a) {\n"
   "  int32x4_t vacc = {1000, 2000, 3000, 4000};\n"
   "  int32x4_t vn = {(int)a * 16 + 8, (int)(-a) * 16 - 8, 255, -511};\n"
   "  int32x4_t vr = vrsraq_n_s32(vacc, vn, 4);\n"
   "  unsigned r = (unsigned)vgetq_lane_s32(vr,0)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,1)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,2)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {7}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  {"ursra_u32",
   "#include <arm_neon.h>\n"
   "long ursra_u32(long a) {\n"
   "  uint32x4_t vacc = {1000, 2000, 3000, 4000};\n"
   "  uint32x4_t vn = {(unsigned)a * 16 + 8, 255, 511, 1023};\n"
   "  uint32x4_t vr = vrsraq_n_u32(vacc, vn, 4);\n"
   "  unsigned r = vgetq_lane_u32(vr,0) ^ vgetq_lane_u32(vr,1)\n"
   "             ^ vgetq_lane_u32(vr,2) ^ vgetq_lane_u32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {7}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  // --- SSHLL / USHLL (shift left long — widening) ---
  {"sshll_s16",
   "#include <arm_neon.h>\n"
   "long sshll_s16(long a) {\n"
   "  int16x4_t vn = {(short)a, -100, 200, -300};\n"
   "  int32x4_t vr = vshll_n_s16(vn, 8);\n"
   "  unsigned r = (unsigned)vgetq_lane_s32(vr,0)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,1)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,2)\n"
   "             ^ (unsigned)vgetq_lane_s32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {42}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  {"ushll_u8",
   "#include <arm_neon.h>\n"
   "long ushll_u8(long a) {\n"
   "  uint8x8_t vn = {(unsigned char)a, 10, 20, 30, 40, 50, 60, 70};\n"
   "  uint16x8_t vr = vshll_n_u8(vn, 4);\n"
   "  unsigned r = vgetq_lane_u16(vr,0) ^ vgetq_lane_u16(vr,1)\n"
   "             ^ vgetq_lane_u16(vr,2) ^ vgetq_lane_u16(vr,3)\n"
   "             ^ vgetq_lane_u16(vr,4) ^ vgetq_lane_u16(vr,5)\n"
   "             ^ vgetq_lane_u16(vr,6) ^ vgetq_lane_u16(vr,7);\n"
   "  return (long)r;\n"
   "}\n",
   {25}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  // --- URSHR (unsigned rounding shift right) ---
  {"urshr_u32",
   "#include <arm_neon.h>\n"
   "long urshr_u32(long a) {\n"
   "  uint32x4_t vn = {(unsigned)a * 16 + 8, 255, 511, 1023};\n"
   "  uint32x4_t vr = vrshrq_n_u32(vn, 4);\n"
   "  unsigned r = vgetq_lane_u32(vr,0) ^ vgetq_lane_u32(vr,1)\n"
   "             ^ vgetq_lane_u32(vr,2) ^ vgetq_lane_u32(vr,3);\n"
   "  return (long)r;\n"
   "}\n",
   {7}, "CoreNEON2", 1, "-march=armv8-a+simd"},

  // --- UMSUBL (unsigned multiply-subtract long) ---
  {"umsubl",
   "#include <arm_neon.h>\n"
   "long umsubl(long a) {\n"
   "  unsigned x = (unsigned)a;\n"
   "  long acc = 1000000;\n"
   "  long r;\n"
   "  asm(\"umsubl %0, %w1, %w2, %3\" : \"=r\"(r) : \"r\"(x), \"r\"(100u), \"r\"(acc));\n"
   "  return r;\n"
   "}\n",
   {42}, "CoreNEON2", 1, "-march=armv8-a+simd"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(CoreNEON2, A64CoreNEON2RT,
                         ::testing::ValuesIn(kCoreNEON2),
                         [](const auto &I) { return I.param.Name; });
