//===- AArch64_CondSelectAliasRTTests.cpp - CINC/CINV/CNEG/CSET ---*- C++ -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 conditional-select PSEUDO-aliases that Capstone 6 resolves back to the
// canonical CSINC/CSINV/CSNEG base forms:
//
//   CINC  Rd,Rn,cond  == CSINC Rd,Rn,Rn,invert(cond)  -> cond ? Rn+1 : Rn
//   CINV  Rd,Rn,cond  == CSINV Rd,Rn,Rn,invert(cond)  -> cond ? ~Rn  : Rn
//   CNEG  Rd,Rn,cond  == CSNEG Rd,Rn,Rn,invert(cond)  -> cond ? -Rn  : Rn
//   CSET  Rd,cond     == CSINC Rd,WZR,WZR,invert(cond) -> cond ? 1  : 0
//   CSETM Rd,cond     == CSINV Rd,WZR,WZR,invert(cond) -> cond ? -1 : 0
//
// Only the *canonical* CSEL/CSINC/CSINV/CSNEG and CSET were under roundtrip
// coverage (AllPlatform_CondLoopRTTests / AArch64_SemanticTests).  The
// alias forms drive a separate path in the lifter (`is_alias && op_count==2`
// for CINC/CINV/CNEG, `op_count==1` for CSET/CSETM) whose correctness hinges on
// whether Capstone reports the *alias-level* condition (un-inverted) — if it
// instead surfaced the encoded base condition, the alias path would pick the
// wrong arm.  These probe both the true and false arm of every alias, plus the
// canonical CSNEG negate-INT_MIN wrap edge as a guardrail.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64CondSelectAliasRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CondSelectAliasRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64CondSelAlias = {

  // ===== CINC: cond ? Rn+1 : Rn (EQ true / false arms). =====
  {"cinc_eq_true",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){unsigned long r;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tcinc %0,%3,eq\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c):\"cc\");"
   "return r;}\n",
   {5, 5, 100}, "CondSelAlias"},

  {"cinc_eq_false",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){unsigned long r;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tcinc %0,%3,eq\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c):\"cc\");"
   "return r;}\n",
   {5, 6, 100}, "CondSelAlias"},

  // ===== CINV: cond ? ~Rn : Rn (NE true / false arms). =====
  {"cinv_ne_true",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){unsigned long r;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tcinv %0,%3,ne\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c):\"cc\");"
   "return r;}\n",
   {5, 6, 0xF0ULL}, "CondSelAlias"},

  {"cinv_ne_false",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){unsigned long r;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tcinv %0,%3,ne\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c):\"cc\");"
   "return r;}\n",
   {5, 5, 0xF0ULL}, "CondSelAlias"},

  // ===== CNEG: cond ? -Rn : Rn (LT true / false arms). =====
  {"cneg_lt_true",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){unsigned long r;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tcneg %0,%3,lt\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c):\"cc\");"
   "return r;}\n",
   {3, 9, 7}, "CondSelAlias"},

  {"cneg_lt_false",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){unsigned long r;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tcneg %0,%3,lt\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c):\"cc\");"
   "return r;}\n",
   {9, 3, 7}, "CondSelAlias"},

  // ===== CSET: cond ? 1 : 0. =====
  {"cset_ge_true",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long r;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tcset %0,ge\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {9, 3, 0}, "CondSelAlias"},

  // ===== CSETM: cond ? -1 : 0 (all-ones mask). =====
  {"csetm_lt_true",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long r;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tcsetm %0,lt\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {3, 9, 0}, "CondSelAlias"},

  {"csetm_lt_false",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long r;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tcsetm %0,lt\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {9, 3, 0}, "CondSelAlias"},

  // ===== 32-bit (Wd) alias forms exercise the 4-byte dest width. =====
  {"cinc_w_eq_true",
   "unsigned f(unsigned a,unsigned b,unsigned c){unsigned r;"
   "__asm__ volatile(\"cmp %w1,%w2\\n\\tcinc %w0,%w3,eq\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c):\"cc\");"
   "return r;}\n",
   {5, 5, 0xFFFFFFFFULL}, "CondSelAlias"},

  {"cneg_w_lt_true",
   "unsigned f(unsigned a,unsigned b,unsigned c){unsigned r;"
   "__asm__ volatile(\"cmp %w1,%w2\\n\\tcneg %w0,%w3,lt\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c):\"cc\");"
   "return r;}\n",
   {3, 9, 7}, "CondSelAlias"},

  // ===== Canonical CSNEG negate-INT_MIN wrap guardrail (NE true -> -Rm). =====
  // Rm = 0x8000000000000000; two's-complement negate wraps back to itself.
  {"csneg_intmin_wrap",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){unsigned long r;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tcsneg %0,%3,%3,ne\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c):\"cc\");"
   "return r;}\n",
   {3, 9, 0x8000000000000000ULL}, "CondSelAlias"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(CondSelAlias, A64CondSelectAliasRT,
                         ::testing::ValuesIn(kA64CondSelAlias), rtTCName);
