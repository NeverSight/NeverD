//===- AllPlatform_SignedFlagFoldRTTests.cpp - signed setcc fold -*- C++ -*-==//
//
// The roundtrip harness only compares return values, so MedFlags' folding of a
// signed condition (setl/setge/setle/setg, cset lt/ge/le/gt, movlt/...) back to
// a comparison of the flag source's operands is invisible unless folded into
// the result.
//
// A signed relation reads SF^OF (x86) / N^V (ARM), plus ZF/Z for LE/GT.  After
// a real `cmp`/`subs` (SUB) the fold reconstructs `A <signed> B`, which is
// exact.  But after an `add`/`cmn` (OF/V from INT_SOVF), a register-writing
// SUB whose result is re-read through a SUBBYTES (findCmpSource lands on its
// else branch with B=0), or a logical op (OF=0), the fold collapses to
// `result <signed> 0` (SF/N only) and silently drops OF/V — wrong whenever the
// source signed-overflowed.  These probes drive `add;setcc` / `cmn;cset` at the
// signed-overflow boundary and fold the flag into the return value, so any
// OF/V loss shows as a mismatch.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SignedFlagFoldRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SignedFlagFoldRT, Verify) { roundTripX64(GetParam()); }

class A64SignedFlagFoldRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64SignedFlagFoldRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32SignedFlagFoldRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32SignedFlagFoldRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // ===== add; setcc at INT_MAX+1 (pos+pos→neg, SF=1 OF=1). =====
  {"add_setl_ovf",
   "long f(long a,long b){unsigned x=(unsigned)a,y=(unsigned)b;unsigned char r;"
   "__asm__ volatile(\"addl %2,%1\\n\\tsetl %0\":\"=q\"(r),\"+r\"(x):\"r\"(y):\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFULL, 1}, "SignedFold"},
  {"add_setge_ovf",
   "long f(long a,long b){unsigned x=(unsigned)a,y=(unsigned)b;unsigned char r;"
   "__asm__ volatile(\"addl %2,%1\\n\\tsetge %0\":\"=q\"(r),\"+r\"(x):\"r\"(y):\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFULL, 1}, "SignedFold"},
  {"add_setle_ovf",
   "long f(long a,long b){unsigned x=(unsigned)a,y=(unsigned)b;unsigned char r;"
   "__asm__ volatile(\"addl %2,%1\\n\\tsetle %0\":\"=q\"(r),\"+r\"(x):\"r\"(y):\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFULL, 1}, "SignedFold"},
  {"add_setg_ovf",
   "long f(long a,long b){unsigned x=(unsigned)a,y=(unsigned)b;unsigned char r;"
   "__asm__ volatile(\"addl %2,%1\\n\\tsetg %0\":\"=q\"(r),\"+r\"(x):\"r\"(y):\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFULL, 1}, "SignedFold"},
  // neg+neg→0 (SF=0 OF=1): setl=1 (folding to result<0 wrongly gives 0).
  {"add_setl_negovf",
   "long f(long a,long b){unsigned x=(unsigned)a,y=(unsigned)b;unsigned char r;"
   "__asm__ volatile(\"addl %2,%1\\n\\tsetl %0\":\"=q\"(r),\"+r\"(x):\"r\"(y):\"cc\");"
   "return r;}\n",
   {0x80000000ULL, 0x80000000ULL}, "SignedFold"},
  {"addq_setl_ovf",
   "long f(long a,long b){unsigned long x=a,y=b;unsigned char r;"
   "__asm__ volatile(\"addq %2,%1\\n\\tsetl %0\":\"=q\"(r),\"+r\"(x):\"r\"(y):\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFFFFFFFFFULL, 1}, "SignedFold"},
  {"addw_setg_ovf",
   "long f(long a,long b){unsigned short x=(unsigned short)a,y=(unsigned short)b;"
   "unsigned char r;"
   "__asm__ volatile(\"addw %2,%1\\n\\tsetg %0\":\"=q\"(r),\"+r\"(x):\"r\"(y):\"cc\");"
   "return r;}\n",
   {0x7FFFULL, 1}, "SignedFold"},
  // register-writing sub re-read through SUBBYTES also drops OF (else-branch).
  {"sub_setl_ovf",
   "long f(long a,long b){int x=(int)a,y=(int)b;unsigned char r;"
   "__asm__ volatile(\"subl %2,%1\\n\\tsetl %0\":\"=q\"(r),\"+r\"(x):\"r\"(y):\"cc\");"
   "return r;}\n",
   {0x80000000ULL, 1}, "SignedFold"},
  // ---- Controls (clean cmp / logical / EQ fold correctly) ----
  {"cmp_setl_ctrl",
   "long f(long a,long b){int x=(int)a,y=(int)b;unsigned char r;"
   "__asm__ volatile(\"cmpl %2,%1\\n\\tsetl %0\":\"=q\"(r):\"r\"(x),\"r\"(y):\"cc\");"
   "return r;}\n",
   {3, 5}, "SignedFold"},
  {"cmp_setg_ctrl",
   "long f(long a,long b){int x=(int)a,y=(int)b;unsigned char r;"
   "__asm__ volatile(\"cmpl %2,%1\\n\\tsetg %0\":\"=q\"(r):\"r\"(x),\"r\"(y):\"cc\");"
   "return r;}\n",
   {5, 3}, "SignedFold"},
  {"cmp_setl_ovf_ctrl",
   "long f(long a,long b){int x=(int)a,y=(int)b;unsigned char r;"
   "__asm__ volatile(\"cmpl %2,%1\\n\\tsetl %0\":\"=q\"(r):\"r\"(x),\"r\"(y):\"cc\");"
   "return r;}\n",
   {0x80000000ULL, 1}, "SignedFold"},
  {"test_setl_ctrl",
   "long f(long a){int x=(int)a;unsigned char r;"
   "__asm__ volatile(\"testl %1,%1\\n\\tsetl %0\":\"=q\"(r):\"r\"(x):\"cc\");"
   "return r;}\n",
   {0x80000000ULL}, "SignedFold"},
  {"add_sete_ctrl",
   "long f(long a,long b){unsigned x=(unsigned)a,y=(unsigned)b;unsigned char r;"
   "__asm__ volatile(\"addl %2,%1\\n\\tsete %0\":\"=q\"(r),\"+r\"(x):\"r\"(y):\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFULL, 1}, "SignedFold"},
  {"add_setl_noovf_ctrl",
   "long f(long a,long b){unsigned x=(unsigned)a,y=(unsigned)b;unsigned char r;"
   "__asm__ volatile(\"addl %2,%1\\n\\tsetl %0\":\"=q\"(r),\"+r\"(x):\"r\"(y):\"cc\");"
   "return r;}\n",
   {2, 3}, "SignedFold"},
};

static const std::vector<RoundTripTC> kA64 = {
  // cmn = adds-discard; cset lt/ge/le/gt read N^V (and Z).  AArch64 models the
  // overflow inline, so these are guards confirming correctness across arches.
  {"cmn_cset_lt_ovf",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"cmn %w1,%w2\\n\\tcset %w0,lt\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFULL, 1}, "SignedFold"},
  {"cmn_cset_ge_ovf",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"cmn %w1,%w2\\n\\tcset %w0,ge\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFULL, 1}, "SignedFold"},
  {"cmn_cset_le_ovf",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"cmn %w1,%w2\\n\\tcset %w0,le\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {0x80000000ULL, 0x80000000ULL}, "SignedFold"},
  {"cmn_cset_gt_ovf",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"cmn %w1,%w2\\n\\tcset %w0,gt\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFULL, 1}, "SignedFold"},
  // adds writes a register (then re-read); cset lt must keep V.
  {"adds_cset_lt_ovf",
   "long f(long a,long b){long r;int t;"
   "__asm__ volatile(\"adds %w1,%w2,%w3\\n\\tcset %w0,lt\":\"=r\"(r),\"=&r\"(t):\"r\"((int)a),\"r\"((int)b):\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFULL, 1}, "SignedFold"},
  // Control: clean cmp folds correctly.
  {"cmp_cset_lt_ctrl",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"cmp %w1,%w2\\n\\tcset %w0,lt\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {3, 5}, "SignedFold"},
  {"subs_cset_lt_ovf_ctrl",
   "long f(long a,long b){long r;int t;"
   "__asm__ volatile(\"subs %w1,%w2,%w3\\n\\tcset %w0,lt\":\"=r\"(r),\"=&r\"(t):\"r\"((int)a),\"r\"((int)b):\"cc\");"
   "return r;}\n",
   {0x80000000ULL, 1}, "SignedFold"},
};

static const std::vector<RoundTripTC> kARM32 = {
  // cmn = adds-discard; predicated movlt/movge go through MedFlags' SELECT pass.
  {"cmn_movlt_ovf",
   "int f(int a,int b){int r;"
   "__asm__ volatile(\"mov %0,#0\\n\\tcmn %1,%2\\n\\tmovlt %0,#1\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFULL, 1}, "SignedFold"},
  {"cmn_movge_ovf",
   "int f(int a,int b){int r;"
   "__asm__ volatile(\"mov %0,#0\\n\\tcmn %1,%2\\n\\tmovge %0,#1\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFULL, 1}, "SignedFold"},
  // neg+neg→0 (N=0 V=1): lt=N^V=1.
  {"cmn_movlt_negovf",
   "int f(int a,int b){int r;"
   "__asm__ volatile(\"mov %0,#0\\n\\tcmn %1,%2\\n\\tmovlt %0,#1\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {0x80000000ULL, 0x80000000ULL}, "SignedFold"},
  // adds writes a register; movlt must keep V.
  {"adds_movlt_ovf",
   "int f(int a,int b){int r;int t;"
   "__asm__ volatile(\"mov %0,#0\\n\\tadds %1,%2,%3\\n\\tmovlt %0,#1\":\"=r\"(r),\"=&r\"(t):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFULL, 1}, "SignedFold"},
  // Control: clean cmp folds correctly.
  {"cmp_movlt_ctrl",
   "int f(int a,int b){int r;"
   "__asm__ volatile(\"mov %0,#0\\n\\tcmp %1,%2\\n\\tmovlt %0,#1\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {3, 5}, "SignedFold"},
  {"subs_movlt_ovf_ctrl",
   "int f(int a,int b){int r;int t;"
   "__asm__ volatile(\"mov %0,#0\\n\\tsubs %1,%2,%3\\n\\tmovlt %0,#1\":\"=r\"(r),\"=&r\"(t):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {0x80000000ULL, 1}, "SignedFold"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(SignedFold, X64SignedFlagFoldRT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(SignedFold, A64SignedFlagFoldRT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(SignedFold, ARM32SignedFlagFoldRT,
                         ::testing::ValuesIn(kARM32), rtTCName);
