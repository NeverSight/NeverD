//===- AllPlatform_OptStress285RTTests.cpp - select/clamp/minmax probe ====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O2 integer kernels stressing the conditional-select lowering that MedFlags
// and the codegen repeatedly miscompiled: min/max/clamp idioms (SMIN/SMAX/
// UMIN/UMAX, CMOVcc, CSEL), abs/negate/sign (CSNEG/CSINV idioms), manual
// saturation via compare-select, conditional inc/dec/neg/not (CSINC/CSINV/
// CSNEG), and boolean flatten (SETcc/CSET into 0/1 arithmetic).
//
//   * minmaxcl - min/max plus signed clamp chains.
//   * absnsign - abs / negated-abs / three-way sign (computed unsigned, no UB).
//   * cmovsel  - chained ternary selects folded into a recurrence.
//   * satcmp   - manual u8 / i16 saturation via compare-select.
//   * condinc  - conditional increment / negate / complement (CSINC/CSNEG).
//   * boolmix  - comparison results flattened to 0/1 and combined.
//
// All signed negation uses unsigned two's-complement to stay UB-free so the
// native and lifted builds agree bit-for-bit.  No division.  All four, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress285RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress285RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress285RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress285RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress285RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress285RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress285RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress285RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress285TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // min/max plus signed clamp chains (SMIN/SMAX/CMOV/CSEL).
    {p+"_minmaxcl",
     t+" "+p+"_minmaxcl("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    int v=(int)(h>>2); int lo=-500000, hi=500000;\n"
     "    int c=v<lo?lo:(v>hi?hi:v);\n"
     "    int mn=(acc<c)?acc:c; int mx=(acc>c)?acc:c;\n"
     "    acc=(mn>>1)+(mx>>2)+((c>>3)^i); }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress285", 2},

    // abs / negated-abs / three-way sign, all via unsigned to avoid UB.
    {p+"_absnsign",
     t+" "+p+"_absnsign("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    int v=(int)h;\n"
     "    unsigned av=(v<0)?(unsigned)(0u-(unsigned)v):(unsigned)v;\n"
     "    unsigned nv=(v<0)?(unsigned)v:(unsigned)(0u-(unsigned)v);\n"
     "    int sg=(v>0)-(v<0);\n"
     "    acc+=(av>>1)^nv^(unsigned)(sg*0x101)^(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress285", 2},

    // chained ternary selects folded into a recurrence (CMOV/CSEL).
    {p+"_cmovsel",
     t+" "+p+"_cmovsel("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h, y=h>>7, z=h>>15;\n"
     "    unsigned r=(x<y)?z:(x^y);\n"
     "    r=(r&1u)?(r+0x9E37u):(r*3u);\n"
     "    acc=acc*131u+((acc>r)?acc-r:r-acc)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress285", 2},

    // manual u8 / i16 saturation via compare-select (UMIN/UMAX clamp).
    {p+"_satcmp",
     t+" "+p+"_satcmp("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    int s=(int)(h>>8)-8000000;\n"
     "    int u8=s<0?0:(s>255?255:s);\n"
     "    int i16=s<-32768?-32768:(s>32767?32767:s);\n"
     "    acc=acc*131u+(unsigned)u8+((unsigned)(i16&0xFFFF)<<8)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress285", 2},

    // conditional increment / negate / complement (CSINC/CSNEG/CSINV).
    {p+"_condinc",
     t+" "+p+"_condinc("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned v=h;\n"
     "    acc+=(v&1u)?1u:0xFFFFFFFFu;\n"
     "    acc=(v&2u)?acc:(unsigned)(0u-acc);\n"
     "    if(v&4u) acc=~acc;\n"
     "    acc^=(v>>3); acc+=(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress285", 2},

    // comparison results flattened to 0/1 and combined (SETcc/CSET).
    {p+"_boolmix",
     t+" "+p+"_boolmix("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h, y=h>>9;\n"
     "    unsigned b=(unsigned)(x<y)+(unsigned)(x>y)*2u+(unsigned)(x==y)*4u\n"
     "      +(unsigned)((x&0xFFu)>(y&0xFFu))*8u;\n"
     "    int sx=(int)x, sy=(int)y;\n"
     "    b+=(unsigned)(sx<sy)*16u+(unsigned)(sx>=sy)*32u;\n"
     "    acc=acc*131u+b+(h&0xFu)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress285", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress285TC("x64o285", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress285TC("x86o285", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress285TC("a64o285", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress285TC("armo285", "int");

INSTANTIATE_TEST_SUITE_P(OptStress285, X64OptStress285RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress285, X86OptStress285RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress285, A64OptStress285RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress285, ARM32OptStress285RT, ::testing::ValuesIn(kARM), rtTCName);
