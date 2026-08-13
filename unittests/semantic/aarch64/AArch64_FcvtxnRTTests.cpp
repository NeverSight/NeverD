//===- AArch64_FcvtxnRTTests.cpp - FCVTXN/FCVTXN2 round-to-odd ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for AArch64 FCVTXN/FCVTXN2: FP inexact narrowing f64->f32
// using round-to-ODD (jamming), used for double-rounding-safe narrowing.  Any
// inexact narrowing jams the result to the neighbouring f32 with an odd
// mantissa LSB, which differs from round-to-nearest-even on every inexact case.
// FCVTXN2 additionally writes the narrowed pair into the HIGH 64 bits of Vd,
// preserving the low 64 bits.
//
// The lifter (AArch64LiftSIMD.cpp) used a plain whole-register
// `FLOAT_FLOAT2FLOAT` (round-to-nearest-even, no per-lane, and -- for the "2"
// form -- no high-half placement).  Now mapped to the real sisd/neon fcvtxn
// intrinsic so codegen emits `fcvtxn` and the recompiled code is bit-exact.
//
// Data moves through integer `fmov`/`ins`/`umov`.  FCVTXN is base AdvSIMD
// (ARMv8.0), executed natively by the Unicorn CPU.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64FcvtxnRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64FcvtxnRT, Verify) { roundTripAArch64(GetParam()); }

// Scalar fcvtxn Sd, Dn: load a double, narrow with round-to-odd, read the f32.
#define XN_S(NAME, BITS)                                                        \
  {NAME,                                                                        \
   "long f(long a){unsigned int r;"                                            \
   "__asm__ volatile(\"fmov d0,%1\\n fcvtxn s0,d0\\n fmov %w0,s0\""            \
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\");return (long)r;}\n",            \
   {BITS}, "Fcvtxn"}

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // --- Scalar: inexact narrowing rounds to ODD (RED: round-even differs) ---
  XN_S("fcvtxn_s_half",  0x3FF0000010000000ULL),  // 1+2^-24 -> 0x3F800001 (odd)
  XN_S("fcvtxn_s_tiny",  0x3FF0000000000001ULL),  // 1+2^-52 -> 0x3F800001 (jam)
  XN_S("fcvtxn_s_neg",   0xBFF0000010000000ULL),  // -(1+2^-24) -> 0xBF800001
  // --- Scalar: exact narrowing (guard: round-even == round-odd) ---
  XN_S("fcvtxn_s_exact", 0x3FF8000000000000ULL),  // 1.5 -> 0x3FC00000

  // --- Vector .2s <- .2d: per-lane round-to-odd ---
  {"fcvtxn_v2s",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d1,%1\\n ins v1.d[1],%2\\n"
   " fcvtxn v0.2s,v1.2d\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b):\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x3FF0000010000000ULL, 0x4000000010000000ULL}, "Fcvtxn"}, // [1+2^-24, 2+2^-23]

  // --- FCVTXN2 high half: narrowed pair goes to HIGH 64, low 64 preserved ---
  {"fcvtxn2_hi",
   "long f(long a,long b,long c){unsigned long r;"
   "__asm__ volatile(\"dup v0.2d,%1\\n fmov d1,%2\\n ins v1.d[1],%3\\n"
   " fcvtxn2 v0.4s,v1.2d\\n umov %0,v0.d[1]\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b),"
   "\"r\"((unsigned long)c):\"v0\",\"v1\");return (long)r;}\n",
   {0x1111111122222222ULL, 0x3FF0000010000000ULL, 0x4000000010000000ULL},
   "Fcvtxn"},

  // --- FCVTXN2 low half must be preserved (guard) ---
  {"fcvtxn2_lo",
   "long f(long a,long b,long c){unsigned long r;"
   "__asm__ volatile(\"dup v0.2d,%1\\n fmov d1,%2\\n ins v1.d[1],%3\\n"
   " fcvtxn2 v0.4s,v1.2d\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b),"
   "\"r\"((unsigned long)c):\"v0\",\"v1\");return (long)r;}\n",
   {0x1111111122222222ULL, 0x3FF0000010000000ULL, 0x4000000010000000ULL},
   "Fcvtxn"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(Fcvtxn, AArch64FcvtxnRT, ::testing::ValuesIn(kA64),
                         rtTCName);
