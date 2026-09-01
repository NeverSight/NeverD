//===- AArch64_FjcvtzsRTTests.cpp - FJCVTZS (FEAT_JSCVT) ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for AArch64 FJCVTZS (FEAT_JSCVT): JavaScript convert
// double -> int32, rounding toward zero with modulo-2^32 WRAP on overflow and
// 0 for NaN/Inf (the semantics of JavaScript's `x|0`).  The PSTATE.Z "inexact"
// flag side effect is not exercised here (no caller reads it).
//
// The lifter (AArch64LiftSIMD.cpp) used a plain `FLOAT_TRUNC` (FPToSI), which
// SATURATES out-of-range / Inf inputs (clamping to INT32_MIN/MAX) instead of
// wrapping.  For in-range values FPToSI happens to match, so only overflow /
// Inf / huge inputs expose the bug.  Now mapped to the real
// llvm.aarch64.fjcvtzs intrinsic so codegen emits `fjcvtzs` and the recompiled
// code wraps bit-exactly.
//
// Data moves through integer `fmov`.  Requires -march=armv8.3-a (FEAT_JSCVT)
// and Unicorn's MAX CPU; the default Cortex-A72 lacks FEAT_JSCVT.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64FjcvtzsRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64FjcvtzsRT, Verify) { roundTripAArch64(GetParam()); }

#define JSFLAGS                                                                \
  "Fjcvtzs", 0, "-march=armv8.3-a+jscvt", false, "", UC_CPU_ARM64_MAX

// One C template: load the double bit pattern, fjcvtzs into a GPR, return it.
#define JCVT(NAME, BITS)                                                        \
  {NAME,                                                                        \
   "long f(long a){unsigned int r;"                                             \
   "__asm__ volatile(\"fmov d0,%1\\n fjcvtzs %w0,d0\""                          \
   ":\"=r\"(r):\"r\"((unsigned long)a):\"v0\",\"cc\");return (long)r;}\n",      \
   {BITS}, JSFLAGS}

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // --- In-range: truncate toward zero (guards: FPToSI already matched) ---
  JCVT("jcvt_inrange",  0x400F333333333333ULL),  // 3.9 -> 3
  JCVT("jcvt_neg",      0xC00F333333333333ULL),  // -3.9 -> -3 (0xFFFFFFFD)
  JCVT("jcvt_nan",      0x7FF8000000000000ULL),  // NaN -> 0 (guard)

  // --- Overflow: WRAP modulo 2^32 (RED: FPToSI saturated) ---
  JCVT("jcvt_ovf_2p31", 0x41E0000000000000ULL),  // 2^31 -> 0x80000000
  JCVT("jcvt_ovf_2p32p5",0x41F0000000500000ULL), // 2^32+5 -> 5
  JCVT("jcvt_neg_ovf",  0xC1F0000000000000ULL),  // -2^32 -> 0
  JCVT("jcvt_large_2p40",0x4270000000000000ULL), // 2^40 -> 0 (low 32 bits)
  JCVT("jcvt_huge_2p63", 0x43E0000000000000ULL), // 2^63 -> 0

  // --- Inf -> 0 (RED: FPToSI saturated to INT32_MIN/MAX) ---
  JCVT("jcvt_posinf",   0x7FF0000000000000ULL),  // +Inf -> 0
  JCVT("jcvt_neginf",   0xFFF0000000000000ULL),  // -Inf -> 0
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(Fjcvtzs, AArch64FjcvtzsRT, ::testing::ValuesIn(kA64),
                         rtTCName);
