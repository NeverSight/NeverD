//===- AllPlatform_OptStress67RTTests.cpp - FP-return consume ---*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Follow-up to the #469 call/return-ABI fixes (FP accumulator across a call,
// int return dropped by an xorps self-zero): exercise the FP return of a helper
// consumed in contexts other than a plain accumulator — compared in an
// if/else (FP flags read in a successor block), selected, and fed forward
// through a chain — so any remaining FP-return modeling gap surfaces as a
// return mismatch.  Static `__attribute__((noinline))` helpers force a real
// call with the internal FP register convention.
//
//   * fpcmp     - if/else on a helper's FP return (compare in a separate block;
//                 regression for the #469 cross-block FP-return rewrite).
//   * fpsel     - select on a helper's FP return into an accumulator.
//   * fpchain   - chain: each helper's FP return feeds the next call's arg.
//
// (FP return consumed at a multi-predecessor merge / ternary, in a recursive
// accumulator, or two FP returns compared on AArch64/ARM are tracked as open
// bug O-5 in the Unicorn unsupported-instructions doc #469.)
//
// Each folds to one integer return; no libm, no 64-bit divide helper.
// All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress67RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress67RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress67RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress67RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress67RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress67RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress67RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress67RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress67TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Branch on a helper's FP return (FP compare flags consumed after the call).
    {p+"_fpcmp",
     "static double "+p+"_h(double) __attribute__((noinline));\n"
     +t+" "+p+"_fpcmp("+t+" a){ unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    double r="+p+"_h((double)((s>>8)&0xff));\n"
     "    if(r>64.0) h=h*131u+7u; else if(r<16.0) h=h*131u+3u; else h=h*131u+1u; }\n"
     "  return ("+t+")h; }\n"
     "static double "+p+"_h(double x){ return x*0.5 + x*x*0.001; }\n",
     {0x41u}, "OptStress67", 2},

    // Select on a helper's FP return into an accumulator.
    {p+"_fpsel",
     "static double "+p+"_h(double,double) __attribute__((noinline));\n"
     +t+" "+p+"_fpsel("+t+" a){ unsigned s=(unsigned)a; double acc=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    double r="+p+"_h((double)((s>>7)&0xff),(double)((s>>15)&0xff));\n"
     "    acc += (s&1u)? r : -r; }\n"
     "  return ("+t+")(long long)acc; }\n"
     "static double "+p+"_h(double a,double b){ return a*b*0.01 - a + b; }\n",
     {0x42u}, "OptStress67", 2},

    // Chain: each helper's FP return feeds the next call's argument.
    {p+"_fpchain",
     "static double "+p+"_h(double,double) __attribute__((noinline));\n"
     +t+" "+p+"_fpchain("+t+" a){ unsigned s=(unsigned)a; double r=1.0; double tot=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    r="+p+"_h(r, (double)((s>>9)&0x3f)); tot+=r; if(r>1e6) r=1.0; }\n"
     "  return ("+t+")(long long)tot; }\n"
     "static double "+p+"_h(double a,double b){ return a*0.5 + b*0.25 + 1.0; }\n",
     {0x43u}, "OptStress67", 2},

  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress67TC("x64o67", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress67TC("x86o67", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress67TC("a64o67", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress67TC("armo67", "int");

INSTANTIATE_TEST_SUITE_P(OptStress67, X64OptStress67RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress67, X86OptStress67RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress67, A64OptStress67RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress67, ARM32OptStress67RT, ::testing::ValuesIn(kARM), rtTCName);
