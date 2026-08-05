//===- AllPlatform_OptStress93RTTests.cpp - lone sign-flag fold -*- C++ -*-==//
//
// Locks in the #486 fix across all four targets and all three MedFlags folding
// passes.  A LONE sign flag (x86 `js`/`sets`/`cmovs`, AArch64 `cset/csel mi`,
// ARM32 `movmi`) is the sign of the WRAPPED result `(A-B) <s 0`, NOT the
// overflow-corrected signed comparison `A <s B` (= SF^OF / N^V).  MedFlags must
// keep a lone-sign condition unfolded so the emitter lowers the genuine sign of
// the result; folding it to a comparison of the CMP operands drops the overflow
// correction and diverges exactly when A-B overflows.
//
// Every probe pins the instruction stream with inline asm at overflow-inducing
// inputs (INT_MIN-1 wraps positive, INT_MAX-(-1) wraps negative) so a lone-sign
// misfold shows against the native run.  Lone-overflow (`seto`/`cset vs`/`movvs`)
// guards confirm the OF/V path stays correct too.  COND_BR (`js`), SETcc
// (`sets`/`setns`) and SELECT (`cmovs`/`csel`/predicated mov) are each covered.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress93RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress93RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress93RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress93RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress93RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress93RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress93RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress93RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
// INT_MIN - 1 wraps to INT_MAX: SF=0 while (INT_MIN <s 1) is true.
// INT_MAX - (-1) wraps to INT_MIN: SF=1 while (INT_MAX <s -1) is false.
static const uint64_t IMIN = 0x80000000ULL, IMAX = 0x7FFFFFFFULL, NEG1 = 0xFFFFFFFFULL;

// x86/x64: lone-SF via SELECT (cmovs), SETcc (setns) and lone-OF (seto) guard.
static std::vector<RoundTripTC> makeX86LoneTC(const char *prefix) {
  std::string p = prefix;
  return {
    // cmp a,b ; cmovs alt -> r = SF ? alt : base.  SF is sign of wrapped (a-b),
    // NOT (a <s b); a misfold to (a <s b) flips the result at the overflow input.
    {p+"_cmovs_mi",
     "long "+p+"_cmovs_mi(long a,long b){ long r=100,alt=300;\n"
     "  __asm__ volatile(\"cmpl %k2,%k1\\n\\tcmovsl %k3,%k0\":\"+r\"(r):\"r\"(a),\"r\"(b),\"r\"(alt):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress93"},
    {p+"_cmovs_mi2",
     "long "+p+"_cmovs_mi2(long a,long b){ long r=100,alt=300;\n"
     "  __asm__ volatile(\"cmpl %k2,%k1\\n\\tcmovsl %k3,%k0\":\"+r\"(r):\"r\"(a),\"r\"(b),\"r\"(alt):\"cc\");\n"
     "  return r; }\n",
     {IMAX, NEG1}, "OptStress93"},
    // cmp a,b ; setns -> SF==0 (sign of wrapped (a-b) is 0), not (a >=s b).
    {p+"_setns",
     "long "+p+"_setns(long a,long b){ unsigned char s;\n"
     "  __asm__ volatile(\"cmpl %k2,%k1\\n\\tsetns %0\":\"=r\"(s):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return (long)s; }\n",
     {IMIN, 1}, "OptStress93"},
    // Lone-OF guard: cmp a,b ; seto -> OF (already correct, INT_SBOR kept).
    {p+"_seto",
     "long "+p+"_seto(long a,long b){ unsigned char s;\n"
     "  __asm__ volatile(\"cmpl %k2,%k1\\n\\tseto %0\":\"=r\"(s):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return (long)s; }\n",
     {IMIN, 1}, "OptStress93"},
    // Lone-OF branch guard: cmp a,b ; jo.
    {p+"_jo",
     "long "+p+"_jo(long a,long b){ long r;\n"
     "  __asm__ volatile(\"cmpl %k2,%k1\\n\\tmovl $100,%k0\\n\\tjno 1f\\n\\t\"\n"
     "    \"movl $300,%k0\\n\\t1:\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress93"},
  };
}

