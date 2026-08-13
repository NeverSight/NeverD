//===- AllPlatform_OptStress230RTTests.cpp - narrow value + flags + call =//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Combined stressor: each case piles three historically fragile features into
// one function -- a narrow (8/16-bit) sub-register value, a comparison whose
// flags are consumed across a branch, and a noinline helper call whose result
// flows through that control flow.  Latent bugs tend to hide in the seam where
// sub-register tracking (#157f), cross-block flag liveness (#161) and call-
// result propagation (#502) interact, not in any one alone.
//
//   * narrowcall- u8/u16 computed, passed to a helper, result used across a branch.
//   * subcall   - helper returns signed char; caller sign-extends across a branch.
//   * carrycall - add-with-carry; the carry feeds both a helper arg and a branch.
//   * mixretcall- helper returns {char,int}; fields used narrow + wide across CFG.
//   * ptrcall   - helper writes through a pointer to a caller local; read back.
//   * flagcall  - one compare feeds a select and a branch with a call between.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress230RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress230RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress230RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress230RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress230RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress230RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress230RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress230RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress230TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // u8/u16 value passed to a helper, result used across a branch.
    {p+"_narrowcall",
     "static unsigned "+p+"_mix(unsigned char,unsigned short) __attribute__((noinline));\n"
     +t+" "+p+"_narrowcall("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned char b=(unsigned char)(h>>3); unsigned short s=(unsigned short)(h>>11);\n"
     "    unsigned r="+p+"_mix(b,s);\n"
     "    if(b>s) acc+=r; else acc^=r;\n"
     "    acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static unsigned "+p+"_mix(unsigned char b,unsigned short s){ return (unsigned)b*7u+(unsigned)s; }\n",
     {0x12345u}, "OptStress230", 2},

    // Helper returns signed char; caller sign-extends across a branch.
    {p+"_subcall",
     "static signed char "+p+"_sc(int) __attribute__((noinline));\n"
     +t+" "+p+"_subcall("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    signed char c="+p+"_sc((int)h);\n"
     "    int v; if(c<0) v=-(int)c; else v=(int)c*3;\n"
     "    acc=acc*131u+(unsigned)v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static signed char "+p+"_sc(int x){ return (signed char)(x>>7); }\n",
     {0x23456u}, "OptStress230", 2},

    // Add-with-carry; the carry feeds both a helper arg and a branch.
    {p+"_carrycall",
     "static unsigned "+p+"_cc(unsigned,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_carrycall("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0, carry=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned s; unsigned c=__builtin_add_overflow(h, carry, &s);\n"
     "    unsigned r="+p+"_cc(s, c);\n"
     "    if(c) acc+=r; else acc-=r;\n"
     "    carry=c; acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static unsigned "+p+"_cc(unsigned s,unsigned c){ return s*3u + c*0x9E3779B9u; }\n",
     {0x34567u}, "OptStress230", 2},

    // Helper returns {char,int}; fields used narrow + wide across CFG.
    {p+"_mixretcall",
     "typedef struct{signed char c; int w;}"+p+"_CW;\n"
     "static "+p+"_CW "+p+"_mk(int) __attribute__((noinline));\n"
     +t+" "+p+"_mixretcall("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_CW r="+p+"_mk((int)h);\n"
     "    int v; if(r.c<0) v=r.w-(int)r.c; else v=r.w+(int)r.c*5;\n"
     "    acc=acc*131u+(unsigned)v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static "+p+"_CW "+p+"_mk(int x){ "+p+"_CW r; r.c=(signed char)(x>>9); r.w=x*3+1; return r; }\n",
     {0x45678u}, "OptStress230", 2},

    // Helper writes through a pointer to a caller local; read back.
    {p+"_ptrcall",
     "static void "+p+"_wr(unsigned*,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_ptrcall("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0; unsigned slot;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_wr(&slot, h);\n"
     "    if(slot&1u) acc+=slot; else acc^=slot;\n"
     "    acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static void "+p+"_wr(unsigned*p,unsigned v){ *p = v*2654435761u + 1u; }\n",
     {0x56789u}, "OptStress230", 2},

    // One compare feeds a select and a branch with a call in between.
    {p+"_flagcall",
     "static unsigned "+p+"_id(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_flagcall("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)h, y=(int)(h>>6);\n"
     "    int m=(x<y)?x:y;\n"
     "    unsigned r="+p+"_id((unsigned)m);\n"
     "    if(x<y) acc+=r; else acc-=r;\n"
     "    acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static unsigned "+p+"_id(unsigned v){ return v ^ 0x5bd1e995u; }\n",
     {0x6789Au}, "OptStress230", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress230TC("x64o230", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress230TC("x86o230", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress230TC("a64o230", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress230TC("armo230", "int");

INSTANTIATE_TEST_SUITE_P(OptStress230, X64OptStress230RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress230, X86OptStress230RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress230, A64OptStress230RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress230, ARM32OptStress230RT, ::testing::ValuesIn(kARM), rtTCName);
