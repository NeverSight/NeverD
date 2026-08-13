//===- AllPlatform_OptStress279RTTests.cpp - scalar call results -O0 =====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Scalar-returning calls whose result is consumed across control flow at -O0 —
// the integer-return analog of the FP/struct call-result propagation that
// #502 hardened.  At -O0 the call result is spilled to a frame slot and reloaded
// at each use, so a result consumed in both arms of an if, fed to a switch,
// passed to a second call, or accumulated in a nested loop exercises the call-
// return modeling and cross-block value tracking on the explicit form.
//
//   * brconsume - int result consumed in both arms of an if + after.
//   * subret    - signed char result sign-extended across a branch.
//   * swret     - call result feeds a switch selector.
//   * twocall   - two calls combined under a branch.
//   * callarg   - one call's result feeds a second call's arg under a branch.
//   * loopret   - call result threaded through a nested loop accumulator.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress279RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress279RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress279RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress279RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress279RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress279RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress279RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress279RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress279TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // int result consumed in both arms of an if + once more after.
    {p+"_brconsume",
     "static int __attribute__((noinline)) "+p+"_g(int x){ return x*1103515245+12345; }\n"
     +t+" "+p+"_brconsume("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    int r="+p+"_g((int)h); if(h&1u) acc+=r; else acc-=r*2; acc=acc*3+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress279", 0},

    // signed char result sign-extended across a branch.
    {p+"_subret",
     "static signed char __attribute__((noinline)) "+p+"_sc(int x){ return (signed char)(x^0x5a); }\n"
     +t+" "+p+"_subret("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    signed char c="+p+"_sc((int)h); int e=(int)c; if(c<0) acc-=e; else acc+=e; acc=acc*3+e; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress279", 0},

    // call result feeds a switch selector.
    {p+"_swret",
     "static int __attribute__((noinline)) "+p+"_w(int x){ return (x>>3)&7; }\n"
     +t+" "+p+"_swret("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u; int k="+p+"_w((int)h); int v;\n"
     "    switch(k){ case 0:v=(int)h;break; case 1:v=(int)h*3;break; case 2:v=(int)h^7;break;\n"
     "      case 3:v=(int)h>>1;break; case 4:v=(int)h+9;break; case 5:v=(int)h*5;break;\n"
     "      case 6:v=(int)(h&0xffu);break; default:v=~(int)h; }\n"
     "    acc=acc*131+v; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress279", 0},

    // two calls combined under a branch.
    {p+"_twocall",
     "static int __attribute__((noinline)) "+p+"_pp(int x){ return x*7+1; }\n"
     "static int __attribute__((noinline)) "+p+"_qq(int x){ return x*3-5; }\n"
     +t+" "+p+"_twocall("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    int u="+p+"_pp((int)h), v="+p+"_qq((int)(h>>4));\n"
     "    acc=acc*131 + ((h&1u)?u+v:u-v); }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress279", 0},

    // one call's result feeds a second call's arg under a branch.
    {p+"_callarg",
     "static int __attribute__((noinline)) "+p+"_ff(int x){ return x*131+7; }\n"
     +t+" "+p+"_callarg("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    int r="+p+"_ff((int)h); if(h&2u) r="+p+"_ff(r); acc=acc*3+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress279", 0},

    // call result threaded through a nested-loop accumulator.
    {p+"_loopret",
     "static int __attribute__((noinline)) "+p+"_mm(int x,int y){ return x*y+x-y; }\n"
     +t+" "+p+"_loopret("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*1103515245u+12345u; int s=0;\n"
     "    for(int j=0;j<4;j++) s="+p+"_mm(s,(int)(h>>(j*4)));\n"
     "    acc=acc*131+s; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress279", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress279TC("x64o279", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress279TC("x86o279", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress279TC("a64o279", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress279TC("armo279", "int");

INSTANTIATE_TEST_SUITE_P(OptStress279, X64OptStress279RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress279, X86OptStress279RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress279, A64OptStress279RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress279, ARM32OptStress279RT, ::testing::ValuesIn(kARM), rtTCName);
