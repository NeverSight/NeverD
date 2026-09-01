//===- AArch64_FP16ArithRTTests.cpp - half-precision FP arithmetic -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for AArch64 half-precision (FEAT_FP16) floating-point
// arithmetic: FADD/FSUB/FMUL/FDIV, FNEG/FABS/FSQRT, and FMIN/FMAX on the
// .8H/.4H vector arrangements plus the scalar H form.
//
// The lifter's FP arithmetic handlers (AArch64LiftFP.cpp) only recognised the
// .4S/.2S (LaneSz 4) and .2D (LaneSz 8) arrangements, so .8H/.4H fell through
// to the scalar `else` branch and the whole 64/128-bit register was treated as
// ONE floating-point operation (the same "vector treated as scalar" bug class
// as #277's FMIN/FMAX).  In addition the emitter mapped any <=32-bit operand to
// `float`, so even a scalar `half` op reinterpreted the 16 fp16 bits as a tiny
// float32 denormal.  Now lifted per-lane with a `half` emitter type and a
// +fullfp16 codegen feature.
//
// Data is moved in/out of the vector registers via integer `fmov`/`umov` (pure
// bit copies, no fcvt), so these probes exercise ONLY the fp16 arithmetic
// handlers — conversions are a separate concern.  Inputs pack fp16 lane bit
// patterns; values are chosen to stay normal (and include NaN / signed zero for
// the min/max probes).  Requires -march=armv8.2-a+fp16; pin Unicorn's MAX CPU
// because its default Cortex-A72 model lacks FEAT_FP16 arithmetic.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64FP16ArithRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64FP16ArithRT, Verify) { roundTripAArch64(GetParam()); }

#define FP16FLAGS                                                              \
  "FP16Arith", 0, "-march=armv8.2-a+fp16", false, "", UC_CPU_ARM64_MAX

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // --- .4H binary arithmetic (4 fp16 lanes in a D register) ---
  // a = [2.0, 4.0, 8.0, 0.5], b = [1.0, 2.0, 4.0, 0.25]
  {"fadd_4h",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n fadd v0.4h,v0.4h,v1.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x3800480044004000ULL, 0x3400440040003C00ULL}, FP16FLAGS},

  {"fsub_4h",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n fsub v0.4h,v0.4h,v1.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x3800480044004000ULL, 0x3400440040003C00ULL}, FP16FLAGS},

  {"fmul_4h",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n fmul v0.4h,v0.4h,v1.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x3800480044004000ULL, 0x3400440040003C00ULL}, FP16FLAGS},

  {"fdiv_4h",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n fdiv v0.4h,v0.4h,v1.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x3800480044004000ULL, 0x3400440040003C00ULL}, FP16FLAGS},

  // --- .4H unary arithmetic ---
  // a = [-2.0, 4.0, -8.0, 0.5]
  {"fneg_4h",
   "long f(long a){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fneg v0.4h,v0.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\");"
   "return (long)r;}\n",
   {0x3800C8004400C000ULL}, FP16FLAGS},

  {"fabs_4h",
   "long f(long a){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fabs v0.4h,v0.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\");"
   "return (long)r;}\n",
   {0x3800C8004400C000ULL}, FP16FLAGS},

  // a = [4.0, 9.0, 16.0, 1.0] -> sqrt [2.0, 3.0, 4.0, 1.0]
  {"fsqrt_4h",
   "long f(long a){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fsqrt v0.4h,v0.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\");"
   "return (long)r;}\n",
   {0x3C004C0048804400ULL}, FP16FLAGS},

  // --- .4H min/max (NaN propagation + signed zero) ---
  // a = [NaN, 5.0, -0.0, 3.0], b = [1.0, 2.0, +0.0, 7.0]
  // fmin -> [NaN, 2.0, -0.0, 3.0]; fmax -> [NaN, 5.0, +0.0, 7.0]
  {"fmin_4h_nan",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n fmin v0.4h,v0.4h,v1.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x4200800045007E00ULL, 0x4700000040003C00ULL}, FP16FLAGS},

  {"fmax_4h_nan",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n fmax v0.4h,v0.4h,v1.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x4200800045007E00ULL, 0x4700000040003C00ULL}, FP16FLAGS},

  // --- .8H (8 fp16 lanes in a Q register) ---
  // `dup v0.2d,x` fills both 64-bit halves with the same 4 lanes [1.0,2.0,3.0,4.0]
  // (a clean full-128 init, avoiding the orthogonal partial-vector-write path);
  // fadd v0.8h doubles all 8 lanes, read back the low half [2.0,4.0,6.0,8.0].
  {"fadd_8h",
   "long f(long a){unsigned long r;"
   "__asm__ volatile(\"dup v0.2d,%1\\n fadd v0.8h,v0.8h,v0.8h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\");"
   "return (long)r;}\n",
   {0x4400420040003C00ULL}, FP16FLAGS},

  // --- Scalar H form (single fp16) exercises the half emitter path ---
  // a = 3.0 (0x4200), b = 1.5 (0x3E00); fadd h -> 4.5 (0x4480).
  {"fadd_scalar_h",
   "long f(long a,long b){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n fmov s1,%w2\\n fadd h0,h0,h1\\n fmov %w0,s0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned int)b):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x4200ULL, 0x3E00ULL}, FP16FLAGS},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(FP16Arith, AArch64FP16ArithRT,
                         ::testing::ValuesIn(kA64), rtTCName);
