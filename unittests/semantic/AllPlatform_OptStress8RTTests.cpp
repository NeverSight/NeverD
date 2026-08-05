//===- AllPlatform_OptStress8RTTests.cpp - codec / RNG stressors -*-C++*-=//
//
// Optimizer-stress roundtrip probes in domains the OptStress1-7 / VectorAlgo
// series did not reach: a base64 encoder (a rodata character table indexed by
// six-bit fields — exercises constant-pool mapping plus byte-shuffle lowering),
// an xorshift PRNG (dense shift/xor chains), a run-length encoder (data-driven
// inner loop length), a UTF-8 codepoint counter (multi-way branch with a
// variable step), a Morton bit-interleave (magic-mask spreads), and a bit
// reversal (swap idiom).  All integer so no soft-float libcall, none lowers to a
// runtime helper (no variable divide / popcount builtin); each folds to one
// integer return, compiled at -O2 and checked native vs lifted on all four
// targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress8RT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress8RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress8RT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress8RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress8RT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress8RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress8RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress8RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress8TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // base64 encode six derived bytes through a 64-entry rodata table.
    {p+"_b64",
     t+" "+p+"_b64("+t+" a){\n"
     "  static const char tb[65]=\"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/\";\n"
     "  unsigned char in[6]; for(int i=0;i<6;i++) in[i]=(unsigned char)((unsigned)a*(i+1u)+(unsigned)i);\n"
     "  unsigned char out[8]; unsigned h=0;\n"
     "  for(int i=0,j=0;i<6;i+=3,j+=4){\n"
     "    unsigned v=((unsigned)in[i]<<16)|((unsigned)in[i+1]<<8)|(unsigned)in[i+2];\n"
     "    out[j]=tb[(v>>18)&63u]; out[j+1]=tb[(v>>12)&63u];\n"
     "    out[j+2]=tb[(v>>6)&63u]; out[j+3]=tb[v&63u]; }\n"
     "  for(int i=0;i<8;i++) h=h*131u+(unsigned char)out[i];\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x4cULL}, "OptStress8", 2},

    // xorshift32 PRNG: dense shift/xor feedback folded into a hash.
    {p+"_xs",
     t+" "+p+"_xs("+t+" a){\n"
     "  unsigned x=((unsigned)a)|1u, h=0;\n"
     "  for(int i=0;i<24;i++){ x^=x<<13; x^=x>>17; x^=x<<5; h=h*131u+x; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x9bULL}, "OptStress8", 2},

    // run-length encode a 16-byte buffer with few distinct values: the inner
    // run-length loop count is data dependent.
    {p+"_rle",
     t+" "+p+"_rle("+t+" a){\n"
     "  unsigned char b[16]; for(int i=0;i<16;i++) b[i]=(unsigned char)(((unsigned)a>>(i&7))&3u);\n"
     "  unsigned h=0; int i=0;\n"
     "  while(i<16){ unsigned char c=b[i]; int run=1;\n"
     "    while(i+run<16 && b[i+run]==c) run++;\n"
     "    h=h*131u+(unsigned)c*7u+(unsigned)run; i+=run; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xa7ULL}, "OptStress8", 2},

    // UTF-8 codepoint walk: a multi-way branch picks the 1..4-byte step.
    {p+"_u8",
     t+" "+p+"_u8("+t+" a){\n"
     "  unsigned char b[12]; for(int i=0;i<12;i++) b[i]=(unsigned char)((unsigned)a*(unsigned)(i+3)+(unsigned)i);\n"
     "  unsigned cnt=0; int i=0;\n"
     "  while(i<12){ unsigned char c=b[i]; int n;\n"
     "    if(c<0x80u) n=1; else if((c>>5)==6u) n=2; else if((c>>4)==14u) n=3; else n=4;\n"
     "    cnt=cnt*31u+(unsigned)n; i+=n; }\n"
     "  return ("+t+")(unsigned long)cnt; }\n",
     {0x35ULL}, "OptStress8", 2},

    // Morton interleave of two derived bytes (magic-mask bit spread).
    {p+"_morton",
     t+" "+p+"_morton("+t+" a){\n"
     "  unsigned x=(unsigned)a&0xffu, y=((unsigned)a>>8)&0xffu, h=0;\n"
     "  for(int k=0;k<8;k++){\n"
     "    unsigned xx=x, yy=y;\n"
     "    xx=(xx|(xx<<4))&0x0f0fu; xx=(xx|(xx<<2))&0x3333u; xx=(xx|(xx<<1))&0x5555u;\n"
     "    yy=(yy|(yy<<4))&0x0f0fu; yy=(yy|(yy<<2))&0x3333u; yy=(yy|(yy<<1))&0x5555u;\n"
     "    h=h*131u+(xx|(yy<<1)); x=(x+1u)&0xffu; y=(y+3u)&0xffu; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x6dULL}, "OptStress8", 2},

    // Bit reversal of a 32-bit word via the standard swap idiom.
    {p+"_revbits",
     t+" "+p+"_revbits("+t+" a){\n"
     "  unsigned v=(unsigned)a, h=0;\n"
     "  for(int k=0;k<6;k++){\n"
     "    unsigned x=v;\n"
     "    x=((x>>1)&0x55555555u)|((x&0x55555555u)<<1);\n"
     "    x=((x>>2)&0x33333333u)|((x&0x33333333u)<<2);\n"
     "    x=((x>>4)&0x0f0f0f0fu)|((x&0x0f0f0f0fu)<<4);\n"
     "    x=((x>>8)&0x00ff00ffu)|((x&0x00ff00ffu)<<8);\n"
     "    x=(x>>16)|(x<<16);\n"
     "    h=h*131u+x; v=v*1664525u+1013904223u; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x13ULL}, "OptStress8", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress8TC("x64o8", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress8TC("x86o8", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress8TC("a64o8", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress8TC("armo8", "int");

INSTANTIATE_TEST_SUITE_P(OptStress8, X64OptStress8RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress8, X86OptStress8RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress8, A64OptStress8RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress8, ARM32OptStress8RT, ::testing::ValuesIn(kARM), rtTCName);
