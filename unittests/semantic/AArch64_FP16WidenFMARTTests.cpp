//===- AArch64_FP16WidenFMARTTests.cpp - fp16->fp32 widening FMLAL -*- C++ -*-===//
//
// Roundtrip probes for AArch64 FMLAL/FMLAL2/FMLSL (FEAT_FHM): fp16->fp32
// widening fused multiply-add.  `fmlal Vd.4s, Vn.4h, Vm.4h` computes, per f32
// lane i: Vd[i] += widen_f32(Vn.4h[i]) * widen_f32(Vm.4h[i]) with a SINGLE
// rounding; FMLAL2 uses the high four fp16 lanes of a .8h source; FMLSL
// subtracts.
//
// The lifter (AArch64LiftSIMD.cpp) used a naive whole-register
// `FLOAT_MULT` + separate `FLOAT_ADD/SUB`, which (a) never widens fp16->fp32,
// (b) has no per-lane structure, (c) is not fused (double rounding), and
// (d) ignores the FMLAL2 high-lane selection.  Now lifted per-lane with an
// explicit fp16->fp32 widen and a fused FLOAT_FMA.
//
// Data moves through integer `fmov`/`dup`.  fp16: 2.0=0x4000 3.0=0x4200
// 4.0=0x4400 5.0=0x4500; f32: 1.0=0x3F800000.  Requires
// -march=armv8.2-a+fp16fml (FEAT_FHM); executed natively by the Unicorn CPU
// when the feature is present.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64FP16WidenFMART : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64FP16WidenFMART, Verify) { roundTripAArch64(GetParam()); }

