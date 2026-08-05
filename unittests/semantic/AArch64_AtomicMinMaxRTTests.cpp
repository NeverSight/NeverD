//===- AArch64_AtomicMinMaxRTTests.cpp - LSE atomic min/max roundtrip ----===//
//
// Roundtrip probes for the AArch64 FEAT_LSE atomic min/max load-ops, which the
// lifter implemented as a broken placeholder:
//
//   LDSMAX/LDSMIN/LDUMAX/LDUMIN Ws, Wt, [Xn] should:
//       old = *Xn ; Wt = old ; *Xn = {s,u}{max,min}(old, Ws)
//
//   The handler instead emitted  `INT_OR(old, Ws)` into Wt (the destination
//   that must receive the *old* value) and stored the *unchanged* old value
//   back to memory — so the operation was OR (not max/min), the returned old
//   value was wrong, and memory was never updated.
//
// Each probe folds BOTH the returned old value and the resulting memory value
// into the return, so the roundtrip comparison catches all three faults.  The
// LDADD case is a control that already lifts correctly.  Signed/unsigned and
// max/min are cross-checked with values where the OR result, the signed answer
// and the unsigned answer all differ.
//
// FEAT_LSE needs an ARMv8.1 assembler baseline + the MAX Unicorn CPU (which
// executes the real ldsmax/etc. on the original side; the lifted side lowers to
// plain load/compare/store and needs no LSE).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64AtomicMinMaxRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64AtomicMinMaxRT, Verify) { roundTripAArch64(GetParam()); }

// Fields: Category, OptLevel, ExtraFlags, NoOpt, ClangTargetOverride, UcCpuModel
#define A64LSE "AtomicMinMax", 1, "-march=armv8.1-a", false, "", UC_CPU_ARM64_MAX

// clang-format off

static const std::vector<RoundTripTC> kA64MinMax = {

  // ===== X (64-bit) forms — signed =====
  // smax(18,33)=33, old=18.  OR(18,33)=51 (bug would return 51 as old, mem=18).
  {"ldsmax_x",
   "long ldsmax_x(long a){ long m=0x12; long o;"
   " __asm__ volatile(\"ldsmax %x[s],%x[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return o*31+m; }\n",
   {0x21}, A64LSE},

  // smin(33,18)=18, old=33.
  {"ldsmin_x",
   "long ldsmin_x(long a){ long m=0x21; long o;"
   " __asm__ volatile(\"ldsmin %x[s],%x[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return o*31+m; }\n",
   {0x12}, A64LSE},

  // signed min picks the negative: smin(7,-5)=-5, old=7.  (unsigned min would be 7.)
  {"ldsmin_x_neg",
   "long ldsmin_x_neg(long a){ long m=7; long o;"
   " __asm__ volatile(\"ldsmin %x[s],%x[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return o*31+m; }\n",
   {(uint64_t)-5}, A64LSE},

  // signed max picks the positive: smax(-1,5)=5, old=-1.  (unsigned max would be -1.)
  {"ldsmax_x_neg",
   "long ldsmax_x_neg(long a){ long m=-1; long o;"
   " __asm__ volatile(\"ldsmax %x[s],%x[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return o*31+m; }\n",
   {5}, A64LSE},

  // ===== X (64-bit) forms — unsigned =====
  // umax(2,5)=5, old=2.
  {"ldumax_x",
   "long ldumax_x(long a){ unsigned long m=2; unsigned long o;"
   " __asm__ volatile(\"ldumax %x[s],%x[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return (long)(o*31+m); }\n",
   {5}, A64LSE},

  // umin(5,2)=2, old=5.
  {"ldumin_x",
   "long ldumin_x(long a){ unsigned long m=5; unsigned long o;"
   " __asm__ volatile(\"ldumin %x[s],%x[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return (long)(o*31+m); }\n",
   {2}, A64LSE},

  // unsigned max picks the high-bit value: umax(0x7FFF...F, 0x8000...0)=0x8000...0,
  // old=0x7FFF...F.  Signed max would pick 0x7FFF...F (positive) — distinguishes.
  {"ldumax_x_big",
   "long ldumax_x_big(long a){ unsigned long m=0x7FFFFFFFFFFFFFFFUL; unsigned long o;"
   " __asm__ volatile(\"ldumax %x[s],%x[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return (long)(o*31+m); }\n",
   {0x8000000000000000ULL}, A64LSE},

  // unsigned min picks the small value: umin(0xFFFF...F, 5)=5, old=0xFFFF...F.
  // Signed min would pick 0xFFFF...F(-1) — distinguishes.
  {"ldumin_x_big",
   "long ldumin_x_big(long a){ unsigned long m=0xFFFFFFFFFFFFFFFFUL; unsigned long o;"
   " __asm__ volatile(\"ldumin %x[s],%x[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return (long)(o*31+m); }\n",
   {5}, A64LSE},

  // ===== W (32-bit) forms =====
  {"ldsmax_w",
   "long ldsmax_w(long a){ int m=0x12; int o;"
   " __asm__ volatile(\"ldsmax %w[s],%w[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"((int)a),[p]\"r\"(&m):\"memory\");"
   " return (long)(unsigned)(o*31+m); }\n",
   {0x21}, A64LSE},

  // 32-bit unsigned min vs signed: umin(0xFFFFFFFF,5)=5, old=0xFFFFFFFF.
  {"ldumin_w_big",
   "long ldumin_w_big(long a){ unsigned m=0xFFFFFFFFu; unsigned o;"
   " __asm__ volatile(\"ldumin %w[s],%w[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"((unsigned)a),[p]\"r\"(&m):\"memory\");"
   " return (long)(unsigned)(o*31u+m); }\n",
   {5}, A64LSE},

  // ===== Byte / halfword forms (probe access-width handling) =====
  {"ldsmaxb",
   "long ldsmaxb(long a){ unsigned char m=0x12; unsigned o;"
   " __asm__ volatile(\"ldsmaxb %w[s],%w[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"((unsigned)a),[p]\"r\"(&m):\"memory\");"
   " return (long)(unsigned)(o*31u+(unsigned)m); }\n",
   {0x21}, A64LSE},

  {"lduminh",
   "long lduminh(long a){ unsigned short m=0x1234; unsigned o;"
   " __asm__ volatile(\"lduminh %w[s],%w[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"((unsigned)a),[p]\"r\"(&m):\"memory\");"
   " return (long)(unsigned)(o*31u+(unsigned)m); }\n",
   {0x99}, A64LSE},

  // ===== Controls (already-correct atomics — guard against regressions) =====
  {"ldadd_x_ctl",
   "long ldadd_x_ctl(long a){ long m=10; long o;"
   " __asm__ volatile(\"ldadd %x[s],%x[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return o*31+m; }\n",
   {5}, A64LSE},

  {"ldaddb_ctl",
   "long ldaddb_ctl(long a){ unsigned char m=10; unsigned o;"
   " __asm__ volatile(\"ldaddb %w[s],%w[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"((unsigned)a),[p]\"r\"(&m):\"memory\");"
   " return (long)(unsigned)(o*31u+(unsigned)m); }\n",
   {5}, A64LSE},

  {"ldset_x_ctl",
   "long ldset_x_ctl(long a){ unsigned long m=0xF0; unsigned long o;"
   " __asm__ volatile(\"ldset %x[s],%x[o],[%[p]]\":[o]\"=&r\"(o):[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return (long)(o*31+m); }\n",
   {0x0F}, A64LSE},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(AtomicMinMax, A64AtomicMinMaxRT,
                         ::testing::ValuesIn(kA64MinMax), rtTCName);
