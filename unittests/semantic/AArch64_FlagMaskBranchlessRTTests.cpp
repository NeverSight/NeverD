//===- AArch64_FlagMaskBranchlessRTTests.cpp - flag-as-value idioms -*- C++ -==//
//
// The AArch64 analog of the x86 flag-as-value probes: a condition flag is
// materialized as a numeric *mask/value* and fed into arithmetic, rather than
// read into a branch.  Compilers emit these branchless idioms:
//   * `subs xzr,a,b ; sbc rd,xzr,xzr`  -> 0/-1 borrow mask (CF as a value)
//   * `subs xzr,a,b ; ngc rd,xzr`      -> same via the NGC alias
//   * `csetm`/`cset` -> mask -> AND/OR select
//   * `adc rd,rd,rd`                   -> 2*rd + C
//   * `cinc`/`csinc` accumulate
// SBC/NGC route the carry as a *value* (distinct from the MedFlags condition
// fold); a dropped or mis-versioned C flag diverges from the native run.  Each
// probe folds the produced value into the integer return register.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64FlagMaskRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FlagMaskRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // ===== sbc borrow mask: subs sets C=(a>=b unsigned); sbc xzr,xzr = C-1. =====
  // a<b  -> C=0 -> mask=-1 ; a>=b -> C=1 -> mask=0.
  {"sbc_mask_lt",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long m;"
   "__asm__ volatile(\"subs xzr,%1,%2\\n\\tsbc %0,xzr,xzr\":\"=r\"(m):\"r\"(a),\"r\"(b):\"cc\");"
   "return m;}\n",
   {5, 9}, "A64FlagMask"},
  {"sbc_mask_ge",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long m;"
   "__asm__ volatile(\"subs xzr,%1,%2\\n\\tsbc %0,xzr,xzr\":\"=r\"(m):\"r\"(a),\"r\"(b):\"cc\");"
   "return m;}\n",
   {9, 5}, "A64FlagMask"},
  // ngc alias: same borrow mask.
  {"ngc_mask_lt",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long m;"
   "__asm__ volatile(\"subs xzr,%1,%2\\n\\tngc %0,xzr\":\"=r\"(m):\"r\"(a),\"r\"(b):\"cc\");"
   "return m;}\n",
   {5, 9}, "A64FlagMask"},
  // Branchless unsigned min via mask: (a&m)|(b&~m), m=-(a<b).
  {"sbc_mask_select",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long m;"
   "__asm__ volatile(\"subs xzr,%1,%2\\n\\tsbc %0,xzr,xzr\":\"=r\"(m):\"r\"(a),\"r\"(b):\"cc\");"
   "return (a&m)|(b&~m);}\n",
   {5, 9}, "A64FlagMask"},
  // 32-bit (Wd) borrow mask.
  {"sbc_mask_w",
   "unsigned f(unsigned a,unsigned b){unsigned m;"
   "__asm__ volatile(\"subs wzr,%w1,%w2\\n\\tsbc %w0,wzr,wzr\":\"=r\"(m):\"r\"(a),\"r\"(b):\"cc\");"
   "return m;}\n",
   {5, 9}, "A64FlagMask"},

  // ===== adc rd,rd,rd : 2*rd + C. =====
  {"adc_double_c1",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a;"
   "__asm__ volatile(\"cmp xzr,xzr\\n\\tadc %0,%0,%0\":\"+r\"(x)::\"cc\");"
   "return x;}\n",
   {0x40, 0}, "A64FlagMask"},

  // ===== csetm mask then AND-select. =====
  {"csetm_and_select",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long m;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tcsetm %0,lo\":\"=r\"(m):\"r\"(a),\"r\"(b):\"cc\");"
   "return (a&m)|(b&~m);}\n",
   {9, 5}, "A64FlagMask"},

  // ===== predicated accumulate via cinc in a loop (loop-carried + cond). =====
  {"cinc_loop_count",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long cnt=0;"
   "for(unsigned long i=0;i<64;i++){unsigned long bit=(a>>i)&1;"
   "__asm__ volatile(\"cmp %1,#0\\n\\tcinc %0,%0,ne\":\"+r\"(cnt):\"r\"(bit):\"cc\");}"
   "return cnt+b;}\n",
   {0xA5A5A5A5A5A5A5A5ULL, 7}, "A64FlagMask"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(A64FlagMask, A64FlagMaskRT, ::testing::ValuesIn(kA64),
                         rtTCName);
