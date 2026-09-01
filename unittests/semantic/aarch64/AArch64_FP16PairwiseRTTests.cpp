//===- AArch64_FP16PairwiseRTTests.cpp - half-precision pairwise --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for AArch64 half-precision (FEAT_FP16) pairwise floating-
// point min/max (FMAXP/FMINP/FMAXNMP/FMINNMP) and the element-wise
// FMAXNM/FMINNM that shares their lowering.
//
// These carry the same VAS->LaneSz blind spot as the #290/#291 FP16 fixes:
// AArch64LiftNEONFloat.cpp accepted only LaneSz 4 (.4S/.2S) and 8 (.2D), so a
// half-precision `fmaxp v.4h` fell into the whole-register `else` branch and
// the packed fp16 lanes were reinterpreted as ONE f64 min/max — no adjacent
// pairing happened at all and only the low 64 bits of the result were written.
// The scalar `fmaxp h0, v1.2h` form failed even earlier: neonElemSize did not
// recognise VL_2H, so it degenerated to a plain register copy.
//
// Values are chosen so every result lane differs from every other, and from
// both inputs: a lowering that pairs the wrong lanes, drops a half, or reduces
// element-wise instead of pairwise cannot produce the expected pattern.
//
// Data is moved in/out of the vector registers via integer `fmov` (a pure bit
// copy, no fcvt), so these probes exercise ONLY the pairwise handlers.
// fp16 bit patterns: 0.0=0x0000 -0.0=0x8000 1.0=0x3C00 2.0=0x4000 3.0=0x4200
// 4.0=0x4400 5.0=0x4500 6.0=0x4600 7.0=0x4700 qNaN=0x7E00.
// Requires -march=armv8.2-a+fp16; select Unicorn's MAX CPU explicitly so the
// fp16 pairwise min/max instructions execute natively.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64FP16PairwiseRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {
};
TEST_P(AArch64FP16PairwiseRT, Verify) { roundTripAArch64(GetParam()); }

#define FP16FLAGS                                                              \
  "FP16Pairwise", 0, "-march=armv8.2-a+fp16", false, "", UC_CPU_ARM64_MAX

