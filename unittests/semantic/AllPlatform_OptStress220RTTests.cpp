//===- AllPlatform_OptStress220RTTests.cpp - cmp fold + nested select =====//
//
// Comparison-folding and nested-select probes driven with OVERFLOW-INDUCING
// values (INT_MIN/INT_MAX neighbourhoods) so the signed comparisons exercise
// the SF^OF (genuine `slt`) path, not just the lone-sign-flag path, and the
// unsigned comparisons exercise carry-from-subtract.  clang -O2 lowers these
// to setcc/cmov (x86), cset/csel (AArch64), cmp+mov-cond (ARM32) cascades; a
// wrong flag fold or select mis-merge diverges from the original.
//
//   * cmp3way - acc += (v<d) + 2*(v==d) + 4*(v>d), both signed and unsigned in
//               one expression on the same loop-carried, overflow-prone value.
//   * selnest - 2-deep nested ternary selecting among four arms by signed cmp.
//   * boolchain - &&/|| short-circuit boolean algebra feeding the accumulator.
//   * minmaxmix - signed min and unsigned max of the same pair, combined.
//
// Integer in / integer out, LCG-seeded, folded to one integer return; no float
// / divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress220RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress220RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress220RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress220RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress220RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress220RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress220RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress220RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
// T = return type; ST = signed type; UT = unsigned type (loop width).
static std::vector<RoundTripTC> makeOptStress220TC(const char *prefix,
                                                   const char *T, const char *ST,
                                                   const char *UT) {
  std::string p = prefix, t = T, st = ST, ut = UT;
  return {
    // Three-way compare encoded as bit weights, both signed and unsigned.
    {p+"_cmp3way",
     t+" "+p+"_cmp3way("+t+" a){\n"
     "  "+ut+" h=("+ut+")a^0x5A5A5A5Au;\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){\n"
     "    "+st+" v=("+st+")h; "+st+" d=("+st+")(h>>7);\n"
     "    "+ut+" uv=h; "+ut+" ud=h*2654435761u+1u;\n"
     "    acc+= (unsigned)((v<d)+2*(v==d)+4*(v>d));\n"
     "    acc+= 8u*(unsigned)((uv<ud)+2*(uv==ud)+4*(uv>ud));\n"
     "    acc=acc*131u+(unsigned)i;\n"
     "    h=h*1103515245u+12345u; }\n"
     "  return ("+t+")acc; }\n",
     {0x7FFFFFF1u}, "OptStress220", 2},

    // Two-deep nested ternary: pick one of four arms by signed comparisons.
    {p+"_selnest",
     t+" "+p+"_selnest("+t+" a){\n"
     "  "+ut+" h=("+ut+")a*2246822519u+1u;\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){\n"
     "    "+st+" v=("+st+")h; "+st+" d=("+st+")(h>>11);\n"
     "    "+st+" e=("+st+")(h<<3); "+st+" f=("+st+")(h^0x80000000u);\n"
     "    "+st+" r=(v<d)?((v<e)?v+e:v-e):((v<f)?v^f:v+f);\n"
     "    acc=acc*131u+(unsigned)r+(unsigned)i;\n"
     "    h=h*1103515245u+12345u; }\n"
     "  return ("+t+")acc; }\n",
     {0x80000005u}, "OptStress220", 2},

    // Short-circuit boolean algebra of overflow-prone comparisons.
    {p+"_boolchain",
     t+" "+p+"_boolchain("+t+" a){\n"
     "  "+ut+" h=("+ut+")a^0xDEADBEEFu;\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){\n"
     "    "+st+" b1=("+st+")h, b2=("+st+")(h>>5), b3=("+st+")(h<<7), b4=("+st+")(h>>13);\n"
     "    int c=((b1<b2)&&(b3<b4))||((b2<b3)&&!(b4<b1));\n"
     "    int d=(b1<=b4)^(b2>=b3);\n"
     "    acc=acc*131u+(unsigned)c+2u*(unsigned)d+(unsigned)i;\n"
     "    h=h*1103515245u+12345u; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345678u}, "OptStress220", 2},

    // Signed min and unsigned max of the same pair, combined per step.
    {p+"_minmaxmix",
     t+" "+p+"_minmaxmix("+t+" a){\n"
     "  "+ut+" h=("+ut+")a+0x9E3779B9u;\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){\n"
     "    "+st+" x=("+st+")h, y=("+st+")(h*40503u+7u);\n"
     "    "+ut+" ux=h, uy=h^0xFFFFFFFFu;\n"
     "    "+st+" smin=(x<y)?x:y;\n"
     "    "+ut+" umax=(ux>uy)?ux:uy;\n"
     "    acc=acc*131u+(unsigned)smin+(umax>>1)+(unsigned)i;\n"
     "    h=h*1103515245u+12345u; }\n"
     "  return ("+t+")acc; }\n",
     {0x0BADF00Du}, "OptStress220", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 =
    makeOptStress220TC("x64o220", "long", "int", "unsigned");
static const std::vector<RoundTripTC> kX86 =
    makeOptStress220TC("x86o220", "int", "int", "unsigned");
static const std::vector<RoundTripTC> kA64 =
    makeOptStress220TC("a64o220", "long", "int", "unsigned");
static const std::vector<RoundTripTC> kARM =
    makeOptStress220TC("armo220", "int", "int", "unsigned");

INSTANTIATE_TEST_SUITE_P(OptStress220, X64OptStress220RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress220, X86OptStress220RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress220, A64OptStress220RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress220, ARM32OptStress220RT, ::testing::ValuesIn(kARM), rtTCName);
