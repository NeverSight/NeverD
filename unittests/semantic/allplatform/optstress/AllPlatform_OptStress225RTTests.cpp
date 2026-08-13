//===- AllPlatform_OptStress225RTTests.cpp - shift/rotate + subword CFG ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Breadth probes orthogonal to the call-ABI work: variable-count shifts,
// rotates and funnel shifts whose result is consumed across a branch, plus
// nested-loop loop-carried sub-word accumulation.  These stress the shift-count
// masking (x86 masks the count mod width; a count of exactly the width must be
// modeled per-arch), funnel-shift lowering, and sub-word truncation in a
// control-flow context rather than straight-line.
//
//   * vrot32  - variable rotate-left of a 32-bit value, count from data, used
//               after a branch (count masked mod 32).
//   * funnel  - `(hi<<n)|(lo>>(W-n))` funnel shift with a runtime, branch-
//               selected count.
//   * shcond  - shift amount picked by a comparison, result merged.
//   * sub8loop- nested loop accumulating into a u8 that wraps, widened read.
//   * dshift  - double-width (64-bit) variable shift on a loop-carried value.
//   * bittog  - per-bit toggle/test with a runtime bit index across a branch.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress225RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress225RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress225RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress225RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress225RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress225RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress225RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress225RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress225TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Variable rotate-left (32-bit), count masked mod 32, used after a branch.
    {p+"_vrot32",
     t+" "+p+"_vrot32("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned n=(h>>3)&31u; unsigned v=h;\n"
     "    unsigned r=(v<<n)|(v>>((32u-n)&31u));\n"
     "    if(h&1u) acc+=r; else acc^=r;\n"
     "    acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress225", 2},

    // Funnel shift (hi<<n)|(lo>>(32-n)) with branch-selected count.
    {p+"_funnel",
     t+" "+p+"_funnel("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned hi=h, lo=h*2654435761u; unsigned n=1u+((h>>5)&30u);\n"
     "    unsigned f=(h&2u)? ((hi<<n)|(lo>>(32u-n))) : ((lo<<n)|(hi>>(32u-n)));\n"
     "    acc=acc*131u+f+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress225", 2},

    // Shift amount selected by a comparison, results merged.
    {p+"_shcond",
     t+" "+p+"_shcond("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)h; unsigned n=(x<0)?((h>>7)&15u):((h>>11)&15u);\n"
     "    unsigned v=(x<0)?(h>>n):(h<<n);\n"
     "    acc=acc*131u+v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress225", 2},

    // Nested loop accumulating into a u8 that wraps; widened read.
    {p+"_sub8loop",
     t+" "+p+"_sub8loop("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*1103515245u+12345u; unsigned char b=(unsigned char)h;\n"
     "    for(int j=0;j<5;j++){ b=(unsigned char)(b*3u+(unsigned char)(h>>(j*4))); }\n"
     "    acc=acc*131u+(unsigned)b+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress225", 2},

    // Double-width (64-bit) variable shift on a loop-carried value.
    {p+"_dshift",
     t+" "+p+"_dshift("+t+" a){ unsigned long long h=(unsigned long long)a^0x123456789ABCDEFULL;\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ unsigned n=(unsigned)(h&63u);\n"
     "    unsigned long long s=(h<<n)|(h>>((64u-n)&63u));\n"
     "    acc=acc*131u+(unsigned)(s^(s>>32))+(unsigned)i;\n"
     "    h=h*6364136223846793005ULL+1u; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress225", 2},

    // Per-bit toggle/test with a runtime bit index across a branch.
    {p+"_bittog",
     t+" "+p+"_bittog("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned bit=(h>>9)&31u; unsigned v=h^(1u<<bit);\n"
     "    if((v>>bit)&1u) acc+=v; else acc-=v;\n"
     "    acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress225", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress225TC("x64o225", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress225TC("x86o225", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress225TC("a64o225", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress225TC("armo225", "int");

INSTANTIATE_TEST_SUITE_P(OptStress225, X64OptStress225RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress225, X86OptStress225RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress225, A64OptStress225RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress225, ARM32OptStress225RT, ::testing::ValuesIn(kARM), rtTCName);
