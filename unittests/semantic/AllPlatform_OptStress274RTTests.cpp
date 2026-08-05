//===- AllPlatform_OptStress274RTTests.cpp - boolean/compare chains -O0 ==//
//
// Short-circuit boolean chains, nested ternaries, and comparison-result
// arithmetic at -O0 — the dual of the -O2 compare-folding probe OptStress220.
// At -O0 each && / || / ?: emits an explicit cmp + setcc / conditional branch
// instead of a folded branchless select, and a single compare often feeds both
// a select and a branch.  This stresses MedFlags folding (COND_BR / SETcc /
// SELECT passes) and the boolean->integer materialization path on the explicit,
// un-cleaned form where the historical flag bugs hid.
//
//   * andor    - && / || short-circuit chains driving branches.
//   * tern     - nested ternary producing a small signed code.
//   * cmpsum   - several comparison booleans summed with weights.
//   * dualuse  - one compare feeds both a select and a branch (#161 cross-use).
//   * notchain - logical-not chains (!x, !!x, !(x>0)).
//   * range    - chained range checks bucketing a signed value.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress274RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress274RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress274RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress274RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress274RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress274RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress274RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress274RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress274TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // && / || short-circuit chains driving branches.
    {p+"_andor",
     t+" "+p+"_andor("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)(h&0xffffu)-0x8000; int y=(int)((h>>16)&0xffffu)-0x8000;\n"
     "    if(x>0 && y>0 && (x+y)<10000) acc+=x+y;\n"
     "    else if(x<0 || y<0) acc-=1; else acc+=2;\n"
     "    acc=acc*3 + ((x>y)&&(y>0)); }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress274", 0},

    // nested ternary producing a small signed code.
    {p+"_tern",
     t+" "+p+"_tern("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)h;\n"
     "    int r = (x<0) ? ((x<-1000000)?-2:-1) : ((x>1000000)?2:((x>0)?1:0));\n"
     "    acc=acc*131 + r; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress274", 0},

    // several comparison booleans summed with weights (signed + unsigned).
    {p+"_cmpsum",
     t+" "+p+"_cmpsum("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h, y=h*2654435761u;\n"
     "    int s=(x<y)+(x>y)*2+(x==y)*4+((int)x<(int)y)*8+((int)x<0)*16;\n"
     "    acc=acc*131 + s; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress274", 0},

    // one compare feeds both a select and a branch (#161 cross-use form).
    {p+"_dualuse",
     t+" "+p+"_dualuse("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)h, y=(int)(h>>1); int c=(x<y);\n"
     "    int m=c?x:y; if(c) acc+=m; else acc-=m; acc=acc*3+c; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress274", 0},

    // logical-not chains (!x, !!x, !(x>0)).
    {p+"_notchain",
     t+" "+p+"_notchain("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)(h&0x7u)-3; int b=!x; int b2=!!x; int b3=!(x>0);\n"
     "    if(!(x&1) && !b) acc+=1; acc=acc*3 + b + b2*2 + b3*4; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress274", 0},

    // chained range checks bucketing a signed value.
    {p+"_range",
     t+" "+p+"_range("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)(h&0xffffu)-0x8000;\n"
     "    int bucket; if(x<-1000) bucket=0; else if(x<0) bucket=1; else if(x<1000) bucket=2; else bucket=3;\n"
     "    int inrange = (x>=-500 && x<=500);\n"
     "    acc=acc*131 + bucket*4 + inrange; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress274", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress274TC("x64o274", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress274TC("x86o274", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress274TC("a64o274", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress274TC("armo274", "int");

INSTANTIATE_TEST_SUITE_P(OptStress274, X64OptStress274RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress274, X86OptStress274RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress274, A64OptStress274RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress274, ARM32OptStress274RT, ::testing::ValuesIn(kARM), rtTCName);
