//===- AllPlatform_OptStress92RTTests.cpp - sign-flag vs signed-less -C++-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// `singleFlagCond(NF)` maps a lone sign flag to CondCode::SLT.  But the sign
// flag after a real subtraction is `(A-B) < 0` on the WRAPPED result, which is
// NOT the signed comparison `A <s B` (the latter is SF^OF, overflow-corrected).
// They diverge exactly when `A - B` overflows.  If the flag pass folds a
// `cmp A,B; js/sets` (lone SF) chain to `A <s B` using the CMP operands it would
// silently drop the overflow correction — the same family as the
// carry/overflow folds that condNeedsGenuineSub already guards, but for SF.
//
// These probes force the lone-SF idiom with inline asm at overflow-inducing
// values so any SF->SLT misfold shows up against the native run, and contrast
// it with the genuine signed-less (`jl`/`setl`, which IS `A <s B`) and the
// `test x,x; sets` sign test (OF=0, where SF == `x <s 0`, the sound case).  The
// pure-C `(int)((unsigned)a-(unsigned)b) < 0` form drives all four targets:
// unsigned subtract defeats the no-signed-overflow assumption so the compiler
// must test the wrapped sign rather than fold to `a < b`.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress92RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress92RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress92RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress92RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress92RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress92RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress92RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress92RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
// x86/x64-only inline-asm probes that pin the exact lone-SF instruction stream.
static std::vector<RoundTripTC> makeX86SignTC(const char *prefix) {
  std::string p = prefix;
  return {
    // cmp a,b ; sets  -> SF = sign of (a-b) wrapped, NOT (a <s b).
    // INT_MIN - 1 wraps positive (SF=0) while (INT_MIN <s 1) is true.
    {p+"_cmp_sets_a",
     "long "+p+"_cmp_sets_a(long a,long b){ unsigned char s;\n"
     "  __asm__ volatile(\"cmpl %k2,%k1\\n\\tsets %0\":\"=r\"(s):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return (long)s; }\n",
     {0x80000000ULL, 1}, "OptStress92"},
    // INT_MAX - (-1) wraps to INT_MIN (SF=1) while (INT_MAX <s -1) is false.
    {p+"_cmp_sets_b",
     "long "+p+"_cmp_sets_b(long a,long b){ unsigned char s;\n"
     "  __asm__ volatile(\"cmpl %k2,%k1\\n\\tsets %0\":\"=r\"(s):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return (long)s; }\n",
     {0x7FFFFFFFULL, 0xFFFFFFFFULL}, "OptStress92"},
    // cmp a,b ; js  (branch form of the same lone-SF test).
    {p+"_cmp_js",
     "long "+p+"_cmp_js(long a,long b){ long r;\n"
     "  __asm__ volatile(\"cmpl %k2,%k1\\n\\tmovl $100,%k0\\n\\tjns 1f\\n\\t\"\n"
     "    \"movl $300,%k0\\n\\t1:\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {0x80000000ULL, 1}, "OptStress92"},
    // Control: setl IS the signed comparison (SF^OF) -> must equal (a <s b).
    {p+"_cmp_setl",
     "long "+p+"_cmp_setl(long a,long b){ unsigned char s;\n"
     "  __asm__ volatile(\"cmpl %k2,%k1\\n\\tsetl %0\":\"=r\"(s):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return (long)s; }\n",
     {0x80000000ULL, 1}, "OptStress92"},
    // Control: test x,x; sets -> OF=0 so SF == (x <s 0); the sound lone-SF case.
    {p+"_test_sets",
     "long "+p+"_test_sets(long a){ unsigned char s;\n"
     "  __asm__ volatile(\"testl %k1,%k1\\n\\tsets %0\":\"=r\"(s):\"r\"(a):\"cc\");\n"
     "  return (long)s; }\n",
     {0x80000000ULL}, "OptStress92"},
    // sub then sets: SF = sign of the wrapped difference held in a register.
    {p+"_sub_sets",
     "long "+p+"_sub_sets(long a,long b){ unsigned char s; unsigned d=(unsigned)a;\n"
     "  __asm__ volatile(\"subl %k2,%k1\\n\\tsets %0\":\"=r\"(s),\"+r\"(d):\"r\"(b):\"cc\");\n"
     "  return (long)s; }\n",
     {0x80000000ULL, 1}, "OptStress92"},
  };
}

// All-platform pure-C: unsigned subtract (defined wrap) then signed sign test in
// a loop so the wrapped-sign result steers the accumulation.  Overflow-straddling
// inputs make `(int)(ua-ub) < 0` differ from `a < b` every other step.
static std::vector<RoundTripTC> makeAllSignTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    {p+"_wrapsign",
     t+" "+p+"_wrapsign("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long acc=0;\n"
     "  for(int i=0;i<256;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned ua=s^0x80000000u, ub=(s>>3)|0x40000000u;\n"
     "    int d=(int)(ua-ub);\n"
     "    if(d<0) acc=acc*131+1; else acc=acc*131+2;\n"
     "    acc^=acc>>7; }\n"
     "  return ("+t+")acc; }\n",
     {0x55u}, "OptStress92", 2},
  };
}
// clang-format on

static std::vector<RoundTripTC> kX64 = [](){
  auto v = makeX86SignTC("x64o92");
  for (auto &t : makeAllSignTC("x64o92", "long")) v.push_back(t);
  return v;
}();
static std::vector<RoundTripTC> kX86 = [](){
  auto v = makeX86SignTC("x86o92");
  for (auto &t : makeAllSignTC("x86o92", "int")) v.push_back(t);
  return v;
}();
static const std::vector<RoundTripTC> kA64 = makeAllSignTC("a64o92", "long");
static const std::vector<RoundTripTC> kARM = makeAllSignTC("armo92", "int");

INSTANTIATE_TEST_SUITE_P(OptStress92, X64OptStress92RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress92, X86OptStress92RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress92, A64OptStress92RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress92, ARM32OptStress92RT, ::testing::ValuesIn(kARM), rtTCName);
