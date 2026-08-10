//===- AllPlatform_OptStress276RTTests.cpp - nested loop CFG at -O0 ======//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Deeply nested loops with break / continue / early-exit and several loop-
// carried accumulators at -O0 — exercises irreducible-free but dense control
// flow where every induction variable and accumulator is a frame slot reloaded
// each iteration, stressing the lifter's CFG recovery, PHI placement, and
// loop-carried value tracking on the un-cleaned form (no LICM/unroll).
//
//   * nest3   - triple nested loop, two carried accumulators, continue + break.
//   * search  - early-exit nested search returning first match coordinates.
//   * dowhile - do/while with continue, data-dependent trip count.
//   * brkcont - break from inner, continue in outer, mixed.
//   * multiacc- one loop, four accumulators updated under different conditions.
//   * sweep   - forward then backward sweep over a local array.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress276RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress276RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress276RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress276RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress276RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress276RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress276RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress276RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress276TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // triple nested loop, two carried accumulators, continue + break.
    {p+"_nest3",
     t+" "+p+"_nest3("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0,s1=1u,s2=0u;\n"
     "  for(int i=0;i<12;i++){ for(int j=0;j<12;j++){ for(int k=0;k<12;k++){\n"
     "    h=h*1103515245u+12345u; if((h&7u)==0u) continue; if((h&0x3f0u)==0x3f0u) break;\n"
     "    s1=s1*3u+h; s2^=h>>3; } acc=acc*131u+s1; } acc=acc*7u+s2; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress276", 0},

    // early-exit nested search returning the first match position.
    {p+"_search",
     t+" "+p+"_search("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int rep=0;rep<48;rep++){ h=h*1103515245u+12345u; unsigned tgt=h&0x3fu; int found=-1;\n"
     "    for(int i=0;i<8&&found<0;i++) for(int j=0;j<8;j++){\n"
     "      unsigned cell=((unsigned)(i*8+j)*2654435761u)&0x3fu;\n"
     "      if(cell==tgt){ found=i*8+j; break; } }\n"
     "    acc=acc*131u+(unsigned)(found+1); }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress276", 0},

    // do/while with continue, data-dependent trip count.
    {p+"_dowhile",
     t+" "+p+"_dowhile("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int rep=0;rep<96;rep++){ h=h*1103515245u+12345u; unsigned v=h|1u; unsigned s=0;\n"
     "    do{ if(v&1u){ s+=v; } v>>=1; if((s&0xfu)==0xfu) continue; s^=v; }while(v>3u);\n"
     "    acc=acc*131u+s; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress276", 0},

    // break from inner, continue in outer, mixed.
    {p+"_brkcont",
     t+" "+p+"_brkcont("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    if((h&3u)==0u) continue; unsigned s=0;\n"
     "    for(int j=0;j<32;j++){ if(((h>>j)&1u)&&(j>20)) break; s+=(h>>j)&1u; }\n"
     "    acc=acc*131u+s; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress276", 0},

    // one loop, four accumulators updated under different conditions.
    {p+"_multiacc",
     t+" "+p+"_multiacc("+t+" a){ unsigned h=(unsigned)a; unsigned a0=0,a1=1u,a2=2u,a3=3u;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    if(h&1u) a0+=h; if(h&2u) a1*=3u; if(h&4u) a2^=h>>5; if(!(h&8u)) a3-=h>>7; }\n"
     "  return ("+t+")(a0^a1^a2^a3); }\n",
     {0x56789u}, "OptStress276", 0},

    // forward then backward sweep over a local array (two passes, opposite dirs).
    {p+"_sweep",
     t+" "+p+"_sweep("+t+" a){ unsigned h=(unsigned)a; unsigned buf[32]; unsigned acc=0;\n"
     "  for(int j=0;j<32;j++){ h=h*1103515245u+12345u; buf[j]=h; }\n"
     "  for(int j=1;j<32;j++) buf[j]+=buf[j-1];\n"
     "  for(int j=30;j>=0;j--) buf[j]^=buf[j+1];\n"
     "  for(int j=0;j<32;j++) acc=acc*131u+buf[j];\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress276", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress276TC("x64o276", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress276TC("x86o276", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress276TC("a64o276", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress276TC("armo276", "int");

INSTANTIATE_TEST_SUITE_P(OptStress276, X64OptStress276RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress276, X86OptStress276RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress276, A64OptStress276RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress276, ARM32OptStress276RT, ::testing::ValuesIn(kARM), rtTCName);
