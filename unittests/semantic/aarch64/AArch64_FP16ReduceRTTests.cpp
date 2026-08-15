//===- AArch64_FP16ReduceRTTests.cpp - half-precision FADDP / FMAXV -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for AArch64 half-precision (FEAT_FP16) pairwise add
// (FADDP) and horizontal min/max reductions (FMAXV/FMINV/FMAXNMV/FMINNMV).
//
// Same VAS->LaneSz whitelist gap as #41: FADDP only accepted .4S/.2S/.2D, so
// `faddp v.4h` became one f64 add on packed bits, and scalar `faddp h, v.2h`
// copied the source.  FMAXV/FMINV/FMAXNMV/FMINNMV treated ElemSz < 4 as
// "take the low lane", so an FP16 reduction never looked past lane 0.
//
// Inputs are chosen so a whole-register f64 add, a lane0 copy, and the real
// pairwise / across-lane result are three different bit patterns.
// Data moves via integer `fmov` (bit copy, no fcvt).
// fp16 bits: 0.0=0x0000 -0.0=0x8000 1.0=0x3C00 4.0=0x4400 5.0=0x4500
// 9.0=0x4880 11.0=0x4980 13.0=0x4A80 qNaN=0x7E00.
// Requires -march=armv8.2-a+fp16; Unicorn MAX executes these natively.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64FP16ReduceRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64FP16ReduceRT, Verify) { roundTripAArch64(GetParam()); }

#define FP16FLAGS "FP16Reduce", 0, "-march=armv8.2-a+fp16"

#define H_1425 0x4200400044003C00ULL // [1.0, 4.0, 2.0, 3.0]
#define H_4123 0x420040003C004400ULL // [4.0, 1.0, 2.0, 3.0]
#define H_5067 0x4700460000004500ULL // [5.0, 0.0, 6.0, 7.0]
#define H_NAN5 0x4200800045007E00ULL // [qNaN, 5.0, -0.0, 3.0]
#define H_9810 0x4900498048004880ULL // [9.0, 8.0, 11.0, 10.0]

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // --- .4H FADDP: pairs (a0+a1)(a2+a3) then (b0+b1)(b2+b3) ---
  // a=[1,4,2,3] b=[5,0,6,7] -> [5,5,5,13] = 0x4A80450045004500
  {"faddp_4h",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n"
   " faddp v2.4h,v0.4h,v1.4h\\n fmov %0,d2\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {H_1425, H_5067}, FP16FLAGS},

  // --- .8H FADDP: XOR the two result halves so a low-64-only lowering fails ---
  // dup'd a -> low [5,5,5,5]; dup'd b -> high [5,13,5,13]; xor = 0x0F8000000F800000
  {"faddp_8h",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"dup v0.2d,%1\\n dup v1.2d,%2\\n"
   " faddp v2.8h,v0.8h,v1.8h\\n"
   " fmov x8,d2\\n mov x9,v2.d[1]\\n eor %0,x8,x9\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\",\"v2\",\"x8\",\"x9\");"
   "return (long)r;}\n",
   {H_1425, H_5067}, FP16FLAGS},

  // --- Scalar FADDP h,.2h: [4,1,..] so a copy of lane0 (4.0) != 4+1 (5.0) ---
  {"faddp_scalar_2h",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov d0,%1\\n faddp h1,v0.2h\\n fmov %w0,s1\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {H_4123}, FP16FLAGS},

  // --- .4H FMAXV: max(1,4,2,3)=4.0=0x4400; a lane0 copy would be 1.0 ---
  {"fmaxv_4h",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmaxv h1,v0.4h\\n fmov %w0,s1\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {H_1425}, FP16FLAGS},

  // --- .4H FMINV: [4,1,2,3] so min=1.0 but lane0=4.0 ---
  {"fminv_4h",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov d0,%1\\n fminv h1,v0.4h\\n fmov %w0,s1\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {H_4123}, FP16FLAGS},

  // --- .4H FMAXV / FMAXNMV NaN: V propagates, NMV suppresses to 5.0 ---
  {"fmaxv_4h_nan",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmaxv h1,v0.4h\\n fmov %w0,s1\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {H_NAN5}, FP16FLAGS},

  {"fmaxnmv_4h_nan",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmaxnmv h1,v0.4h\\n fmov %w0,s1\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {H_NAN5}, FP16FLAGS},

  // --- .4H FMINV / FMINNMV NaN: V propagates, NMV returns -0 ---
  {"fminv_4h_nan",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov d0,%1\\n fminv h1,v0.4h\\n fmov %w0,s1\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {H_NAN5}, FP16FLAGS},

  {"fminnmv_4h_nan",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov d0,%1\\n fminnmv h1,v0.4h\\n fmov %w0,s1\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {H_NAN5}, FP16FLAGS},

  // --- .8H FMAXV: max lives in the high half (11.0); lane0 copy is 1.0 ---
  {"fmaxv_8h",
   "long f(long a,long b){unsigned int r;"
   "__asm__ volatile(\"fmov d0,%1\\n mov v0.d[1],%2\\n"
   " fmaxv h1,v0.8h\\n fmov %w0,s1\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\");"
   "return (long)r;}\n",
   {H_1425, H_9810}, FP16FLAGS},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(FP16Reduce, AArch64FP16ReduceRT,
                         ::testing::ValuesIn(kA64), rtTCName);
