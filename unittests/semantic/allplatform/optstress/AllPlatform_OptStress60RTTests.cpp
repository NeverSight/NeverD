//===- AllPlatform_OptStress60RTTests.cpp - branchless / select -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Branchless conditional idioms that clang -O2 lowers to flag-consuming
// predicated moves (CMOV / CSEL / CSINC / Thumb IT, SETcc / CSET) rather than
// branches.  These hammer the MedFlags flag-folding and predicated-select
// reconstruction paths — historically the single richest source of optimizer
// miscompiles in this project (signed-condition folding, carry survival,
// multi-CMP-per-block).  Every kernel mixes several distinct comparison
// predicates so a wrong flag source surfaces as a return mismatch.
//
//   * clampacc - saturating clamp (min/max) accumulate -> CMOV/CSEL pair.
//   * signsel  - sign / abs / negate selects across signed + unsigned.
//   * cmpchain - chained &&/|| comparison predicates folded to flags.
//   * minmax3  - running min/max/median of a 3-window over a stream.
//   * condacc  - conditional add/sub/xor driven by independent flags.
//   * terntree - deep nested ternary tree (many fused selects).
//
// All integer, fold to one return, no float / 64-bit divide helper.
// All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress60RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress60RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress60RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress60RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress60RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress60RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress60RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress60RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress60TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Saturating clamp (min/max) accumulate -> CMOV/CSEL pair.
    {p+"_clampacc",
     t+" "+p+"_clampacc("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    int v=(int)(s>>3)-(int)0x10000000; int lo=-4096, hi=4096;\n"
     "    if(v<lo) v=lo; if(v>hi) v=hi;\n"
     "    unsigned uv=(s>>5); if(uv>50000u) uv=50000u;\n"
     "    h=h*131u+(unsigned)(v+8192)+uv; h^=h>>13; }\n"
     "  return ("+t+")h; }\n",
     {0x81u}, "OptStress60", 2},

    // Sign / abs / negate selects across signed + unsigned.
    {p+"_signsel",
     t+" "+p+"_signsel("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    int x=(int)s, y=(int)(s>>7);\n"
     "    int ax=x<0?-x:x; int sg=(x>0)-(x<0);\n"
     "    int mx=x>y?x:y; int mn=x<y?x:y;\n"
     "    h=h*131u+(unsigned)ax+(unsigned)(sg+2)+(unsigned)(mx-mn); h^=h>>11; }\n"
     "  return ("+t+")h; }\n",
     {0x82u}, "OptStress60", 2},

    // Chained &&/|| comparison predicates folded to flags.
    {p+"_cmpchain",
     t+" "+p+"_cmpchain("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    int x=(int)(s>>2), y=(int)(s>>9), z=(int)(s>>16);\n"
     "    int c1=(x<y && y<z); int c2=(x>=y || z<0); int c3=(x<=z)==(y>0);\n"
     "    unsigned u=(s&0xffff); int c4=(u>=100u && u<=40000u);\n"
     "    h=h*131u+(unsigned)(c1+2*c2+4*c3+8*c4); h^=h>>14; }\n"
     "  return ("+t+")h; }\n",
     {0x83u}, "OptStress60", 2},

    // Running min/max/median of a 3-window over a stream.
    {p+"_minmax3",
     t+" "+p+"_minmax3("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0, p0=0,p1=0,p2=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    p0=p1; p1=p2; p2=(s>>4)&0xffff;\n"
     "    unsigned mn=p0,mx=p0;\n"
     "    if(p1<mn)mn=p1; if(p2<mn)mn=p2;\n"
     "    if(p1>mx)mx=p1; if(p2>mx)mx=p2;\n"
     "    unsigned med=p0+p1+p2-mn-mx;\n"
     "    h=h*131u+mn*3u+med*5u+mx*7u; h^=h>>12; }\n"
     "  return ("+t+")h; }\n",
     {0x84u}, "OptStress60", 2},

    // Conditional add/sub/xor driven by independent flags.
    {p+"_condacc",
     t+" "+p+"_condacc("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0; int acc=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    int d=(int)(s>>6)&0x3ff;\n"
     "    acc += (s&1u)? d : -d;\n"
     "    acc = (s&2u)? acc^0x55 : acc;\n"
     "    if(acc>30000) acc-=30000; else if(acc<-30000) acc+=30000;\n"
     "    h=h*131u+(unsigned)(acc+32768); h^=h>>10; }\n"
     "  return ("+t+")h; }\n",
     {0x85u}, "OptStress60", 2},

    // Deep nested ternary tree (many fused selects).
    {p+"_terntree",
     t+" "+p+"_terntree("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    int x=(int)(s>>3)&0xff; int y=(int)(s>>12)&0xff;\n"
     "    int r = x<32 ? (y<16? x+y : x-y)\n"
     "          : x<64 ? (y<128? x*2-y : y-x)\n"
     "          : x<128? (y&1? (x^y) : (x|y))\n"
     "                 : (y<200? (x&y)+3 : (x>y?x:y));\n"
     "    h=h*131u+(unsigned)(r+256); h^=h>>9; }\n"
     "  return ("+t+")h; }\n",
     {0x86u}, "OptStress60", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress60TC("x64o60", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress60TC("x86o60", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress60TC("a64o60", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress60TC("armo60", "int");

INSTANTIATE_TEST_SUITE_P(OptStress60, X64OptStress60RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress60, X86OptStress60RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress60, A64OptStress60RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress60, ARM32OptStress60RT, ::testing::ValuesIn(kARM), rtTCName);