#define FHMFLAGS "FP16WidenFMA", 0, "-march=armv8.2-a+fp16fml"

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // --- FMLAL Vd.4S, Vn.4H, Vm.4H : low 4 fp16 lanes, widen+fused-add ---
  // v0=[1,1,1,1]f32 (dup) v1.4h=[2,3,4,5] v2.4h=[2,2,2,2]
  // -> v0[i]=1+v1[i]*2 = [5,7,9,11]; read low 64 = [5.0,7.0].
  {"fmlal_4s",
   "long f(long a,long b,long c){unsigned long r;"
   "__asm__ volatile(\"dup v0.4s,%w1\\n fmov d1,%2\\n fmov d2,%3\\n"
   " fmlal v0.4s,v1.4h,v2.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned long)b),"
   "\"r\"((unsigned long)c):\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {0x3F800000ULL, 0x4500440042004000ULL, 0x4000400040004000ULL}, FHMFLAGS},

  // --- FMLSL Vd.4S, Vn.4H, Vm.4H : widen + fused-subtract ---
  // v0=[20,20,20,20]f32 v1.4h=[2,3,4,5] v2.4h=[2,2,2,2]
  // -> v0[i]=20-v1[i]*2 = [16,14,12,10]; read low 64 = [16.0,14.0].
  {"fmlsl_4s",
   "long f(long a,long b,long c){unsigned long r;"
   "__asm__ volatile(\"dup v0.4s,%w1\\n fmov d1,%2\\n fmov d2,%3\\n"
   " fmlsl v0.4s,v1.4h,v2.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned long)b),"
   "\"r\"((unsigned long)c):\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {0x41A00000ULL, 0x4500440042004000ULL, 0x4000400040004000ULL}, FHMFLAGS},

  // --- FMLAL Vd.2S, Vn.2H, Vm.2H : 2-lane form ---
  // v0=[1,1]f32 v1.2h=[2,3] v2.2h=[2,2] -> [1+2*2,1+3*2]=[5.0,7.0]
  {"fmlal_2s",
   "long f(long a,long b,long c){unsigned long r;"
   "__asm__ volatile(\"dup v0.2s,%w1\\n fmov s1,%w2\\n fmov s2,%w3\\n"
   " fmlal v0.2s,v1.2h,v2.2h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned int)b),"
   "\"r\"((unsigned int)c):\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {0x3F800000ULL, 0x42004000ULL, 0x40004000ULL}, FHMFLAGS},

  // --- By-element: FMLAL Vd.4S, Vn.4H, Vm.H[1] (broadcast v2 lane 1 = 3.0) ---
  // v0=[1,1,1,1] v1.4h=[2,3,4,5] -> [1+2*3,1+3*3,1+4*3,1+5*3]=[7,10,13,16]
  {"fmlal_4s_byelem",
   "long f(long a,long b,long c){unsigned long r;"
   "__asm__ volatile(\"dup v0.4s,%w1\\n fmov d1,%2\\n fmov d2,%3\\n"
   " fmlal v0.4s,v1.4h,v2.h[1]\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned long)b),"
   "\"r\"((unsigned long)c):\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {0x3F800000ULL, 0x4500440042004000ULL, 0x0000000042000000ULL}, FHMFLAGS},

  // --- FMLAL2 Vd.4S, Vn.4H, Vm.4H : reads the HIGH 4 fp16 lanes (lanes 4-7) ---
  // v0=[1,1,1,1]f32; v1 high.4h=[2,3,4,5] v2 high.4h=[2,2,2,2] (low halves 0)
  // -> v0[i]=1+v1.hi[i]*2 = [5,7,9,11]; read low 64 = [5.0,7.0].  The old whole-
  // register FLOAT_MULT placeholder (no high-lane select, no widen) is wrong.
  {"fmlal2_4s",
   "long f(long a,long b,long c){unsigned long r;"
   "__asm__ volatile(\"dup v0.4s,%w1\\n movi v1.2d,#0\\n ins v1.d[1],%2\\n"
   " movi v2.2d,#0\\n ins v2.d[1],%3\\n"
   " fmlal2 v0.4s,v1.4h,v2.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned long)b),"
   "\"r\"((unsigned long)c):\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {0x3F800000ULL, 0x4500440042004000ULL, 0x4000400040004000ULL}, FHMFLAGS},

  // --- FMLSL2 Vd.4S, Vn.4H, Vm.4H : high 4 fp16 lanes, widen + fused-subtract ---
  // v0=[20,20,20,20]f32; v1 high=[2,3,4,5] v2 high=[2,2,2,2]
  // -> v0[i]=20-v1.hi[i]*2 = [16,14,12,10]; read low 64 = [16.0,14.0].
  {"fmlsl2_4s",
   "long f(long a,long b,long c){unsigned long r;"
   "__asm__ volatile(\"dup v0.4s,%w1\\n movi v1.2d,#0\\n ins v1.d[1],%2\\n"
   " movi v2.2d,#0\\n ins v2.d[1],%3\\n"
   " fmlsl2 v0.4s,v1.4h,v2.4h\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned long)b),"
   "\"r\"((unsigned long)c):\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {0x41A00000ULL, 0x4500440042004000ULL, 0x4000400040004000ULL}, FHMFLAGS},

  // --- By-element FMLAL2 Vd.4S, Vn.4H, Vm.H[1] : Vn high 4 lanes, Vm lane 1 ---
  // v0=[1,1,1,1]; v1 high.4h=[2,3,4,5] v2.h[1]=3.0 -> [1+2*3,..,1+5*3]=[7,10,13,16]
  {"fmlal2_4s_byelem",
   "long f(long a,long b,long c){unsigned long r;"
   "__asm__ volatile(\"dup v0.4s,%w1\\n movi v1.2d,#0\\n ins v1.d[1],%2\\n"
   " fmov d2,%3\\n fmlal2 v0.4s,v1.4h,v2.h[1]\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned long)b),"
   "\"r\"((unsigned long)c):\"v0\",\"v1\",\"v2\");"
   "return (long)r;}\n",
   {0x3F800000ULL, 0x4500440042004000ULL, 0x0000000042000000ULL}, FHMFLAGS},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(FP16WidenFMA, AArch64FP16WidenFMART,
                         ::testing::ValuesIn(kA64), rtTCName);
