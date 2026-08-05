//===- AllPlatform_OptStress268RTTests.cpp - switch + calls at -O0 =======//
//
// switch dispatch interleaved with function calls at -O0 — combines the #509
// nested-switch register-reuse area with the #502/#508 "call near the table"
// area: case bodies call functions (clobbering caller-saved registers including
// the one that held the index), the index itself comes from a call, and calls
// sit between the index computation and the dispatch.  All at -O0 where every
// value is spilled around the calls.
//
//   * swcallbody - case bodies call functions (clobber the index register).
//   * swidxcall  - index = call_result combined with an inner loop counter.
//   * callsw     - a call result feeds the switch, inside a nested loop.
//   * swcallret  - case bodies call functions returning sub-width values.
//   * swcallarg  - case bodies call with the loop counter as an argument.
//   * mixcall    - switch dispatch + a direct-call accumulator + nested loop.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress268RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress268RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress268RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress268RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress268RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress268RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress268RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress268RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress268TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  std::string fns =
     "static int e0(int x){ return x*7+1; }\n"
     "static int e1(int x){ return x^0x5a5a5a5a; }\n"
     "static int e2(int x){ return (x>>3)+x; }\n"
     "static int e3(int x){ return x*0x10001; }\n";
  return {
    // Case bodies call functions (clobber the index register).
    {p+"_swcallbody",
     fns+
     t+" "+p+"_swcallbody("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int j=0;j<160;j++){ h=h*1103515245u+12345u; unsigned v=h&7u; unsigned r;\n"
     "    switch(v){ case 0:r=(unsigned)e0((int)h);break; case 1:r=(unsigned)e1((int)h);break;\n"
     "      case 2:r=(unsigned)e2((int)h);break; case 3:r=(unsigned)e3((int)h);break;\n"
     "      case 4:r=h+1u;break; case 5:r=~h;break; case 6:r=h*5u;break; default:r=h-1u;break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress268", 0},

    // index = call_result combined with an inner loop counter.
    {p+"_swidxcall",
     "static int pick(int x){ return (x ^ (x>>5)) & 7; }\n"
     +t+" "+p+"_swidxcall("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<32;i++){ h=h*1103515245u+12345u;\n"
     "   for(int j=0;j<5;j++){ unsigned v=(unsigned)pick((int)(h>>j)); unsigned r;\n"
     "    switch(v){ case 0:r=acc+1u;break; case 1:r=acc^h;break; case 2:r=acc*3u;break;\n"
     "      case 3:r=acc>>1;break; case 4:r=acc+h;break; case 5:r=~acc;break;\n"
     "      case 6:r=acc*5u;break; default:r=acc-(unsigned)j;break; }\n"
     "    acc=acc*131u+r; } }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress268", 0},

    // A call result feeds the switch, inside a nested loop.
    {p+"_callsw",
     "static int sel(int x,int y){ return (x*3+y) & 7; }\n"
     +t+" "+p+"_callsw("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*1103515245u+12345u;\n"
     "   for(int j=0;j<4;j++){ unsigned v=(unsigned)sel((int)h,j); unsigned r;\n"
     "    switch(v){ case 0:r=h+1u;break; case 1:r=h^0xffu;break; case 2:r=h*3u;break;\n"
     "      case 3:r=h>>2;break; case 4:r=h+9u;break; case 5:r=~h;break;\n"
     "      case 6:r=h*7u;break; default:r=h-(unsigned)j;break; }\n"
     "    acc=acc*131u+r; } }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress268", 0},

    // Case bodies call functions returning sub-width values.
    {p+"_swcallret",
     "static signed char rb(int x){ return (signed char)(x*5+3); }\n"
     "static short rh(int x){ return (short)(x^0x1234); }\n"
     +t+" "+p+"_swcallret("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int j=0;j<160;j++){ h=h*1103515245u+12345u; unsigned v=h&3u; int r;\n"
     "    switch(v){ case 0:r=(int)rb((int)h);break; case 1:r=(int)rh((int)h);break;\n"
     "      case 2:r=(int)(unsigned char)rb((int)(h>>4));break; default:r=(int)h;break; }\n"
     "    acc=acc*131+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress268", 0},

    // Case bodies call with the loop counter as an argument.
    {p+"_swcallarg",
     "static int comb(int x,int j){ return x + j*131 + (x>>j); }\n"
     +t+" "+p+"_swcallarg("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int j=0;j<160;j++){ h=h*1103515245u+12345u; unsigned v=h&3u; unsigned r;\n"
     "    switch(v){ case 0:r=(unsigned)comb((int)h,j&7);break; case 1:r=h^(unsigned)j;break;\n"
     "      case 2:r=(unsigned)comb((int)(h>>2),(j+1)&7);break; default:r=h;break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress268", 0},

    // switch dispatch + a direct-call accumulator + nested loop.
    {p+"_mixcall",
     "static int fin(int v){ return v*0x9e3779b9u + 1; }\n"
     +t+" "+p+"_mixcall("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<32;i++){ h=h*1103515245u+12345u;\n"
     "   for(int j=0;j<5;j++){ unsigned v=(h>>(j*2))&7u; unsigned r;\n"
     "    switch(v){ case 0:r=h+1u;break; case 1:r=h^0xa5u;break; case 2:r=h*3u;break;\n"
     "      case 3:r=h>>1;break; case 4:r=h+0x33u;break; case 5:r=~h;break;\n"
     "      case 6:r=h*5u;break; default:r=h-1u;break; }\n"
     "    acc=acc*131u + (unsigned)fin((int)r); } }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress268", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress268TC("x64o268", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress268TC("x86o268", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress268TC("a64o268", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress268TC("armo268", "int");

INSTANTIATE_TEST_SUITE_P(OptStress268, X64OptStress268RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress268, X86OptStress268RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress268, A64OptStress268RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress268, ARM32OptStress268RT, ::testing::ValuesIn(kARM), rtTCName);
