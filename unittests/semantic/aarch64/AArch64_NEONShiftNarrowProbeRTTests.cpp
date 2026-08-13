//===- AArch64_NEONShiftNarrowProbeRTTests.cpp - shift/narrow ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for AArch64 NEON instruction families that clang -O2 rarely
// emits on its own but that share lifter code paths — a bug in one usually
// implicates several:
//   * register shifts  : SQRSHL/UQRSHL (sat rounding), SRSHL/URSHL (rounding),
//                        SQSHL/UQSHL (saturating), per-lane signed shift count.
//   * immediate shifts : SQSHLU (sat shift-left unsigned).
//   * high-half narrow  : ADDHN/RADDHN, SUBHN/RSUBHN (add/sub then take high
//                        half, with/without rounding).
//   * narrowing shifts  : SQRSHRN (sat rounding), RSHRN (rounding), SQXTUN
//                        (saturating extract unsigned).
//   * absdiff-accumulate: ABA / ABAL (per-lane |a-b| added to accumulator).
//   * across-vector     : ADDV / SADDLV (horizontal reductions to a scalar).
//
// Each probe drives saturation / rounding / borrow boundaries so a full-width or
// non-rounding placeholder diverges.  Lanes hash-reduce to a bit-exact return.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64ShiftNarrowProbeRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64ShiftNarrowProbeRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kShiftNarrow = {
  // SQRSHL: signed saturating rounding shift left by per-lane signed count.
  // Negative counts round on the right shift; large positive counts saturate.
  {"sqrshl_4s",
   "#include <arm_neon.h>\n"
   "long sqrshl_4s(long a){\n"
   "  int x=(int)a;\n"
   "  int32x4_t v={x+2000000000,x-2000000000,12345,-12345};\n"
   "  int32x4_t s={5,-3,28,-1};\n"
   "  int32x4_t r=vqrshlq_s32(v,s);\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned)r[i];\n"
   "  return (long)o;\n"
   "}\n",
   {100}, "ShiftNarrow", 1, "-march=armv8-a+simd"},

  // UQRSHL: unsigned saturating rounding shift left, signed per-lane count.
  {"uqrshl_4s",
   "#include <arm_neon.h>\n"
   "long uqrshl_4s(long a){\n"
   "  unsigned x=(unsigned)a;\n"
   "  uint32x4_t v={x+4000000000u,7u,0x12345678u,0xFFu};\n"
   "  int32x4_t s={4,-5,20,-1};\n"
   "  uint32x4_t r=vqrshlq_u32(v,s);\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^r[i];\n"
   "  return (long)o;\n"
   "}\n",
   {123}, "ShiftNarrow", 1, "-march=armv8-a+simd"},

  // SRSHL: signed rounding shift left (no saturation), 8x i16.
  {"srshl_8h",
   "#include <arm_neon.h>\n"
   "long srshl_8h(long a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t v={x,(short)-x,1000,-1000,32767,-32768,3,-3};\n"
   "  int16x8_t s={3,-2,7,-1,1,-15,4,-4};\n"
   "  int16x8_t r=vrshlq_s16(v,s);\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^(unsigned short)r[i];\n"
   "  return (long)o;\n"
   "}\n",
   {77}, "ShiftNarrow", 1, "-march=armv8-a+simd"},

  // URSHL: unsigned rounding shift left, 16x i8.
  {"urshl_16b",
   "#include <arm_neon.h>\n"
   "long urshl_16b(long a){\n"
   "  unsigned char x=(unsigned char)a;\n"
   "  uint8x16_t v={x,255,1,128,200,5,3,17, x,254,2,127,199,6,4,18};\n"
   "  int8x16_t s={2,-1,5,-3,1,-2,7,-7, 3,-4,4,-1,2,-2,6,-6};\n"
   "  uint8x16_t r=vrshlq_u8(v,s);\n"
   "  unsigned o=0; for(int i=0;i<16;i++) o=o*131u^r[i];\n"
   "  return (long)o;\n"
   "}\n",
   {88}, "ShiftNarrow", 1, "-march=armv8-a+simd"},

  // SQSHL: signed saturating shift left by per-lane count (no rounding).
  {"sqshl_4s",
   "#include <arm_neon.h>\n"
   "long sqshl_4s(long a){\n"
   "  int x=(int)a;\n"
   "  int32x4_t v={x,1000000,-1000000,3};\n"
   "  int32x4_t s={20,12,12,-2};\n"
   "  int32x4_t r=vqshlq_s32(v,s);\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned)r[i];\n"
   "  return (long)o;\n"
   "}\n",
   {5000}, "ShiftNarrow", 1, "-march=armv8-a+simd"},

  // SQSHLU: signed-to-unsigned saturating shift left by immediate.
  {"sqshlu_8h",
   "#include <arm_neon.h>\n"
   "long sqshlu_8h(long a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t v={x,(short)-x,100,-100,200,-1,32767,-32768};\n"
   "  uint16x8_t r=vqshluq_n_s16(v,4);\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^r[i];\n"
   "  return (long)o;\n"
   "}\n",
   {300}, "ShiftNarrow", 1, "-march=armv8-a+simd"},

  // ADDHN: add then take the high half (narrowing), 4x i32 -> 4x i16.
  {"addhn_4h",
   "#include <arm_neon.h>\n"
   "long addhn_4h(long a){\n"
   "  int x=(int)a;\n"
   "  int32x4_t va={x<<10,0x7FFF0000,0x12340000,-1};\n"
   "  int32x4_t vb={0x00010000,0x00020000,0x10000000,0x00030000};\n"
   "  int16x4_t r=vaddhn_s32(va,vb);\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned short)r[i];\n"
   "  return (long)o;\n"
   "}\n",
   {7}, "ShiftNarrow", 1, "-march=armv8-a+simd"},

  // RADDHN: rounding add then high half, 8x i16 -> 8x i8.
  {"raddhn_8b",
   "#include <arm_neon.h>\n"
   "long raddhn_8b(long a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t va={(short)(x<<6),0x7F80,0x1234,-1,0x0180,0x00C0,0x4040,0x7FFF};\n"
   "  int16x8_t vb={0x0080,0x0080,0x0100,0x0080,0x0080,0x0040,0x00C0,0x0001};\n"
   "  int8x8_t r=vraddhn_s16(va,vb);\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^(unsigned char)r[i];\n"
   "  return (long)o;\n"
   "}\n",
   {9}, "ShiftNarrow", 1, "-march=armv8-a+simd"},

  // SUBHN: subtract then high half, 4x i32 -> 4x i16.
  {"subhn_4h",
   "#include <arm_neon.h>\n"
   "long subhn_4h(long a){\n"
   "  int x=(int)a;\n"
   "  int32x4_t va={x<<10,0x7FFF0000,0x12340000,5};\n"
   "  int32x4_t vb={0x00010000,0x10000000,0x00020000,0x00030000};\n"
   "  int16x4_t r=vsubhn_s32(va,vb);\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned short)r[i];\n"
   "  return (long)o;\n"
   "}\n",
   {11}, "ShiftNarrow", 1, "-march=armv8-a+simd"},

  // SQRSHRN: signed saturating rounding shift right narrow, i32 -> i16.
  {"sqrshrn_4h",
   "#include <arm_neon.h>\n"
   "long sqrshrn_4h(long a){\n"
   "  int x=(int)a;\n"
   "  int32x4_t v={x<<8,0x7FFFFFFF,(int)0x80000000,-12345};\n"
   "  int16x4_t r=vqrshrn_n_s32(v,10);\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned short)r[i];\n"
   "  return (long)o;\n"
   "}\n",
   {54321}, "ShiftNarrow", 1, "-march=armv8-a+simd"},

  // RSHRN: rounding shift right narrow (no saturation), i16 -> i8.
  {"rshrn_8b",
   "#include <arm_neon.h>\n"
   "long rshrn_8b(long a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t v={x,(short)-x,0x7FFF,(short)0x8000,0x0180,0x00C0,0x00FF,0x1234};\n"
   "  int8x8_t r=vrshrn_n_s16(v,4);\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^(unsigned char)r[i];\n"
   "  return (long)o;\n"
   "}\n",
   {333}, "ShiftNarrow", 1, "-march=armv8-a+simd"},

  // SQXTUN: signed saturating extract to unsigned narrow, i16 -> u8.
  {"sqxtun_8b",
   "#include <arm_neon.h>\n"
   "long sqxtun_8b(long a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t v={x,(short)-x,300,-1,255,256,32767,-32768};\n"
   "  uint8x8_t r=vqmovun_s16(v);\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^r[i];\n"
   "  return (long)o;\n"
   "}\n",
   {200}, "ShiftNarrow", 1, "-march=armv8-a+simd"},

  // ABA: absolute-difference accumulate, 16x i8.
  {"aba_16b",
   "#include <arm_neon.h>\n"
   "long aba_16b(long a){\n"
   "  signed char x=(signed char)a;\n"
   "  int8x16_t acc={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};\n"
   "  int8x16_t va={x,-100,100,-1,0,127,-128,50, x,-100,100,-1,0,127,-128,50};\n"
   "  int8x16_t vb={-50,100,-100,1,0,-128,127,-50, -50,100,-100,1,0,-128,127,-50};\n"
   "  int8x16_t r=vabaq_s8(acc,va,vb);\n"
   "  unsigned o=0; for(int i=0;i<16;i++) o=o*131u^(unsigned char)r[i];\n"
   "  return (long)o;\n"
   "}\n",
   {33}, "ShiftNarrow", 1, "-march=armv8-a+simd"},

  // ABAL: absolute-difference accumulate long, i8 -> i16.
  {"abal_8h",
   "#include <arm_neon.h>\n"
   "long abal_8h(long a){\n"
   "  signed char x=(signed char)a;\n"
   "  int16x8_t acc={1000,-1000,2000,-2000,3000,-3000,4000,-4000};\n"
   "  int8x8_t va={x,-100,100,-1,0,127,-128,50};\n"
   "  int8x8_t vb={-50,100,-100,1,0,-128,127,-50};\n"
   "  int16x8_t r=vabal_s8(acc,va,vb);\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^(unsigned short)r[i];\n"
   "  return (long)o;\n"
   "}\n",
   {44}, "ShiftNarrow", 1, "-march=armv8-a+simd"},

  // ADDV: across-vector add to a scalar, 4x i32.
  {"addv_4s",
   "#include <arm_neon.h>\n"
   "long addv_4s(long a){\n"
   "  int x=(int)a;\n"
   "  int32x4_t v={x,x*3,-x,x>>1};\n"
   "  int s=vaddvq_s32(v);\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {123456}, "ShiftNarrow", 1, "-march=armv8-a+simd"},

  // SADDLV: signed across-vector add long to a wider scalar, 8x i16 -> i32.
  {"saddlv_8h",
   "#include <arm_neon.h>\n"
   "long saddlv_8h(long a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t v={x,(short)-x,30000,-30000,1,-1,12345,-6789};\n"
   "  int s=vaddlvq_s16(v);\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {25000}, "ShiftNarrow", 1, "-march=armv8-a+simd"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ShiftNarrow, A64ShiftNarrowProbeRT,
                         ::testing::ValuesIn(kShiftNarrow), rtTCName);
