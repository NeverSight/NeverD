//===- X64_PushfPopfRTTests.cpp - PUSHF/POPF flag (de)serialize -*- C++ -*-===//
//
// Roundtrip probes for x86 PUSHF/POPF (push/pop the flags register).  NeverD
// previously modelled them as placeholders: PUSHF stored an *uninitialised*
// temp (never assembling the flags) and POPF copied uninitialised temps into
// the flag registers (never reading the popped value) — so flags were never
// actually serialised or deserialised.
//
// The fix assembles/scatters the modelled flags at their architectural EFLAGS
// positions: CF(0), PF(2), AF(4), ZF(6), SF(7), DF(10), OF(11) (reserved bit 1
// reads as 1).  System flags (TF/IF/IOPL/...) are not modelled, so probes mask
// the popped EFLAGS down to the modelled bits before comparing.
//
// PUSHF probes set flags with an arithmetic op (or STD for DF), push, pop, and
// mask; POPF probes push a value, popf, then read flags back via LAHF (SF/ZF/
// AF/PF/CF) + SETO (OF).  All x86 native => pure lift placeholder fix.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PushfPopfRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PushfPopfRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // PUSHF after add 0xFFFF..F + 1 = 0: CF=ZF=AF=PF=1, SF=OF=0.  Mask 0x8D5
  // (CF|PF|AF|ZF|SF|OF, excludes the uncontrolled DF) => 0x55.
  {"pushf_add_czap",
   "long f(long a,long b){unsigned long o;"
   "__asm__ volatile(\"addq %2,%1\\n\\tpushfq\\n\\tpopq %0\\n\\tandq $0x8D5,%0\""
   ":\"=&r\"(o),\"+r\"(a):\"r\"(b):\"cc\",\"memory\");return (long)o;}\n",
   {0xFFFFFFFFFFFFFFFFULL, 1ULL}, "PushfPopf", 0},

  // PUSHF after add INT64_MAX + 1: SF=1, OF=1, AF=1, PF=1, CF=0, ZF=0.
  // Mask 0x8D5 => 0x894.
  {"pushf_add_sfof",
   "long f(long a,long b){unsigned long o;"
   "__asm__ volatile(\"addq %2,%1\\n\\tpushfq\\n\\tpopq %0\\n\\tandq $0x8D5,%0\""
   ":\"=&r\"(o),\"+r\"(a):\"r\"(b):\"cc\",\"memory\");return (long)o;}\n",
   {0x7FFFFFFFFFFFFFFFULL, 1ULL}, "PushfPopf", 0},

  // PUSHF captures DF: STD sets DF=1, push, pop, CLD restores; mask 0x400.
  {"pushf_df",
   "long f(long a){unsigned long o;(void)a;"
   "__asm__ volatile(\"std\\n\\tpushfq\\n\\tpopq %0\\n\\tcld\\n\\tandq $0x400,%0\""
   ":\"=r\"(o)::\"cc\",\"memory\");return (long)o;}\n",
   {0ULL}, "PushfPopf", 0},

  // POPF 0x894 sets PF/AF/SF/OF; LAHF reads back AH=0x96, SETO=1 => 0x10096.
  {"popf_set",
   "long f(long a){unsigned long o;unsigned int of;"
   "__asm__ volatile(\"pushq %2\\n\\tpopfq\\n\\tlahf\\n\\tmovzbl %%ah,%k0\\n\\t\""
   "\"seto %b1\":\"=&r\"(o),\"=&r\"(of):\"r\"(a):\"ax\",\"cc\",\"memory\");"
   "return (long)o|((long)(of&1)<<16);}\n",
   {0x894ULL}, "PushfPopf", 0},

  // POPF 0x002 (only the reserved bit): all modelled flags 0; LAHF AH=0x02.
  {"popf_zero",
   "long f(long a){unsigned long o;unsigned int of;"
   "__asm__ volatile(\"pushq %2\\n\\tpopfq\\n\\tlahf\\n\\tmovzbl %%ah,%k0\\n\\t\""
   "\"seto %b1\":\"=&r\"(o),\"=&r\"(of):\"r\"(a):\"ax\",\"cc\",\"memory\");"
   "return (long)o|((long)(of&1)<<16);}\n",
   {0x002ULL}, "PushfPopf", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(PushfPopf, X64PushfPopfRT, ::testing::ValuesIn(kX64),
                         rtTCName);
