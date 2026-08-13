//===- AllPlatform_OptStress261RTTests.cpp - recursion / return width -O0 //
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Recursion and call-return-width variety at -O0 — the direct neighbor of the
// #508 bug (ARM32 -O0 recursive int return mis-typed as an i64 register pair).
// At -O0 the callee's result and the next call's argument live in overlapping
// registers with dead PHIs, exactly the shape that mis-classifies return width.
// This drives that area across all four targets with several recursion / chain /
// cross-block-consume shapes plus a genuine long-long (register-pair) return.
//
//   * recacc   - tail-recursive int accumulator (the #508 shape, fixed).
//   * recmut   - mutual recursion (FA<->FB) threading an accumulator.
//   * recll    - recursion returning long long (register pair on 32-bit).
//   * recbr    - recursive int result consumed in both arms of a branch.
//   * rectwo   - recursive int result consumed twice after the call.
//   * callw    - char->short->int return-width chain (sign-extend across calls).
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  Fixed
// (data-independent) recursion depth so emulation stays bounded.  All four
// targets, -O0.  Only add/sub/mul/xor/shift-by-const, so the long-long path
// stays libcall-free on i386/ARM32.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress261RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress261RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress261RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress261RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress261RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress261RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress261RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress261RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress261TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Tail-recursive int accumulator (the #508 recuracc shape).
    {p+"_recacc",
     "static int RA(int n,int acc){ if(n<=0) return acc; return RA(n-1, acc*31+n); }\n"
     +t+" "+p+"_recacc("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u; acc=acc*131u + (unsigned)RA(20,(int)(h&0xffffu)); }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress261", 0},

    // Mutual recursion (FA<->FB) threading an accumulator.
    {p+"_recmut",
     "static int FB(int n,int s); static int FA(int n,int s){ if(n<=0) return s; return FB(n-1, s+n); }\n"
     "static int FB(int n,int s){ if(n<=0) return s; return FA(n-1, s*2-n); }\n"
     +t+" "+p+"_recmut("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u; acc=acc*131u + (unsigned)FA(18,(int)(h&0x7fffu)); }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress261", 0},

    // Recursion returning long long (register pair on 32-bit), folded to int.
    {p+"_recll",
     "static long long RL(int n,long long acc){ if(n<=0) return acc;\n"
     "  return RL(n-1, (acc<<5) ^ (acc>>2) ^ ((long long)n<<33)); }\n"
     +t+" "+p+"_recll("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    long long v=RL(18,(long long)(int)h); acc=acc*131u + (unsigned)v + (unsigned)(v>>32); }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress261", 0},

    // Recursive int result consumed in both arms of a branch.
    {p+"_recbr",
     "static int RB(int n,int acc){ if(n<=0) return acc; return RB(n-1, acc*3+n); }\n"
     +t+" "+p+"_recbr("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    int r=RB(16,(int)(h&0x7fffu)); int s=(h&1u)?(r*2+1):(r-7);\n"
     "    acc=acc*131u + (unsigned)s; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress261", 0},

    // Recursive int result consumed twice after the call.
    {p+"_rectwo",
     "static int RT(int n,int acc){ if(n<=0) return acc; return RT(n-1, acc + (n^acc)); }\n"
     +t+" "+p+"_rectwo("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    int r=RT(14,(int)(h&0xffffu)); acc=acc*131u + (unsigned)r + (unsigned)(r*r); }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress261", 0},

    // char->short->int return-width chain (sign-extend across calls).
    {p+"_callw",
     "static signed char CC(int x){ return (signed char)(x*7+1); }\n"
     "static short CS(int x){ return (short)(CC(x) + x*131); }\n"
     "static int CI(int x){ return (int)CS(x) + x*65537; }\n"
     +t+" "+p+"_callw("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u; acc=acc*131u + (unsigned)CI((int)h); }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress261", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress261TC("x64o261", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress261TC("x86o261", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress261TC("a64o261", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress261TC("armo261", "int");

INSTANTIATE_TEST_SUITE_P(OptStress261, X64OptStress261RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress261, X86OptStress261RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress261, A64OptStress261RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress261, ARM32OptStress261RT, ::testing::ValuesIn(kARM), rtTCName);
