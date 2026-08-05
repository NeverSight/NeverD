//===- AllPlatform_FlagFoldEdge2RTTests.cpp - flag-fold edge 2 --*- C++ -*-===//
//
// Follow-on to AllPlatform_SignedFlagFoldRTTests: exercises the other two
// MedFlags fold paths (CMOV/predicated SELECT, conditional branch) plus
// AArch64 conditional-compare (CCMP/CCMN) chains at signed-overflow
// boundaries, folding the result into the return value so any dropped OF/V or
// mis-resolved conditional flag shows as a mismatch.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FlagFold2RT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FlagFold2RT, Verify) { roundTripX64(GetParam()); }

class A64FlagFold2RT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FlagFold2RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32FlagFold2RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FlagFold2RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // CMOV after add at overflow (Pass 3 SELECT): cmovl fires on SF^OF.
  {"add_cmovl_ovf",
   "int f(int a,int b){int r=0,one=1;"
   "__asm__ volatile(\"addl %[y],%[x]\\n\\tcmovl %[one],%[r]\""
   ":[r]\"+r\"(r),[x]\"+r\"(a):[y]\"r\"(b),[one]\"r\"(one):\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFULL, 1}, "FlagFold2"},
  {"add_cmovge_ovf",
   "int f(int a,int b){int r=0,one=1;"
   "__asm__ volatile(\"addl %[y],%[x]\\n\\tcmovge %[one],%[r]\""
   ":[r]\"+r\"(r),[x]\"+r\"(a):[y]\"r\"(b),[one]\"r\"(one):\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFULL, 1}, "FlagFold2"},
  // dec/inc/neg set OF/SF/ZF (not CF); signed setcc at boundary.
  {"dec_setl_ovf",
   "int f(int a){int x=a;unsigned char r;"
   "__asm__ volatile(\"decl %1\\n\\tsetl %0\":\"=q\"(r),\"+r\"(x)::\"cc\");"
   "return r;}\n",
   {0x80000000ULL}, "FlagFold2"},
  {"inc_setg_ovf",
   "int f(int a){int x=a;unsigned char r;"
   "__asm__ volatile(\"incl %1\\n\\tsetg %0\":\"=q\"(r),\"+r\"(x)::\"cc\");"
   "return r;}\n",
   {0x7FFFFFFFULL}, "FlagFold2"},
  {"neg_setle_ovf",
   "int f(int a){int x=a;unsigned char r;"
   "__asm__ volatile(\"negl %1\\n\\tsetle %0\":\"=q\"(r),\"+r\"(x)::\"cc\");"
   "return r;}\n",
   {0x80000000ULL}, "FlagFold2"},
  // Control: cmov after cmp folds correctly.
  {"cmp_cmovl_ctrl",
   "int f(int a,int b){int r=0,one=1;"
   "__asm__ volatile(\"cmpl %[y],%[x]\\n\\tcmovl %[one],%[r]\""
   ":[r]\"+r\"(r),[x]\"+r\"(a):[y]\"r\"(b),[one]\"r\"(one):\"cc\");"
   "return r;}\n",
   {3, 5}, "FlagFold2"},
};

static const std::vector<RoundTripTC> kA64 = {
  // CCMP: if (a>0) compare b with 1 else NZCV=0; cset lt reads N^V.
  // Inner compare overflows when b=INT_MIN (b-1).
  {"ccmp_inner_ovf_lt",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"cmp %w1,#0\\n\\tccmp %w2,#1,#0,gt\\n\\tcset %w0,lt\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {1, 0x80000000ULL}, "FlagFold2"},
  // a<=0 path: ccmp false → NZCV=#0 → lt=0.
  {"ccmp_else_path",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"cmp %w1,#0\\n\\tccmp %w2,#1,#0,gt\\n\\tcset %w0,lt\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {0, 0x80000000ULL}, "FlagFold2"},
  // CCMN (add-based conditional compare): if (a>0) flags = (b + 1).
  {"ccmn_inner_lt",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"cmp %w1,#0\\n\\tccmn %w2,#1,#0,gt\\n\\tcset %w0,lt\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {1, 0x7FFFFFFFULL}, "FlagFold2"},
  // C-expression compound condition (compiler picks ccmp) at boundary.
  {"cexpr_and_boundary",
   "long f(long a,long b){return (a>0 && b<=(-2147483647-1)) ? 7 : 3;}\n",
   {5, 0x80000000ULL}, "FlagFold2"},
  {"cexpr_or_boundary",
   "long f(long a,long b){return (a>2147483647L || b==0) ? 7 : 3;}\n",
   {0, 0}, "FlagFold2"},
};

static const std::vector<RoundTripTC> kARM32 = {
  // Predicated SELECT after sub (FromSub) at overflow — control.
  {"subs_movle_ovf",
   "int f(int a,int b){int r=0;int t;"
   "__asm__ volatile(\"mov %0,#0\\n\\tsubs %1,%2,%3\\n\\tmovle %0,#1\""
   ":\"=r\"(r),\"=&r\"(t):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {0x80000000ULL, 1}, "FlagFold2"},
  // cmn (add-based) predicated le at overflow boundary.
  {"cmn_movle_ovf",
   "int f(int a,int b){int r=0;"
   "__asm__ volatile(\"mov %0,#0\\n\\tcmn %1,%2\\n\\tmovle %0,#1\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {0x7FFFFFFFULL, 1}, "FlagFold2"},
  // C-expression compound condition.
  {"cexpr_and",
   "int f(int a,int b){return (a>0 && b<0) ? 7 : 3;}\n",
   {5, 0x80000000ULL}, "FlagFold2"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(FlagFold2, X64FlagFold2RT, ::testing::ValuesIn(kX64),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(FlagFold2, A64FlagFold2RT, ::testing::ValuesIn(kA64),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(FlagFold2, ARM32FlagFold2RT,
                         ::testing::ValuesIn(kARM32), rtTCName);