// Lane packing is little-endian: lane0 occupies bits 0..15.
#define H_1425 0x4200400044003C00ULL // [1.0, 4.0, 2.0, 3.0]
#define H_4123 0x420040003C004400ULL // [4.0, 1.0, 2.0, 3.0]
#define H_5067 0x4700460000004500ULL // [5.0, 0.0, 6.0, 7.0]
#define H_NAN5 0x4200800045007E00ULL // [qNaN, 5.0, -0.0, 3.0]
#define H_120P 0x4700000040003C00ULL // [1.0, 2.0, +0.0, 7.0]

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // --- .4H FMAXP: pairs are (a0,a1)(a2,a3) then (b0,b1)(b2,b3) ---
  // a=[1,4,2,3] b=[5,0,6,7] -> [4,3,5,7] = 0x4700450042004400
  {"fmaxp_4h",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n"
   " fmaxp v2.4h,v0.4h,v1.4h\\n fmov %0,d2\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {H_1425, H_5067}, FP16FLAGS},

  // --- .4H FMINP ---
  // a=[1,4,2,3] b=[5,0,6,7] -> [1,2,0,6] = 0x4600000040003C00
  {"fminp_4h",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n"
   " fminp v2.4h,v0.4h,v1.4h\\n fmov %0,d2\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {H_1425, H_5067}, FP16FLAGS},

  // --- .4H FMINP with NaN + signed zero: FMIN semantics PROPAGATE the NaN ---
  // a=[NaN,5,-0,3] b=[1,2,+0,7] -> [NaN,-0,1,+0] = 0x00003C0080007E00
  {"fminp_4h_nan",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n"
   " fminp v2.4h,v0.4h,v1.4h\\n fmov %0,d2\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {H_NAN5, H_120P}, FP16FLAGS},

  // --- .4H FMINNMP: minNum SUPPRESSES the NaN (lane0 = 5.0, not NaN) ---
  // a=[NaN,5,-0,3] b=[1,2,+0,7] -> [5,-0,1,+0] = 0x00003C0080004500
  {"fminnmp_4h_nan",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n"
   " fminnmp v2.4h,v0.4h,v1.4h\\n fmov %0,d2\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {H_NAN5, H_120P}, FP16FLAGS},

  // --- .4H FMAXNMP: maxNum, NaN-suppressing ---
  // a=[NaN,5,-0,3] b=[1,2,+0,7] -> [5,3,2,7] = 0x4700400042004500
  {"fmaxnmp_4h_nan",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n"
   " fmaxnmp v2.4h,v0.4h,v1.4h\\n fmov %0,d2\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {H_NAN5, H_120P}, FP16FLAGS},

  // --- .8H FMAXP (8 fp16 lanes in a Q register) ---
  // `dup v.2d,x` fills both 64-bit halves with the same 4 lanes (clean full-128
  // init, avoiding the orthogonal partial-vector-write path).  Result lanes 0-3
  // come from Vn and lanes 4-7 from Vm, so XOR-ing the two result halves checks
  // that BOTH source vectors reached the right output half:
  // low=[4,3,4,3] high=[5,7,5,7] -> xor = 0x0500010005000100
  {"fmaxp_8h",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"dup v0.2d,%1\\n dup v1.2d,%2\\n"
   " fmaxp v2.8h,v0.8h,v1.8h\\n"
   " fmov x8,d2\\n mov x9,v2.d[1]\\n eor %0,x8,x9\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\",\"v2\",\"x8\",\"x9\");"
   "return (long)r;}\n",
   {H_1425, H_5067}, FP16FLAGS},

  // --- Scalar FMAXP h,.2h: reduces the low lane pair to a single half ---
  // a=[1,4,..] -> max(1,4) = 4.0 = 0x4400 (upper bits zeroed by the H write)
  {"fmaxp_scalar_2h",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmaxp h1,v0.2h\\n fmov %w0,s1\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {H_1425}, FP16FLAGS},

  // --- Scalar FMINP h,.2h ---
  // Uses the DESCENDING [4,1,..] input: with [1,4,..] the broken lowering's
  // plain register copy returns lane0 = 1.0, which coincidentally equals
  // min(1,4) and passes.  Here min(4,1) = 1.0 = 0x3C00 but lane0 is 4.0, so a
  // copy is distinguishable from a real pairwise reduction.
  {"fminp_scalar_2h",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov d0,%1\\n fminp h1,v0.2h\\n fmov %w0,s1\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {H_4123}, FP16FLAGS},

  // --- .4H FMAXNM / FMINNM: element-wise (NOT pairwise) minNum/maxNum, which
  // share the pairwise handler's lane-size guard in AArch64LiftNEONFloat.cpp
  // and were reinterpreted as one f64 op for the same reason. ---
  // a=[NaN,5,-0,3] b=[1,2,+0,7] -> max [1,5,+0,7] = 0x4700000045003C00
  {"fmaxnm_4h_nan",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n"
   " fmaxnm v2.4h,v0.4h,v1.4h\\n fmov %0,d2\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {H_NAN5, H_120P}, FP16FLAGS},

  // --- .8H FMAXNM: initialise both Q-register halves independently, then XOR
  // the result halves so dropping or zeroing either half is observable. ---
  // low maxNum([NaN,5,-0,3], [1,2,+0,7]) = [1,5,+0,7]
  // high maxNum([1,4,2,3], [5,0,6,7]) = [5,4,6,7]
  // low64 ^ high64 = 0x0000460001007900.
  {"fmaxnm_8h",
   "long f(long al,long ah,long bl,long bh){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n mov v0.d[1],%2\\n"
   " fmov d1,%3\\n mov v1.d[1],%4\\n"
   " fmaxnm v2.8h,v0.8h,v1.8h\\n"
   " fmov x8,d2\\n mov x9,v2.d[1]\\n eor %0,x8,x9\""
   ":\"=r\"(r):\"r\"((unsigned long)al),\"r\"((unsigned long)ah),"
   "\"r\"((unsigned long)bl),\"r\"((unsigned long)bh)"
   ":\"v0\",\"v1\",\"v2\",\"x8\",\"x9\");"
   "return (long)r;}\n",
   {H_NAN5, H_1425, H_120P, H_5067}, FP16FLAGS},

  // a=[NaN,5,-0,3] b=[1,2,+0,7] -> min [1,2,-0,3] = 0x4200800040003C00
  {"fminnm_4h_nan",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n"
   " fminnm v2.4h,v0.4h,v1.4h\\n fmov %0,d2\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {H_NAN5, H_120P}, FP16FLAGS},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(FP16Pairwise, AArch64FP16PairwiseRT,
                         ::testing::ValuesIn(kA64), rtTCName);
