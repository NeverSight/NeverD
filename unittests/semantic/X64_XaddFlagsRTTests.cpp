//===- X64_XaddFlagsRTTests.cpp - x86 XADD flag aliasing -------*- C++ -*-===//
//
// XADD dst, src computes sum = dst + src, then writes src = old dst and
// dst = sum.  The status flags come from that add and must be derived from the
// ORIGINAL operands, but the lift computed them after the register writes, so a
// register destination/source resolved to the post-write value (the same
// write-before-snapshot class as #309 ADCX/ADCS).  These probes fold CF/OF/ZF
// into the return value to expose it.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64XaddFlagsRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64XaddFlagsRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // dst=0xFFFFFFFF, src=1 -> sum=0, CF=1, ZF=1.  Fold CF.
  {"xadd_cf",
   "long f(long a,long b){unsigned d=(unsigned)a,s=(unsigned)b;unsigned char r;"
   "__asm__ volatile(\"xaddl %2,%0\\n\\tsetc %1\""
   ":\"+r\"(d),\"=&q\"(r),\"+r\"(s)::\"cc\");return (long)(unsigned char)r;}\n",
   {0xFFFFFFFFULL, 1}, "Xadd"},
  // Same operands -> ZF after sum==0.
  {"xadd_zf",
   "long f(long a,long b){unsigned d=(unsigned)a,s=(unsigned)b;unsigned char r;"
   "__asm__ volatile(\"xaddl %2,%0\\n\\tsetz %1\""
   ":\"+r\"(d),\"=&q\"(r),\"+r\"(s)::\"cc\");return (long)(unsigned char)r;}\n",
   {0xFFFFFFFFULL, 1}, "Xadd"},
  // Signed overflow: dst=0x7FFFFFFF, src=1 -> sum=INT_MIN, OF=1.
  {"xadd_of",
   "long f(long a,long b){unsigned d=(unsigned)a,s=(unsigned)b;unsigned char r;"
   "__asm__ volatile(\"xaddl %2,%0\\n\\tseto %1\""
   ":\"+r\"(d),\"=&q\"(r),\"+r\"(s)::\"cc\");return (long)(unsigned char)r;}\n",
   {0x7FFFFFFFULL, 1}, "Xadd"},
  // Sign flag: dst=0x7FFFFFFF, src=1 -> sum negative, SF=1.
  {"xadd_sf",
   "long f(long a,long b){unsigned d=(unsigned)a,s=(unsigned)b;unsigned char r;"
   "__asm__ volatile(\"xaddl %2,%0\\n\\tsets %1\""
   ":\"+r\"(d),\"=&q\"(r),\"+r\"(s)::\"cc\");return (long)(unsigned char)r;}\n",
   {0x7FFFFFFFULL, 1}, "Xadd"},
  // 64-bit XADD carry: dst=-1, src=1 -> sum=0, CF=1.
  {"xadd_q_cf",
   "long f(long a,long b){unsigned long d=(unsigned long)a,s=(unsigned long)b;"
   "unsigned char r;__asm__ volatile(\"xaddq %2,%0\\n\\tsetc %1\""
   ":\"+r\"(d),\"=&q\"(r),\"+r\"(s)::\"cc\");return (long)(unsigned char)r;}\n",
   {0xFFFFFFFFFFFFFFFFULL, 1}, "Xadd"},
  // Result check: dst becomes sum, src becomes old dst.
  {"xadd_result",
   "long f(long a,long b){unsigned long d=(unsigned long)a,s=(unsigned long)b;"
   "__asm__ volatile(\"xaddq %1,%0\":\"+r\"(d),\"+r\"(s)::\"cc\");"
   "return (long)(d*3+s);}\n",
   {100, 7}, "Xadd"},
  // Control: no-carry add, CF=0.
  {"xadd_cf_clear",
   "long f(long a,long b){unsigned d=(unsigned)a,s=(unsigned)b;unsigned char r;"
   "__asm__ volatile(\"xaddl %2,%0\\n\\tsetc %1\""
   ":\"+r\"(d),\"=&q\"(r),\"+r\"(s)::\"cc\");return (long)(unsigned char)r;}\n",
   {2, 3}, "Xadd"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(Xadd, X64XaddFlagsRT, ::testing::ValuesIn(kX64),
                         rtTCName);
