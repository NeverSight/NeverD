//===- ARM32_FlagMaskBranchlessRTTests.cpp - flag-as-value idioms -*- C++ -==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// ARM32 analog of the x86/AArch64 flag-as-value probes plus ARM's distinctive
// *conditional execution* (predicated ALU ops).  A condition is materialized as
// a value/mask or drives predicated arithmetic, rather than a branch:
//   * `subs rt,a,b ; sbc rd,rd,rd`  -> 0/-1 borrow mask (C as a value)
//   * `cmp a,b ; addlt/subge`       -> predicated add/sub (cond drives the op)
//   * `cmp a,b ; movlt/movge`       -> predicated mov (mask/select)
//   * `adc rd,rd,rd`                -> 2*rd + C
// Predicated ops and the SBC carry-as-value path are a separate lift surface
// from the MedFlags condition fold; a dropped predicate or mis-versioned C
// diverges from the native run.  Returns fold into R0 (low 32 bits).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32FlagMaskRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FlagMaskRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kARM32 = {
  // ===== sbc borrow mask: subs sets C=(a>=b unsigned); sbc rd,rd,rd = C-1. =====
  {"sbc_mask_lt",
   "int f(int a,int b){int m;"
   "__asm__ volatile(\"subs r4,%1,%2\\n\\tsbc %0,%0,%0\":\"=r\"(m):\"r\"(a),\"r\"(b):\"r4\",\"cc\");"
   "return m;}\n",
   {5, 9}, "ARM32FlagMask"},
  {"sbc_mask_ge",
   "int f(int a,int b){int m;"
   "__asm__ volatile(\"subs r4,%1,%2\\n\\tsbc %0,%0,%0\":\"=r\"(m):\"r\"(a),\"r\"(b):\"r4\",\"cc\");"
   "return m;}\n",
   {9, 5}, "ARM32FlagMask"},
  // Branchless unsigned min via mask.
  {"sbc_mask_select",
   "int f(int a,int b){int m;"
   "__asm__ volatile(\"subs r4,%1,%2\\n\\tsbc %0,%0,%0\":\"=r\"(m):\"r\"(a),\"r\"(b):\"r4\",\"cc\");"
   "return (a&m)|(b&~m);}\n",
   {5, 9}, "ARM32FlagMask"},

  // ===== Predicated ADD/SUB: cond ? a+100 : a-100. =====
  {"pred_addsub_lt",
   "int f(int a,int b){int r=a;"
   "__asm__ volatile(\"cmp %1,%2\\n\\taddlt %0,%0,#100\\n\\tsubge %0,%0,#100\""
   ":\"+r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {3, 9}, "ARM32FlagMask"},
  {"pred_addsub_ge",
   "int f(int a,int b){int r=a;"
   "__asm__ volatile(\"cmp %1,%2\\n\\taddlt %0,%0,#100\\n\\tsubge %0,%0,#100\""
   ":\"+r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {9, 3}, "ARM32FlagMask"},

  // ===== Predicated MOV select (mask = cond ? 1 : 0 form). =====
  {"pred_mov_sel",
   "int f(int a,int b){int r;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tmovlt %0,%1\\n\\tmovge %0,%2\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {3, 9}, "ARM32FlagMask"},

  // ===== Predicated ADD with a register operand in a loop (loop-carried). =====
  {"pred_loop_acc",
   "int f(int a,int b){int acc=0;"
   "for(int i=0;i<32;i++){int bit=(a>>i)&1;"
   "__asm__ volatile(\"cmp %1,#0\\n\\taddne %0,%0,%2\""
   ":\"+r\"(acc):\"r\"(bit),\"r\"(b):\"cc\");}"
   "return acc;}\n",
   {0xA5A5A5A5, 7}, "ARM32FlagMask"},

  // ===== adc rd,rd,rd : 2*rd + C.  adds of a high-bit value sets C=1. =====
  {"adc_double_c1",
   "int f(int a){int x=a;"
   "__asm__ volatile(\"adds r4,%1,%1\\n\\tadc %0,%0,%0\""
   ":\"+r\"(x):\"r\"(a):\"r4\",\"cc\");"
   "return x;}\n",
   {(uint64_t)0x90000000U}, "ARM32FlagMask"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ARM32FlagMask, ARM32FlagMaskRT,
                         ::testing::ValuesIn(kARM32), rtTCName);
