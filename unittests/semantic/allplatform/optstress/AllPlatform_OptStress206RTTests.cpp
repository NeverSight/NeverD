//===- AllPlatform_OptStress206RTTests.cpp - indirect-call ABI stress ====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for INDIRECT calls (function pointers) carrying the ABI shapes
// that stress argument recovery -- by-value structs, overflow stack arguments,
// FP arguments, wide (64-bit) arguments, and struct returns.  The INDIR_CALL path
// recovers arguments from the call site alone (the callee arity is unknown), a
// path distinct from direct calls; each target is selected at runtime through a
// 2-entry function-pointer table (`tab[x&1]`) so clang cannot devirtualize.
//
//   * icstruct - struct{int,int,int,int} by value through a function pointer.
//   * icovf    - 9 word args (overflow the parameter registers) through a ptr.
//   * icfp     - 6 double args through a function pointer.
//   * icwide   - 4 long long args (8 i386 slots) through a function pointer.
//   * icret    - struct{int,int} returned through a function pointer.
//   * icmany   - 12 int args (deep overflow) through a function pointer.
//
// Two distinct targets per table force a genuine indirect call; integer/FP
// add/xor only, -O2, all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

#include <algorithm>

class X64OptStress206RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress206RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress206RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress206RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress206RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress206RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress206RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress206RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress206TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // struct{int,int,int,int} by value through a function pointer.
    {p+"_icstruct",
     "typedef struct{int a,b,c,d;}"+p+"_S4;\n"
     +t+" "+p+"_sf("+p+"_S4 s){ return ("+t+")(s.a^(s.b*3)^(s.c*5)^(s.d*7)); }\n"
     +t+" "+p+"_sg("+p+"_S4 s){ return ("+t+")(s.a+2*s.b+3*s.c+4*s.d); }\n"
     +t+" (*const "+p+"_st[2])("+p+"_S4)={"+p+"_sf,"+p+"_sg};\n"
     +t+" "+p+"_icstruct("+t+" x){ "+p+"_S4 s; s.a=(int)x; s.b=(int)x+1; s.c=(int)x+2; s.d=(int)x+3;\n"
     "  return "+p+"_st[x&1](s); }\n",
     {0x21ULL}, "OptStress206", 2},

    // 9 word args (overflow the parameter registers) through a function pointer.
    {p+"_icovf",
     t+" "+p+"_of("+t+" a,"+t+" b,"+t+" c,"+t+" d,"+t+" e,"+t+" f,"+t+" g,"+t+" h,"+t+" i){\n"
     "  return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h+9*i; }\n"
     +t+" "+p+"_og("+t+" a,"+t+" b,"+t+" c,"+t+" d,"+t+" e,"+t+" f,"+t+" g,"+t+" h,"+t+" i){\n"
     "  return a^b^c^d^e^f^g^h^i; }\n"
     +t+" (*const "+p+"_ot[2])("+t+","+t+","+t+","+t+","+t+","+t+","+t+","+t+","+t+")={"+p+"_of,"+p+"_og};\n"
     +t+" "+p+"_icovf("+t+" x){\n"
     "  return "+p+"_ot[x&1](x,x+1,x+2,x+3,x+4,x+5,x+6,x+7,x+8); }\n",
     {0x22ULL}, "OptStress206", 2},

    // 6 double args through a function pointer (int return; the wide 8-byte stack
    // arguments exercise indirect wide-argument recovery).  A double RETURN here
    // hits the i386 PIC indirect FP-return path the link+emulate harness cannot
    // ground-truth (the original itself yields a wrong value), so it is left as a
    // documented follow-up; an int return keeps the wide-arg path testable.
    {p+"_icfp",
     "int "+p+"_ff(double a,double b,double c,double d,double e,double f){\n"
     "  return (int)(a+2*b+3*c+4*d+5*e+6*f); }\n"
     "int "+p+"_fg(double a,double b,double c,double d,double e,double f){\n"
     "  return (int)(a*b+c*d+e*f); }\n"
     "int (*const "+p+"_ft[2])(double,double,double,double,double,double)={"+p+"_ff,"+p+"_fg};\n"
     +t+" "+p+"_icfp("+t+" x){ double v=(double)(int)x;\n"
     "  return ("+t+")"+p+"_ft[x&1](v,v+1,v+2,v+3,v+4,v+5); }\n",
     {0x23ULL}, "OptStress206", 2},

    // 4 long long args (8 i386 slots) through a function pointer.
    {p+"_icwide",
     "long long "+p+"_wf(long long a,long long b,long long c,long long d){\n"
     "  return a+2*b+3*c+4*d; }\n"
     "long long "+p+"_wg(long long a,long long b,long long c,long long d){\n"
     "  return a^b^c^d; }\n"
     "long long (*const "+p+"_wt[2])(long long,long long,long long,long long)={"+p+"_wf,"+p+"_wg};\n"
     +t+" "+p+"_icwide("+t+" x){ long long a=(long long)x;\n"
     "  return ("+t+")"+p+"_wt[x&1](a,a+1,a+2,a+3); }\n",
     {0x24ULL}, "OptStress206", 2},

    // struct{int,int} returned through a function pointer.
    {p+"_icret",
     "typedef struct{int a,b;}"+p+"_R2;\n"
     +p+"_R2 "+p+"_rf(int x){ "+p+"_R2 r; r.a=x*3+1; r.b=x-5; return r; }\n"
     +p+"_R2 "+p+"_rg(int x){ "+p+"_R2 r; r.a=x^0x55; r.b=x*7; return r; }\n"
     +p+"_R2 (*const "+p+"_rt[2])(int)={"+p+"_rf,"+p+"_rg};\n"
     +t+" "+p+"_icret("+t+" x){ "+p+"_R2 r="+p+"_rt[x&1]((int)x);\n"
     "  return ("+t+")(r.a^(r.b*3)); }\n",
     {0x25ULL}, "OptStress206", 2},

    // 12 int args (deep overflow) through a function pointer.
    {p+"_icmany",
     "int "+p+"_nf(int a,int b,int c,int d,int e,int f,int g,int h,int i,int j,int k,int l){\n"
     "  return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h+9*i+10*j+11*k+12*l; }\n"
     "int "+p+"_ng(int a,int b,int c,int d,int e,int f,int g,int h,int i,int j,int k,int l){\n"
     "  return a^b^c^d^e^f^g^h^i^j^k^l; }\n"
     "int (*const "+p+"_nt[2])(int,int,int,int,int,int,int,int,int,int,int,int)={"+p+"_nf,"+p+"_ng};\n"
     +t+" "+p+"_icmany("+t+" x){ int n=(int)x;\n"
     "  return ("+t+")"+p+"_nt[x&1](n,n+1,n+2,n+3,n+4,n+5,n+6,n+7,n+8,n+9,n+10,n+11); }\n",
     {0x26ULL}, "OptStress206", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress206TC("x64o206", "long");
static const std::vector<RoundTripTC> kA64 = makeOptStress206TC("a64o206", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress206TC("armo206", "int");

// `icret` returns a struct{int,int} which i386 SysV returns through a hidden sret
// pointer the callee pops (`ret $4`) -- reached here through a PIC indirect call.
// The link+emulate harness cannot ground-truth that original on i386 (the
// original itself faults), so the probe is restricted to the targets where the
// ground truth runs; direct struct returns are covered by OptStress204.
static const std::vector<RoundTripTC> kX86 = [] {
  auto V = makeOptStress206TC("x86o206", "int");
  V.erase(std::remove_if(V.begin(), V.end(),
                         [](const RoundTripTC &TC) {
                           return TC.Name == "x86o206_icret";
                         }),
          V.end());
  return V;
}();

INSTANTIATE_TEST_SUITE_P(OptStress206, X64OptStress206RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress206, X86OptStress206RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress206, A64OptStress206RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress206, ARM32OptStress206RT, ::testing::ValuesIn(kARM), rtTCName);
