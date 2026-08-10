//===- AllPlatform_OptStress242RTTests.cpp - saturation / clamp =========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Clamps to bit-width boundaries that clang lowers to ARM `ssat`/`usat`
// (and the saturating-add/sub idioms to `qadd`/`qsub`) — the ARM32 saturation
// family that was previously a coverage gap.  On x86/AArch64 the
// same clamps lower to cmov/csel/min-max, so every platform round-trips while
// ARM exercises the saturation instructions directly.
//
//   * ssat12  - signed clamp to [-2048, 2047]   (ssat #12).
//   * usat10  - unsigned clamp to [0, 1023]      (usat #10).
//   * ssatsh  - ssat applied to a shifted value  (ssat with lsl).
//   * qadd    - saturating signed add.
//   * qsub    - saturating signed subtract.
//   * satmix  - chained sat add then clamp window.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress242RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress242RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress242RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress242RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress242RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress242RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress242RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress242RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress242TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Signed clamp to [-2048, 2047] (ssat #12).
    {p+"_ssat12",
     t+" "+p+"_ssat12("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<150;i++){ h=h*1103515245u+12345u;\n"
     "    int v=(int)h; if(v>2047)v=2047; if(v<-2048)v=-2048;\n"
     "    acc=acc*131u+(unsigned)v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress242", 2},

    // Unsigned clamp to [0, 1023] (usat #10).
    {p+"_usat10",
     t+" "+p+"_usat10("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<150;i++){ h=h*1103515245u+12345u;\n"
     "    int v=(int)h; if(v>1023)v=1023; if(v<0)v=0;\n"
     "    acc=acc*131u+(unsigned)v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress242", 2},

    // ssat applied to a shifted value.
    {p+"_ssatsh",
     t+" "+p+"_ssatsh("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<150;i++){ h=h*1103515245u+12345u;\n"
     "    int v=(int)h>>3; if(v>32767)v=32767; if(v<-32768)v=-32768;\n"
     "    acc=acc*131u+(unsigned)v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress242", 2},

    // Saturating signed add (qadd idiom).
    {p+"_qadd",
     t+" "+p+"_qadd("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<150;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)h>>16, y=(int)(h<<3)>>16;\n"
     "    long long s=(long long)x+(long long)y;\n"
     "    if(s>2147483647LL)s=2147483647LL; if(s<-2147483648LL)s=-2147483648LL;\n"
     "    acc=acc*131+(int)s+i; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x45678u}, "OptStress242", 2},

    // Saturating signed subtract (qsub idiom).
    {p+"_qsub",
     t+" "+p+"_qsub("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<150;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)h>>16, y=(int)(h<<5)>>16;\n"
     "    long long s=(long long)x-(long long)y;\n"
     "    if(s>2147483647LL)s=2147483647LL; if(s<-2147483648LL)s=-2147483648LL;\n"
     "    acc=acc*131+(int)s+i; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x56789u}, "OptStress242", 2},

    // Chained saturating add then clamp window.
    {p+"_satmix",
     t+" "+p+"_satmix("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<150;i++){ h=h*1103515245u+12345u;\n"
     "    int v=(int)h>>8; if(v>4095)v=4095; if(v<-4096)v=-4096;\n"
     "    int w=v+((int)(h>>20)&0xff); if(w>4095)w=4095; if(w<-4096)w=-4096;\n"
     "    acc=acc*131u+(unsigned)(v^w)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress242", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress242TC("x64o242", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress242TC("x86o242", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress242TC("a64o242", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress242TC("armo242", "int");

INSTANTIATE_TEST_SUITE_P(OptStress242, X64OptStress242RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress242, X86OptStress242RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress242, A64OptStress242RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress242, ARM32OptStress242RT, ::testing::ValuesIn(kARM), rtTCName);
