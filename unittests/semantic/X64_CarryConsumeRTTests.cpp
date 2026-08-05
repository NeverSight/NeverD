//===- X64_CarryConsumeRTTests.cpp - CF producer -> ADC/SBB consumer -*- C++ -*-=//
//
// Existing flag tests read a flag straight into a setcc/seto/jcc.  A different,
// untested path is a non-arithmetic CF *producer* (shift, rotate, BT, NEG, or a
// direct CMC/STC/CLC) feeding the CF as the carry-in *value* of a later ADC/SBB.
// This exercises the SSA flag value-flow (not the MedFlags condition fold): the
// producer writes reg(CF); ADC/SBB read it as a carry operand.  If the optimizer
// drops the CF def, mis-versions it, or the ADC/SBB carry-in is mismodelled, the
// recompiled run diverges.  Each probe folds the producer's own result and the
// ADC/SBB result into disjoint multiplicative fields so any error shows up.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64CarryConsumeRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64CarryConsumeRT, Verify) { roundTripX64(GetParam()); }

// Fold two 64-bit outputs into one return value with odd multipliers so both
// components are independently observable.
#define FOLD2 "return x*0xD1B54A32D192ED03ULL+y*0x9E3779B97F4A7C15ULL;}\n"

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // ===== Shift CF -> ADC (CF = last bit shifted out). =====
  // shr: CF = bit0(a).  a=0xF -> x=7, CF=1, y=b+1.
  {"shr_adc",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a,y=b;"
   "__asm__ volatile(\"shrq $1,%[x]\\n\\tadcq $0,%[y]\""
   ":[x]\"+r\"(x),[y]\"+r\"(y)::\"cc\");" FOLD2,
   {0xF, 100}, "CarryConsume"},
  // shl: CF = bit63(a).  a=1<<63 -> x=0, CF=1, y=b+1.
  {"shl_adc",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a,y=b;"
   "__asm__ volatile(\"shlq $1,%[x]\\n\\tadcq $0,%[y]\""
   ":[x]\"+r\"(x),[y]\"+r\"(y)::\"cc\");" FOLD2,
   {0x8000000000000000ULL, 100}, "CarryConsume"},
  // sar: CF = bit0(a); preserves sign.  a=-3 (odd) -> CF=1.
  {"sar_adc",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a,y=b;"
   "__asm__ volatile(\"sarq $1,%[x]\\n\\tadcq $0,%[y]\""
   ":[x]\"+r\"(x),[y]\"+r\"(y)::\"cc\");" FOLD2,
   {(uint64_t)(int64_t)-3, 100}, "CarryConsume"},
  // shr by 0 leaves CF unchanged (entry CF unknown -> both runs agree on the
  // *modelled* CF=0 baseline; pair shifts by 1 so the bit is deterministic).
  {"shr_adc_clear",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a,y=b;"
   "__asm__ volatile(\"shrq $1,%[x]\\n\\tadcq $0,%[y]\""
   ":[x]\"+r\"(x),[y]\"+r\"(y)::\"cc\");" FOLD2,
   {0x10, 100}, "CarryConsume"},

  // ===== BT family CF -> ADC (CF = tested bit). =====
  // bt $5: CF = bit5(a).  a=0x20 -> CF=1.
  {"bt_adc_set",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a,y=b;"
   "__asm__ volatile(\"btq $5,%[x]\\n\\tadcq $0,%[y]\""
   ":[y]\"+r\"(y):[x]\"r\"(x):\"cc\");" FOLD2,
   {0x20, 100}, "CarryConsume"},
  {"bt_adc_clr",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a,y=b;"
   "__asm__ volatile(\"btq $5,%[x]\\n\\tadcq $0,%[y]\""
   ":[y]\"+r\"(y):[x]\"r\"(x):\"cc\");" FOLD2,
   {0x10, 100}, "CarryConsume"},
  // btr: CF = bit3(a) and clears it.  a=0xF -> CF=1, x=0x7.
  {"btr_adc",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a,y=b;"
   "__asm__ volatile(\"btrq $3,%[x]\\n\\tadcq $0,%[y]\""
   ":[x]\"+r\"(x),[y]\"+r\"(y)::\"cc\");" FOLD2,
   {0xF, 100}, "CarryConsume"},

  // ===== Rotate CF -> ADC/SBB. =====
  // ror $1: CF = bit0(a) (rotated into MSB).  a=3 -> CF=1, x=0x8000..1.
  {"ror_adc",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a,y=b;"
   "__asm__ volatile(\"rorq $1,%[x]\\n\\tadcq $0,%[y]\""
   ":[x]\"+r\"(x),[y]\"+r\"(y)::\"cc\");" FOLD2,
   {0x3, 100}, "CarryConsume"},
  // rol $1: CF = bit63(a) (rotated into LSB).  a=1<<63 -> CF=1, sbb subtracts 1.
  {"rol_sbb",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a,y=b;"
   "__asm__ volatile(\"rolq $1,%[x]\\n\\tsbbq $0,%[y]\""
   ":[x]\"+r\"(x),[y]\"+r\"(y)::\"cc\");" FOLD2,
   {0x8000000000000000ULL, 100}, "CarryConsume"},

  // ===== NEG CF -> SBB (CF = (src != 0)). =====
  {"neg_sbb_set",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a,y=b;"
   "__asm__ volatile(\"negq %[x]\\n\\tsbbq $0,%[y]\""
   ":[x]\"+r\"(x),[y]\"+r\"(y)::\"cc\");" FOLD2,
   {5, 100}, "CarryConsume"},
  {"neg_sbb_clr",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a,y=b;"
   "__asm__ volatile(\"negq %[x]\\n\\tsbbq $0,%[y]\""
   ":[x]\"+r\"(x),[y]\"+r\"(y)::\"cc\");" FOLD2,
   {0, 100}, "CarryConsume"},

  // ===== Direct carry manipulation -> ADC. =====
  // stc;cmc -> CF=0; clc;cmc -> CF=1.  Pins the CMC/STC/CLC handlers feeding ADC.
  {"stc_cmc_adc",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a,y=b;"
   "__asm__ volatile(\"stc\\n\\tcmc\\n\\tadcq $0,%[y]\""
   ":[y]\"+r\"(y)::\"cc\");" FOLD2,
   {7, 100}, "CarryConsume"},
  {"clc_cmc_adc",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a,y=b;"
   "__asm__ volatile(\"clc\\n\\tcmc\\n\\tadcq $0,%[y]\""
   ":[y]\"+r\"(y)::\"cc\");" FOLD2,
   {7, 100}, "CarryConsume"},

  // ===== Double-precision shift CF -> ADC. =====
  // shrd shifts y:x right; CF = last bit shifted out of x.  count=1 over x.
  {"shrd_adc",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a,y=b;"
   "__asm__ volatile(\"shrdq $1,%[hi],%[x]\\n\\tadcq $0,%[y]\""
   ":[x]\"+r\"(x),[y]\"+r\"(y):[hi]\"r\"(b):\"cc\");" FOLD2,
   {0xF, 0x1}, "CarryConsume"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(CarryConsume, X64CarryConsumeRT,
                         ::testing::ValuesIn(kX64), rtTCName);
