//===- AArch64_NegCarryRTTests.cpp - NGC / NGCS negate-with-carry -*- C++-*-=//
//
// AArch64 `NGC Xd, Xm` / `NGCS Xd, Xm` are aliases of `SBC Xd, XZR, Xm` /
// `SBCS ...`: Xd = 0 - Xm - NOT(C) = -Xm - 1 + C.  capstone decodes them as the
// distinct mnemonics NGC/NGCS (not normalized to SBC), and there is no dedicated
// NGC handler — they ride the generic SBC-with-XZR path.  That path had ZERO
// roundtrip coverage, and the result flips on the incoming carry (C=1 -> -Xm,
// C=0 -> -Xm-1), so a mishandled borrow is invisible without driving BOTH carry
// states head-on.  Each probe sets a known carry with `subs xzr,a,b`
// (C = a>=b unsigned), runs NGC/NGCS on b, and folds the result; the NGCS form
// additionally writes NZCV, read back via `cset`.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64NegCarryRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64NegCarryRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // NGC, carry IN = 1 (a>=b): r = -b.
  {"ngc_c1",
   "long f(long a,long b){ long r;\n"
   "  __asm__ volatile(\"subs xzr,%1,%2\\n\\tngc %0,%2\\n\\t\"\n"
   "    :\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\"); return r;}\n",
   {100, 50}, "NegCarry"},

  // NGC, carry IN = 0 (a<b): r = -b-1.
  {"ngc_c0",
   "long f(long a,long b){ long r;\n"
   "  __asm__ volatile(\"subs xzr,%1,%2\\n\\tngc %0,%2\\n\\t\"\n"
   "    :\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\"); return r;}\n",
   {50, 100}, "NegCarry"},

  // NGCS (sets NZCV); fold result + N flag (via cset mi).
  {"ngcs_c1",
   "long f(long a,long b){ long r,n;\n"
   "  __asm__ volatile(\"subs xzr,%2,%3\\n\\tngcs %0,%3\\n\\tcset %1,mi\\n\\t\"\n"
   "    :\"=r\"(r),\"=r\"(n):\"r\"(a),\"r\"(b):\"cc\"); return r*4+n;}\n",
   {200, 30}, "NegCarry"},

  {"ngcs_c0",
   "long f(long a,long b){ long r,n;\n"
   "  __asm__ volatile(\"subs xzr,%2,%3\\n\\tngcs %0,%3\\n\\tcset %1,mi\\n\\t\"\n"
   "    :\"=r\"(r),\"=r\"(n):\"r\"(a),\"r\"(b):\"cc\"); return r*4+n;}\n",
   {30, 200}, "NegCarry"},

  // NGCS with b==0: -0-1+C -> C=1 gives 0 (Z set), C=0 gives -1.
  {"ngcs_zero_c1",
   "long f(long a,long b){ long r,z;\n"
   "  __asm__ volatile(\"subs xzr,%2,%3\\n\\tngcs %0,%3\\n\\tcset %1,eq\\n\\t\"\n"
   "    :\"=r\"(r),\"=r\"(z):\"r\"(a),\"r\"(b):\"cc\"); return r*4+z;}\n",
   {5, 0}, "NegCarry"},

  // 32-bit (Wd) form.
  {"ngc_w_c0",
   "long f(long a,long b){ unsigned r;\n"
   "  __asm__ volatile(\"subs wzr,%w1,%w2\\n\\tngc %w0,%w2\\n\\t\"\n"
   "    :\"=r\"(r):\"r\"((unsigned)a),\"r\"((unsigned)b):\"cc\");\n"
   "  return (long)(unsigned)r;}\n",
   {7, 99}, "NegCarry"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(NegCarry, A64NegCarryRT,
                         ::testing::ValuesIn(kA64), rtTCName);
