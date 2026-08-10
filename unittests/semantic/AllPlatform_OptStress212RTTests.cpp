//===- AllPlatform_OptStress212RTTests.cpp - bit-scan value correctness ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails that stress the EXACT numeric result of clz/ctz/ffs lifts:
// the count drives a rodata table index or a switch selector, so an off-by-one
// in the `(bits-1) - LZCOUNT` / zero-source handling shows up as a wrong value
// or an out-of-bounds read rather than being masked by a tolerant fold.
//
//   * clzidx - 64-bit clz(value)|1 indexes a rodata weight table.
//   * ctzidx - 64-bit ctz of an or-guarded value indexes a rodata table.
//   * ffsmix - __builtin_ffsll (returns 0 for a zero input, else 1+ctz) folded
//              in a loop, exercising the find-first-set zero path.
//   * clzsw  - switch on the clz bucket of a loop-carried value.
//
// Integer in / integer out, file-scope const (rodata) tables, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress212RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress212RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress212RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress212RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress212RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress212RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress212RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress212RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress212TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // clz bucket (0..64) indexes a 65-entry rodata weight table.
    {p+"_clzidx",
     "static const unsigned "+p+"_w[65]={\n"
     "3,5,7,11,13,17,19,23,29,31,37,41,43,47,53,59,61,67,71,73,79,83,89,97,\n"
     "101,103,107,109,113,127,131,137,139,149,151,157,163,167,173,179,181,\n"
     "191,193,197,199,211,223,227,229,233,239,241,251,257,263,269,271,277,\n"
     "281,283,293,307,311,313,317};\n"
     +t+" "+p+"_clzidx("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a^0x9E3779B97F4A7C15ULL;\n"
     "  unsigned out=0;\n"
     "  for(int i=0;i<48;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL;\n"
     "    unsigned c=(unsigned)__builtin_clzll(h|1ULL);\n"
     "    out=out*131u+"+p+"_w[c]; }\n"
     "  return ("+t+")out; }\n",
     {0x51u}, "OptStress212", 2},

    // ctz bucket (0..63) indexes a rodata table.
    {p+"_ctzidx",
     "static const unsigned "+p+"_t[64]={\n"
     "9,7,5,3,11,13,2,17,19,23,29,31,37,41,43,47,53,59,61,67,71,73,79,83,\n"
     "89,97,101,103,107,109,113,127,131,137,139,149,151,157,163,167,173,179,\n"
     "181,191,193,197,199,211,223,227,229,233,239,241,251,257,263,269,271,\n"
     "277,281,283,293};\n"
     +t+" "+p+"_ctzidx("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a^0xD1B54A32D192ED03ULL;\n"
     "  unsigned out=0;\n"
     "  for(int i=0;i<48;i++){ h=h*6364136223846793005ULL+1u;\n"
     "    unsigned c=(unsigned)__builtin_ctzll(h|0x8000000000000000ULL);\n"
     "    out=out*131u+"+p+"_t[c&63u]; }\n"
     "  return ("+t+")out; }\n",
     {0x2Du}, "OptStress212", 2},

    // __builtin_ffsll: 0 for a zero input, else 1 + ctz (find-first-set zero path).
    {p+"_ffsmix",
     t+" "+p+"_ffsmix("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a*0x100000001B3ULL;\n"
     "  unsigned s=(unsigned)a|1u, out=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned long long v=h&((unsigned long long)s<<((s>>9)&31u));\n"
     "    int f=__builtin_ffsll(v);\n"
     "    out=out*131u+(unsigned)f; h+=v^0x5851F42D4C957F2DULL+(unsigned)f; }\n"
     "  return ("+t+")out; }\n",
     {0x88u}, "OptStress212", 2},

    // switch on the clz bucket of a loop-carried value.
    {p+"_clzsw",
     t+" "+p+"_clzsw("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a^0xABCDEF0123456789ULL;\n"
     "  unsigned out=0;\n"
     "  for(int i=0;i<64;i++){ h=h*6364136223846793005ULL+1u;\n"
     "    unsigned c=(unsigned)__builtin_clzll(h|1ULL);\n"
     "    unsigned r;\n"
     "    switch(c>>3){\n"
     "      case 0: r=c*7u+1u; break;\n"
     "      case 1: r=c*5u+3u; break;\n"
     "      case 2: r=c*3u+5u; break;\n"
     "      case 3: r=c*2u+7u; break;\n"
     "      default: r=c+11u; break; }\n"
     "    out=out*131u+r; }\n"
     "  return ("+t+")out; }\n",
     {0x6Au}, "OptStress212", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress212TC("x64o212", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress212TC("x86o212", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress212TC("a64o212", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress212TC("armo212", "int");

INSTANTIATE_TEST_SUITE_P(OptStress212, X64OptStress212RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress212, X86OptStress212RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress212, A64OptStress212RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress212, ARM32OptStress212RT, ::testing::ValuesIn(kARM), rtTCName);
