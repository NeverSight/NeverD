//===- AllPlatform_OptStress322RTTests.cpp - -O3 control flow ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O3 control-flow shapes: jump-table generation, branch-to-select conversion,
// loop rotation/unswitching and nested CFG differ markedly from the -O0/-O2
// forms prior switch/jump-table probes used (#508-#511).  All integer, LCG
// seeded, folded single return; 32-bit targets stay libcall-free (no i64 div,
// no i64 variable shift).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress322RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress322RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress322RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress322RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress322RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress322RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress322RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress322RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress322TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // Dense switch in a loop (jump table), each case mutates the accumulator
    // differently; selector derived from the running hash.
    {p+"_densesw",
     t+" "+p+"_densesw("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=(unsigned)a;\n"
     "  for(int i=0;i<96;i++){ w=w*1103515245u+12345u;\n"
     "    switch((w>>5)&15){\n"
     "    case 0: acc+=w; break; case 1: acc-=w; break; case 2: acc^=w; break;\n"
     "    case 3: acc=acc*3+1; break; case 4: acc+=(long long)(int)w*5; break;\n"
     "    case 5: acc^=acc>>13; break; case 6: acc+=w&0xff; break;\n"
     "    case 7: acc-=(w>>3); break; case 8: acc=~acc; break;\n"
     "    case 9: acc+=(long long)w<<4; break; case 10: acc^=0x5a5a5a5a; break;\n"
     "    case 11: acc+=i; break; case 12: acc=(acc<<7)|(acc>>57); break;\n"
     "    case 13: acc-=(long long)(int)w; break; case 14: acc+=w*7; break;\n"
     "    default: acc^=(long long)w*131; }\n"
     "    acc ^= acc>>27; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x1234u}, "OptStress322", Opt},

    // Nested branches that -O3 converts to select/cmov chains.
    {p+"_selchain",
     t+" "+p+"_selchain("+t+" a){ unsigned w=(unsigned)a^0x77u; long long acc=0;\n"
     "  for(int i=0;i<80;i++){ w=w*22695477u+1u; int x=(int)w;\n"
     "    long long t1 = (x>0)? (long long)x*3 : (long long)x+7;\n"
     "    long long t2 = (x&1)? (t1^0xff) : (t1<<2);\n"
     "    long long t3 = ((unsigned)x>0x80000000u)? (t2-13) : (t2+x);\n"
     "    acc += (x>(int)(w>>8))? t3 : -t3; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x2345u}, "OptStress322", Opt},

    // Short-circuit boolean chains driving a branch (and/or fold).
    {p+"_boolsc",
     t+" "+p+"_boolsc("+t+" a){ unsigned w=(unsigned)a+0x9u; long long acc=0;\n"
     "  for(int i=0;i<80;i++){ w=w*1664525u+1013904223u; int x=(int)w;\n"
     "    if((x>0 && (x&3)==0) || ((unsigned)x>0xC0000000u && (x>>5)!=0))\n"
     "      acc += (long long)x*5;\n"
     "    else if(x<0 && ((x^i)&1)) acc -= x;\n"
     "    else acc ^= (long long)x<<3; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x3456u}, "OptStress322", Opt},

    // Loop with early-exit search + do/while continue (loop rotation at -O3).
    {p+"_loopexit",
     t+" "+p+"_loopexit("+t+" a){ unsigned w=(unsigned)a|3u; long long acc=0; int found=-1;\n"
     "  for(int i=0;i<200;i++){ w=w*1103515245u+12345u;\n"
     "    if(((w>>7)&0x3ff)==0x123){ found=i; break; }\n"
     "    acc += (long long)(int)w; if((w&7)==0) continue; acc ^= acc>>17; }\n"
     "  return ("+t+")((acc ^ (acc>>32)) + found*1000); }\n",
     {0x4567u}, "OptStress322", Opt},

    // Sparse switch with duplicated targets (case clustering at -O3).
    {p+"_sparsesw",
     t+" "+p+"_sparsesw("+t+" a){ unsigned w=(unsigned)a^0xa5u; long long acc=(unsigned)a;\n"
     "  for(int i=0;i<96;i++){ w=w*22695477u+1u;\n"
     "    switch(w&0xff){\n"
     "    case 0: case 17: case 200: acc+=w; break;\n"
     "    case 5: case 99: acc-=(long long)(int)w; break;\n"
     "    case 42: acc=acc*7+3; break;\n"
     "    case 128: case 255: acc^=(long long)w<<8; break;\n"
     "    default: acc+=(w&0x3f); }\n"
     "    acc ^= acc>>23; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x5678u}, "OptStress322", Opt},

    // Nested loops with two accumulators, break/continue interplay.
    {p+"_nested2",
     t+" "+p+"_nested2("+t+" a){ unsigned w=(unsigned)a+0x55u; long long acc=0,acc2=0;\n"
     "  for(int i=0;i<24;i++){ for(int j=0;j<24;j++){ w=w*1664525u+1013904223u;\n"
     "    if((w&15)==0) continue; if(((w>>9)&0x1ff)==0x1ff) break;\n"
     "    acc += (long long)(int)w * (j+1); acc2 ^= (long long)((int)w << (j&7)); }\n"
     "    acc -= acc2>>3; }\n"
     "  return ("+t+")((acc ^ (acc>>32)) + (acc2 ^ (acc2>>32))); }\n",
     {0x6789u}, "OptStress322", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress322TC("x64o322", "long", 3);
static const std::vector<RoundTripTC> kX86 = makeOptStress322TC("x86o322", "int", 3);
static const std::vector<RoundTripTC> kA64 = makeOptStress322TC("a64o322", "long", 3);
static const std::vector<RoundTripTC> kARM = makeOptStress322TC("armo322", "int", 3);

INSTANTIATE_TEST_SUITE_P(OptStress322, X64OptStress322RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress322, X86OptStress322RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress322, A64OptStress322RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress322, ARM32OptStress322RT, ::testing::ValuesIn(kARM), rtTCName);
