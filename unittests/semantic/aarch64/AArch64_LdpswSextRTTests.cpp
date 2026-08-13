//===- AArch64_LdpswSextRTTests.cpp - LDPSW signed pair load ----*- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 `LDPSW Xt1, Xt2, [Xn{, #imm}]` loads a PAIR of 32-bit words and
// SIGN-extends each into its 64-bit destination:
//
//   Xt1 = SignExtend(Mem[EA + 0]<31:0>, 64)
//   Xt2 = SignExtend(Mem[EA + 4]<31:0>, 64)
//
// It is the signed sibling of LDP (32-bit form) and the one place in the
// load-pair family where the WIDTH conversion is sign- rather than zero-extend.
// The decomposed handler forms `EA = base (+disp unless post-index)`, does two
// 4-byte LOADs at EA and EA+4, and `INT_SEXT`s each into the X destination,
// plus base writeback for the pre-/post-index forms.  Two hazards hide here:
//
//   1. The conversion MUST be sign-extend — a zero-extend would turn any word
//      with bit31 set (a negative int) into a bogus positive 64-bit value
//      (0x00000000_8xxxxxxx instead of 0xFFFFFFFF_8xxxxxxx).
//   2. The two lanes read EA and EA+4 (Xt1 is the LOW word); the #imm offset is
//      a byte displacement (multiple of 4) folded into the base, and the
//      pre-/post-index forms write the updated base back to Xn.
//
// LDPSW had no dedicated roundtrip coverage (it was only referenced as a
// "correct" sibling while fixing LDNP/STNP).  These probes seed an int[] on the
// stack — including negative words to split sign- from zero-extend head-on —
// run LDPSW in every addressing mode (base, +off, -off, pre-index!, post-index),
// and fold the loaded lanes (and the writeback delta) into the return value, so
// any extend/lane/offset/writeback error diverges from the native run.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64LdpswSextRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64LdpswSextRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {

  // ===== Sign-extend, low lane (Xt1).  buf[0] has bit31 set -> result must be
  // 0xFFFFFFFF_80000000; a zero-extend bug gives 0x00000000_80000000. =====
  {"ldpsw_sext_lo",
   "long f(long lo,long hi){ int buf[2]; buf[0]=(int)lo; buf[1]=(int)hi;"
   " long r0,r1; __asm__ volatile(\"ldpsw %0,%1,[%2]\""
   " :\"=r\"(r0),\"=r\"(r1):\"r\"(buf):\"memory\"); return r0; }\n",
   {0x80000000ULL, 0x11ULL}, "LdpswSext"},

  // Sign-extend, high lane (Xt2).  buf[1]=-1 -> 0xFFFFFFFF_FFFFFFFF.
  {"ldpsw_sext_hi",
   "long f(long lo,long hi){ int buf[2]; buf[0]=(int)lo; buf[1]=(int)hi;"
   " long r0,r1; __asm__ volatile(\"ldpsw %0,%1,[%2]\""
   " :\"=r\"(r0),\"=r\"(r1):\"r\"(buf):\"memory\"); return r1; }\n",
   {0x11ULL, 0xFFFFFFFFULL}, "LdpswSext"},

  // Both lanes negative, folded together: pins that EACH lane is sign-extended
  // (not just one) and that Xt1/Xt2 read EA / EA+4 in order.
  {"ldpsw_sext_both",
   "long f(long lo,long hi){ int buf[2]; buf[0]=(int)lo; buf[1]=(int)hi;"
   " long r0,r1; __asm__ volatile(\"ldpsw %0,%1,[%2]\""
   " :\"=r\"(r0),\"=r\"(r1):\"r\"(buf):\"memory\"); return r0*7+r1*13; }\n",
   {0x90ABCDEFULL, 0xFF000001ULL}, "LdpswSext"},

  // Positive control: bit31 clear in both -> sign- and zero-extend agree, so
  // this stays green regardless; guards the lane order / fold.
  {"ldpsw_pos_both",
   "long f(long lo,long hi){ int buf[2]; buf[0]=(int)lo; buf[1]=(int)hi;"
   " long r0,r1; __asm__ volatile(\"ldpsw %0,%1,[%2]\""
   " :\"=r\"(r0),\"=r\"(r1):\"r\"(buf):\"memory\"); return r0*7+r1*13; }\n",
   {0x7FFFFFFFULL, 0x12345678ULL}, "LdpswSext"},

  // ===== Positive byte offset [base,#8] -> reads buf[2]/buf[3]. =====
  {"ldpsw_off8",
   "long f(long lo,long hi){ int buf[4]; buf[2]=(int)lo; buf[3]=(int)hi;"
   " long r0,r1; __asm__ volatile(\"ldpsw %0,%1,[%2,#8]\""
   " :\"=r\"(r0),\"=r\"(r1):\"r\"(buf):\"memory\"); return r0*7+r1*13; }\n",
   {0x80000005ULL, 0x40ULL}, "LdpswSext"},

  // Negative byte offset [p,#-8] with p=&buf[2] -> reads buf[0]/buf[1]; pins
  // the signed displacement.
  {"ldpsw_offneg8",
   "long f(long lo,long hi){ int buf[4]; buf[0]=(int)lo; buf[1]=(int)hi;"
   " long r0,r1; int *p=&buf[2]; __asm__ volatile(\"ldpsw %0,%1,[%2,#-8]\""
   " :\"=r\"(r0),\"=r\"(r1):\"r\"(p):\"memory\"); return r0*7+r1*13; }\n",
   {0xFF800000ULL, 0x55ULL}, "LdpswSext"},

  // ===== Pre-index [base,#8]! : EA=base+8 (reads buf[2]/buf[3]) AND base+=8.
  // Folds the writeback delta (must be 8) with both loaded lanes. =====
  {"ldpsw_preidx",
   "long f(long lo,long hi){ int buf[4]; buf[2]=(int)lo; buf[3]=(int)hi;"
   " long r0,r1,base=(long)buf;"
   " __asm__ volatile(\"ldpsw %0,%1,[%2,#8]!\""
   " :\"=r\"(r0),\"=r\"(r1),\"+r\"(base)::\"memory\");"
   " return (base-(long)buf) + r0 + r1; }\n",
   {0x88000000ULL, 0x99000000ULL}, "LdpswSext"},

  // Post-index [base],#8 : EA=base (reads buf[0]/buf[1]) THEN base+=8.  Folds
  // the writeback delta with both lanes.
  {"ldpsw_postidx",
   "long f(long lo,long hi){ int buf[4]; buf[0]=(int)lo; buf[1]=(int)hi;"
   " long r0,r1,base=(long)buf;"
   " __asm__ volatile(\"ldpsw %0,%1,[%2],#8\""
   " :\"=r\"(r0),\"=r\"(r1),\"+r\"(base)::\"memory\");"
   " return (base-(long)buf) + r0 + r1; }\n",
   {0x80000001ULL, 0x7FFFFFFFULL}, "LdpswSext"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(LdpswSext, A64LdpswSextRT,
                         ::testing::ValuesIn(kA64), rtTCName);
