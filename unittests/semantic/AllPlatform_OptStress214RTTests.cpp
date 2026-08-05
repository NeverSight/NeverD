//===- AllPlatform_OptStress214RTTests.cpp - byte-swap / reverse / rotate =//
//
// Cross-target byte-manipulation guardrails: `__builtin_bswap*` (x86 `bswap`,
// ARM/AArch64 `rev`/`rev16`), variable rotates (x86 `rol`/`ror`, ARM `ror`),
// and hand-rolled byte/nibble reshuffles whose result drives a rodata index, so
// a wrong byte order or rotate amount shows up as a value/READ mismatch.
//
//   * bswapmix - __builtin_bswap64/32/16 folded into a hash mixer.
//   * rotidx   - a variable rotate result indexes a rodata table.
//   * byterev  - hand-rolled byte reversal (shift/mask/or) cross-checked vs swap.
//   * nibperm  - per-nibble permutation through a 16-entry rodata table.
//
// Integer in / integer out, LCG-seeded, folded to one integer return; no float
// / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress214RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress214RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress214RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress214RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress214RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress214RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress214RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress214RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress214TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // __builtin_bswap{16,32,64} folded into a hash mixer.
    {p+"_bswapmix",
     t+" "+p+"_bswapmix("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a^0x0123456789ABCDEFULL;\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    h^=__builtin_bswap64(h+s);\n"
     "    unsigned lo=__builtin_bswap32((unsigned)h ^ s);\n"
     "    unsigned short w=__builtin_bswap16((unsigned short)(h>>16));\n"
     "    h=h*6364136223846793005ULL+lo+w; h^=h>>31; }\n"
     "  return ("+t+")(h^(h>>32)); }\n",
     {0x4Bu}, "OptStress214", 2},

    // A variable rotate result indexes a 64-entry rodata table.
    {p+"_rotidx",
     "static const unsigned "+p+"_tb[64]={\n"
     "2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,53,59,61,67,71,73,79,83,89,\n"
     "97,101,103,107,109,113,127,131,137,139,149,151,157,163,167,173,179,\n"
     "181,191,193,197,199,211,223,227,229,233,239,241,251,257,263,269,271,\n"
     "277,281,283,293,307};\n"
     +t+" "+p+"_rotidx("+t+" a){\n"
     "  unsigned h=(unsigned)a^0xDEADBEEFu, out=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned r=(h>>3)&31u;\n"
     "    unsigned v=(h<<r)|(h>>((32u-r)&31u));\n"
     "    out=out*131u+"+p+"_tb[v&63u]; h^=v; }\n"
     "  return ("+t+")out; }\n",
     {0x4Cu}, "OptStress214", 2},

    // Hand-rolled 32-bit byte reversal cross-checked against the builtin swap.
    {p+"_byterev",
     t+" "+p+"_byterev("+t+" a){\n"
     "  unsigned h=(unsigned)a|1u, out=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned r=((h&0xFFu)<<24)|((h&0xFF00u)<<8)|((h>>8)&0xFF00u)|((h>>24)&0xFFu);\n"
     "    out=out*131u+(r^__builtin_bswap32(h)); h^=r; }\n"
     "  return ("+t+")out; }\n",
     {0x4Du}, "OptStress214", 2},

    // Per-nibble permutation through a 16-entry rodata table.
    {p+"_nibperm",
     "static const unsigned char "+p+"_np[16]={\n"
     "0x9,0x4,0xA,0xB,0xD,0x1,0x8,0x5,0x6,0x2,0x0,0x3,0xC,0xE,0xF,0x7};\n"
     +t+" "+p+"_nibperm("+t+" a){\n"
     "  unsigned h=(unsigned)a^0x5A5A5A5Au, out=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u; unsigned r=0;\n"
     "    for(int n=0;n<8;n++){ unsigned nib=(h>>(n*4))&0xFu; r|=(unsigned)"+p+"_np[nib]<<(n*4); }\n"
     "    out=out*131u+r; h^=r; }\n"
     "  return ("+t+")out; }\n",
     {0x4Eu}, "OptStress214", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress214TC("x64o214", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress214TC("x86o214", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress214TC("a64o214", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress214TC("armo214", "int");

INSTANTIATE_TEST_SUITE_P(OptStress214, X64OptStress214RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress214, X86OptStress214RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress214, A64OptStress214RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress214, ARM32OptStress214RT, ::testing::ValuesIn(kARM), rtTCName);
