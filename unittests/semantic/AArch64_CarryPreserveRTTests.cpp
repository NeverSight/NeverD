//===- AArch64_CarryPreserveRTTests.cpp - C survives intervening ops -----===//
//
// The AArch64 carry probes so far drive ADCS/SBCS back-to-back with their ADDS
// producer.  The x86 PartialFlag suite additionally pins a case AArch64 never
// did: the C flag set by a flag producer must SURVIVE an intervening
// non-flag-setting instruction (plain ADD/ORR/EOR/LSL, which do not touch NZCV)
// and still feed a later ADC/SBC value or a CSET condition.  The lifter only
// emits a CFLAG write for the `S` form, so the carry SSA value must thread
// across the intervening ops untouched; an optimizer that re-versions or folds
// the flag against the wrong producer diverges from hardware.  All inline asm
// so the exact stream is fixed (clang never emits ADC in a live carry chain
// bridged by a plain ADD).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64CarryPreserveRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CarryPreserveRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // C set by ADDS survives a run of plain ADD/ORR/EOR, then feeds ADC.
  {"adc_thru_add",
   "long f(long a){ unsigned long lo=(unsigned long)a, hi=(unsigned long)(a>>1)+0x100UL,"
   " sc=(unsigned long)a^0x33UL;\n"
   "  __asm__ volatile(\"adds %[lo],%[lo],%[k]\\n\\tadd %[sc],%[sc],#9\\n\\t"
   "orr %[sc],%[sc],#1\\n\\teor %[sc],%[sc],#2\\n\\tadc %[hi],%[hi],xzr\"\n"
   "    :[lo]\"+r\"(lo),[hi]\"+r\"(hi),[sc]\"+r\"(sc)\n"
   "    :[k]\"r\"(0xFFFFFFFFFFFFFFFFUL):\"cc\");\n"
   "  return (long)(hi*1000003UL+lo*131UL+sc); }\n",
   {0x1234567ULL}, "CarryPreserve"},

  // C set by ADDS survives intervening non-S ops, then captured by CSET (the
  // MedFlags condition-fold path must fold CSET against the ADDS, not the ADDs).
  {"cset_thru",
   "long f(long a){ unsigned long x=(unsigned long)a, sc=(unsigned long)a*7UL+1UL, cf;\n"
   "  __asm__ volatile(\"adds %[x],%[x],%[k]\\n\\tadd %[sc],%[sc],#5\\n\\t"
   "lsl %[sc],%[sc],#1\\n\\tcset %[cf],cs\"\n"
   "    :[x]\"+r\"(x),[sc]\"+r\"(sc),[cf]\"=r\"(cf)\n"
   "    :[k]\"r\"(0x8000000000000000UL):\"cc\");\n"
   "  return (long)(x+cf*100003UL+sc); }\n",
   {0x9876543ULL}, "CarryPreserve"},

  // Two ADDS producers: the CSET must reflect the NEAREST (second) one.  The
  // first sets C=1, the second C=0; a fold against the wrong producer flips cf.
  {"nearest_setter",
   "long f(long a){ unsigned long x=(unsigned long)a|1UL, y=5UL, cf;\n"
   "  __asm__ volatile(\"adds %[x],%[x],%[k1]\\n\\tadds %[y],%[y],%[k2]\\n\\t"
   "cset %[cf],cs\"\n"
   "    :[x]\"+r\"(x),[y]\"+r\"(y),[cf]\"=r\"(cf)\n"
   "    :[k1]\"r\"(0xFFFFFFFFFFFFFFFFUL),[k2]\"r\"(1UL):\"cc\");\n"
   "  return (long)(x+y*7UL+cf*100003UL); }\n",
   {0x2468ACEULL}, "CarryPreserve"},

  // Borrow (SUBS clears C on borrow) survives an intervening ORR, feeds SBC.
  {"sbc_thru",
   "long f(long a){ unsigned long lo=(unsigned long)a, hi=(unsigned long)(a>>2)+0x55UL,"
   " sc=(unsigned long)a^7UL;\n"
   "  __asm__ volatile(\"subs %[lo],%[lo],%[k]\\n\\torr %[sc],%[sc],#4\\n\\t"
   "sbc %[hi],%[hi],xzr\"\n"
   "    :[lo]\"+r\"(lo),[hi]\"+r\"(hi),[sc]\"+r\"(sc)\n"
   "    :[k]\"r\"(1UL):\"cc\");\n"
   "  return (long)(hi*1000003UL+lo*131UL+sc); }\n",
   {0x1357913ULL}, "CarryPreserve"},

  // ADCS (sets flags) bridged by a plain ADD, then a second ADC reads the C the
  // ADCS produced — a two-stage carry chain with a non-S op between the stages.
  {"adcs_then_add_adc",
   "long f(long a){ unsigned long w0=(unsigned long)a, w1=(unsigned long)(a>>1),"
   " w2=(unsigned long)(a>>2), sc=(unsigned long)a^0x5AUL;\n"
   "  __asm__ volatile(\"adds %[w0],%[w0],%[k]\\n\\tadcs %[w1],%[w1],%[k]\\n\\t"
   "add %[sc],%[sc],#3\\n\\tadc %[w2],%[w2],xzr\"\n"
   "    :[w0]\"+r\"(w0),[w1]\"+r\"(w1),[w2]\"+r\"(w2),[sc]\"+r\"(sc)\n"
   "    :[k]\"r\"(0xFFFFFFFFFFFFFFFFUL):\"cc\");\n"
   "  return (long)(w0+w1*131UL+w2*1000003UL+sc); }\n",
   {0xDEADBEEULL}, "CarryPreserve"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(CarryPreserve, A64CarryPreserveRT,
                         ::testing::ValuesIn(kA64), rtTCName);
