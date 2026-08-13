//===- AllPlatform_OptStress262RTTests.cpp - indirect calls at -O0 =======//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Function-pointer / indirect calls at -O0 — the neighbor of the #506 indirect-
// call ABI bug (i386 wide stack arg truncated through INDIR_CALL).  At -O0 the
// code-pointer table and the selected pointer round-trip through the frame, and
// the indirect call's ABI (arg marshaling, return width) must survive CFG
// recovery and code-pointer symbolization to the recompiled functions.
//
//   * icglob  - indirect call through a global const function-pointer table.
//   * icarg   - function pointer passed as an argument, then called.
//   * icret   - indirect callee returns a sub-width value (sign-extend).
//   * icsel   - pointer chosen by a branch, called past the merge.
//   * ic3arg  - indirect call with three int args (arg marshaling).
//   * icmix   - table dispatch combined with a direct call accumulator.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress262RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress262RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress262RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress262RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress262RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress262RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress262RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress262RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress262TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  std::string ops =
     "static int oadd(int a,int b){ return a+b; }\n"
     "static int osub(int a,int b){ return a-b; }\n"
     "static int omul(int a,int b){ return a*b; }\n"
     "static int oxor(int a,int b){ return a^b; }\n";
  return {
    // Indirect call through a global const function-pointer table.
    {p+"_icglob",
     ops+
     "typedef int (*OpFn)(int,int);\n"
     "static const OpFn OPS[4]={oadd,osub,omul,oxor};\n"
     +t+" "+p+"_icglob("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    OpFn f=OPS[h&3u]; acc=acc*131u + (unsigned)f((int)h,(int)(h>>7)); }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress262", 0},

    // Function pointer passed as an argument, then called.
    {p+"_icarg",
     ops+
     "typedef int (*OpFn)(int,int);\n"
     "static int apply(OpFn f,int x,int y){ return f(x,y)+1; }\n"
     +t+" "+p+"_icarg("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  OpFn t4[4]={oadd,osub,omul,oxor};\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    acc=acc*131u + (unsigned)apply(t4[h&3u],(int)h,(int)(h>>5)); }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress262", 0},

    // Indirect callee returns a sub-width value (sign-extend).
    {p+"_icret",
     "static signed char rb(int x){ return (signed char)(x*5+3); }\n"
     "static signed char rc(int x){ return (signed char)(x^0x5a); }\n"
     +t+" "+p+"_icret("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  signed char (*tb[2])(int)={rb,rc};\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    signed char r=tb[h&1u]((int)h); acc=acc*131 + (int)r; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress262", 0},

    // Pointer chosen by a branch, called past the merge.
    {p+"_icsel",
     ops+
     "typedef int (*OpFn)(int,int);\n"
     +t+" "+p+"_icsel("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    OpFn f; if(h&8u) f=(h&1u)?oadd:osub; else f=(h&2u)?omul:oxor;\n"
     "    acc=acc*131u + (unsigned)f((int)h,(int)(h>>9)); }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress262", 0},

    // Indirect call with three int args (arg marshaling).
    {p+"_ic3arg",
     "static int t3a(int a,int b,int c){ return a+b*2+c*3; }\n"
     "static int t3b(int a,int b,int c){ return a^b^c; }\n"
     +t+" "+p+"_ic3arg("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  int (*tt[2])(int,int,int)={t3a,t3b};\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    acc=acc*131u + (unsigned)tt[h&1u]((int)h,(int)(h>>4),(int)(h>>9)); }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress262", 0},

    // Table dispatch combined with a direct call accumulator.
    {p+"_icmix",
     ops+
     "typedef int (*OpFn)(int,int);\n"
     "static const OpFn OPS2[4]={oxor,omul,osub,oadd};\n"
     "static int finalize(int v){ return v*2654435761u + 1; }\n"
     +t+" "+p+"_icmix("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    int v=OPS2[(h>>2)&3u]((int)h,(int)(h>>6)); acc=acc*131u + (unsigned)finalize(v); }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress262", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress262TC("x64o262", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress262TC("x86o262", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress262TC("a64o262", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress262TC("armo262", "int");

INSTANTIATE_TEST_SUITE_P(OptStress262, X64OptStress262RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress262, X86OptStress262RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress262, A64OptStress262RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress262, ARM32OptStress262RT, ::testing::ValuesIn(kARM), rtTCName);
