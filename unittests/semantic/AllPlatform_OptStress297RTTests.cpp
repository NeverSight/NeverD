//===- AllPlatform_OptStress297RTTests.cpp - compare/sort-network probe ===//
//
// -O2 integer kernels stressing compare-swap networks, min/max pairs and
// sorting-network codegen paths (no division, bounded loops):
//
//   * minmax2  - pairwise min/max reduction over array.
//   * bubbles3 - 3-element bubble-sort network (fixed depth).
//   * sortnet4 - 4-element sorting network (Batcher).
//   * median5  - median-of-5 via partial sort network.
//   * rank3    - rank of value among 3 elements (0/1/2 counts).
//   * topswap  - conditional swap-if-greater chain.
//
// All comparisons use plain int/unsigned, all loops bounded, and there is no
// division -- so native and lifted agree bit-for-bit.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress297RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress297RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress297RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress297RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress297RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress297RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress297RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress297RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress297TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // pairwise min/max reduction over array.
    {p+"_minmax2",
     t+" "+p+"_minmax2("+t+" a){ unsigned buf[32]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<32;i++){ h=h*1103515245u+12345u; buf[i]=h; }\n"
     "  unsigned mn=buf[0], mx=buf[0];\n"
     "  for(int i=1;i<32;i++){ if(buf[i]<mn) mn=buf[i]; if(buf[i]>mx) mx=buf[i]; }\n"
     "  return ("+t+")(mn*131u+mx); }\n",
     {0x12345u}, "OptStress297", 2},

    // 3-element bubble-sort network (fixed depth).
    {p+"_bubbles3",
     t+" "+p+"_bubbles3("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int k=0;k<50;k++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h, y=h>>8, z=h>>16;\n"
     "    if(x>y){ unsigned t=x; x=y; y=t; } if(y>z){ unsigned t=y; y=z; z=t; } if(x>y){ unsigned t=x; x=y; y=t; }\n"
     "    acc=acc*131u+x+y+z+(unsigned)k; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress297", 2},

    // 4-element sorting network (Batcher).
    {p+"_sortnet4",
     t+" "+p+"_sortnet4("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int k=0;k<40;k++){ h=h*1103515245u+12345u;\n"
     "    unsigned v0=h&0xFFu,v1=(h>>8)&0xFFu,v2=(h>>16)&0xFFu,v3=(h>>24)&0xFFu;\n"
     "    #define CS(a,b) if(a>b){unsigned t=a;a=b;b=t;}\n"
     "    CS(v0,v2) CS(v1,v3) CS(v0,v1) CS(v2,v3) CS(v1,v2)\n"
     "    #undef CS\n"
     "    acc=acc*131u+v0+v1+v2+v3+(unsigned)k; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress297", 2},

    // median-of-5 via partial sort network.
    {p+"_median5",
     t+" "+p+"_median5("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int k=0;k<40;k++){ h=h*1103515245u+12345u;\n"
     "    unsigned v[5]={h&0xFFu,(h>>4)&0xFFu,(h>>8)&0xFFu,(h>>12)&0xFFu,(h>>16)&0xFFu};\n"
     "    for(int p=0;p<5;p++) for(int q=p+1;q<5;q++) if(v[p]>v[q]){unsigned t=v[p];v[p]=v[q];v[q]=t;}\n"
     "    acc=acc*131u+v[2]+(unsigned)k; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress297", 2},

    // rank of value among 3 elements (0/1/2 counts).
    {p+"_rank3",
     t+" "+p+"_rank3("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int k=0;k<60;k++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h&0xFFu,y=(h>>8)&0xFFu,z=(h>>16)&0xFFu;\n"
     "    unsigned r=(x<y)+(x<z)+(y<z); acc=acc*131u+r+(unsigned)k; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress297", 2},

    // conditional swap-if-greater chain.
    {p+"_topswap",
     t+" "+p+"_topswap("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  unsigned top=0, second=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned v=h&0xFFFFu;\n"
     "    if(v>top){ second=top; top=v; } else if(v>second){ second=v; }\n"
     "    acc=acc*131u+top+second+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress297", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress297TC("x64o297", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress297TC("x86o297", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress297TC("a64o297", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress297TC("armo297", "int");

INSTANTIATE_TEST_SUITE_P(OptStress297, X64OptStress297RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress297, X86OptStress297RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress297, A64OptStress297RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress297, ARM32OptStress297RT, ::testing::ValuesIn(kARM), rtTCName);
