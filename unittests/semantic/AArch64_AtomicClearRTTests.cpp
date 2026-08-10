//===- AArch64_AtomicClearRTTests.cpp - LSE LDCLR load-op roundtrip ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// FEAT_LSE atomic bit-clear LOAD form (returns the old value):
//
//   LDCLR Ws, Wt, [Xn]:  old = *Xn ; *Xn = old AND NOT(Ws) ; Wt = old
//
// The store-only sibling STCLR (no return) is covered by AtomicStoreOpRTTests,
// and the other load-ops (LDADD/LDSET load form by AtomicMinMax controls,
// LDSMAX/LDSMIN/LDUMAX/LDUMIN by AtomicMinMax, LDADD/LDEOR/LDSET Rs==Rt by
// AtomicSwapAlias).  The LDCLR *load* form — the one that both clears bits in
// memory AND hands back the pre-clear value — had no roundtrip coverage at all,
// a hole in the FEAT_LSE matrix (a high-priority "LSE variants" task).
//
// The lifter computes `NV = OldVal AND NOT(Src)` and writes the destination with
// the loaded old value (shared loadOpPrologue, same path the passing LDADD/LDSET
// controls use), so this is a gap-closing guardrail: each probe folds BOTH the
// returned old value and the post-op memory cell, and the input bit patterns are
// picked so that clear, EOR, OR and ADD all yield DIFFERENT results — a handler
// that confused LDCLR with any sibling op would diverge.  Ordering (A/L/AL) and
// access-width (B/H/W/X) variants share the one handler and are pinned here.
//
// FEAT_LSE needs an ARMv8.1 assembler baseline + the MAX Unicorn CPU (which runs
// the real ldclr on the original side; the lifted side lowers to plain
// load/and/store and needs no LSE).  Oracle: original-Unicorn vs lifted-Unicorn.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64AtomicClearRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64AtomicClearRT, Verify) { roundTripAArch64(GetParam()); }

// Fields after CSrc: Args, Category, OptLevel, ExtraFlags, NoOpt, Triple, UcCpu
#define A64LSE "AtomicClear", 1, "-march=armv8.1-a", false, "", UC_CPU_ARM64_MAX

// clang-format off
static const std::vector<RoundTripTC> kA64Clear = {

  // ===== X (64-bit) — plain LDCLR. =====
  // m=0xF0.. , s=0xCC.. -> new = 0xF0 & ~0xCC = 0x30 per byte; old = 0xF0..
  // (clear 0x30, eor 0x3C, or 0xFC, add 0x1BC -> all distinct, so the op is pinned).
  {"ldclr_x",
   "long ldclr_x(long a){ unsigned long m=0xF0F0F0F0F0F0F0F0UL; unsigned long o;"
   " __asm__ volatile(\"ldclr %x[s],%x[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return (long)(o*31+m); }\n",
   {0xCCCCCCCCCCCCCCCCULL}, A64LSE},

  // ===== X — ordering variants (acquire / release / acquire-release). =====
  {"ldclra_x",
   "long ldclra_x(long a){ unsigned long m=0xF0F0F0F0F0F0F0F0UL; unsigned long o;"
   " __asm__ volatile(\"ldclra %x[s],%x[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return (long)(o*31+m); }\n",
   {0xCCCCCCCCCCCCCCCCULL}, A64LSE},
  {"ldclrl_x",
   "long ldclrl_x(long a){ unsigned long m=0xF0F0F0F0F0F0F0F0UL; unsigned long o;"
   " __asm__ volatile(\"ldclrl %x[s],%x[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return (long)(o*31+m); }\n",
   {0xCCCCCCCCCCCCCCCCULL}, A64LSE},
  {"ldclral_x",
   "long ldclral_x(long a){ unsigned long m=0xF0F0F0F0F0F0F0F0UL; unsigned long o;"
   " __asm__ volatile(\"ldclral %x[s],%x[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return (long)(o*31+m); }\n",
   {0xCCCCCCCCCCCCCCCCULL}, A64LSE},

  // ===== W (32-bit) — result must zero-extend into X[63:32]. =====
  {"ldclr_w",
   "long ldclr_w(long a){ unsigned m=0xF0F0F0F0u; unsigned o;"
   " __asm__ volatile(\"ldclr %w[s],%w[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"((unsigned)a),[p]\"r\"(&m):\"memory\");"
   " return (long)(unsigned)(o*31u+m); }\n",
   {0xCCCCCCCCu}, A64LSE},
  {"ldclral_w",
   "long ldclral_w(long a){ unsigned m=0x0FF00FF0u; unsigned o;"
   " __asm__ volatile(\"ldclral %w[s],%w[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"((unsigned)a),[p]\"r\"(&m):\"memory\");"
   " return (long)(unsigned)(o*31u+m); }\n",
   {0x33CC55AAu}, A64LSE},

  // ===== Byte / halfword — access-width narrowing path. =====
  {"ldclrb",
   "long ldclrb(long a){ unsigned char m=0xF0; unsigned o;"
   " __asm__ volatile(\"ldclrb %w[s],%w[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"((unsigned)a),[p]\"r\"(&m):\"memory\");"
   " return (long)(unsigned)(o*31u+(unsigned)m); }\n",
   {0xCC}, A64LSE},
  {"ldclrh",
   "long ldclrh(long a){ unsigned short m=0xF0F0; unsigned o;"
   " __asm__ volatile(\"ldclrh %w[s],%w[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"((unsigned)a),[p]\"r\"(&m):\"memory\");"
   " return (long)(unsigned)(o*31u+(unsigned)m); }\n",
   {0xCCCC}, A64LSE},
  {"ldclralb",
   "long ldclralb(long a){ unsigned char m=0xAA; unsigned o;"
   " __asm__ volatile(\"ldclralb %w[s],%w[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"((unsigned)a),[p]\"r\"(&m):\"memory\");"
   " return (long)(unsigned)(o*31u+(unsigned)m); }\n",
   {0x3C}, A64LSE},

  // ===== Control: LDEOR with the SAME inputs (already-correct sibling). =====
  // Distinguishes clear from eor: on m=0xF0..,s=0xCC.. clear->0x30, eor->0x3C.
  {"ldeor_x_ctl",
   "long ldeor_x_ctl(long a){ unsigned long m=0xF0F0F0F0F0F0F0F0UL; unsigned long o;"
   " __asm__ volatile(\"ldeor %x[s],%x[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return (long)(o*31+m); }\n",
   {0xCCCCCCCCCCCCCCCCULL}, A64LSE},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(AtomicClear, A64AtomicClearRT,
                         ::testing::ValuesIn(kA64Clear), rtTCName);