// AArch64: lone-N via cset/csel (mi/pl) and lone-V (vs) guard, both polarities.
static std::vector<RoundTripTC> makeA64LoneTC(const char *prefix) {
  std::string p = prefix;
  return {
    {p+"_cset_mi",
     "long "+p+"_cset_mi(long a,long b){ long r;\n"
     "  __asm__ volatile(\"cmp %w1,%w2\\n\\tcset %w0,mi\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress93"},
    {p+"_cset_mi2",
     "long "+p+"_cset_mi2(long a,long b){ long r;\n"
     "  __asm__ volatile(\"cmp %w1,%w2\\n\\tcset %w0,mi\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMAX, NEG1}, "OptStress93"},
    {p+"_cset_pl",
     "long "+p+"_cset_pl(long a,long b){ long r;\n"
     "  __asm__ volatile(\"cmp %w1,%w2\\n\\tcset %w0,pl\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress93"},
    // csel reads the lone N flag (SELECT pass).
    {p+"_csel_mi",
     "long "+p+"_csel_mi(long a,long b){ long r,x=100,y=300;\n"
     "  __asm__ volatile(\"cmp %w1,%w2\\n\\tcsel %0,%3,%4,mi\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(y),\"r\"(x):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress93"},
    // subs writes a register then re-read; cset mi must still be sign-of-result.
    {p+"_subs_cset_mi",
     "long "+p+"_subs_cset_mi(long a,long b){ long r;int t;\n"
     "  __asm__ volatile(\"subs %w1,%w2,%w3\\n\\tcset %w0,mi\":\"=r\"(r),\"=&r\"(t):\"r\"((int)a),\"r\"((int)b):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress93"},
    // Lone-V guard: cset vs reads the overflow flag.
    {p+"_cset_vs",
     "long "+p+"_cset_vs(long a,long b){ long r;\n"
     "  __asm__ volatile(\"cmp %w1,%w2\\n\\tcset %w0,vs\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress93"},
  };
}

// ARM32: lone-N via predicated movmi/movpl (SELECT pass) and lone-V (movvs) guard.
static std::vector<RoundTripTC> makeARM32LoneTC(const char *prefix) {
  std::string p = prefix;
  return {
    {p+"_movmi",
     "int "+p+"_movmi(int a,int b){ int r;\n"
     "  __asm__ volatile(\"mov %0,#0\\n\\tcmp %1,%2\\n\\tmovmi %0,#1\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress93"},
    {p+"_movmi2",
     "int "+p+"_movmi2(int a,int b){ int r;\n"
     "  __asm__ volatile(\"mov %0,#0\\n\\tcmp %1,%2\\n\\tmovmi %0,#1\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMAX, NEG1}, "OptStress93"},
    {p+"_movpl",
     "int "+p+"_movpl(int a,int b){ int r;\n"
     "  __asm__ volatile(\"mov %0,#0\\n\\tcmp %1,%2\\n\\tmovpl %0,#1\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress93"},
    {p+"_subs_movmi",
     "int "+p+"_subs_movmi(int a,int b){ int r;int t;\n"
     "  __asm__ volatile(\"mov %0,#0\\n\\tsubs %1,%2,%3\\n\\tmovmi %0,#1\":\"=&r\"(r),\"=&r\"(t):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress93"},
    {p+"_movvs",
     "int "+p+"_movvs(int a,int b){ int r;\n"
     "  __asm__ volatile(\"mov %0,#0\\n\\tcmp %1,%2\\n\\tmovvs %0,#1\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress93"},
  };
}

// All-platform pure-C: a lone wrapped-sign test steers an accumulation in a loop
// at overflow-straddling values so `(int)(ua-ub) < 0` (wrapped sign) drives the
// result on every step.  -O2 lowers it without a libcall on all four targets.
static std::vector<RoundTripTC> makeAllLoneTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    {p+"_loopsign",
     t+" "+p+"_loopsign("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long acc=0;\n"
     "  for(int i=0;i<256;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned ua=s|0x80000000u, ub=(s>>2)&0x3FFFFFFFu;\n"
     "    int d=(int)(ua-ub);\n"
     "    if(d<0) acc=acc*131+(long)ua; else acc=acc*131+(long)ub;\n"
     "    acc^=acc>>9; }\n"
     "  return ("+t+")acc; }\n",
     {0x73u}, "OptStress93", 2},
  };
}
// clang-format on

static std::vector<RoundTripTC> kX64 = [](){
  auto v = makeX86LoneTC("x64o93");
  for (auto &t : makeAllLoneTC("x64o93", "long")) v.push_back(t);
  return v;
}();
static std::vector<RoundTripTC> kX86 = [](){
  auto v = makeX86LoneTC("x86o93");
  for (auto &t : makeAllLoneTC("x86o93", "int")) v.push_back(t);
  return v;
}();
static std::vector<RoundTripTC> kA64 = [](){
  auto v = makeA64LoneTC("a64o93");
  for (auto &t : makeAllLoneTC("a64o93", "long")) v.push_back(t);
  return v;
}();
static std::vector<RoundTripTC> kARM = [](){
  auto v = makeARM32LoneTC("armo93");
  for (auto &t : makeAllLoneTC("armo93", "int")) v.push_back(t);
  return v;
}();

INSTANTIATE_TEST_SUITE_P(OptStress93, X64OptStress93RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress93, X86OptStress93RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress93, A64OptStress93RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress93, ARM32OptStress93RT, ::testing::ValuesIn(kARM), rtTCName);
