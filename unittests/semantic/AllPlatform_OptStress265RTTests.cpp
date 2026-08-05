//===- AllPlatform_OptStress265RTTests.cpp - switch index sources at -O0 =//
//
// switch / jump-table dispatch where the selector comes from non-trivial sources
// at -O0 — the neighbor of the #508 jump-table fix (ARM32 -O0 PIC table whose
// index was spilled to the stack and disconnected from its bound guard).  Here
// the selector is produced by a call return, a rodata load, or a multi-op
// computation, each of which round-trips through the frame at -O0 before driving
// the table, stressing index-to-guard reconnection across more sources.
//
//   * swcall  - selector is a function return value.
//   * swload  - selector is loaded from a rodata table.
//   * swcomp  - selector is a multi-op computation (mul/xor/shift).
//   * swnest  - switch inside a nested loop with a carried accumulator.
//   * swwide  - selector derived from a 64-bit value's low bits.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit ops in the table arms, so i386/ARM32 stay
// libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress265RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress265RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress265RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress265RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress265RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress265RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress265RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress265RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress265TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Selector is a function return value.
    {p+"_swcall",
     "static int pick(int x){ return (x ^ (x>>7) ^ (x>>13)) & 7; }\n"
     +t+" "+p+"_swcall("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; int v=pick((int)h); unsigned r;\n"
     "    switch(v){ case 0:r=h+1u;break; case 1:r=h^0xffu;break; case 2:r=h*3u;break;\n"
     "      case 3:r=h>>2;break; case 4:r=h+0x55u;break; case 5:r=~h;break;\n"
     "      case 6:r=h*5u+1u;break; default:r=h-7u;break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress265", 0},

    // Selector is loaded from a rodata table.
    {p+"_swload",
     "static const unsigned char SEL[16]={0,1,2,3,4,5,6,7,7,6,5,4,3,2,1,0};\n"
     +t+" "+p+"_swload("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned v=SEL[h&15u]; unsigned r;\n"
     "    switch(v){ case 0:r=h+1u;break; case 1:r=h^0x33u;break; case 2:r=h*3u;break;\n"
     "      case 3:r=h>>3;break; case 4:r=h+9u;break; case 5:r=~h;break;\n"
     "      case 6:r=h*7u;break; default:r=h-3u;break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress265", 0},

    // Selector is a multi-op computation.
    {p+"_swcomp",
     t+" "+p+"_swcomp("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned v=((h*2654435761u) ^ (h>>17)) & 7u; unsigned r;\n"
     "    switch(v){ case 0:r=h+1u;break; case 1:r=h^0xa5u;break; case 2:r=h*3u;break;\n"
     "      case 3:r=h>>1;break; case 4:r=h+0x1234u;break; case 5:r=~h;break;\n"
     "      case 6:r=h*9u;break; default:r=h-5u;break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress265", 0},

    // switch inside a nested loop with a carried accumulator.
    {p+"_swnest",
     t+" "+p+"_swnest("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*1103515245u+12345u;\n"
     "    for(int j=0;j<5;j++){ unsigned v=(h>>(j*2))&7u; unsigned r;\n"
     "      switch(v){ case 0:r=acc+1u;break; case 1:r=acc^h;break; case 2:r=acc*3u;break;\n"
     "        case 3:r=acc>>1;break; case 4:r=acc+h;break; case 5:r=~acc;break;\n"
     "        case 6:r=acc*5u;break; default:r=acc-j;break; }\n"
     "      acc=acc*131u+r; } }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress265", 0},

    // Selector derived from a 64-bit value's low bits.
    {p+"_swwide",
     t+" "+p+"_swwide("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned long long w=(unsigned long long)h | ((unsigned long long)(h^0x9e3779b9u)<<32);\n"
     "    unsigned v=(unsigned)((w + (w>>32)) & 7u); unsigned r;\n"
     "    switch(v){ case 0:r=h+1u;break; case 1:r=h^0x0fu;break; case 2:r=h*3u;break;\n"
     "      case 3:r=h>>2;break; case 4:r=h+0x77u;break; case 5:r=~h;break;\n"
     "      case 6:r=h*11u;break; default:r=h-9u;break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress265", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress265TC("x64o265", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress265TC("x86o265", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress265TC("a64o265", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress265TC("armo265", "int");

INSTANTIATE_TEST_SUITE_P(OptStress265, X64OptStress265RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress265, X86OptStress265RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress265, A64OptStress265RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress265, ARM32OptStress265RT, ::testing::ValuesIn(kARM), rtTCName);
