//===- AllPlatform_OptStress281RTTests.cpp - flag/subreg optimizer probe ==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O2 integer kernels aimed at the optimizer paths that have historically
// miscompiled: signed-saturation flag folding (MedFlags), sub-byte/sub-word
// register aliasing on loop-carried accumulators, nested-loop carry chains
// with cross-block flag liveness, runtime signed div/rem sign correction, and
// comparison-chain multi-way dispatch writing different sub-word results.
//
//   * clampacc  - signed saturating accumulate with min/max clamps.
//   * nibswap   - nibble-granular pack/unpack via shifts + masks.
//   * condcarry - nested-loop conditional add-with-carry accumulation.
//   * signdiv   - runtime signed div/rem with sign correction.
//   * multiway  - comparison-chain dispatch into distinct sub-word writes.
//   * rolhash   - rotate-based hash mixing 8/16/32-bit sub-register reads.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  Heavy
// math stays in 32-bit `unsigned`/`int` so i386/ARM32 stay libcall-free; only
// the final value is cast to the platform return type.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress281RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress281RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress281RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress281RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress281RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress281RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress281RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress281RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress281TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // signed saturating accumulate with min/max clamps (flag-fold idiom).
    {p+"_clampacc",
     t+" "+p+"_clampacc("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    int v=(int)(h>>9)-1073741824; acc+=v>>20;\n"
     "    if(acc>1000000) acc=1000000; if(acc<-1000000) acc=-1000000; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress281", 2},

    // nibble-granular pack/unpack via shifts + masks (sub-word aliasing).
    {p+"_nibswap",
     t+" "+p+"_nibswap("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<80;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h; unsigned r=0;\n"
     "    for(int n=0;n<8;n++){ r=(r<<4)|((x>>(n*4))&0xFu); }\n"
     "    acc=acc*131u+(r ^ (h>>16)); }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress281", 2},

    // nested-loop conditional add-with-carry accumulation (cross-block flags).
    {p+"_condcarry",
     t+" "+p+"_condcarry("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0; unsigned carry=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    for(int j=0;j<4;j++){ unsigned w=(h>>(j*8))&0xFFu;\n"
     "      unsigned s=acc+w+carry; carry=(s<acc)?1u:0u; acc=s;\n"
     "      if((w&1u)!=0u) acc^=0x9E3779B9u; }\n"
     "    acc=(acc<<1)|(acc>>31); }\n"
     "  return ("+t+")(acc+carry); }\n",
     {0x34567u}, "OptStress281", 2},

    // runtime signed div/rem with sign correction.
    {p+"_signdiv",
     t+" "+p+"_signdiv("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<72;i++){ h=h*1103515245u+12345u;\n"
     "    int num=(int)h; int den=(int)((h>>11)&0x3Fu)-32; if(den==0) den=7;\n"
     "    int q=num/den, r=num%den; acc+=q^(r<<1)^i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress281", 2},

    // comparison-chain dispatch into distinct sub-word writes.
    {p+"_multiway",
     t+" "+p+"_multiway("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<88;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned k=(h>>7)&0xFFu; unsigned o;\n"
     "    if(k<32u) o=(unsigned char)(k*3u+1u);\n"
     "    else if(k<96u) o=(unsigned short)(k*257u);\n"
     "    else if(k<160u) o=(k<<3)^0xABCDu;\n"
     "    else o=~k & 0xFFFFFFu;\n"
     "    acc=acc*131u+o+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress281", 2},

    // rotate-based hash mixing 8/16/32-bit sub-register reads.
    {p+"_rolhash",
     t+" "+p+"_rolhash("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0x811C9DC5u;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned char b=(unsigned char)(h>>3);\n"
     "    unsigned short w=(unsigned short)(h>>11);\n"
     "    unsigned r=((h<<7)|(h>>25));\n"
     "    acc=(acc^b)*16777619u; acc=acc+w; acc=((acc<<5)|(acc>>27))^r; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress281", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress281TC("x64o281", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress281TC("x86o281", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress281TC("a64o281", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress281TC("armo281", "int");

INSTANTIATE_TEST_SUITE_P(OptStress281, X64OptStress281RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress281, X86OptStress281RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress281, A64OptStress281RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress281, ARM32OptStress281RT, ::testing::ValuesIn(kARM), rtTCName);
