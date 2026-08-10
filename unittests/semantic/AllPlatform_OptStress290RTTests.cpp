//===- AllPlatform_OptStress290RTTests.cpp - control-flow probe ==========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O2 integer kernels stressing branchy control flow, nested loops and
// data-dependent iteration counts:
//
//   * nestloop  - 2D nested loop with a 3-way per-iteration branch.
//   * earlyexit - inner loop with a data-dependent early break.
//   * statemach - small switch-driven state machine.
//   * condacc   - branch-heavy conditional accumulation (pos/neg split).
//   * gcdloop   - subtractive Euclid GCD (bounded, no division).
//   * collatz   - Collatz sequence length (bounded, shift not divide).
//
// Every loop is bounded by a guard so iteration counts converge identically in
// the native and lifted builds; abs/negate stay in the unsigned domain and
// there is no division -- so the two builds agree bit-for-bit.  Four, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress290RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress290RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress290RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress290RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress290RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress290RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress290RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress290RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress290TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 2D nested loop with a 3-way per-iteration branch.
    {p+"_nestloop",
     t+" "+p+"_nestloop("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<24;i++) for(int j=0;j<24;j++){ h=h*1103515245u+12345u;\n"
     "    if((h&3u)==0u) acc+=h; else if((h&3u)==1u) acc^=h>>2; else acc=acc*131u+(unsigned)(i*j); }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress290", 2},

    // inner loop with a data-dependent early break.
    {p+"_earlyexit",
     t+" "+p+"_earlyexit("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int k=0;k<100;k++){ unsigned cnt=0;\n"
     "    for(int i=0;i<64;i++){ h=h*1103515245u+12345u; acc=acc*131u+h; cnt++; if((h&0xFFu)<8u) break; }\n"
     "    acc+=cnt*7u+(unsigned)k; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress290", 2},

    // small switch-driven state machine.
    {p+"_statemach",
     t+" "+p+"_statemach("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0; int st=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned in=h&7u;\n"
     "    switch(st){ case 0: acc+=in; st=(in&1u)?1:2; break;\n"
     "                case 1: acc^=in<<2; st=(in&2u)?2:0; break;\n"
     "                case 2: acc=acc*131u+in; st=(in&4u)?0:1; break;\n"
     "                default: st=0; }\n"
     "    acc+=(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress290", 2},

    // branch-heavy conditional accumulation (pos/neg split).
    {p+"_condacc",
     t+" "+p+"_condacc("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0; unsigned pos=0,neg=0;\n"
     "  for(int i=0;i<150;i++){ h=h*1103515245u+12345u; int v=(int)h;\n"
     "    if(v<0) neg+=(unsigned)(0u-(unsigned)v); else pos+=(unsigned)v;\n"
     "    if((h&7u)>4u) acc=acc*131u+pos; else acc=acc*131u+neg;\n"
     "    acc+=(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress290", 2},

    // subtractive Euclid GCD (bounded, no division).
    {p+"_gcdloop",
     t+" "+p+"_gcdloop("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int k=0;k<60;k++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=(h>>3)|1u, y=(h>>11)|1u; int guard=0;\n"
     "    while(x!=y && guard<200){ if(x>y) x-=y; else y-=x; guard++; }\n"
     "    acc=acc*131u+x+(unsigned)guard+(unsigned)k; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress290", 2},

    // Collatz sequence length (bounded, shift not divide).
    {p+"_collatz",
     t+" "+p+"_collatz("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int k=0;k<60;k++){ h=h*1103515245u+12345u;\n"
     "    unsigned n=(h&0xFFFFu)|1u; unsigned steps=0;\n"
     "    while(n!=1u && steps<500){ if(n&1u) n=3u*n+1u; else n>>=1; steps++; }\n"
     "    acc=acc*131u+steps+(unsigned)k; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress290", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress290TC("x64o290", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress290TC("x86o290", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress290TC("a64o290", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress290TC("armo290", "int");

INSTANTIATE_TEST_SUITE_P(OptStress290, X64OptStress290RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress290, X86OptStress290RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress290, A64OptStress290RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress290, ARM32OptStress290RT, ::testing::ValuesIn(kARM), rtTCName);
