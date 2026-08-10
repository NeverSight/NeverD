//===- AllPlatform_OptStress224RTTests.cpp - call result cross-CFG use ====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Hardening probes for the cross-block call-result propagation fixed in #502:
// an FP / struct-return call whose result is consumed not straight-line but
// across a richer control-flow shape -- a merge block after an if/else (multi-
// predecessor, dominated by the call via a PHI), a switch dispatch, and a use
// guarded inside a nested inner loop.  These exercise the PHI-arg path and the
// single-predecessor dominated-successor path of the result rewiring.
//
//   * fpmerge - FP call result used in the merge block after an if/else.
//   * fpswitch- FP call result selected through a switch.
//   * fpinner - FP call result used inside a nested inner loop.
//   * stmerge - {int,double} return: a field used in the merge block.
//   * fp2call - result of one FP call feeds the argument of a second, branched.
//   * fpguard - FP call result used only on one guarded path, else default.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress224RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress224RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress224RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress224RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress224RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress224RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress224RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress224RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress224TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // FP call result used in the merge block after an if/else.
    {p+"_fpmerge",
     "static double "+p+"_fd(double) __attribute__((noinline));\n"
     +t+" "+p+"_fpmerge("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    double r="+p+"_fd((double)((int)h>>10));\n"
     "    int v; if(h&1u) v=(int)(h>>3); else v=-(int)(h>>4);\n"
     "    acc=acc*131u+(unsigned)((int)r + v)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static double "+p+"_fd(double v){ return v*0.5 + 3.0; }\n",
     {0x12345u}, "OptStress224", 2},

    // FP call result selected through a switch.
    {p+"_fpswitch",
     "static double "+p+"_fd(double) __attribute__((noinline));\n"
     +t+" "+p+"_fpswitch("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    double r="+p+"_fd((double)((h>>6)&0x7f));\n"
     "    int w; switch(h&3u){ case 0: w=(int)r; break; case 1: w=(int)(r*2.0); break;\n"
     "      case 2: w=(int)(r+1.0); break; default: w=-(int)r; }\n"
     "    acc=acc*131u+(unsigned)w+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static double "+p+"_fd(double v){ return v*0.75 - 1.0; }\n",
     {0x23456u}, "OptStress224", 2},

    // FP call result used inside a nested inner loop.
    {p+"_fpinner",
     "static double "+p+"_fd(double) __attribute__((noinline));\n"
     +t+" "+p+"_fpinner("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<60;i++){ h=h*1103515245u+12345u;\n"
     "    double r="+p+"_fd((double)((h>>8)&0x3f));\n"
     "    for(int j=0;j<3;j++){ acc=acc*131u+(unsigned)((int)r + j); }\n"
     "    acc+=(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static double "+p+"_fd(double v){ return v + 0.5; }\n",
     {0x34567u}, "OptStress224", 2},

    // {int,double} return: a field used in the merge block after an if/else.
    {p+"_stmerge",
     "typedef struct{int a; double b;}"+p+"_ID;\n"
     +p+"_ID "+p+"_mk(int) __attribute__((noinline));\n"
     +t+" "+p+"_stmerge("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_ID r="+p+"_mk((int)h);\n"
     "    int v; if(h&2u) v=r.a*3; else v=-r.a;\n"
     "    acc=acc*131u+(unsigned)(v ^ (int)r.b)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     +p+"_ID "+p+"_mk(int x){ "+p+"_ID r; r.a=x*5+1; r.b=(double)(x>>2)-0.5; return r; }\n",
     {0x45678u}, "OptStress224", 2},

    // Result of one FP call feeds the argument of a second, on a branch.
    {p+"_fp2call",
     "static double "+p+"_g(double) __attribute__((noinline));\n"
     "static double "+p+"_h(double) __attribute__((noinline));\n"
     +t+" "+p+"_fp2call("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    double r="+p+"_g((double)((h>>7)&0x3f));\n"
     "    double s=(h&1u)? "+p+"_h(r) : r*2.0;\n"
     "    acc=acc*131u+(unsigned)(int)s+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static double "+p+"_g(double v){ return v*0.5 + 1.0; }\n"
     "static double "+p+"_h(double v){ return v*1.5 - 2.0; }\n",
     {0x56789u}, "OptStress224", 2},

    // FP call result used only on one guarded path (else uses a default).
    {p+"_fpguard",
     "static double "+p+"_fd(double) __attribute__((noinline));\n"
     +t+" "+p+"_fpguard("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    double r="+p+"_fd((double)((int)h>>11));\n"
     "    if((h&7u)!=0){ acc=acc*131u+(unsigned)(int)r; } else { acc=acc*131u+99u; }\n"
     "    acc+=(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static double "+p+"_fd(double v){ return v*0.25 + 5.0; }\n",
     {0x6789Au}, "OptStress224", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress224TC("x64o224", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress224TC("x86o224", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress224TC("a64o224", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress224TC("armo224", "int");

INSTANTIATE_TEST_SUITE_P(OptStress224, X64OptStress224RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress224, X86OptStress224RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress224, A64OptStress224RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress224, ARM32OptStress224RT, ::testing::ValuesIn(kARM), rtTCName);
