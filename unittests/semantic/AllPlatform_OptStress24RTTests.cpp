//===- AllPlatform_OptStress24RTTests.cpp - opt-stress probes --*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// OptStress 21-23 hammered the partial-write -> wide-parent merge in the
// entry-seed (#430) and wide-read-return (#431) shapes.  This round varies the
// *control-flow shape* around an 8/16-bit partial write and its wide parent
// read, plus a few sub-register corners earlier probes only grazed:
//
//   * diamond16 - a 16-bit value partially written in only *one* arm of an
//                 in-loop if/else, then read wide after the merge.  Exercises a
//                 partial-write parent merged at a control-flow join (a phi with
//                 two forward edges), not just the loop back-edge or entry seed.
//   * diamond8  - the 8-bit (byte offset 0) version of diamond16.
//   * mixwidth  - one loop-carried 16-bit value read three ways each iteration
//                 (zext to 32, sext to 32, low byte) so the merged parent feeds
//                 narrower-and-wider reads simultaneously.
//   * setcc8    - an 8-bit accumulator fed only by comparison (setcc 0/1)
//                 results across a branchy loop, then read wide.
//   * swaphalf  - rotate-by-16 (high/low half swap) interleaved with 16-bit
//                 low-half arithmetic, loop-carried (16-bit write + rol/ror).
//   * carrymask - a borrow/compare turned into a full-width -(cond) mask used to
//                 blend two sub-word lanes (flag -> mask -> value).
//
// Every kernel is integer-only, folds to a single integer return and lowers to
// no runtime helper (no 64-bit divide / no float), so all four targets are
// checked native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress24RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress24RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress24RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress24RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress24RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress24RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress24RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress24RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress24TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 16-bit partial write in only ONE arm of an in-loop if, read wide after merge.
    {p+"_diamond16",
     t+" "+p+"_diamond16("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  unsigned short w=(unsigned short)(a^0x1357u);\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    if((s>>28)&1u){ w=(unsigned short)(w*3u+(unsigned short)(s>>9)); }\n"
     "    h=h*131u+(unsigned)w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x4cULL}, "OptStress24", 2},

    // 8-bit partial write in only ONE arm of an in-loop if, read wide after merge.
    {p+"_diamond8",
     t+" "+p+"_diamond8("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  unsigned char c=(unsigned char)(a^0x5au);\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    if((s>>27)&1u){ c=(unsigned char)(c*5u+(unsigned char)(s>>11)); }\n"
     "    h=h*131u+(unsigned)c; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9bULL}, "OptStress24", 2},

    // One loop-carried 16-bit value read as zext32, sext32 and low byte each pass.
    {p+"_mixwidth",
     t+" "+p+"_mixwidth("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  unsigned short w=(unsigned short)a;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    w=(unsigned short)(w+(unsigned short)(s>>13));\n"
     "    unsigned uz=(unsigned)w;\n"
     "    int sx=(int)(short)w;\n"
     "    unsigned lb=(unsigned char)w;\n"
     "    h=h*131u+uz+(unsigned)sx+lb; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xa7ULL}, "OptStress24", 2},

    // 8-bit accumulator fed only by setcc (0/1) comparison results, read wide.
    {p+"_setcc8",
     t+" "+p+"_setcc8("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0; unsigned char acc=(unsigned char)a;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    int x=(int)s, y=(int)(s*2654435761u);\n"
     "    acc=(unsigned char)(acc+(unsigned char)(x<y)\n"
     "        +(unsigned char)((unsigned)x>(unsigned)y)*3u\n"
     "        -(unsigned char)(x==y));\n"
     "    h=h*131u+(unsigned)acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x35ULL}, "OptStress24", 2},

    // Rotate-by-16 (half swap) interleaved with 16-bit low-half arithmetic.
    {p+"_swaphalf",
     t+" "+p+"_swaphalf("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0; unsigned v=(unsigned)a|1u;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    v=(v>>16)|(v<<16);\n"
     "    unsigned short lo=(unsigned short)v;\n"
     "    lo=(unsigned short)(lo*3u+(unsigned short)(s>>7));\n"
     "    v=(v&0xffff0000u)|lo;\n"
     "    h=h*131u+v; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress24", 2},

    // Compare/borrow turned into a full-width -(cond) mask blending sub-word lanes.
    {p+"_carrymask",
     t+" "+p+"_carrymask("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0; unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned x=s, y=s*2654435761u+1u;\n"
     "    unsigned m=(unsigned)(0u-(unsigned)(x<y));\n"
     "    unsigned blend=(x&m)|(y&~m);\n"
     "    unsigned short lo=(unsigned short)((acc&m)|((acc>>1)&~m));\n"
     "    acc=(blend^((unsigned)lo<<3))+ (m&7u);\n"
     "    h=h*131u+acc+(unsigned)lo; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x13ULL}, "OptStress24", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress24TC("x64o24", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress24TC("x86o24", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress24TC("a64o24", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress24TC("armo24", "int");

INSTANTIATE_TEST_SUITE_P(OptStress24, X64OptStress24RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress24, X86OptStress24RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress24, A64OptStress24RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress24, ARM32OptStress24RT, ::testing::ValuesIn(kARM), rtTCName);
