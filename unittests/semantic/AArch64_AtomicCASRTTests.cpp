//===- AArch64_AtomicCASRTTests.cpp - single-word CAS roundtrip --*- C++ -===//
//
// FEAT_LSE single compare-and-swap `CAS Ws, Wt, [Xn]`:
//   old = *[Xn];  if old == Ws then *[Xn] = Wt;  Ws = old
//
// The lifter read Ws (the compare value) as a live register reference, then
// wrote Ws = loaded BEFORE deriving the compare operand from it — so the compare
// became `loaded == loaded` (ALWAYS true) and the new value was stored
// UNCONDITIONALLY, even on a mismatch.  A match-only probe cannot see this (a
// match stores anyway), so the failing case is the MISMATCH: memory must be left
// unchanged.  The pair form (CASP) already wrote the destination last and was
// correct; only single CAS was affected.  Probes fold the post-op memory cell
// and the loaded register into the return.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64AtomicCASRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64AtomicCASRT, Verify) { roundTripAArch64(GetParam()); }

// Fields after CSrc: Args, Category, OptLevel, ExtraFlags, NoOpt, Triple, UcCpu
#define LSE "AtomicCAS", 1, "-march=armv8.1-a", false, "", UC_CPU_ARM64_MAX

// clang-format off
static const std::vector<RoundTripTC> kA64CAS = {

  // MISMATCH: mem(50)!=Ws(40) -> mem stays 50, Ws=old(50) -> 50*7+50*13=1000.
  // RED before fix: compare folded to true, mem stored desired(99) ->
  // 50*7 + 99*13 = 1637.
  {"cas_x_nomatch",
   "long f(long v){long mem=50; long s=40,t=99;"
   "__asm__ volatile(\"cas %0,%2,[%1]\":\"+r\"(s):\"r\"(&mem),\"r\"(t):\"memory\");"
   "return s*7+mem*13;}\n",
   {0}, LSE},

  // MATCH control: mem(50)==Ws(50) -> mem=desired(99), Ws=old(50) -> 1637.
  {"cas_x_match",
   "long f(long v){long mem=50; long s=50,t=99;"
   "__asm__ volatile(\"cas %0,%2,[%1]\":\"+r\"(s):\"r\"(&mem),\"r\"(t):\"memory\");"
   "return s*7+mem*13;}\n",
   {0}, LSE},

  // CASA / CASL / CASAL ordering variants, MISMATCH (the discriminating case).
  {"casa_x_nomatch",
   "long f(long v){long mem=50; long s=40,t=99;"
   "__asm__ volatile(\"casa %0,%2,[%1]\":\"+r\"(s):\"r\"(&mem),\"r\"(t):\"memory\");"
   "return s*7+mem*13;}\n",
   {0}, LSE},
  {"casl_x_nomatch",
   "long f(long v){long mem=50; long s=40,t=99;"
   "__asm__ volatile(\"casl %0,%2,[%1]\":\"+r\"(s):\"r\"(&mem),\"r\"(t):\"memory\");"
   "return s*7+mem*13;}\n",
   {0}, LSE},
  {"casal_x_nomatch",
   "long f(long v){long mem=50; long s=40,t=99;"
   "__asm__ volatile(\"casal %0,%2,[%1]\":\"+r\"(s):\"r\"(&mem),\"r\"(t):\"memory\");"
   "return s*7+mem*13;}\n",
   {0}, LSE},

  // 32-bit CAS, MISMATCH and MATCH.
  {"cas_w_nomatch",
   "int f(int v){int mem=50; int s=40,t=99;"
   "__asm__ volatile(\"cas %w0,%w2,[%1]\":\"+r\"(s):\"r\"(&mem),\"r\"(t):\"memory\");"
   "return s*7+mem*13;}\n",
   {0}, LSE},
  {"cas_w_match",
   "int f(int v){int mem=50; int s=50,t=99;"
   "__asm__ volatile(\"cas %w0,%w2,[%1]\":\"+r\"(s):\"r\"(&mem),\"r\"(t):\"memory\");"
   "return s*7+mem*13;}\n",
   {0}, LSE},

  // Byte / halfword CAS, MISMATCH (access-size narrowing path).
  {"casb_nomatch",
   "int f(int v){unsigned char mem=50; int s=40,t=99;"
   "__asm__ volatile(\"casb %w0,%w2,[%1]\":\"+r\"(s):\"r\"(&mem),\"r\"(t):\"memory\");"
   "return (s&0xff)*7+mem*13;}\n",
   {0}, LSE},
  {"cash_nomatch",
   "int f(int v){unsigned short mem=5000; int s=4000,t=9999;"
   "__asm__ volatile(\"cash %w0,%w2,[%1]\":\"+r\"(s):\"r\"(&mem),\"r\"(t):\"memory\");"
   "return (s&0xffff)*7+mem*13;}\n",
   {0}, LSE},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(AtomicCAS, A64AtomicCASRT,
                         ::testing::ValuesIn(kA64CAS), rtTCName);
