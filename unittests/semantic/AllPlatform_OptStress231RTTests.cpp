//===- AllPlatform_OptStress231RTTests.cpp - O0 lifting stress ===========//
//
// The whole OptStress breadth so far compiles at -O2, whose register allocator
// keeps most values in full-width registers and folds flag chains.  -O0 emits
// dramatically different machine code: explicit sub-register moves (movb /
// movzbl / setcc), per-statement stack spills and reloads, and an unfolded
// flag computation for every comparison.  That is exactly the shape where
// sub-register tracking (#157f) and flag handling bugs live and where -O2 can
// hide them.  This file re-probes the most bug-prone families at -O0.
//
//   * cmpsel - one compare feeds a select and a branch (cross-block flags).
//   * uaddc  - loop-carried unsigned add-with-carry.
//   * bytemix- 4-byte decompose / 8-bit wrap / 16-bit slice.
//   * signext- sign-extend i8 / i16 slices, sum widened.
//   * mword  - 64-bit value split, 64-bit compare picks the merge.
//   * histbin- load-modify-store into a local count array.
//
// Only 32-bit math plus 64-bit add / constant-shift / compare is used so no
// 64-bit shift/div libcall is emitted at -O0.  Integer in / integer out,
// LCG-seeded, folded to one integer return.  All four targets, -O0.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress231RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress231RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress231RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress231RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress231RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress231RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress231RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress231RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress231TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // One compare feeds a select and a branch.
    {p+"_cmpsel",
     t+" "+p+"_cmpsel("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)h, y=(int)(h>>5);\n"
     "    int m=(x<y)?x:y;\n"
     "    if(x<y) acc+=(unsigned)m; else acc-=(unsigned)m;\n"
     "    unsigned u=h, w=h*2654435761u;\n"
     "    acc += (u<w) ? (u-w) : (w-u);\n"
     "    acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress231", 0},

    // Loop-carried unsigned add-with-carry.
    {p+"_uaddc",
     t+" "+p+"_uaddc("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0, carry=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned s; unsigned c=__builtin_add_overflow(h, carry, &s);\n"
     "    unsigned s2; c |= __builtin_add_overflow(s, (h>>7), &s2);\n"
     "    carry=c; acc=acc*131u+s2+c+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress231", 0},

    // 4-byte decompose / 8-bit wrap / 16-bit slice.
    {p+"_bytemix",
     t+" "+p+"_bytemix("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned b0=h&0xffu, b1=(h>>8)&0xffu, b2=(h>>16)&0xffu, b3=(h>>24)&0xffu;\n"
     "    unsigned char r=(unsigned char)(b0+b1+b2+b3);\n"
     "    unsigned short s=(unsigned short)(h ^ (h>>16));\n"
     "    acc=acc*131u+(unsigned)r+(unsigned)s+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress231", 0},

    // Sign-extend i8 / i16 slices, sum widened.
    {p+"_signext",
     t+" "+p+"_signext("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    signed char sb=(signed char)h; short sh=(short)(h>>8);\n"
     "    int v=(int)sb + (int)sh;\n"
     "    acc=acc*131u+(unsigned)v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress231", 0},

    // 64-bit value split, 64-bit compare picks the merge.
    {p+"_mword",
     t+" "+p+"_mword("+t+" a){ unsigned long long h=(unsigned long long)a ^ 0x9E3779B97F4A7C15ULL;\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL;\n"
     "    unsigned long long g=h+((unsigned long long)(unsigned)i<<32);\n"
     "    unsigned hi=(unsigned)(g>>32), lo=(unsigned)g;\n"
     "    unsigned v = (g > 0x8000000000000000ULL) ? (hi^lo) : (hi+lo);\n"
     "    acc=acc*131u+v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress231", 0},

    // Load-modify-store into a local count array.
    {p+"_histbin",
     t+" "+p+"_histbin("+t+" a){ unsigned h=(unsigned)a; unsigned cnt[16];\n"
     "  for(int i=0;i<16;i++) cnt[i]=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; cnt[h&15u]+=((h>>4)&7u)+1u; }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<16;i++){ acc=acc*131u+cnt[i]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress231", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress231TC("x64o231", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress231TC("x86o231", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress231TC("a64o231", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress231TC("armo231", "int");

INSTANTIATE_TEST_SUITE_P(OptStress231, X64OptStress231RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress231, X86OptStress231RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress231, A64OptStress231RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress231, ARM32OptStress231RT, ::testing::ValuesIn(kARM), rtTCName);
