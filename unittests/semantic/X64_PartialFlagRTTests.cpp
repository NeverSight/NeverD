//===- X64_PartialFlagRTTests.cpp - partial-flag preservation --*- C++ -*-===//
//
// Roundtrip probes for x86 partial-flag semantics that stress the self-written
// MedIR flag pass (MedFlags):
//
//  * INC/DEC preserve CF (they only touch OF/SF/ZF/AF/PF).  A carry chain that
//    threads CF through an INC/DEC (`stc; inc; adc`) must keep the carry.
//  * A CF/ZF consumer (setcc/jcc) separated from its producing CMP by an
//    intervening flag-modifying-but-CF-preserving instruction must still fold
//    against the right CMP, not the intervening op.
//  * Two independent CMP+SETcc pairs in one block must each match their own CMP.
//
// These all use inline asm so the exact instruction stream is fixed; clang's
// own codegen would never emit `inc` in a live carry chain.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PartialFlagRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PartialFlagRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // CF=1 survives INC -> adc adds 1.
  {"cf_survives_inc",
   "long f(long a,long b){__asm__ volatile(\"stc; incq %0; adcq $0,%1\""
   ":\"+r\"(a),\"+r\"(b)::\"cc\");return b;}\n",
   {5, 10}, "PartialFlag", 0},

  // CF=1 survives DEC -> adc adds 1.
  {"cf_survives_dec",
   "long f(long a,long b){__asm__ volatile(\"stc; decq %0; adcq $0,%1\""
   ":\"+r\"(a),\"+r\"(b)::\"cc\");return b;}\n",
   {5, 10}, "PartialFlag", 0},

  // CF=0 survives INC -> adc adds 0 (opposite polarity guard).
  {"cf0_survives_inc",
   "long f(long a,long b){__asm__ volatile(\"clc; incq %0; adcq $0,%1\""
   ":\"+r\"(a),\"+r\"(b)::\"cc\");return b;}\n",
   {5, 10}, "PartialFlag", 0},

  // CF survives a run of INC/DEC.
  {"cf_survives_chain",
   "long f(long a,long b){__asm__ volatile("
   "\"stc; incq %0; incq %0; decq %0; adcq $0,%1\""
   ":\"+r\"(a),\"+r\"(b)::\"cc\");return b;}\n",
   {5, 10}, "PartialFlag", 0},

  // CMP sets CF; intervening INC preserves CF; SETB reads the CMP's CF.
  {"cf_across_inc_setb",
   "long f(long a,long b,long c){unsigned char r;"
   "__asm__ volatile(\"cmpq %3,%2; incq %1; setb %0\""
   ":\"=r\"(r),\"+r\"(c):\"r\"(a),\"r\"(b):\"cc\");return (long)r;}\n",
   {3, 5, 100}, "PartialFlag", 0},

  // Same but ZF: CMP sets ZF; intervening INC overwrites ZF; SETE must read the
  // INC's ZF result (nearest producer), not the CMP — sanity that we don't fold
  // too aggressively.  inc of 0 -> result 1 -> ZF=0 -> sete=0.
  {"zf_nearest_inc",
   "long f(long a,long b,long c){unsigned char r;"
   "__asm__ volatile(\"cmpq %2,%2; incq %1; sete %0\""
   ":\"=r\"(r),\"+r\"(c):\"r\"(a):\"cc\");return (long)r;}\n",
   {7, 0, 0}, "PartialFlag", 0},

  // Two independent CMP+SETcc pairs in one block (MedFlags nearest-CMP).
  {"two_cmp_setcc",
   "long f(long a,long b,long c,long d){unsigned char x,y;"
   "__asm__ volatile(\"cmpq %3,%2; setl %0; cmpq %5,%4; setg %1\""
   ":\"=&r\"(x),\"=&r\"(y):\"r\"(a),\"r\"(b),\"r\"(c),\"r\"(d):\"cc\");"
   "return (long)x*10+(long)y;}\n",
   {1, 9, 9, 1}, "PartialFlag", 0},

  // ADC carry chain bridged by an unrelated INC of a 3rd register.
  {"adc_chain_inc_bridge",
   "long f(long a,long b,long c){__asm__ volatile("
   "\"addq %2,%0; incq %1; adcq $0,%0\""
   ":\"+r\"(a),\"+r\"(c):\"r\"(b):\"cc\");return a;}\n",
   {0xFFFFFFFFFFFFFFFFULL, 2, 0}, "PartialFlag", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(PartialFlag, X64PartialFlagRT, ::testing::ValuesIn(kX64),
                         rtTCName);
