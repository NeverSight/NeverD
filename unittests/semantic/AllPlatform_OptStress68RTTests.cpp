//===- AllPlatform_OptStress68RTTests.cpp - deeper FP-return -*-C++*-=//
//
// Follow-up to the #470 FP-return modeling fix (modelCallFPReturn now rewrites
// narrow sub-register and loop-carried reads of the call's FP return, not just
// the wide vector form): exercise the deeper consume shapes that were tracked
// as open bug O-5 — an FP return merged at a ternary PHI (multi-predecessor), a
// recursive FP accumulator, and two helpers' FP returns compared.  Each must
// observe the call result, not the stale pre-call register value.
//
//   * fptern    - acc += cond ? h(x) : x*0.5  (call result merged at a PHI).
//   * recuracc  - recursive `x*0.5 + rec(n-1, x*0.5)` FP accumulation.
//   * twofpcmp  - if (h(x) > g(y))  (two FP returns compared).
//
// Each folds to one integer return; no libm, no 64-bit divide helper.
// All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress68RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress68RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress68RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress68RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress68RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress68RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress68RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress68RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress68TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // FP return merged at a ternary PHI (one arm is the call result).
    {p+"_fptern",
     "static double "+p+"_h(double) __attribute__((noinline));\n"
     +t+" "+p+"_fptern("+t+" a){ unsigned s=(unsigned)a; double acc=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    double x=(double)((s>>8)&0xff);\n"
     "    acc += (s&1u)? "+p+"_h(x) : x*0.5; }\n"
     "  return ("+t+")(long long)acc; }\n"
     "static double "+p+"_h(double x){ return x*x*0.01 + 1.0; }\n",
     {0x51u}, "OptStress68", 2},

    // Recursive FP accumulation: each level adds the recursive call's FP return.
    {p+"_recuracc",
     "static double "+p+"_rec(int,double) __attribute__((noinline));\n"
     +t+" "+p+"_recuracc("+t+" a){ unsigned s=(unsigned)a; double acc=0;\n"
     "  for(int i=0;i<60;i++){ s=s*1103515245u+12345u;\n"
     "    acc += "+p+"_rec((int)((s>>10)&7),(double)((s>>4)&0x1f)); }\n"
     "  return ("+t+")(long long)acc; }\n"
     "static double "+p+"_rec(int n,double x){ if(n<=0) return x;\n"
     "  return x*0.5 + "+p+"_rec(n-1, x*0.5); }\n",
     {0x52u}, "OptStress68", 2},

    // Two helpers' FP returns compared in a branch.
    {p+"_twofpcmp",
     "static double "+p+"_h(double) __attribute__((noinline));\n"
     "static double "+p+"_g(double) __attribute__((noinline));\n"
     +t+" "+p+"_twofpcmp("+t+" a){ unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    double u="+p+"_h((double)((s>>8)&0xff));\n"
     "    double v="+p+"_g((double)((s>>16)&0xff));\n"
     "    if(u>v) h=h*131u+7u; else h=h*131u+3u; }\n"
     "  return ("+t+")h; }\n"
     "static double "+p+"_h(double x){ return x*0.5 + 1.0; }\n"
     "static double "+p+"_g(double x){ return x*0.25 + 2.0; }\n",
     {0x53u}, "OptStress68", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress68TC("x64o68", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress68TC("x86o68", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress68TC("a64o68", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress68TC("armo68", "int");

INSTANTIATE_TEST_SUITE_P(OptStress68, X64OptStress68RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress68, X86OptStress68RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress68, A64OptStress68RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress68, ARM32OptStress68RT, ::testing::ValuesIn(kARM), rtTCName);
