//===- AllPlatform_OptStress59RTTests.cpp - 64-bit integer mixing -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// 64-bit-heavy integer mixing / hashing kernels.  On the 32-bit targets
// (i386, ARM32) every 64-bit op is split into register-pair sequences —
// ADD/ADC, SUB/SBB, the three-way 64-bit IMUL/UMULL expansion, SHLD/SHRD and
// long-constant literal-pool loads.  Each kernel folds the high 32 bits back
// into the low half before the (possibly 32-bit) return so the optimizer
// cannot drop the upper word — forcing genuine wide-int lowering on all four
// targets.  No 64-bit variable divide/modulo (that would emit a __divdi3-style
// library call the harness cannot run); only multiply-by-constant, shift,
// rotate, xor, add/sub.
//
//   * splitmix - SplitMix64 finalizer chain over an LCG seed stream.
//   * fmix64   - Murmur3 64-bit finalizer (fmix64) accumulate.
//   * xxmix    - xxHash64-style 4-lane accumulate with rotl + prime multiply.
//   * rotmix   - rotate-heavy mixing (variable + constant rotl/rotr).
//   * wlcg     - 64-bit LCG, fold low^high each step.
//   * popmix   - 64-bit popcount / parity / bit-reverse mixing.
//
// All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress59RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress59RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress59RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress59RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress59RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress59RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress59RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress59RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress59TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // SplitMix64 finalizer chain over an LCG seed stream.
    {p+"_splitmix",
     t+" "+p+"_splitmix("+t+" a){\n"
     "  unsigned long long h=0, z=(unsigned long long)a;\n"
     "  for(int i=0;i<200;i++){ z+=0x9E3779B97F4A7C15ull;\n"
     "    unsigned long long x=z;\n"
     "    x=(x^(x>>30))*0xBF58476D1CE4E5B9ull;\n"
     "    x=(x^(x>>27))*0x94D049BB133111EBull;\n"
     "    x=x^(x>>31);\n"
     "    h+=x; h^=h>>29; }\n"
     "  h^=h>>32; return ("+t+")h; }\n",
     {0x71u}, "OptStress59", 2},

    // Murmur3 64-bit finalizer (fmix64) accumulate.
    {p+"_fmix64",
     t+" "+p+"_fmix64("+t+" a){\n"
     "  unsigned long long h=0, s=(unsigned long long)a;\n"
     "  for(int i=0;i<200;i++){ s=s*6364136223846793005ull+1442695040888963407ull;\n"
     "    unsigned long long k=s;\n"
     "    k^=k>>33; k*=0xff51afd7ed558ccdull;\n"
     "    k^=k>>33; k*=0xc4ceb9fe1a85ec53ull;\n"
     "    k^=k>>33;\n"
     "    h^=k; h=(h<<27)|(h>>37); h=h*5ull+0x52dce729ull; }\n"
     "  h^=h>>32; return ("+t+")h; }\n",
     {0x72u}, "OptStress59", 2},

    // xxHash64-style 4-lane accumulate with rotl + prime multiply.
    {p+"_xxmix",
     t+" "+p+"_xxmix("+t+" a){\n"
     "  unsigned long long s=(unsigned long long)a;\n"
     "  unsigned long long v1=s+0x9E3779B185EBCA87ull, v2=s+0xC2B2AE3D27D4EB4Full,"
     " v3=s, v4=s-0x9E3779B185EBCA87ull;\n"
     "  for(int i=0;i<200;i++){ s=s*2654435761ull+40503ull;\n"
     "    v1+=s*0xC2B2AE3D27D4EB4Full; v1=((v1<<31)|(v1>>33))*0x9E3779B185EBCA87ull;\n"
     "    v2+=(s^v1)*0xC2B2AE3D27D4EB4Full; v2=((v2<<29)|(v2>>35))*0x9E3779B185EBCA87ull;\n"
     "    v3+=(s+v2)*0x165667B19E3779F9ull; v3=((v3<<27)|(v3>>37));\n"
     "    v4^=(s-v3)*0x27D4EB2F165667C5ull; v4=((v4<<23)|(v4>>41)); }\n"
     "  unsigned long long h=((v1<<1)|(v1>>63))+((v2<<7)|(v2>>57))"
     "+((v3<<12)|(v3>>52))+((v4<<18)|(v4>>46));\n"
     "  h^=h>>32; return ("+t+")h; }\n",
     {0x73u}, "OptStress59", 2},

    // Rotate-heavy mixing: 32-bit variable rotate (SHL/SHR by CL, no 64-bit
     // variable-shift libcall on i386) folded into 64-bit constant rotates.
    {p+"_rotmix",
     t+" "+p+"_rotmix("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a|1ull, s=h;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245ull+12345ull;\n"
     "    unsigned r=((unsigned)(s>>11)&31u)|1u;\n"
     "    unsigned lo=(unsigned)h; unsigned rl=(lo<<r)|(lo>>(32-r));\n"
     "    unsigned long long x=h ^ ((unsigned long long)rl<<13);\n"
     "    x=(x>>17)|(x<<47);\n"
     "    h+=x*0x100000001B3ull; h^=(h<<25)|(h>>39); }\n"
     "  h^=h>>32; return ("+t+")h; }\n",
     {0x74u}, "OptStress59", 2},

    // 64-bit LCG, fold low^high each step.
    {p+"_wlcg",
     t+" "+p+"_wlcg("+t+" a){\n"
     "  unsigned long long h=0, s=(unsigned long long)a+0x123456789ABCDEFull;\n"
     "  for(int i=0;i<240;i++){ s=s*0x5DEECE66Dull+0xBull;\n"
     "    unsigned long long t2=s*0x2545F4914F6CDD1Dull;\n"
     "    h+=(t2&0xffffffffull)^(t2>>32);\n"
     "    h=h*0x9E3779B1ull+(s>>21); }\n"
     "  h^=h>>32; return ("+t+")h; }\n",
     {0x75u}, "OptStress59", 2},

    // 64-bit popcount / parity / bit-reverse mixing.
    {p+"_popmix",
     t+" "+p+"_popmix("+t+" a){\n"
     "  unsigned long long h=0, s=(unsigned long long)a;\n"
     "  for(int i=0;i<200;i++){ s=s*6364136223846793005ull+1ull;\n"
     "    unsigned long long x=s;\n"
     "    x=x-((x>>1)&0x5555555555555555ull);\n"
     "    x=(x&0x3333333333333333ull)+((x>>2)&0x3333333333333333ull);\n"
     "    x=(x+(x>>4))&0x0f0f0f0f0f0f0f0full;\n"
     "    unsigned long long pc=(x*0x0101010101010101ull)>>56;\n"
     "    unsigned long long par=s^(s>>1); par^=par>>2; par&=0x1111111111111111ull;\n"
     "    par=(par*0x1111111111111111ull)>>60;\n"
     "    h+=pc*131u+par; h^=(h<<21)|(h>>43); }\n"
     "  h^=h>>32; return ("+t+")h; }\n",
     {0x76u}, "OptStress59", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress59TC("x64o59", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress59TC("x86o59", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress59TC("a64o59", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress59TC("armo59", "int");

INSTANTIATE_TEST_SUITE_P(OptStress59, X64OptStress59RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress59, X86OptStress59RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress59, A64OptStress59RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress59, ARM32OptStress59RT, ::testing::ValuesIn(kARM), rtTCName);
