//===- AllPlatform_OptStress228RTTests.cpp - 64-bit compare/select =======//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Breadth probes for 64-bit compare / select / min-max, which on the 32-bit
// targets lower to multi-word (hi/lo) compare-and-conditional sequences and on
// the 64-bit targets to single-instruction compares whose flags feed a select
// or a branch.  Only 64-bit add / sub / shift / compare / and-or and 32x32->64
// widening multiply are used so no 64-bit div/overflow libcall is emitted.
//
//   * cmp64    - signed AND unsigned 64-bit compare, select + branch.
//   * sel64    - 64-bit select on the sign bit, loop-carried.
//   * wide_add - 64-bit add with a carry-detection compare folded back.
//   * minmax64 - running 64-bit min and max.
//   * eq64     - 64-bit equality / half-word equality cascade into a select.
//   * mul64c   - 32x32->64 widening multiply accumulated, sign-bit branch.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress228RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress228RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress228RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress228RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress228RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress228RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress228RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress228RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress228TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Signed AND unsigned 64-bit compare; select then branch.
    {p+"_cmp64",
     t+" "+p+"_cmp64("+t+" a){ unsigned long long x=(unsigned long long)a^0x1234567890ABCDEFULL;\n"
     "  unsigned long long y=x*2654435761u+1u; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ x=x*6364136223846793005ULL+1u; y=y*1103515245u+12345u;\n"
     "    unsigned v = (x<y) ? (unsigned)(x>>20) : (unsigned)(y>>12);\n"
     "    if((long long)x < (long long)y) v+=7u; else v+=3u;\n"
     "    acc=acc*131u+v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress228", 2},

    // 64-bit select on the sign bit, loop-carried.
    {p+"_sel64",
     t+" "+p+"_sel64("+t+" a){ unsigned long long h=(unsigned long long)a^0xDEADBEEFCAFEBABEULL;\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL;\n"
     "    unsigned long long m = (h&0x8000000000000000ULL) ? (h>>3) : (h<<3);\n"
     "    acc=acc*131u+(unsigned)(m^(m>>32))+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress228", 2},

    // 64-bit add with a carry-detection compare folded back.
    {p+"_wide_add",
     t+" "+p+"_wide_add("+t+" a){ unsigned long long acc64=(unsigned long long)a;\n"
     "  unsigned long long h=acc64|1u;\n"
     "  for(int i=0;i<128;i++){ h=h*6364136223846793005ULL+1u;\n"
     "    acc64 += h; acc64 ^= (acc64>>29);\n"
     "    if(acc64 < h) acc64 += 0x100000001ULL; }\n"
     "  return ("+t+")(unsigned)(acc64 ^ (acc64>>32)); }\n",
     {0x34567u}, "OptStress228", 2},

    // Running 64-bit min and max.
    {p+"_minmax64",
     t+" "+p+"_minmax64("+t+" a){ unsigned long long h=(unsigned long long)a^0xA5A5A5A5A5A5A5A5ULL;\n"
     "  unsigned long long mn=~0ULL, mx=0;\n"
     "  for(int i=0;i<128;i++){ h=h*6364136223846793005ULL+1u;\n"
     "    if(h<mn) mn=h; if(h>mx) mx=h; }\n"
     "  unsigned long long d=mn^mx;\n"
     "  return ("+t+")(unsigned)(d ^ (d>>32)); }\n",
     {0x45678u}, "OptStress228", 2},

    // 64-bit equality / half-word equality cascade into a select.
    {p+"_eq64",
     t+" "+p+"_eq64("+t+" a){ unsigned long long h=(unsigned long long)a;\n"
     "  unsigned long long g=h^0xFFFFFFFFFFFFFFFFULL; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*6364136223846793005ULL+1u; g=g*2862933555777941757ULL+3u;\n"
     "    unsigned v;\n"
     "    if(h==g) v=1u; else if((h&0xffffffffULL)==(g&0xffffffffULL)) v=2u;\n"
     "    else if((h>>32)==(g>>32)) v=3u; else v=(unsigned)(h^g);\n"
     "    acc=acc*131u+v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress228", 2},

    // 32x32->64 widening multiply accumulated, sign-bit branch.
    {p+"_mul64c",
     t+" "+p+"_mul64c("+t+" a){ unsigned h=(unsigned)a; unsigned long long acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h, y=h*2654435761u;\n"
     "    unsigned long long pr=(unsigned long long)x*(unsigned long long)y;\n"
     "    acc += pr ^ (pr>>17);\n"
     "    if(acc & 0x8000000000000000ULL) acc ^= 0x55ULL; }\n"
     "  return ("+t+")(unsigned)(acc ^ (acc>>32)); }\n",
     {0x6789Au}, "OptStress228", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress228TC("x64o228", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress228TC("x86o228", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress228TC("a64o228", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress228TC("armo228", "int");

INSTANTIATE_TEST_SUITE_P(OptStress228, X64OptStress228RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress228, X86OptStress228RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress228, A64OptStress228RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress228, ARM32OptStress228RT, ::testing::ValuesIn(kARM), rtTCName);
