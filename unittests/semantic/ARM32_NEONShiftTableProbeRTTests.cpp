//===- ARM32_NEONShiftTableProbeRTTests.cpp - shift/table -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for ARM32 NEON families clang -O2 rarely selects and that
// capstone often decodes without reliable vector_data (the project's richest
// bug source).  A bug in one usually implicates several:
//   * doubling mul high : VQDMULH / VQRDMULH (rounding).
//   * register shifts   : VQRSHL/VRSHL (rounding), VQSHL (saturating).
//   * immediate shifts  : VQSHLU (sat shift-left unsigned).
//   * narrowing shifts  : VQRSHRN / VRSHRN / VQRSHRUN (sat rounding to unsigned).
//   * narrow extract    : VQMOVUN, high-half VRADDHN.
//   * pairwise long     : VPADDL / VPADAL (add-accumulate-long).
//   * table lookup      : VTBL2 / VTBX2 (2-register table, out-of-range index).
//   * saturating unary  : VQABS / VQNEG (INT_MIN boundary).
//
// Lanes hash-reduce to a bit-exact return so any divergence surfaces.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32ShiftTableProbeRT : public SemanticRoundTripFixture,
                               public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ShiftTableProbeRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kShiftTable = {
  // VQRDMULH: rounding doubling multiply high, signed 8x i16.
  {"vqrdmulh_s16",
   "#include <arm_neon.h>\n"
   "int p_vqrdmulh_s16(int a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t va={x,(short)-x,32767,-32768,16384,-16384,1,-1};\n"
   "  int16x8_t vb={32767,-32768,32767,-32768,2,2,32767,32767};\n"
   "  int16x8_t r=vqrdmulhq_s16(va,vb);\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^(unsigned short)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {300}, "ShiftTable", 1, "-mfpu=neon"},

  // VQDMULH: doubling multiply high (no rounding), signed 4x i32.
  {"vqdmulh_s32",
   "#include <arm_neon.h>\n"
   "int p_vqdmulh_s32(int a){\n"
   "  int x=(int)a;\n"
   "  int32x4_t va={x,-x,0x7FFFFFFF,(int)0x80000000};\n"
   "  int32x4_t vb={0x7FFFFFFF,(int)0x80000000,0x7FFFFFFF,(int)0x80000000};\n"
   "  int32x4_t r=vqdmulhq_s32(va,vb);\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {123456}, "ShiftTable", 1, "-mfpu=neon"},

  // VQRSHL: signed saturating rounding shift left by per-lane signed count.
  {"vqrshl_s32",
   "#include <arm_neon.h>\n"
   "int p_vqrshl_s32(int a){\n"
   "  int x=(int)a;\n"
   "  int32x4_t v={x+2000000000,x-2000000000,12345,-12345};\n"
   "  int32x4_t s={5,-3,28,-1};\n"
   "  int32x4_t r=vqrshlq_s32(v,s);\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {100}, "ShiftTable", 1, "-mfpu=neon"},

  // VRSHL: signed rounding shift left (no saturation), 8x i16.
  {"vrshl_s16",
   "#include <arm_neon.h>\n"
   "int p_vrshl_s16(int a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t v={x,(short)-x,1000,-1000,32767,-32768,3,-3};\n"
   "  int16x8_t s={3,-2,7,-1,1,-15,4,-4};\n"
   "  int16x8_t r=vrshlq_s16(v,s);\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^(unsigned short)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {77}, "ShiftTable", 1, "-mfpu=neon"},

  // VQSHL: signed saturating shift left by per-lane count.
  {"vqshl_s32",
   "#include <arm_neon.h>\n"
   "int p_vqshl_s32(int a){\n"
   "  int x=(int)a;\n"
   "  int32x4_t v={x,1000000,-1000000,3};\n"
   "  int32x4_t s={20,12,12,-2};\n"
   "  int32x4_t r=vqshlq_s32(v,s);\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {5000}, "ShiftTable", 1, "-mfpu=neon"},

  // VQSHLU: signed->unsigned saturating shift left by immediate.
  {"vqshlu_s16",
   "#include <arm_neon.h>\n"
   "int p_vqshlu_s16(int a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t v={x,(short)-x,100,-100,200,-1,32767,-32768};\n"
   "  uint16x8_t r=vqshluq_n_s16(v,4);\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {300}, "ShiftTable", 1, "-mfpu=neon"},

  // VQRSHRN: signed saturating rounding shift right narrow, i32 -> i16.
  {"vqrshrn_s32",
   "#include <arm_neon.h>\n"
   "int p_vqrshrn_s32(int a){\n"
   "  int x=(int)a;\n"
   "  int32x4_t v={x<<8,0x7FFFFFFF,(int)0x80000000,-12345};\n"
   "  int16x4_t r=vqrshrn_n_s32(v,10);\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned short)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {54321}, "ShiftTable", 1, "-mfpu=neon"},

  // VRSHRN: rounding shift right narrow (no saturation), i16 -> i8.
  {"vrshrn_s16",
   "#include <arm_neon.h>\n"
   "int p_vrshrn_s16(int a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t v={x,(short)-x,0x7FFF,(short)0x8000,0x0180,0x00C0,0x00FF,0x1234};\n"
   "  int8x8_t r=vrshrn_n_s16(v,4);\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^(unsigned char)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {333}, "ShiftTable", 1, "-mfpu=neon"},

  // VQRSHRUN: signed saturating rounding shift right narrow to unsigned.
  {"vqrshrun_s16",
   "#include <arm_neon.h>\n"
   "int p_vqrshrun_s16(int a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t v={x,(short)-x,0x7FFF,(short)0x8000,300,-1,255,256};\n"
   "  uint8x8_t r=vqrshrun_n_s16(v,4);\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {500}, "ShiftTable", 1, "-mfpu=neon"},

  // VQMOVUN: signed saturating extract to unsigned narrow, i16 -> u8.
  {"vqmovun_s16",
   "#include <arm_neon.h>\n"
   "int p_vqmovun_s16(int a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t v={x,(short)-x,300,-1,255,256,32767,-32768};\n"
   "  uint8x8_t r=vqmovun_s16(v);\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {200}, "ShiftTable", 1, "-mfpu=neon"},

  // VRADDHN: rounding add then high half, i32 -> i16.
  {"vraddhn_s32",
   "#include <arm_neon.h>\n"
   "int p_vraddhn_s32(int a){\n"
   "  int x=(int)a;\n"
   "  int32x4_t va={x<<10,0x7FFF8000,0x12348000,-1};\n"
   "  int32x4_t vb={0x00008000,0x00008000,0x00010000,0x00008000};\n"
   "  int16x4_t r=vraddhn_s32(va,vb);\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned short)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {7}, "ShiftTable", 1, "-mfpu=neon"},

  // VPADDL: signed pairwise add long, i16 -> i32.
  {"vpaddl_s16",
   "#include <arm_neon.h>\n"
   "int p_vpaddl_s16(int a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t v={x,(short)-x,30000,30000,-30000,-30000,1,-2};\n"
   "  int32x4_t r=vpaddlq_s16(v);\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {12345}, "ShiftTable", 1, "-mfpu=neon"},

  // VPADAL: signed pairwise add-accumulate long, i8 -> i16.
  {"vpadal_s8",
   "#include <arm_neon.h>\n"
   "int p_vpadal_s8(int a){\n"
   "  signed char x=(signed char)a;\n"
   "  int16x8_t acc={1000,-1000,2000,-2000,3000,-3000,4000,-4000};\n"
   "  int8x16_t v={x,(signed char)-x,100,100,-100,-100,1,-1,\n"
   "               50,-50,127,127,-128,-128,2,-2};\n"
   "  int16x8_t r=vpadalq_s8(acc,v);\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^(unsigned short)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {44}, "ShiftTable", 1, "-mfpu=neon"},

  // VTBL2: table lookup over two d-registers, out-of-range index -> 0.
  {"vtbl2_u8",
   "#include <arm_neon.h>\n"
   "int p_vtbl2_u8(int a){\n"
   "  unsigned char x=(unsigned char)a;\n"
   "  uint8x8x2_t tab;\n"
   "  tab.val[0]=(uint8x8_t){10,11,12,13,14,15,16,17};\n"
   "  tab.val[1]=(uint8x8_t){20,21,22,23,24,25,26,27};\n"
   "  uint8x8_t idx={x,0,15,16,7,8,200,3};\n"
   "  uint8x8_t r=vtbl2_u8(tab,idx);\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {5}, "ShiftTable", 1, "-mfpu=neon"},

  // VTBX2: table extend over two d-registers (out-of-range keeps dest).
  {"vtbx2_u8",
   "#include <arm_neon.h>\n"
   "int p_vtbx2_u8(int a){\n"
   "  unsigned char x=(unsigned char)a;\n"
   "  uint8x8_t dst={100,101,102,103,104,105,106,107};\n"
   "  uint8x8x2_t tab;\n"
   "  tab.val[0]=(uint8x8_t){10,11,12,13,14,15,16,17};\n"
   "  tab.val[1]=(uint8x8_t){20,21,22,23,24,25,26,27};\n"
   "  uint8x8_t idx={x,0,15,16,7,8,200,3};\n"
   "  uint8x8_t r=vtbx2_u8(dst,tab,idx);\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {5}, "ShiftTable", 1, "-mfpu=neon"},

  // VQABS / VQNEG: saturating abs/neg, INT_MIN boundary, 4x i32.
  {"vqabs_vqneg_s32",
   "#include <arm_neon.h>\n"
   "int p_vqabs_vqneg_s32(int a){\n"
   "  int x=(int)a;\n"
   "  int32x4_t v={x,-x,(int)0x80000000,0x7FFFFFFF};\n"
   "  int32x4_t qa=vqabsq_s32(v);\n"
   "  int32x4_t qn=vqnegq_s32(v);\n"
   "  unsigned o=0;\n"
   "  for(int i=0;i<4;i++) o=o*131u^(unsigned)qa[i];\n"
   "  for(int i=0;i<4;i++) o=o*131u^(unsigned)qn[i];\n"
   "  return (int)o;\n"
   "}\n",
   {12345}, "ShiftTable", 1, "-mfpu=neon"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ShiftTable, ARM32ShiftTableProbeRT,
                         ::testing::ValuesIn(kShiftTable), rtTCName);
