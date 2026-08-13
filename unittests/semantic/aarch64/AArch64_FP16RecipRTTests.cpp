//===- AArch64_FP16RecipRTTests.cpp - half-precision reciprocal / fmulx -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for AArch64 half-precision (FEAT_FP16) reciprocal family
// FRECPE/FRSQRTE (estimate), FRECPS/FRSQRTS (Newton-Raphson step) and FMULX on
// the .4H/.8H arrangements plus the scalar H form.
//
// These are the leftover half-precision paths from #292/#293.  The reciprocal
// lifters (AArch64LiftCoreNEON.cpp) pass the float element width to the emitter
// via neonElemSize(), but then clamped it with `if (ElemSz != 4 && ElemSz != 8)
// ElemSz = (Dst.Size>=8)?8:4`, which forced the .4H half width (2) up to 8 (a
// single f64) -> the whole register became one lane.  The shared emitter
// reciprocal handler (MedLLVMAArch64ValueEmitter.cpp) likewise only accepted a
// trailing ElemSz of 4 or 8 and only built float/double element types.  FMULX
// (#293) only detected .4S/.2S/.2D, so half fell back to the same f64 clamp.
// Now half (ElemSz 2 -> `half` element type) is accepted end-to-end; the #290
// codegen scan auto-enables +fullfp16 when it sees the half-typed intrinsic.
//
// The estimate ops (FRECPE/FRSQRTE) are architecturally-defined approximations:
// mapping to the same NEON intrinsic makes the recompiled code emit the real
// frecpe/frsqrte, so Unicorn computes a bit-identical estimate on both sides.
// Data moves through integer `fmov` (pure bit copies).  fp16 patterns:
// 0.25=0x3400 0.5=0x3800 1.0=0x3C00 2.0=0x4000 3.0=0x4200 4.0=0x4400
// 6.0=0x4600 8.0=0x4800 16.0=0x4C00 +Inf=0x7C00.  Requires -march=armv8.2-a+fp16.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64FP16RecipRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64FP16RecipRT, Verify) { roundTripAArch64(GetParam()); }

#define FP16FLAGS "FP16Recip", 0, "-march=armv8.2-a+fp16"

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // --- .4H FRECPE (reciprocal estimate, per-lane) ---
  // v0=[2.0,4.0,1.0,0.5]; estimate ~[0.5,0.25,1.0,2.0] (exact bits via the
  // real frecpe on both sides).  Bug clamps the 4 half lanes into one f64.
  {"frecpe_4h",
   "long f(long a){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n frecpe v0.4h,v0.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\");"
   "return (long)r;}\n",
   {0x38003C0044004000ULL}, FP16FLAGS},

  // --- .4H FRSQRTE (reciprocal-sqrt estimate, per-lane) ---
  // v0=[4.0,1.0,0.25,16.0]; estimate ~[0.5,1.0,2.0,0.25].
  {"frsqrte_4h",
   "long f(long a){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n frsqrte v0.4h,v0.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\");"
   "return (long)r;}\n",
   {0x4C0034003C004400ULL}, FP16FLAGS},

  // --- .4H FRECPS (reciprocal step = 2.0 - a*b, per-lane) ---
  // v0=[2.0,1.0,0.5,4.0] v1=[0.5,1.0,2.0,0.25] -> 2-a*b = [1.0,1.0,1.0,1.0]
  // packed lane0..3: v0 = 4.0|0.5|1.0|2.0, v1 = 0.25|2.0|1.0|0.5
  {"frecps_4h",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n"
   " frecps v0.4h,v0.4h,v1.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x440038003C004000ULL, 0x340040003C003800ULL}, FP16FLAGS},

  // --- .4H FRSQRTS (rsqrt step = (3.0 - a*b)/2, per-lane) ---
  // v0=[2.0,1.0,4.0,0.5] v1=[0.5,1.0,0.25,2.0] -> (3-a*b)/2 = [1.0,1.0,1.0,1.0]
  // packed lane0..3: v0 = 0.5|4.0|1.0|2.0, v1 = 2.0|0.25|1.0|0.5
  {"frsqrts_4h",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n"
   " frsqrts v0.4h,v0.4h,v1.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x380044003C004000ULL, 0x400034003C003800ULL}, FP16FLAGS},

  // --- .4H FMULX (per-lane; #293 only handled f32/f64) ---
  // v0=[2.0,3.0,4.0,0.5] v1=[3.0,2.0,1.0,8.0] -> [6.0,6.0,4.0,4.0]
  {"fmulx_4h",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n"
   " fmulx v0.4h,v0.4h,v1.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x3800440042004000ULL, 0x48003C0040004200ULL}, FP16FLAGS},

  // --- Scalar H FMULX 0*Inf = 2.0 (exercises scalar half fallback + 0*Inf) ---
  // a=0.0 b=+Inf -> +2.0=0x4000 (FMUL would give NaN; buggy half fallback
  // computed NLanes=0 and returned 0).
  {"fmulx_h_zero_inf",
   "long f(long a,long b){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n fmov s1,%w2\\n"
   " fmulx h0,h0,h1\\n fmov %w0,s0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned int)b)"
   ":\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x0000ULL, 0x7C00ULL}, FP16FLAGS},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(FP16Recip, AArch64FP16RecipRT,
                         ::testing::ValuesIn(kA64), rtTCName);
