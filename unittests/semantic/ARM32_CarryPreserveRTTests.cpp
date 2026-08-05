//===- ARM32_CarryPreserveRTTests.cpp - C survives intervening ops -------===//
//
// ARM32 analogue of the x86 PartialFlag suite (missing until now): the C flag
// produced by a flag-setter (`adds`/`subs`, or the shift-sets-carry `lsls`)
// must survive an intervening NON-flag-setting instruction (plain `add`/`orr`,
// no `s` suffix) and still feed a later `adc`/`sbc`/`rrx`.  The lifter emits a
// CFLAG write only for the flag-setting form, so the carry SSA value threads
// across the bridge untouched; an optimizer that re-versions the flag or folds
// a condition against the wrong producer diverges from hardware.  Inline asm
// pins the exact stream (clang never bridges a live carry chain with a plain
// `add`).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32CarryPreserveRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CarryPreserveRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kARM = {
  // C set by ADDS survives plain ADD/ORR, then feeds ADC.
  {"adc_thru_add",
   "int f(int a){ unsigned lo=(unsigned)a, hi=(unsigned)(a>>1)+0x100u, sc=(unsigned)a^0x33u;\n"
   "  __asm__ volatile(\"adds %[lo],%[lo],%[k]\\n\\tadd %[sc],%[sc],#9\\n\\t"
   "orr %[sc],%[sc],#1\\n\\tadc %[hi],%[hi],#0\"\n"
   "    :[lo]\"+r\"(lo),[hi]\"+r\"(hi),[sc]\"+r\"(sc)\n"
   "    :[k]\"r\"(0xFFFFFFFFu):\"cc\");\n"
   "  return (int)(hi*1000003u+lo*131u+sc); }\n",
   {0x1234567ULL}, "CarryPreserve"},

  // C from the shift-sets-carry LSLS (C = last bit shifted out) survives a plain
  // ADD, then feeds ADC — a non-arithmetic carry producer the x86 suite mirrors
  // with SHR/SHL but ARM32 had no roundtrip guard for.
  {"shiftc_adc",
   "int f(int a){ unsigned x=(unsigned)a|0x80000000u, hi=(unsigned)a+7u, sc=(unsigned)a^5u;\n"
   "  __asm__ volatile(\"lsls %[x],%[x],#1\\n\\tadd %[sc],%[sc],#3\\n\\t"
   "adc %[hi],%[hi],#0\"\n"
   "    :[x]\"+r\"(x),[hi]\"+r\"(hi),[sc]\"+r\"(sc)::\"cc\");\n"
   "  return (int)(x+hi*1000003u+sc); }\n",
   {0x9876543ULL}, "CarryPreserve"},

  // RRX (rotate right through carry) reads C: LSLS sets C, an intervening ADD
  // must preserve it, RRX folds it into bit 31.
  {"rrx_thru",
   "int f(int a){ unsigned x=(unsigned)a|0x80000000u, r, sc=(unsigned)a^9u;\n"
   "  __asm__ volatile(\"lsls %[x],%[x],#1\\n\\tadd %[sc],%[sc],#1\\n\\trrx %[r],%[x]\"\n"
   "    :[r]\"=r\"(r),[x]\"+r\"(x),[sc]\"+r\"(sc)::\"cc\");\n"
   "  return (int)(r+sc); }\n",
   {0x2468ACEULL}, "CarryPreserve"},

  // Two ADDS producers: the captured C must reflect the NEAREST (second) one.
  // First sets C=1, second C=0; mis-folding against the first flips cf.
  {"nearest_setter",
   "int f(int a){ unsigned x=(unsigned)a|1u, y=5u, cf;\n"
   "  __asm__ volatile(\"adds %[x],%[x],%[k1]\\n\\tadds %[y],%[y],%[k2]\\n\\t"
   "mov %[cf],#0\\n\\tadc %[cf],%[cf],#0\"\n"
   "    :[x]\"+r\"(x),[y]\"+r\"(y),[cf]\"=r\"(cf)\n"
   "    :[k1]\"r\"(0xFFFFFFFFu),[k2]\"r\"(1u):\"cc\");\n"
   "  return (int)(x+y*7u+cf*100003u); }\n",
   {0x1357913ULL}, "CarryPreserve"},

  // Borrow (SUBS) survives an intervening ORR, then feeds SBC.
  {"sbc_thru",
   "int f(int a){ unsigned lo=(unsigned)a, hi=(unsigned)(a>>2)+0x55u, sc=(unsigned)a^7u;\n"
   "  __asm__ volatile(\"subs %[lo],%[lo],%[k]\\n\\torr %[sc],%[sc],#4\\n\\t"
   "sbc %[hi],%[hi],#0\"\n"
   "    :[lo]\"+r\"(lo),[hi]\"+r\"(hi),[sc]\"+r\"(sc)\n"
   "    :[k]\"r\"(1u):\"cc\");\n"
   "  return (int)(hi*1000003u+lo*131u+sc); }\n",
   {0xDEADBEEULL}, "CarryPreserve"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(CarryPreserve, ARM32CarryPreserveRT,
                         ::testing::ValuesIn(kARM), rtTCName);
