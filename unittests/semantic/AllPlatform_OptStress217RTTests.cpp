//===- AllPlatform_OptStress217RTTests.cpp - funnel / double-shift =======//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Variable-count funnel shifts — the SHLD/SHRD family on x86 and the
// equivalent two-source shift+OR the other targets synthesise.  A funnel shift
// `(hi << n) | (lo >> (W-n))` reads two independent source words and a runtime
// count, so it stresses the shift-amount masking and the OR-merge that the
// double-precision-shift lift builds.  Counts are forced into `[1, W-1]` so the
// complementary `W-n` shift is also in range — every shift stays defined, which
// matters because a stray shift-by-width is UB→poison (the RCL/RCR class of
// bug).  Existing double-shift probes are x86-only; these run all four targets
// at the native word width (64-bit on x64/a64, 32-bit on i386/ARM32) so neither
// path needs a shift library helper.
//
//   * funnel - left + right funnel of two loop-carried words by a runtime count.
//   * rot    - variable rotate-left/right (single-source funnel, the `rol`/`ror`
//              and `__builtin_rotate*` shape) folded together.
//   * maskcnt- count taken modulo the width with a +1 bias, exercising the
//              implicit `n & (W-1)` masking the hardware shift applies.
//
// Integer in / integer out, LCG-seeded, folded to one integer return; no float
// / divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress217RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress217RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress217RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress217RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress217RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress217RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress217RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress217RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
// T  = signed return type (long / int); U = unsigned word type; W = bit width.
static std::vector<RoundTripTC> makeOptStress217TC(const char *prefix,
                                                   const char *T, const char *U,
                                                   const char *W) {
  std::string p = prefix, t = T, u = U, w = W;
  return {
    // Left + right funnel of two loop-carried words by a runtime count in [1,W-1].
    {p+"_funnel",
     t+" "+p+"_funnel("+t+" a){\n"
     "  "+u+" hi=("+u+")a^("+u+")0x9E3779B97F4A7C15ULL;\n"
     "  "+u+" lo=("+u+")a*("+u+")0xD1B54A32D192ED03ULL+1u;\n"
     "  "+u+" out=0;\n"
     "  for(int i=0;i<"+w+";i++){ unsigned n=(unsigned)((hi>>(i&("+w+"-1)))&("+w+"-1)); n=(n&("+w+"-2))+1u;\n"
     "    "+u+" f=(hi<<n)|(lo>>("+w+"-n));\n"
     "    "+u+" g=(lo>>n)|(hi<<("+w+"-n));\n"
     "    out=out*131u+f+g;\n"
     "    hi=hi*6364136223846793005ULL+1u; lo=lo*2862933555777941757ULL+(unsigned)i; }\n"
     "  return ("+t+")out; }\n",
     {0x1357924u}, "OptStress217", 2},

    // Variable rotate left/right (single-source funnel).
    {p+"_rot",
     t+" "+p+"_rot("+t+" a){\n"
     "  "+u+" x=("+u+")a|1u, out=0;\n"
     "  for(int i=0;i<"+w+";i++){ unsigned n=(unsigned)((x>>3)&("+w+"-1)); n=(n&("+w+"-2))+1u;\n"
     "    "+u+" rl=(x<<n)|(x>>("+w+"-n));\n"
     "    "+u+" rr=(x>>n)|(x<<("+w+"-n));\n"
     "    out=out*131u+rl+rr; x=x*6364136223846793005ULL+0x9E37u+(unsigned)i; }\n"
     "  return ("+t+")out; }\n",
     {0x2468ACEu}, "OptStress217", 2},

    // Count taken modulo width with a +1 bias (exercise hardware n&(W-1) mask).
    {p+"_maskcnt",
     t+" "+p+"_maskcnt("+t+" a){\n"
     "  "+u+" x=("+u+")a^0xABCDu, y=("+u+")a*7u+3u, out=0;\n"
     "  for(int i=0;i<"+w+";i++){ unsigned n=((unsigned)i*5u+1u)&("+w+"-1); if(!n) n=1u;\n"
     "    "+u+" s=(x<<n)|(y>>("+w+"-n));\n"
     "    out=out*131u+s+(x>>n); x=x*2862933555777941757ULL+1u; y=y+out; }\n"
     "  return ("+t+")out; }\n",
     {0x9ABCDEFu}, "OptStress217", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 =
    makeOptStress217TC("x64o217", "long", "unsigned long", "64");
static const std::vector<RoundTripTC> kX86 =
    makeOptStress217TC("x86o217", "int", "unsigned", "32");
static const std::vector<RoundTripTC> kA64 =
    makeOptStress217TC("a64o217", "long", "unsigned long", "64");
static const std::vector<RoundTripTC> kARM =
    makeOptStress217TC("armo217", "int", "unsigned", "32");

INSTANTIATE_TEST_SUITE_P(OptStress217, X64OptStress217RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress217, X86OptStress217RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress217, A64OptStress217RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress217, ARM32OptStress217RT, ::testing::ValuesIn(kARM), rtTCName);
