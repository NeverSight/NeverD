//===- X86_DoubleShiftZeroRTTests.cpp - SHLD/SHRD zero-count value -*- C++ -*-===//
//
// SHLD/SHRD leave the destination UNCHANGED when the (post-mask) count is 0,
// exactly like the single shifts.  X64_DoubleShiftFlagsRTTests already pins the
// flag rule, but only ever reads the flags back — never the destination value.
//
// The lift snapshots/restores the flags for a zero count, yet computes the
// result value unconditionally as `(dst << cnt) | (src >> (bits - cnt))`.  At
// cnt==0 the `src >> bits` term is a runtime shift by the full width, which the
// hardware masks back to `src` (not 0) — so the recompiled value becomes
// `dst | src` instead of `dst`.  These probes pass the count through a function
// argument (runtime-0, invisible to the optimizer) and return the destination,
// so any corruption of the unchanged-destination contract surfaces.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64DoubleShiftZeroRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64DoubleShiftZeroRT, Verify) { roundTripX64(GetParam()); }

class X86DoubleShiftZeroRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86DoubleShiftZeroRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
// 32-bit double shifts: valid in both x86_64 and i386.
static const std::vector<RoundTripTC> kDblZero32 = {
  // shldl, count=0 (arg): dest must stay 0x12345678, not OR in the source.
  {"shldl_cnt0_value",
   "long f(long a,long cnt){unsigned d=(unsigned)a,c=(unsigned)cnt;"
   "__asm__ volatile(\"shldl %%cl,%1,%0\":\"+r\"(d):\"r\"(0x9ABCDEF0u),"
   "\"c\"(c):\"cc\");return (long)(unsigned)d;}\n",
   {0x12345678ULL, 0ULL}, "DblShiftZero"},
  // shrdl, count=0 (arg): dest must stay unchanged.
  {"shrdl_cnt0_value",
   "long f(long a,long cnt){unsigned d=(unsigned)a,c=(unsigned)cnt;"
   "__asm__ volatile(\"shrdl %%cl,%1,%0\":\"+r\"(d):\"r\"(0x9ABCDEF0u),"
   "\"c\"(c):\"cc\");return (long)(unsigned)d;}\n",
   {0x12345678ULL, 0ULL}, "DblShiftZero"},
  // Control: shldl with a nonzero runtime count must still combine correctly.
  {"shldl_cnt8_value",
   "long f(long a,long cnt){unsigned d=(unsigned)a,c=(unsigned)cnt;"
   "__asm__ volatile(\"shldl %%cl,%1,%0\":\"+r\"(d):\"r\"(0x9ABCDEF0u),"
   "\"c\"(c):\"cc\");return (long)(unsigned)d;}\n",
   {0x12345678ULL, 8ULL}, "DblShiftZero"},
  // Control: shrdl with a nonzero runtime count.
  {"shrdl_cnt8_value",
   "long f(long a,long cnt){unsigned d=(unsigned)a,c=(unsigned)cnt;"
   "__asm__ volatile(\"shrdl %%cl,%1,%0\":\"+r\"(d):\"r\"(0x9ABCDEF0u),"
   "\"c\"(c):\"cc\");return (long)(unsigned)d;}\n",
   {0x12345678ULL, 8ULL}, "DblShiftZero"},
};

// 64-bit double shifts: x86_64 only (shldq/shrdq are illegal in i386).
static const std::vector<RoundTripTC> kDblZero64 = {
  {"shldq_cnt0_value",
   "long f(long a,long cnt){unsigned long d=(unsigned long)a,c=(unsigned long)cnt;"
   "__asm__ volatile(\"shldq %%cl,%1,%0\":\"+r\"(d):"
   "\"r\"(0x9ABCDEF012345678ULL),\"c\"(c):\"cc\");return (long)d;}\n",
   {0x0123456789ABCDEFULL, 0ULL}, "DblShiftZero"},
  {"shrdq_cnt0_value",
   "long f(long a,long cnt){unsigned long d=(unsigned long)a,c=(unsigned long)cnt;"
   "__asm__ volatile(\"shrdq %%cl,%1,%0\":\"+r\"(d):"
   "\"r\"(0x9ABCDEF012345678ULL),\"c\"(c):\"cc\");return (long)d;}\n",
   {0x0123456789ABCDEFULL, 0ULL}, "DblShiftZero"},
  {"shldq_cnt20_value",
   "long f(long a,long cnt){unsigned long d=(unsigned long)a,c=(unsigned long)cnt;"
   "__asm__ volatile(\"shldq %%cl,%1,%0\":\"+r\"(d):"
   "\"r\"(0x9ABCDEF012345678ULL),\"c\"(c):\"cc\");return (long)d;}\n",
   {0x0123456789ABCDEFULL, 20ULL}, "DblShiftZero"},
};

static std::vector<RoundTripTC> makeX64DblZero() {
  std::vector<RoundTripTC> V = kDblZero32;
  V.insert(V.end(), kDblZero64.begin(), kDblZero64.end());
  return V;
}
// clang-format on

static const std::vector<RoundTripTC> kX64DblZero = makeX64DblZero();

INSTANTIATE_TEST_SUITE_P(DblShiftZero, X64DoubleShiftZeroRT,
                         ::testing::ValuesIn(kX64DblZero), rtTCName);
INSTANTIATE_TEST_SUITE_P(DblShiftZero, X86DoubleShiftZeroRT,
                         ::testing::ValuesIn(kDblZero32), rtTCName);
