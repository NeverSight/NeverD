//===- AllPlatform_OptStress236RTTests.cpp - SWAR bit tricks ============//
//
// SIMD-within-a-register (SWAR) byte-parallel idioms and bit-permutation
// sequences.  These are long chains of AND/OR/XOR/SHIFT/SUB with magic
// constants — exactly the shape the optimizer is most tempted to over-simplify
// or mis-fold, and clang frequently pattern-matches some of them to a single
// popcount / rbit / rev instruction, so the round-trip also exercises
// instruction selection.  Pure 32-bit (no 64-bit variable shift / divide), so
// i386 and ARM32 stay libcall-free.
//
//   * popcnt - SWAR population count.
//   * bitrev - 32-bit bit reversal by mask/shift.
//   * haszero- byte-zero detection idiom folded into the hash.
//   * pavg   - parallel byte average.
//   * parity - bit parity via xor-fold + magic table shift.
//   * nybswap- nibble/byte permutation network.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress236RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress236RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress236RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress236RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress236RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress236RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress236RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress236RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress236TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // SWAR population count.
    {p+"_popcnt",
     t+" "+p+"_popcnt("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned v=h;\n"
     "    v=v-((v>>1)&0x55555555u);\n"
     "    v=(v&0x33333333u)+((v>>2)&0x33333333u);\n"
     "    v=(v+(v>>4))&0x0F0F0F0Fu;\n"
     "    unsigned r=(v*0x01010101u)>>24;\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress236", 2},

    // 32-bit bit reversal.
    {p+"_bitrev",
     t+" "+p+"_bitrev("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned x=h;\n"
     "    x=((x>>1)&0x55555555u)|((x&0x55555555u)<<1);\n"
     "    x=((x>>2)&0x33333333u)|((x&0x33333333u)<<2);\n"
     "    x=((x>>4)&0x0F0F0F0Fu)|((x&0x0F0F0F0Fu)<<4);\n"
     "    x=((x>>8)&0x00FF00FFu)|((x&0x00FF00FFu)<<8);\n"
     "    x=(x>>16)|(x<<16);\n"
     "    acc=acc*131u+x+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress236", 2},

    // Byte-zero detection idiom folded into the hash.
    {p+"_haszero",
     t+" "+p+"_haszero("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned v=h|0x00010000u;\n"
     "    unsigned z=(v-0x01010101u)&~v&0x80808080u;\n"
     "    unsigned r=z?(z>>7):(v^0xdeadbeefu);\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress236", 2},

    // Parallel byte average.
    {p+"_pavg",
     t+" "+p+"_pavg("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h, y=h*2654435761u;\n"
     "    unsigned avg=(x&y)+(((x^y)>>1)&0x7F7F7F7Fu);\n"
     "    acc=acc*131u+avg+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress236", 2},

    // Bit parity via xor-fold + magic-table shift.
    {p+"_parity",
     t+" "+p+"_parity("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned x=h;\n"
     "    x^=x>>16; x^=x>>8; x^=x>>4; x&=0xfu;\n"
     "    unsigned par=(0x6996u>>x)&1u;\n"
     "    unsigned r=par?(h^0x55555555u):(h+0x9E3779B9u);\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress236", 2},

    // Nibble/byte permutation network.
    {p+"_nybswap",
     t+" "+p+"_nybswap("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned x=h;\n"
     "    x=((x&0x0F0F0F0Fu)<<4)|((x&0xF0F0F0F0u)>>4);\n"
     "    x=((x&0x00FF00FFu)<<8)|((x&0xFF00FF00u)>>8);\n"
     "    unsigned y=((x<<3)|(x>>29))^((x>>5)|(x<<27));\n"
     "    acc=acc*131u+y+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress236", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress236TC("x64o236", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress236TC("x86o236", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress236TC("a64o236", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress236TC("armo236", "int");

INSTANTIATE_TEST_SUITE_P(OptStress236, X64OptStress236RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress236, X86OptStress236RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress236, A64OptStress236RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress236, ARM32OptStress236RT, ::testing::ValuesIn(kARM), rtTCName);
