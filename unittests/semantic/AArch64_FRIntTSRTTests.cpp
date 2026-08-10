//===- AArch64_FRIntTSRTTests.cpp - FRINT32/64 (FEAT_FRINTTS) ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for AArch64 FRINT32Z/FRINT32X/FRINT64Z/FRINT64X
// (FEAT_FRINTTS): round a floating-point value to an integral float, then
// clamp the result to the signed 32-bit (FRINT32*) or 64-bit (FRINT64*)
// integer range.  Out-of-range, NaN or Inf inputs yield INT_MIN as a float
// (-2^31 for FRINT32*, -2^63 for FRINT64*).  The "Z" forms round toward zero;
// the "X" forms use the FPCR rounding mode (default round-to-nearest-even).
//
// The lifter (AArch64LiftSIMD.cpp) used a single whole-register `FLOAT_ROUND`
// (round-half-away-from-zero), which is wrong three ways: (a) wrong rounding
// mode (Z must truncate, X must round-to-even), (b) no integer-range clamping
// (overflow/NaN must give INT_MIN), and (c) for vectors it collapsed all lanes
// into one FP value.  Now lifted per-lane with the correct rounding plus a
// float-comparison range clamp matching QEMU's frint_s/frint_d helpers.
//
// Data moves through integer `fmov`/`ins`.  Requires -march=armv8.5-a
// (FEAT_FRINTTS); executed natively by the Unicorn CPU.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64FRIntTSRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64FRIntTSRT, Verify) { roundTripAArch64(GetParam()); }

#define TSFLAGS "FRIntTS", 0, "-march=armv8.5-a"

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // --- FRINT32Z scalar S: round toward zero (3.7 -> 3.0, NOT 4.0) ---
  {"frint32z_s",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n frint32z s0,s0\\n fmov %w0,s0\""
   ":\"=r\"(r):\"r\"((unsigned int)a):\"v0\");return (long)r;}\n",
   {0x40666666ULL}, TSFLAGS},   // 3.7f

  // --- FRINT32X scalar S: round to nearest EVEN (2.5 -> 2.0, NOT 3.0) ---
  {"frint32x_s",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n frint32x s0,s0\\n fmov %w0,s0\""
   ":\"=r\"(r):\"r\"((unsigned int)a):\"v0\");return (long)r;}\n",
   {0x40200000ULL}, TSFLAGS},   // 2.5f

  // --- FRINT32Z scalar S overflow: 2^31 is out of int32 range -> -2^31 ---
  {"frint32z_s_ovf",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n frint32z s0,s0\\n fmov %w0,s0\""
   ":\"=r\"(r):\"r\"((unsigned int)a):\"v0\");return (long)r;}\n",
   {0x4F000000ULL}, TSFLAGS},   // 2^31 = 2147483648.0f

  // --- FRINT32Z scalar S NaN -> -2^31 (clamp, not NaN) ---
  {"frint32z_s_nan",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n frint32z s0,s0\\n fmov %w0,s0\""
   ":\"=r\"(r):\"r\"((unsigned int)a):\"v0\");return (long)r;}\n",
   {0x7FC00000ULL}, TSFLAGS},   // quiet NaN

  // --- FRINT64Z scalar S overflow: 2^63 out of int64 range -> -2^63 ---
  {"frint64z_s_ovf",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n frint64z s0,s0\\n fmov %w0,s0\""
   ":\"=r\"(r):\"r\"((unsigned int)a):\"v0\");return (long)r;}\n",
   {0x5F000000ULL}, TSFLAGS},   // 2^63 = 9223372036854775808.0f

  // --- FRINT64Z scalar S in range: 2^40 fits in int64 -> unchanged ---
  {"frint64z_s_inrange",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n frint64z s0,s0\\n fmov %w0,s0\""
   ":\"=r\"(r):\"r\"((unsigned int)a):\"v0\");return (long)r;}\n",
   {0x53800000ULL}, TSFLAGS},   // 2^40 = 1099511627776.0f

  // --- FRINT32X scalar D: round to nearest even (2.5 -> 2.0) ---
  {"frint32x_d",
   "long f(long a){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n frint32x d0,d0\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\");return (long)r;}\n",
   {0x4004000000000000ULL}, TSFLAGS},   // 2.5

  // --- FRINT64X scalar D negative tie: -2.5 -> -2.0 (even), NOT -3.0 ---
  {"frint64x_d_neg",
   "long f(long a){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n frint64x d0,d0\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\");return (long)r;}\n",
   {0xC004000000000000ULL}, TSFLAGS},   // -2.5

  // --- FRINT32Z scalar D int32 overflow: 2^32 out of int32 range -> -2^31 ---
  {"frint32z_d_ovf",
   "long f(long a){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n frint32z d0,d0\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\");return (long)r;}\n",
   {0x41F0000000000000ULL}, TSFLAGS},   // 2^32 = 4294967296.0

  // --- FRINT64Z scalar D in range: 2^32 fits in int64 -> unchanged ---
  {"frint64z_d_inrange",
   "long f(long a){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n frint64z d0,d0\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\");return (long)r;}\n",
   {0x41F0000000000000ULL}, TSFLAGS},   // 2^32

  // --- FRINT32Z vector .2S: per-lane [3.7,-2.9] -> [3.0,-2.0] ---
  // The old whole-register FLOAT_ROUND treated the 8 bytes as one f64.
  {"frint32z_v2s",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov s0,%w1\\n ins v0.s[1],%w2\\n"
   " frint32z v0.2s,v0.2s\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned int)b):\"v0\");"
   "return (long)r;}\n",
   {0x40666666ULL, 0xC039999AULL}, TSFLAGS},   // [3.7f, -2.9f]

  // --- FRINT32X vector .2S: per-lane round-even [2.5,3.5] -> [2.0,4.0] ---
  {"frint32x_v2s",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov s0,%w1\\n ins v0.s[1],%w2\\n"
   " frint32x v0.2s,v0.2s\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned int)b):\"v0\");"
   "return (long)r;}\n",
   {0x40200000ULL, 0x40600000ULL}, TSFLAGS},   // [2.5f, 3.5f]
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(FRIntTS, AArch64FRIntTSRT, ::testing::ValuesIn(kA64),
                         rtTCName);
