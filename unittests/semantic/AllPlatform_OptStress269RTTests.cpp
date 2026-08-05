//===- AllPlatform_OptStress269RTTests.cpp - switch corner cases -O0 =====//
//
// switch / jump-table corner cases at -O0 — further hardening of the #509/#510
// -O0 register-reuse jump-table fixes with index/guard shapes that drive
// different resolver paths: non-zero-base normalization (`sub base; cmp`), two
// tables in one body (both real jump tables, not one table + one chain), a
// signed switch value, a wide (long-long-derived) index, and a large dense
// switch that forces a table on every target.
//
//   * swnorm   - cases with a non-zero base (10..18): sub-base normalization.
//   * swtwotbl - two switches in one body, both forced to jump tables.
//   * swsigned - switch on a signed value (negative LCG samples).
//   * swllidx  - index from a long long's low bits.
//   * swbig    - 16-way dense switch (forces a real table everywhere).
//   * swmiddef - default case physically in the middle of the case bodies.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit ops in the arms, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress269RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress269RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress269RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress269RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress269RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress269RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress269RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress269RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress269TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Cases with a non-zero base (10..18): sub-base normalization + guard.
    {p+"_swnorm",
     t+" "+p+"_swnorm("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned v=10u+(h%9u); unsigned r;\n"
     "    switch(v){ case 10:r=h+1u;break; case 11:r=h^0xffu;break; case 12:r=h*3u;break;\n"
     "      case 13:r=h>>2;break; case 14:r=h+0x55u;break; case 15:r=~h;break;\n"
     "      case 16:r=h*5u;break; case 17:r=h-3u;break; case 18:r=h+0x1234u;break;\n"
     "      default:r=h;break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress269", 0},

    // Two switches in one body, both forced to jump tables (8-way each).
    {p+"_swtwotbl",
     t+" "+p+"_swtwotbl("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int j=0;j<200;j++){ h=h*1103515245u+12345u; unsigned v=h&7u, w=(h>>8)&7u; unsigned r1,r2;\n"
     "    switch(v){ case 0:r1=h+1u;break; case 1:r1=h^0xffu;break; case 2:r1=h*3u;break;\n"
     "      case 3:r1=h>>2;break; case 4:r1=h+9u;break; case 5:r1=~h;break;\n"
     "      case 6:r1=h*5u;break; default:r1=h-7u;break; }\n"
     "    switch(w){ case 0:r2=h+2u;break; case 1:r2=h*7u;break; case 2:r2=h>>4;break;\n"
     "      case 3:r2=h^0x33u;break; case 4:r2=h+0x77u;break; case 5:r2=~h+1u;break;\n"
     "      case 6:r2=h*9u;break; default:r2=h-1u;break; }\n"
     "    acc=acc*131u + r1 + r2*31u; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress269", 0},

    // switch on a signed value (negative LCG samples reach the default).
    {p+"_swsigned",
     t+" "+p+"_swsigned("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; int v=(int)(h&0xfu)-4; unsigned r;\n"
     "    switch(v){ case 0:r=h+1u;break; case 1:r=h^0xffu;break; case 2:r=h*3u;break;\n"
     "      case 3:r=h>>2;break; case 4:r=h+0x55u;break; case 5:r=~h;break;\n"
     "      case 6:r=h*5u;break; default:r=h+(unsigned)(v&7);break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress269", 0},

    // Index from a long long's low bits.
    {p+"_swllidx",
     t+" "+p+"_swllidx("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned long long w=(unsigned long long)h | ((unsigned long long)(h>>5)<<32);\n"
     "    unsigned v=(unsigned)((w ^ (w>>32)) & 7u); unsigned r;\n"
     "    switch(v){ case 0:r=h+1u;break; case 1:r=h^0xffu;break; case 2:r=h*3u;break;\n"
     "      case 3:r=h>>2;break; case 4:r=h+0x55u;break; case 5:r=~h;break;\n"
     "      case 6:r=h*5u;break; default:r=h-7u;break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress269", 0},

    // 16-way dense switch (forces a real table on every target).
    {p+"_swbig",
     t+" "+p+"_swbig("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned v=h&15u; unsigned r;\n"
     "    switch(v){ case 0:r=h+1u;break; case 1:r=h^1u;break; case 2:r=h*3u;break;\n"
     "      case 3:r=h>>1;break; case 4:r=h+4u;break; case 5:r=h*5u;break;\n"
     "      case 6:r=h>>3;break; case 7:r=~h;break; case 8:r=h+8u;break;\n"
     "      case 9:r=h*9u;break; case 10:r=h>>4;break; case 11:r=h^0xbu;break;\n"
     "      case 12:r=h+12u;break; case 13:r=h*13u;break; case 14:r=h>>5;break;\n"
     "      default:r=h+15u;break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress269", 0},

    // Default body physically between the case bodies.
    {p+"_swmiddef",
     t+" "+p+"_swmiddef("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned v=h&7u; unsigned r=0;\n"
     "    switch(v){ case 0:r=h+1u;break; case 1:r=h^0xffu;break; case 2:r=h*3u;break;\n"
     "      default:r=h+0xabu;break;\n"
     "      case 4:r=h+0x55u;break; case 5:r=~h;break; case 6:r=h*5u;break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress269", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress269TC("x64o269", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress269TC("x86o269", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress269TC("a64o269", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress269TC("armo269", "int");

INSTANTIATE_TEST_SUITE_P(OptStress269, X64OptStress269RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress269, X86OptStress269RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress269, A64OptStress269RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress269, ARM32OptStress269RT, ::testing::ValuesIn(kARM), rtTCName);
