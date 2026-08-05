//===- ARM32_NEONScalarWidenMACRTTests.cpp - by-scalar widen MAC -*- C++ -*-=//
//
// Roundtrip probes for ARM32 NEON multiply families #386 only partially fixed.
// #386 taught the SAME-width VMUL/VMLA/VMLS handlers to broadcast a `dM[idx]`
// scalar lane; the WIDENING / SATURATING-DOUBLING siblings were left behind:
//   * VMULL / VMLAL / VMLSL  (widening)              — by-scalar lane.
//   * VQDMULL / VQDMULH / VQRDMULH                   — by-scalar lane.
//   * VQDMLAL / VQDMLSL (saturating doubling MAC)    — bare INT_MULT placeholder
//     (no doubling, no saturation, no widening, and VQDMLSL even accumulated
//      with INT_ADD instead of INT_SUB).
//
// Each kernel uses a non-zero scalar lane (so the buggy per-lane walk reads
// dM[0..N] instead of broadcasting dM[idx]) and INT_MIN/INT_MAX operands (so a
// missing doubling-saturation diverges).  Lanes hash-reduce to a bit-exact
// scalar return.  vmul_lane/vmla_lane controls confirm the #386 path still works.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32ScalarWidenMACRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ScalarWidenMACRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kScalarWidenMAC = {
  // --- controls: same-width by-scalar (fixed in #386), must stay green ---
  {"vmul_lane_s16",
   "#include <arm_neon.h>\n"
   "int p_vmul_lane_s16(int a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t va={x,(short)-x,1000,-1000,7,-7,32767,-32768};\n"
   "  int16x4_t vb={3,7,11,13};\n"
   "  int16x8_t r=vmulq_lane_s16(va,vb,1);\n"  // multiplier = vb[1]=7
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^(unsigned short)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {300}, "ScalarWidenMAC", 1, "-mfpu=neon"},

  {"vmla_lane_s32",
   "#include <arm_neon.h>\n"
   "int p_vmla_lane_s32(int a){\n"
   "  int x=(int)a;\n"
   "  int32x4_t acc={100,200,300,400};\n"
   "  int32x4_t va={x,-x,12345,-12345};\n"
   "  int32x2_t vb={7,13};\n"
   "  int32x4_t r=vmlaq_lane_s32(acc,va,vb,1);\n"  // += va[i]*vb[1]=13
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {500}, "ScalarWidenMAC", 1, "-mfpu=neon"},

  // --- VMULL by-scalar lane (widening, no scalar detection at all) ---
  {"vmull_lane_s16",
   "#include <arm_neon.h>\n"
   "int p_vmull_lane_s16(int a){\n"
   "  short x=(short)a;\n"
   "  int16x4_t va={x,(short)-x,1000,-1000};\n"
   "  int16x4_t vb={3,7,11,13};\n"
   "  int32x4_t r=vmull_lane_s16(va,vb,1);\n"  // multiplier = vb[1]=7
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {321}, "ScalarWidenMAC", 1, "-mfpu=neon"},

  {"vmull_lane_u16",
   "#include <arm_neon.h>\n"
   "int p_vmull_lane_u16(int a){\n"
   "  unsigned short x=(unsigned short)a;\n"
   "  uint16x4_t va={x,(unsigned short)(x+1),60000,40000};\n"
   "  uint16x4_t vb={3,7,11,13};\n"
   "  uint32x4_t r=vmull_lane_u16(va,vb,2);\n"  // multiplier = vb[2]=11
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {777}, "ScalarWidenMAC", 1, "-mfpu=neon"},

  {"vmull_lane_s32",
   "#include <arm_neon.h>\n"
   "int p_vmull_lane_s32(int a){\n"
   "  int x=(int)a;\n"
   "  int32x2_t va={x,-x};\n"
   "  int32x2_t vb={7,13};\n"
   "  int64x2_t r=vmull_lane_s32(va,vb,1);\n"  // multiplier = vb[1]=13
   "  unsigned o=0; for(int i=0;i<2;i++) o=o*131u^(unsigned)(r[i]^(r[i]>>32));\n"
   "  return (int)o;\n"
   "}\n",
   {54321}, "ScalarWidenMAC", 1, "-mfpu=neon"},

  // --- VMLAL / VMLSL by-scalar lane (BScalar=(B.Size<=InSz) never fires) ---
  {"vmlal_lane_s16",
   "#include <arm_neon.h>\n"
   "int p_vmlal_lane_s16(int a){\n"
   "  short x=(short)a;\n"
   "  int32x4_t acc={100,200,300,400};\n"
   "  int16x4_t va={x,(short)-x,1000,-1000};\n"
   "  int16x4_t vb={3,7,11,13};\n"
   "  int32x4_t r=vmlal_lane_s16(acc,va,vb,1);\n"  // acc[i]+=va[i]*vb[1]=7
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {246}, "ScalarWidenMAC", 1, "-mfpu=neon"},

  {"vmlsl_lane_s16",
   "#include <arm_neon.h>\n"
   "int p_vmlsl_lane_s16(int a){\n"
   "  short x=(short)a;\n"
   "  int32x4_t acc={100000,-200000,300000,-400000};\n"
   "  int16x4_t va={x,(short)-x,1000,-1000};\n"
   "  int16x4_t vb={3,7,11,13};\n"
   "  int32x4_t r=vmlsl_lane_s16(acc,va,vb,2);\n"  // acc[i]-=va[i]*vb[2]=11
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {135}, "ScalarWidenMAC", 1, "-mfpu=neon"},

  // --- VQDMLAL / VQDMLSL vector (placeholder: no doubling/sat/widen) ---
  // acc[i] = SignedSat(acc[i] +/- SignedSat(2*va[i]*vb[i]))
  {"vqdmlal_s16",
   "#include <arm_neon.h>\n"
   "int p_vqdmlal_s16(int a){\n"
   "  int x=(int)a;\n"
   "  int32x4_t acc={x,100,0x7FFFFFF0,(int)0x80000000};\n"
   "  int16x4_t va={(short)x,(short)-x,32767,-32768};\n"
   "  int16x4_t vb={3,5,32767,-32768};\n"
   "  int32x4_t r=vqdmlal_s16(acc,va,vb);\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {1000}, "ScalarWidenMAC", 1, "-mfpu=neon"},

  {"vqdmlsl_s16",
   "#include <arm_neon.h>\n"
   "int p_vqdmlsl_s16(int a){\n"
   "  int x=(int)a;\n"
   "  int32x4_t acc={x,100,(int)0x80000010,0x7FFFFFFF};\n"
   "  int16x4_t va={(short)x,(short)-x,32767,-32768};\n"
   "  int16x4_t vb={3,5,32767,-32768};\n"
   "  int32x4_t r=vqdmlsl_s16(acc,va,vb);\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {1000}, "ScalarWidenMAC", 1, "-mfpu=neon"},

  {"vqdmlal_s32",
   "#include <arm_neon.h>\n"
   "int p_vqdmlal_s32(int a){\n"
   "  long long x=(long long)a;\n"
   "  int64x2_t acc={x,0x7FFFFFFFFFFFFF00LL};\n"
   "  int32x2_t va={(int)x,(int)0x80000000};\n"
   "  int32x2_t vb={5,(int)0x80000000};\n"
   "  int64x2_t r=vqdmlal_s32(acc,va,vb);\n"
   "  unsigned o=0; for(int i=0;i<2;i++) o=o*131u^(unsigned)(r[i]^(r[i]>>32));\n"
   "  return (int)o;\n"
   "}\n",
   {1234}, "ScalarWidenMAC", 1, "-mfpu=neon"},

  // --- VQDMULL by-scalar lane (widening saturating doubling multiply) ---
  {"vqdmull_lane_s16",
   "#include <arm_neon.h>\n"
   "int p_vqdmull_lane_s16(int a){\n"
   "  short x=(short)a;\n"
   "  int16x4_t va={x,(short)-x,32767,-32768};\n"
   "  int16x4_t vb={3,7,32767,-32768};\n"
   "  int32x4_t r=vqdmull_lane_s16(va,vb,1);\n"  // 2*va[i]*vb[1]=7 saturating
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {300}, "ScalarWidenMAC", 1, "-mfpu=neon"},

  // --- VQDMULH / VQRDMULH by-scalar lane (doubling multiply high) ---
  {"vqdmulh_lane_s16",
   "#include <arm_neon.h>\n"
   "int p_vqdmulh_lane_s16(int a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t va={x,(short)-x,32767,-32768,16384,-16384,1,-1};\n"
   "  int16x4_t vb={3,32767,-32768,13};\n"
   "  int16x8_t r=vqdmulhq_lane_s16(va,vb,1);\n"  // doubling mul-high by vb[1]
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^(unsigned short)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {500}, "ScalarWidenMAC", 1, "-mfpu=neon"},

  {"vqrdmulh_lane_s16",
   "#include <arm_neon.h>\n"
   "int p_vqrdmulh_lane_s16(int a){\n"
   "  short x=(short)a;\n"
   "  int16x8_t va={x,(short)-x,32767,-32768,16384,-16384,1,-1};\n"
   "  int16x4_t vb={3,32767,-32768,13};\n"
   "  int16x8_t r=vqrdmulhq_lane_s16(va,vb,2);\n"  // rounding doubling mul-high
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^(unsigned short)r[i];\n"
   "  return (int)o;\n"
   "}\n",
   {600}, "ScalarWidenMAC", 1, "-mfpu=neon"},

  // --- inline-asm by-scalar widening forms `vmull.s16 q,d,d[idx]` ---
  // clang splats the lane (dup + vector op) for the intrinsics above, so the
  // `d[idx]` encoding is forced via asm; ARM32 operandRead ignores vector_index
  // and returns the whole Dm, so VMULL/VMLAL/VMLSL walked d[0..N] per lane.
  // Inputs are derived from `a` at runtime so clang materializes them with
  // immediates instead of a -O0 rodata literal-pool template (which the recomp
  // stack path can't roundtrip and would mask the lane behaviour with a 0).
  {"vmull_s16_byscalar",
   "int p_vmull_s16bs(int a){\n"
   "  short A[4]={(short)a,(short)-a,(short)(a+1000),(short)(a-1000)};\n"
   "  short B[4]={(short)(a+3),(short)(a+7),(short)(a+11),(short)(a+13)};\n"
   "  unsigned R[4];\n"
   "  __asm__ volatile(\n"
   "    \"vld1.16 {d2},[%1]\\n\\t\"\n"
   "    \"vld1.16 {d3},[%2]\\n\\t\"\n"
   "    \"vmull.s16 q0, d2, d3[1]\\n\\t\"\n"
   "    \"vst1.32 {d0,d1},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"q0\",\"d2\",\"d3\",\"memory\");\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^R[i];\n"
   "  return (int)o;\n"
   "}\n",
   {321}, "ScalarWidenMAC", 0, "-mfpu=neon"},

  {"vmull_u16_byscalar",
   "int p_vmull_u16bs(int a){\n"
   "  unsigned short A[4]={(unsigned short)a,(unsigned short)(a+9),\n"
   "                       (unsigned short)(a*3),(unsigned short)(a*5)};\n"
   "  unsigned short B[4]={(unsigned short)(a+3),(unsigned short)(a+7),\n"
   "                       (unsigned short)(a+11),(unsigned short)(a+13)};\n"
   "  unsigned R[4];\n"
   "  __asm__ volatile(\n"
   "    \"vld1.16 {d2},[%1]\\n\\t\"\n"
   "    \"vld1.16 {d3},[%2]\\n\\t\"\n"
   "    \"vmull.u16 q0, d2, d3[2]\\n\\t\"\n"
   "    \"vst1.32 {d0,d1},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"q0\",\"d2\",\"d3\",\"memory\");\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^R[i];\n"
   "  return (int)o;\n"
   "}\n",
   {777}, "ScalarWidenMAC", 0, "-mfpu=neon"},

  {"vmull_s32_byscalar",
   "int p_vmull_s32bs(int a){\n"
   "  int A[2]={(int)a,(int)-a};\n"
   "  int B[2]={(int)(a*7),(int)(a*13)};\n"
   "  unsigned long long R[2];\n"
   "  __asm__ volatile(\n"
   "    \"vld1.32 {d2},[%1]\\n\\t\"\n"
   "    \"vld1.32 {d3},[%2]\\n\\t\"\n"
   "    \"vmull.s32 q0, d2, d3[1]\\n\\t\"\n"
   "    \"vst1.64 {d0,d1},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"q0\",\"d2\",\"d3\",\"memory\");\n"
   "  unsigned o=0; for(int i=0;i<2;i++) o=o*131u^(unsigned)(R[i]^(R[i]>>32));\n"
   "  return (int)o;\n"
   "}\n",
   {54321}, "ScalarWidenMAC", 0, "-mfpu=neon"},

  {"vmlal_s16_byscalar",
   "int p_vmlal_s16bs(int a){\n"
   "  short A[4]={(short)a,(short)-a,(short)(a+1000),(short)(a-1000)};\n"
   "  short B[4]={(short)(a+3),(short)(a+7),(short)(a+11),(short)(a+13)};\n"
   "  int ACC[4]={a+100,a-200,a+300,a-400};\n"
   "  unsigned R[4];\n"
   "  __asm__ volatile(\n"
   "    \"vld1.32 {d0,d1},[%3]\\n\\t\"\n"
   "    \"vld1.16 {d2},[%1]\\n\\t\"\n"
   "    \"vld1.16 {d3},[%2]\\n\\t\"\n"
   "    \"vmlal.s16 q0, d2, d3[1]\\n\\t\"\n"
   "    \"vst1.32 {d0,d1},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B),\"r\"(ACC): \"q0\",\"d2\",\"d3\",\"memory\");\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^R[i];\n"
   "  return (int)o;\n"
   "}\n",
   {246}, "ScalarWidenMAC", 0, "-mfpu=neon"},

  {"vmlsl_s16_byscalar",
   "int p_vmlsl_s16bs(int a){\n"
   "  short A[4]={(short)a,(short)-a,(short)(a+1000),(short)(a-1000)};\n"
   "  short B[4]={(short)(a+3),(short)(a+7),(short)(a+11),(short)(a+13)};\n"
   "  int ACC[4]={a*100,a*-200,a*300,a*-400};\n"
   "  unsigned R[4];\n"
   "  __asm__ volatile(\n"
   "    \"vld1.32 {d0,d1},[%3]\\n\\t\"\n"
   "    \"vld1.16 {d2},[%1]\\n\\t\"\n"
   "    \"vld1.16 {d3},[%2]\\n\\t\"\n"
   "    \"vmlsl.s16 q0, d2, d3[2]\\n\\t\"\n"
   "    \"vst1.32 {d0,d1},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B),\"r\"(ACC): \"q0\",\"d2\",\"d3\",\"memory\");\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^R[i];\n"
   "  return (int)o;\n"
   "}\n",
   {135}, "ScalarWidenMAC", 0, "-mfpu=neon"},

  // VQDMULH/VQRDMULH/VQDMULL by-scalar go through the ArmVqdmul* intrinsic; the
  // lifter passed the whole Dm so the emitter multiplied lane-by-lane instead of
  // broadcasting the selected lane (common in Q15 fixed-point DSP binaries).
  {"vqdmulh_s16_byscalar",
   "int p_vqdmulh_s16bs(int a){\n"
   "  short A[8]={(short)a,(short)-a,(short)(a+9),(short)(a-9),\n"
   "              (short)(a*2),(short)(a*3),(short)(a+5),(short)(a-5)};\n"
   "  short B[4]={(short)(a+3),(short)(a+7),(short)(a+11),(short)(a+13)};\n"
   "  unsigned short R[8];\n"
   "  __asm__ volatile(\n"
   "    \"vld1.16 {d2,d3},[%1]\\n\\t\"\n"
   "    \"vld1.16 {d4},[%2]\\n\\t\"\n"
   "    \"vqdmulh.s16 q0, q1, d4[1]\\n\\t\"\n"
   "    \"vst1.16 {d0,d1},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"q0\",\"q1\",\"d4\",\"memory\");\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^R[i];\n"
   "  return (int)o;\n"
   "}\n",
   {321}, "ScalarWidenMAC", 0, "-mfpu=neon"},

  {"vqrdmulh_s16_byscalar",
   "int p_vqrdmulh_s16bs(int a){\n"
   "  short A[8]={(short)a,(short)-a,(short)(a+9),(short)(a-9),\n"
   "              (short)(a*2),(short)(a*3),(short)(a+5),(short)(a-5)};\n"
   "  short B[4]={(short)(a+3),(short)(a+7),(short)(a+11),(short)(a+13)};\n"
   "  unsigned short R[8];\n"
   "  __asm__ volatile(\n"
   "    \"vld1.16 {d2,d3},[%1]\\n\\t\"\n"
   "    \"vld1.16 {d4},[%2]\\n\\t\"\n"
   "    \"vqrdmulh.s16 q0, q1, d4[2]\\n\\t\"\n"
   "    \"vst1.16 {d0,d1},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"q0\",\"q1\",\"d4\",\"memory\");\n"
   "  unsigned o=0; for(int i=0;i<8;i++) o=o*131u^R[i];\n"
   "  return (int)o;\n"
   "}\n",
   {654}, "ScalarWidenMAC", 0, "-mfpu=neon"},

  {"vqdmull_s16_byscalar",
   "int p_vqdmull_s16bs(int a){\n"
   "  short A[4]={(short)a,(short)-a,(short)(a+1000),(short)(a-1000)};\n"
   "  short B[4]={(short)(a+3),(short)(a+7),(short)(a+11),(short)(a+13)};\n"
   "  unsigned R[4];\n"
   "  __asm__ volatile(\n"
   "    \"vld1.16 {d2},[%1]\\n\\t\"\n"
   "    \"vld1.16 {d4},[%2]\\n\\t\"\n"
   "    \"vqdmull.s16 q0, d2, d4[1]\\n\\t\"\n"
   "    \"vst1.32 {d0,d1},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"q0\",\"d2\",\"d4\",\"memory\");\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^R[i];\n"
   "  return (int)o;\n"
   "}\n",
   {300}, "ScalarWidenMAC", 0, "-mfpu=neon"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ScalarWidenMAC, ARM32ScalarWidenMACRT,
                         ::testing::ValuesIn(kScalarWidenMAC), rtTCName);
