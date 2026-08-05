//===- AArch64_AtomicCASPRTTests.cpp - LSE CASP roundtrip ----------------===//
//
// Roundtrip probes for the AArch64 FEAT_LSE compare-and-swap *pair* (CASP /
// CASPA / CASPAL / CASPL).  CASP Xs,Xs+1, Xt,Xt+1, [Xn] atomically:
//   load the {lo,hi} pair from [Xn]; if it equals the expected pair {Xs,Xs+1}
//   store the desired pair {Xt,Xt+1}; and write the loaded pair back to the
//   expected registers {Xs,Xs+1}.
//
// The lifter modelled CASP as a single `COPY Dst, Src` placeholder — it neither
// compared, nor stored the pair, nor wrote back the old pair.  Probes fold both
// the resulting memory pair and the written-back register pair into the return.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64AtomicCASPRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64AtomicCASPRT, Verify) { roundTripAArch64(GetParam()); }

// Fields: Category, OptLevel, ExtraFlags, NoOpt, ClangTargetOverride, UcCpuModel
#define A64LSE "AtomicCASP", 1, "-march=armv8.1-a", false, "", UC_CPU_ARM64_MAX

// clang-format off

static const std::vector<RoundTripTC> kA64CASP = {

  // Match: mem{10,20} == expected{10,20} -> mem becomes {100,200}; x0:x1 = {10,20}.
  {"casp_x_match",
   "long casp_x_match(long a){ unsigned long mem[2]={10,20};"
   " register unsigned long x0 __asm__(\"x0\")=10; register unsigned long x1 __asm__(\"x1\")=20;"
   " register unsigned long x2 __asm__(\"x2\")=100; register unsigned long x3 __asm__(\"x3\")=200;"
   " __asm__ volatile(\"casp x0,x1,x2,x3,[%[p]]\":\"+r\"(x0),\"+r\"(x1):\"r\"(x2),\"r\"(x3),[p]\"r\"(mem):\"memory\");"
   " return (long)(mem[0]*7+mem[1]*13+x0*17+x1*19); }\n",
   {0}, A64LSE},

  // No match (hi differs): mem unchanged {10,20}; x0:x1 = loaded {10,20}.
  {"casp_x_nomatch",
   "long casp_x_nomatch(long a){ unsigned long mem[2]={10,20};"
   " register unsigned long x0 __asm__(\"x0\")=10; register unsigned long x1 __asm__(\"x1\")=99;"
   " register unsigned long x2 __asm__(\"x2\")=100; register unsigned long x3 __asm__(\"x3\")=200;"
   " __asm__ volatile(\"casp x0,x1,x2,x3,[%[p]]\":\"+r\"(x0),\"+r\"(x1):\"r\"(x2),\"r\"(x3),[p]\"r\"(mem):\"memory\");"
   " return (long)(mem[0]*7+mem[1]*13+x0*17+x1*19); }\n",
   {0}, A64LSE},

  // Ordering variant CASPAL, match.
  {"caspal_x_match",
   "long caspal_x_match(long a){ unsigned long mem[2]={7,8};"
   " register unsigned long x0 __asm__(\"x0\")=7; register unsigned long x1 __asm__(\"x1\")=8;"
   " register unsigned long x2 __asm__(\"x2\")=70; register unsigned long x3 __asm__(\"x3\")=80;"
   " __asm__ volatile(\"caspal x0,x1,x2,x3,[%[p]]\":\"+r\"(x0),\"+r\"(x1):\"r\"(x2),\"r\"(x3),[p]\"r\"(mem):\"memory\");"
   " return (long)(mem[0]*7+mem[1]*13+x0*17+x1*19); }\n",
   {0}, A64LSE},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(AtomicCASP, A64AtomicCASPRT,
                         ::testing::ValuesIn(kA64CASP), rtTCName);
