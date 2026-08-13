//===- ARM32_ExtendRotRTTests.cpp - extend-with-ROR roundtrip ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// ARM32 sign/zero-extend instructions take an optional ROR #8/16/24 on the
// source that rotates the whole register BEFORE the byte/halfword is extracted:
//   SXTB/UXTB/SXTH/UXTH         Rd      = extend(rot(Rm)[low])
//   SXTAB/UXTAB/SXTAH/UXTAH     Rd      = Rn + extend(rot(Rm)[low])
//   SXTB16/UXTB16               Rd      = extend(rot(Rm)[7:0])  : extend(rot(Rm)[23:16])
//   SXTAB16/UXTAB16             Rd      = packed-halfword-add of the above to Rn
//
// The plain (no-rotation) forms are covered elsewhere; the ROR variants exercise
// the operandRead barrel-shifter path feeding the byte/halfword extraction (and,
// for the *16 forms, the byte[0]/byte[2] split after rotation).  Inputs are
// chosen so each rotation selects a distinct, identifiable byte.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32ExtendRotRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ExtendRotRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kARM32 = {
  // sxtb with ror #8: byte[1] (0x80) sign-extended -> 0xFFFFFF80.
  {"sxtb_ror8",
   "int f(int a){int r;"
   "__asm__ volatile(\"sxtb %0,%1,ror #8\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x11228033u}, "ExtRot"},

  // uxtb with ror #16: byte[2] (0x55) zero-extended -> 0x55.
  {"uxtb_ror16",
   "int f(int a){int r;"
   "__asm__ volatile(\"uxtb %0,%1,ror #16\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x11550033u}, "ExtRot"},

  // sxth with ror #16: halfword[1] (0x8001) sign-extended -> 0xFFFF8001.
  {"sxth_ror16",
   "int f(int a){int r;"
   "__asm__ volatile(\"sxth %0,%1,ror #16\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x80010000u}, "ExtRot"},

  // uxth with ror #8: bits[23:8] (0xAB12) zero-extended -> 0xAB12.
  {"uxth_ror8",
   "int f(int a){int r;"
   "__asm__ volatile(\"uxth %0,%1,ror #8\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x00AB1200u}, "ExtRot"},

  // sxtab with ror #24: byte[3] (0x80) sign-extended (-128) + base.
  {"sxtab_ror24",
   "int f(int a,int b){int r;"
   "__asm__ volatile(\"sxtab %0,%1,%2,ror #24\":\"=r\"(r):\"r\"(b),\"r\"(a));"
   "return r;}\n",
   {0x80112233u, 1000}, "ExtRot"},

  // uxtah with ror #16: halfword[1] (0xFFFF) zero-extended + base.
  {"uxtah_ror16",
   "int f(int a,int b){int r;"
   "__asm__ volatile(\"uxtah %0,%1,%2,ror #16\":\"=r\"(r):\"r\"(b),\"r\"(a));"
   "return r;}\n",
   {0xFFFF0000u, 100}, "ExtRot"},

  // sxtb16 with ror #8: rot(Rm)[7:0]=byte[1], rot(Rm)[23:16]=byte[3];
  // sign-extend each into its halfword.  Rm=0x80112280 -> b1=0x22, b3=0x80
  // -> halfword0=0x0022, halfword1=0xFF80 -> 0xFF800022.
  {"sxtb16_ror8",
   "int f(int a){int r;"
   "__asm__ volatile(\"sxtb16 %0,%1,ror #8\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x80112280u}, "ExtRot"},

  // uxtb16 with ror #16: rot[7:0]=byte[2], rot[23:16]=byte[0].
  // Rm=0xAA00BB00 -> byte2=0xAA, byte0=0x00 -> halfword0=0x00AA, halfword1=0.
  {"uxtb16_ror16",
   "int f(int a){int r;"
   "__asm__ volatile(\"uxtb16 %0,%1,ror #16\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0xAA00BB00u}, "ExtRot"},

  // sxtab16 with ror #8: packed halfword add of sign-extended bytes to base.
  {"sxtab16_ror8",
   "int f(int a,int b){int r;"
   "__asm__ volatile(\"sxtab16 %0,%1,%2,ror #8\":\"=r\"(r):\"r\"(b),\"r\"(a));"
   "return r;}\n",
   {0x017F0180u, 0x00100010u}, "ExtRot"},

  // uxtab16 with ror #24: rot #24 selects byte[3]->low, byte[1]->high.
  {"uxtab16_ror24",
   "int f(int a,int b){int r;"
   "__asm__ volatile(\"uxtab16 %0,%1,%2,ror #24\":\"=r\"(r):\"r\"(b),\"r\"(a));"
   "return r;}\n",
   {0xAA00BB00u, 0x00010001u}, "ExtRot"},

  // Control: plain sxtb (no rotation) must still match.
  {"sxtb_noror",
   "int f(int a){int r;"
   "__asm__ volatile(\"sxtb %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x112233F0u}, "ExtRot"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ExtRot, ARM32ExtendRotRT,
                         ::testing::ValuesIn(kARM32), rtTCName);
