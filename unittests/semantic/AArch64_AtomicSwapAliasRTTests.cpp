//===- AArch64_AtomicSwapAliasRTTests.cpp - SWP/LD<op> Rs==Rt ---*- C++ -*-===//
//
// FEAT_LSE swap / load-op instructions allow the source (Rs) and destination
// (Rt) to be the SAME register, e.g. `swp x0,x0,[x1]`:
//   Rt = *[Xn];  *[Xn] = old_Rs   (with Rs==Rt the register ends up = old mem)
//
// The lifter's shared `loadOpPrologue` read Rs as a live register reference,
// then wrote Rt (= loaded memory) BEFORE narrowing/using Rs for the store.  When
// Rs and Rt are the same register the destination write clobbered the source, so
// the store wrote the just-loaded value back and memory was left UNCHANGED.  The
// fix snapshots Rs into a temp before the destination write; these probes fold
// both the post-op memory cell and the loaded register into the return so a
// missing/!wrong store is caught.  Distinct-register forms are controls.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64AtomicSwapAliasRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64AtomicSwapAliasRT, Verify) { roundTripAArch64(GetParam()); }

// Fields after CSrc: Args, Category, OptLevel, ExtraFlags, NoOpt, Triple, UcCpu
#define LSE "SwapAlias", 1, "-march=armv8.1-a", false, "", UC_CPU_ARM64_MAX

// clang-format off
static const std::vector<RoundTripTC> kA64SwapAlias = {

  // SWP with Rs==Rt: mem<-old_reg(55), reg<-old_mem(1000) -> 1000*7+55*13=7715.
  // RED before fix: store used the clobbered register, mem stayed 1000 ->
  // 1000*7+1000*13 = 20000.
  {"swp_same_reg",
   "long f(long v){long mem=1000; long old=v;"
   "__asm__ volatile(\"swp %0,%0,[%1]\":\"+r\"(old):\"r\"(&mem):\"memory\");"
   "return old*7+mem*13;}\n",
   {55}, LSE},

  // SWPA / SWPL / SWPAL ordering variants with Rs==Rt.
  {"swpa_same_reg",
   "long f(long v){long mem=1000; long old=v;"
   "__asm__ volatile(\"swpa %0,%0,[%1]\":\"+r\"(old):\"r\"(&mem):\"memory\");"
   "return old*7+mem*13;}\n",
   {55}, LSE},
  {"swpal_same_reg",
   "long f(long v){long mem=1000; long old=v;"
   "__asm__ volatile(\"swpal %0,%0,[%1]\":\"+r\"(old):\"r\"(&mem):\"memory\");"
   "return old*7+mem*13;}\n",
   {55}, LSE},

  // 32-bit SWP with Rs==Rt.
  {"swp_w_same_reg",
   "int f(int v){int mem=1000; int old=v;"
   "__asm__ volatile(\"swp %w0,%w0,[%1]\":\"+r\"(old):\"r\"(&mem):\"memory\");"
   "return old*7+mem*13;}\n",
   {55}, LSE},

  // Byte / halfword SWP with Rs==Rt (access-size narrowing path).
  {"swpb_same_reg",
   "int f(int v){unsigned char mem=200; int old=v;"
   "__asm__ volatile(\"swpb %w0,%w0,[%1]\":\"+r\"(old):\"r\"(&mem):\"memory\");"
   "return (old&0xff)*7+mem*13;}\n",
   {0x55}, LSE},

  // LDADD with Rs==Rt: Rt<-old mem(1000); mem<-mem+old_Rs(5)=1005.
  // reg=1000 -> 1000*7 + 1005*13 = 7000 + 13065 = 20065.
  {"ldadd_same_reg",
   "long f(long v){long mem=1000; long r=v;"
   "__asm__ volatile(\"ldadd %0,%0,[%1]\":\"+r\"(r):\"r\"(&mem):\"memory\");"
   "return r*7+mem*13;}\n",
   {5}, LSE},

  // LDEOR with Rs==Rt.
  {"ldeor_same_reg",
   "long f(long v){long mem=0x0F0F; long r=v;"
   "__asm__ volatile(\"ldeor %0,%0,[%1]\":\"+r\"(r):\"r\"(&mem):\"memory\");"
   "return r*7+mem*13;}\n",
   {0x00FF}, LSE},

  // LDSET with Rs==Rt.
  {"ldset_same_reg",
   "long f(long v){long mem=0x0F0F; long r=v;"
   "__asm__ volatile(\"ldset %0,%0,[%1]\":\"+r\"(r):\"r\"(&mem):\"memory\");"
   "return r*7+mem*13;}\n",
   {0x00FF}, LSE},

  // ===== Controls: distinct Rs/Rt must still round-trip. =====
  {"swp_distinct_ctl",
   "long f(long v){long mem=1000; long old;"
   "__asm__ volatile(\"swp %2,%0,[%1]\":\"=r\"(old):\"r\"(&mem),\"r\"(v):\"memory\");"
   "return old*7+mem*13;}\n",
   {55}, LSE},
  {"ldadd_distinct_ctl",
   "long f(long v){long mem=1000; long old;"
   "__asm__ volatile(\"ldadd %2,%0,[%1]\":\"=r\"(old):\"r\"(&mem),\"r\"(v):\"memory\");"
   "return old*7+mem*13;}\n",
   {5}, LSE},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(SwapAlias, A64AtomicSwapAliasRT,
                         ::testing::ValuesIn(kA64SwapAlias), rtTCName);
