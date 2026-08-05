//===- ARM32_SMMulWordRTTests.cpp - SMMUL/SMMLA/SMMLS word mul -*- C++ -*-===//
//
// ARM32 signed most-significant-word multiplies compute the high 32 bits of a
// 64-bit intermediate:
//   SMMUL{R}  Rd = (Rn*Rm{ + 0x80000000 })[63:32]
//   SMMLA{R}  Rd = ((Ra<<32) + Rn*Rm{ + 0x80000000 })[63:32]
//   SMMLS{R}  Rd = ((Ra<<32) - Rn*Rm{ + 0x80000000 })[63:32]
//
// The old lifter approximated these on the high word alone:
//   * SMMLS as `Ra - prodHi`, dropping the BORROW the low half generates into
//     bit 32 (`(Ra<<32) - prod` borrows whenever the product's low word != 0).
//   * the R (round) variants ignored the +0x80000000 entirely, dropping the
//     rounding carry into bit 32.
//
// Every prior probe multiplied 0x40000000*0x40000000, whose product's low word
// is exactly 0 — masking both the borrow and the rounding carry (a textbook
// weak-test).  These probes use products with a non-zero low word (and, for the
// rounding variants, a low word > 0x80000000 so the rounding carry does not
// coincidentally cancel the borrow).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32SMMulWordRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32SMMulWordRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kARM32 = {
  // ===== SMMUL: high word of a product whose low word is non-zero. =====
  // 0x00010001^2 = 0x100020001 -> [63:32] = 1.
  {"smmul_lowbits",
   "int f(int a,int b){int r;"
   "__asm__ volatile(\"smmul %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b));"
   "return r;}\n",
   {0x00010001, 0x00010001}, "SMMulWord"},

  // ===== SMMLS: borrow into bit 32 (prodLo != 0). =====
  // 0x0001FFFF^2 = 0x3FFFC0001; ((20<<32) - that)[63:32] = 16 (old `20-3`=17).
  {"smmls_borrow",
   "int f(int a,int b,int c){int r;"
   "__asm__ volatile(\"smmls %0,%1,%2,%3\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c));"
   "return r;}\n",
   {0x0001FFFF, 0x0001FFFF, 20}, "SMMulWord"},

  // ===== SMMLA: control (low word 0 -> no carry; both forms agree). =====
  {"smmla_clean",
   "int f(int a,int b,int c){int r;"
   "__asm__ volatile(\"smmla %0,%1,%2,%3\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c));"
   "return r;}\n",
   {0x40000000, 0x40000000, 10}, "SMMulWord"},

  // ===== SMMLA: non-zero low word (accumulate adds 0 to low half -> still no
  // carry, exercises the full-width path). =====
  {"smmla_lowbits",
   "int f(int a,int b,int c){int r;"
   "__asm__ volatile(\"smmla %0,%1,%2,%3\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c));"
   "return r;}\n",
   {0x00010001, 0x00010001, 10}, "SMMulWord"},

  // ===== SMMULR: rounding carry (prodLo = 0x80000000 -> +round overflows to the
  // high word).  0x10000*0x18000 = 0x180000000; rounded -> 0x200000000 -> 2. =====
  {"smmulr_round",
   "int f(int a,int b){int r;"
   "__asm__ volatile(\"smmulr %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b));"
   "return r;}\n",
   {0x00010000, 0x00018000}, "SMMulWord"},

  // ===== SMMLAR: rounding carry + accumulate. =====
  // ((10<<32) + 0x180000000 + 0x80000000)[63:32] = 12 (old, no round, = 11).
  {"smmlar_round",
   "int f(int a,int b,int c){int r;"
   "__asm__ volatile(\"smmlar %0,%1,%2,%3\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c));"
   "return r;}\n",
   {0x00010000, 0x00018000, 10}, "SMMulWord"},

  // ===== SMMLSR: rounding + borrow (prodLo = 0xFFFC0001 > 0x80000000 so the
  // rounding carry does NOT cancel the borrow). =====
  // ((20<<32) - 0x3FFFC0001 + 0x80000000)[63:32] = 16 (old `20-3` = 17).
  {"smmlsr_round_borrow",
   "int f(int a,int b,int c){int r;"
   "__asm__ volatile(\"smmlsr %0,%1,%2,%3\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c));"
   "return r;}\n",
   {0x0001FFFF, 0x0001FFFF, 20}, "SMMulWord"},

  // ===== Signed product: negative operand exercises sign extension into the
  // high word and the borrow path. =====
  {"smmls_negative",
   "int f(int a,int b,int c){int r;"
   "__asm__ volatile(\"smmls %0,%1,%2,%3\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c));"
   "return r;}\n",
   {(uint64_t)(int)0xFFFE0001, 0x00020001, 7}, "SMMulWord"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(SMMulWord, ARM32SMMulWordRT,
                         ::testing::ValuesIn(kARM32), rtTCName);
