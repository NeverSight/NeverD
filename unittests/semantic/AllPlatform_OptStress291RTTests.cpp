//===- AllPlatform_OptStress291RTTests.cpp - fixed-point/scaling probe ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O2 integer kernels stressing fixed-point and multiply-shift codegen paths
// (32x32->64 widening multiply followed by a constant shift):
//
//   * fxmul     - Q16.16 fixed-point multiply ((x*y)>>16).
//   * fxlerp    - fixed-point linear interpolation A+(B-A)*t.
//   * scaledsum - multiply-by-constant then shift accumulation.
//   * recipmul  - divide-by-3 via magic-number multiply (no DIV).
//   * clampfx   - 64-bit scaled value clamp/saturate.
//   * dotfx     - fixed-point dot product with >>8 rescale.
//
// All wide folds use only +/-/* and constant shifts (no variable 64-bit shifts
// -> no libcalls), and there is no division instruction -- so native and lifted
// builds agree bit-for-bit.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress291RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress291RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress291RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress291RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress291RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress291RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress291RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress291RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress291TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Q16.16 fixed-point multiply ((x*y)>>16).
    {p+"_fxmul",
     t+" "+p+"_fxmul("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)h, y=(int)(h>>1);\n"
     "    long long pr=(long long)x*(long long)y; int r=(int)(pr>>16);\n"
     "    acc=acc*131u+(unsigned)r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress291", 2},

    // fixed-point linear interpolation A+(B-A)*t.
    {p+"_fxlerp",
     t+" "+p+"_fxlerp("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    int A=(int)(h&0xFFFF), B=(int)((h>>8)&0xFFFF); unsigned tt=(h>>3)&0xFFFFu;\n"
     "    int r=A+(int)(((long long)(B-A)*(long long)tt)>>16);\n"
     "    acc=acc*131u+(unsigned)r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress291", 2},

    // multiply-by-constant then shift accumulation.
    {p+"_scaledsum",
     t+" "+p+"_scaledsum("+t+" a){ unsigned h=(unsigned)a; unsigned long long acc64=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    acc64 += ((unsigned long long)h * 0xC0FFEEull) >> 8; }\n"
     "  unsigned acc=(unsigned)acc64 ^ (unsigned)(acc64>>32);\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress291", 2},

    // divide-by-3 via magic-number multiply (no DIV).
    {p+"_recipmul",
     t+" "+p+"_recipmul("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned q=(unsigned)(((unsigned long long)h*2863311531ull)>>33);\n"
     "    acc=acc*131u+q+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress291", 2},

    // 64-bit scaled value clamp/saturate.
    {p+"_clampfx",
     t+" "+p+"_clampfx("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    long long v=(long long)(int)h * 5ll;\n"
     "    if(v>(1ll<<20)) v=(1ll<<20); if(v<-(1ll<<20)) v=-(1ll<<20);\n"
     "    acc=acc*131u+(unsigned)(int)v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress291", 2},

    // fixed-point dot product with >>8 rescale.
    {p+"_dotfx",
     t+" "+p+"_dotfx("+t+" a){ unsigned h=(unsigned)a; long long acc64=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)(h&0xFFFF)-32768, y=(int)((h>>8)&0xFFFF)-32768;\n"
     "    acc64 += ((long long)x*(long long)y)>>8; }\n"
     "  unsigned acc=(unsigned)acc64 ^ (unsigned)(acc64>>32);\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress291", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress291TC("x64o291", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress291TC("x86o291", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress291TC("a64o291", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress291TC("armo291", "int");

INSTANTIATE_TEST_SUITE_P(OptStress291, X64OptStress291RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress291, X86OptStress291RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress291, A64OptStress291RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress291, ARM32OptStress291RT, ::testing::ValuesIn(kARM), rtTCName);
