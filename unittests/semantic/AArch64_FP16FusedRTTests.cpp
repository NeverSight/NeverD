//===- AArch64_FP16FusedRTTests.cpp - half-precision FMA / FABD ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for AArch64 half-precision (FEAT_FP16) fused multiply-add
// (FMLA/FMLS) and absolute-difference (FABD) on the .8H/.4H vector
// arrangements, plus the by-element FMLA form and the scalar H control.
//
// These are the *siblings* of the #290/#291 FP16 arithmetic/conversion fixes:
// the FMLA/FMLS handlers (AArch64LiftCoreNEON.cpp) and the FABD handler
// (AArch64LiftSIMD.cpp) carried the same VAS->LaneSz blind spot, recognising
// only .4S/.2S (LaneSz 4) and .2D (LaneSz 8).  With .8H/.4H missing, a
// half-precision `fmla v.4h` fell into the scalar `else` branch and the whole
// 64/128-bit register was treated as ONE fused multiply-add (the emitter then
// reinterpreted the packed fp16 lanes as a single f32/f64), corrupting every
// lane.  Now lifted per-lane with a `half` emitter type (+fullfp16 codegen).
//
// Data is moved in/out of the vector registers via integer `fmov` (a pure bit
// copy, no fcvt), so these probes exercise ONLY the fp16 FMA/FABD handlers.
// fp16 bit patterns: 1.0=0x3C00 1.5=0x3E00 2.0=0x4000 3.0=0x4200 4.0=0x4400
// 5.0=0x4500 6.0=0x4600 7.0=0x4700 8.0=0x4800 10.0=0x4900 0.0=0x0000.
// Requires -march=armv8.2-a+fp16; the default AArch64 Unicorn MAX CPU executes
// fp16 FMA/FABD natively.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64FP16FusedRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64FP16FusedRT, Verify) { roundTripAArch64(GetParam()); }

#define FP16FLAGS "FP16Fused", 0, "-march=armv8.2-a+fp16"

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // --- .4H FMLA: v0[i] += v1[i]*v2[i] (fused, single rounding) ---
  // v0=[1,2,3,4] v1=[2,2,2,2] v2=[1.5,1.5,1.5,1.5] -> [4,5,6,7]=0x4700460045004400
  {"fmla_4h",
   "long f(long a,long b,long c){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n fmov d2,%3\\n"
   " fmla v0.4h,v1.4h,v2.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b),"
   "\"r\"((unsigned long)c):\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {0x4400420040003C00ULL, 0x4000400040004000ULL, 0x3E003E003E003E00ULL},
   FP16FLAGS},

  // --- .4H FMLS: v0[i] -= v1[i]*v2[i] (fused) ---
  // v0=[10,10,10,10] v1=[2,2,2,2] v2=[1.5,1.5,1.5,1.5] -> [7,7,7,7]=0x4700470047004700
  {"fmls_4h",
   "long f(long a,long b,long c){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n fmov d2,%3\\n"
   " fmls v0.4h,v1.4h,v2.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b),"
   "\"r\"((unsigned long)c):\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {0x4900490049004900ULL, 0x4000400040004000ULL, 0x3E003E003E003E00ULL},
   FP16FLAGS},

  // --- .4H FABD: v0[i] = |v0[i]-v1[i]| ---
  // v0=[5,2,8,1] v1=[3,7,2,1] -> [2,5,6,0]=0x0000460045004000
  {"fabd_4h",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n"
   " fabd v0.4h,v0.4h,v1.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x3C00480040004500ULL, 0x3C00400047004200ULL}, FP16FLAGS},

  // --- .8H FMLA (8 fp16 lanes in a Q register) ---
  // `dup v.2d,x` fills both 64-bit halves with the same 4 lanes (clean full-128
  // init, avoiding the orthogonal partial-vector-write path); read back low half.
  {"fmla_8h",
   "long f(long a,long b,long c){unsigned long r;"
   "__asm__ volatile(\"dup v0.2d,%1\\n dup v1.2d,%2\\n dup v2.2d,%3\\n"
   " fmla v0.8h,v1.8h,v2.8h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b),"
   "\"r\"((unsigned long)c):\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {0x4400420040003C00ULL, 0x4000400040004000ULL, 0x3E003E003E003E00ULL},
   FP16FLAGS},

  // --- .4H by-element FMLA: broadcast v2.h[1] (=1.5) to every lane ---
  // v0=[1,2,3,4] v1=[2,2,2,2] v2=[7,1.5,3,5]; lane1=1.5 -> v0+2*1.5 -> [4,5,6,7]
  {"fmla_4h_byelem",
   "long f(long a,long b,long c){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n fmov d2,%3\\n"
   " fmla v0.4h,v1.4h,v2.h[1]\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b),"
   "\"r\"((unsigned long)c):\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {0x4400420040003C00ULL, 0x4000400040004000ULL, 0x450042003E004700ULL},
   FP16FLAGS},

  // --- Scalar H FABD control (already correct post-#290: scalar half path) ---
  // a=5.0 b=8.0 -> |5-8|=3.0=0x4200
  {"fabd_scalar_h",
   "long f(long a,long b){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n fmov s1,%w2\\n"
   " fabd h0,h0,h1\\n fmov %w0,s0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned int)b)"
   ":\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x4500ULL, 0x4800ULL}, FP16FLAGS},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(FP16Fused, AArch64FP16FusedRT,
                         ::testing::ValuesIn(kA64), rtTCName);
