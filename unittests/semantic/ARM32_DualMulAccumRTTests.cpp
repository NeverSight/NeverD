//===- ARM32_DualMulAccumRTTests.cpp - SMLALD/SMLSLD 64-bit MAC -*- C++ -*-===//
//
// ARM32 signed dual multiply-accumulate LONG instructions accumulate the sum
// (SMLALD) or difference (SMLSLD) of two signed 16x16 products into a 64-bit
// {RdHi:RdLo} register pair:
//
//   SMLALD{X}  {RdHi:RdLo} += SInt(Rn.lo)*SInt(Rm.lo) + SInt(Rn.hi)*SInt(Rm.hi)
//   SMLSLD{X}  {RdHi:RdLo} += SInt(Rn.lo)*SInt(Rm.lo) - SInt(Rn.hi)*SInt(Rm.hi)
//   (the X variant swaps Rm's halves before the two products)
//
// ARM ARM computes `product1 (+/-) product2` in FULL precision before the
// 64-bit accumulate.  Each signed 16x16 product is at most 0x8000*0x8000 =
// 2^30, so for SMLALD their SUM can reach 2^31 — which overflows a signed
// 32-bit intermediate.  The lifter summed the two products in a 4-byte temp and
// only THEN sign-extended to 64 bits, so when both products are 2^30 the 4-byte
// sum wrapped to -2^31 and sign-extended to 0xFFFFFFFF80000000 (off by 2^32):
// the low word stayed correct but RdHi came out as 0xFFFFFFFF / one less than
// the true high word.  The single-product sibling SMLALBB already widens to 64
// bits before multiplying — the dual-product form was the gap.  These had ZERO
// prior roundtrip coverage (another weak-test gap).
//
// The only SMLALD overflow point is product1==product2==2^30, i.e. all four
// 16-bit halves == 0x8000, so Rn==Rm==0x80008000.  SMLSLD's difference is
// bounded to +/-2147450880 (fits int32) and cannot overflow; its probes are
// guardrails for the (consistency) widening of the same idiom.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32DualMulAccumRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32DualMulAccumRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kARM32DualMulAccum = {

  // ===== SMLALD: dual-product SUM overflows int32 (RED before fix). =====
  // Rn=Rm=0x80008000 -> both products = (-32768)*(-32768) = 2^30, sum = 2^31.
  // {RdHi:RdLo}=0 -> result = 0x0000000080000000: RdHi=0 (buggy lift: 0xFFFFFFFF).
  {"smlald_ovf_hi",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"smlald %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return hi;}\n",
   {0, 0, 0x80008000ULL, 0x80008000ULL}, "DualMulAccum"},

  // SMLALDX: X variant swaps Rm halves; with Rn=Rm=0x80008000 both products are
  // still 2^30, so the sum still overflows.  RED before fix.
  {"smlaldx_ovf_hi",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"smlaldx %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return hi;}\n",
   {0, 0, 0x80008000ULL, 0x80008000ULL}, "DualMulAccum"},

  // SMLALD overflow with a non-zero high accumulator: result = (5<<32)+2^31,
  // RdHi=5 (buggy lift subtracts the spurious borrow -> RdHi=4).  RED before fix.
  {"smlald_ovf_acc_hi",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"smlald %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return hi;}\n",
   {0, 5, 0x80008000ULL, 0x80008000ULL}, "DualMulAccum"},

  // ===== Controls: low word is correct even with the bug (sum mod 2^32). =====
  {"smlald_ovf_lo",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"smlald %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return lo;}\n",
   {0, 0, 0x80008000ULL, 0x80008000ULL}, "DualMulAccum"},

  // 64-bit carry guardrail: small products (sum=2, no dual-sum overflow) plus a
  // low accumulator that carries into RdHi.  RdLo=0xFFFFFFFF -> RdHi=1.
  {"smlald_carry_hi",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"smlald %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return hi;}\n",
   {0xFFFFFFFFULL, 0, 0x00010001ULL, 0x00010001ULL}, "DualMulAccum"},

  // Small mixed-sign control: RdHi result with normal values (no overflow).
  {"smlald_small_lo",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"smlald %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return lo;}\n",
   {100, 0, 0xFFFF0003ULL, 0x00020005ULL}, "DualMulAccum"},

  // ===== SMLSLD: difference is bounded to +/-2147450880 (fits int32) — these
  // are guardrails confirming the (consistency) 64-bit widening is correct. =====
  // Rn=0x80008000 (lo=hi=-32768), Rm=0x7FFF8000 (lo=-32768, hi=32767):
  //   product1 = (-32768)*(-32768) = 2^30; product2 = (-32768)*32767;
  //   diff = 2147450880 = 0x7FFF8000 -> RdLo=0x7FFF8000, RdHi=0.
  {"smlsld_diffmax_lo",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"smlsld %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return lo;}\n",
   {0, 0, 0x80008000ULL, 0x7FFF8000ULL}, "DualMulAccum"},

  {"smlsld_diffmax_hi",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"smlsld %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return hi;}\n",
   {0, 0, 0x80008000ULL, 0x7FFF8000ULL}, "DualMulAccum"},

  // SMLSLDX guardrail (cross, negative difference accumulated).
  {"smlsldx_lo",
   "unsigned f(unsigned lo,unsigned hi,unsigned n,unsigned m){"
   "__asm__ volatile(\"smlsldx %0,%1,%2,%3\":\"+r\"(lo),\"+r\"(hi):\"r\"(n),\"r\"(m):);"
   "return lo;}\n",
   {0, 0, 0x00037FFFULL, 0x80000002ULL}, "DualMulAccum"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(DualMulAccum, ARM32DualMulAccumRT,
                         ::testing::ValuesIn(kARM32DualMulAccum), rtTCName);
