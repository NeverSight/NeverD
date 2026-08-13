//===- ARM32_UmaalDoubleAccRTTests.cpp - UMAAL double-accumulate -*- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// ARM32 `UMAAL RdLo, RdHi, Rn, Rm` (unsigned multiply-accumulate-accumulate
// long) is the odd one out of the long-multiply family.  Unlike UMLAL — which
// adds the *64-bit* pair {RdHi:RdLo} to the product — UMAAL adds RdLo and RdHi
// as TWO INDEPENDENT zero-extended 32-bit addends:
//
//   result = UInt(Rn) * UInt(Rm) + UInt(RdLo) + UInt(RdHi)   // all zext to 64
//   RdHi   = result<63:32>;  RdLo = result<31:0>;
//
// Because each addend is at most 2^32-1, the total maxes out at exactly
// (2^32-1)^2 + 2*(2^32-1) == 2^64-1, so UMAAL can never overflow 64 bits and
// sets no flags.  The trap a lifter falls into is reusing the UMLAL idiom
// (OR RdHi<<32 with RdLo into one 64-bit accumulator): that treats RdHi as the
// HIGH word instead of a second low addend, which is only invisible when
// RdHi==0.  Driving RdHi!=0 splits the two semantics apart head-on.  UMAAL had
// ZERO prior roundtrip coverage — a weak-test gap on a genuinely cold opcode.
//
// `f(unsigned lo, unsigned hi, unsigned n, unsigned m)` seeds RdLo/RdHi/Rn/Rm
// from R0..R3; each probe returns either the resulting RdLo or RdHi half.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32UmaalDoubleAccRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32UmaalDoubleAccRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kARM32Umaal = {

  // ===== Distinguish UMAAL (two 32-bit addends) from a UMLAL-style 64-bit
  // pair accumulate.  Rn=Rm=0, RdLo=0, RdHi=0xFFFFFFFF:
  //   UMAAL : 0 + 0 + 0xFFFFFFFF        = 0x00000000_FFFFFFFF
  //   UMLAL : 0 + (0xFFFFFFFF<<32 | 0)  = 0xFFFFFFFF_00000000  (wrong)
  // RdLo: correct 0xFFFFFFFF vs buggy 0. =====
  {"umaal_pair_lo",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"umaal %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return lo;}\n",
   {0, 0xFFFFFFFFULL, 0, 0}, "Umaal"},

  // Same seed, return RdHi: correct 0 vs buggy 0xFFFFFFFF.
  {"umaal_pair_hi",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"umaal %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return hi;}\n",
   {0, 0xFFFFFFFFULL, 0, 0}, "Umaal"},

  // ===== No-overflow upper bound: every input 0xFFFFFFFF gives exactly 2^64-1.
  //   (2^32-1)^2 + (2^32-1) + (2^32-1) = 2^64-1 -> RdLo=RdHi=0xFFFFFFFF. =====
  {"umaal_max_lo",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"umaal %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return lo;}\n",
   {0xFFFFFFFFULL, 0xFFFFFFFFULL, 0xFFFFFFFFULL, 0xFFFFFFFFULL}, "Umaal"},

  {"umaal_max_hi",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"umaal %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return hi;}\n",
   {0xFFFFFFFFULL, 0xFFFFFFFFULL, 0xFFFFFFFFULL, 0xFFFFFFFFULL}, "Umaal"},

  // ===== Carry from the RdLo addend into RdHi.  Rn=2,Rm=3 (prod=6),
  // RdLo=0xFFFFFFFF, RdHi=0 -> 6 + 0xFFFFFFFF = 0x1_00000005 -> RdLo=5,RdHi=1. =====
  {"umaal_carry_lo",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"umaal %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return lo;}\n",
   {0xFFFFFFFFULL, 0, 2, 3}, "Umaal"},

  {"umaal_carry_hi",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"umaal %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return hi;}\n",
   {0xFFFFFFFFULL, 0, 2, 3}, "Umaal"},

  // ===== Both addends sum past 2^32 with a zero product: confirms BOTH RdLo
  // and RdHi are added (not just one).  Rn=Rm=0, RdLo=RdHi=0xFFFFFFFF ->
  // 0x1_FFFFFFFE -> RdLo=0xFFFFFFFE, RdHi=1.  (UMLAL bug -> RdLo=0xFFFFFFFF.) =====
  {"umaal_accsum_lo",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"umaal %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return lo;}\n",
   {0xFFFFFFFFULL, 0xFFFFFFFFULL, 0, 0}, "Umaal"},

  {"umaal_accsum_hi",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"umaal %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return hi;}\n",
   {0xFFFFFFFFULL, 0xFFFFFFFFULL, 0, 0}, "Umaal"},

  // ===== Product high word: Rn=Rm=0x10000 -> prod=2^32, RdLo=RdHi=0 ->
  // RdLo=0, RdHi=1.  Pins that the product's high half lands in RdHi. =====
  {"umaal_prodhi_hi",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"umaal %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return hi;}\n",
   {0, 0, 0x10000ULL, 0x10000ULL}, "Umaal"},

  // ===== Mixed addends, no carry: Rn=Rm=0, RdLo=0x12345678, RdHi=0xABCDEF00 ->
  // 0xBE024578 -> RdLo=0xBE024578, RdHi=0.  Both addends fold into the low word
  // (UMLAL bug would keep RdLo=0x12345678 / RdHi=0xABCDEF00). =====
  {"umaal_mixed_lo",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"umaal %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return lo;}\n",
   {0x12345678ULL, 0xABCDEF00ULL, 0, 0}, "Umaal"},

  // General mix with a real product + both addends + carry exercised together.
  // Rn=0xDEADBEEF, Rm=0x12345 -> prod=0xFD05_7E1C_DBEB (>32 bits), plus
  // RdLo=0x1000, RdHi=0x2000; lifted vs native must agree on both halves.
  {"umaal_general_lo",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"umaal %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return lo;}\n",
   {0x1000ULL, 0x2000ULL, 0xDEADBEEFULL, 0x12345ULL}, "Umaal"},

  {"umaal_general_hi",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"umaal %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return hi;}\n",
   {0x1000ULL, 0x2000ULL, 0xDEADBEEFULL, 0x12345ULL}, "Umaal"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(Umaal, ARM32UmaalDoubleAccRT,
                         ::testing::ValuesIn(kARM32Umaal), rtTCName);
