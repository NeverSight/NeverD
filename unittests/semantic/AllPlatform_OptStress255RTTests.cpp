//===- AllPlatform_OptStress255RTTests.cpp - bit/div at -O0 ==============//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The most fold-prone families from OptStress252 (bit manipulation) and
// OptStress253 (constant div/mod) rerun at -O0, as a sink differential.  At -O0
// clang spills every temporary and emits explicit movzx/setcc/mul-high
// sequences, so the bit-addressing and magic-multiply lowerings hit different
// lifter paths than the folded -O2 forms.
//
//   * bset0   - bitset set by runtime index + popcount.
//   * bextr0  - variable-width bitfield extract at a runtime offset.
//   * divc0   - unsigned division by several constants.
//   * modc0   - unsigned modulo by several constants.
//   * digsum0 - decimal decomposition via repeated /10 and %10.
//   * bmix0   - interleave test/set/toggle + popcount.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O0.  Only 32-bit ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress255RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress255RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress255RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress255RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress255RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress255RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress255RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress255RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress255TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Bitset set by runtime index + popcount, at -O0.
    {p+"_bset0",
     t+" "+p+"_bset0("+t+" a){ unsigned h=(unsigned)a; unsigned bs[8];\n"
     "  for(int i=0;i<8;i++) bs[i]=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned n=(h>>7)&255u;\n"
     "    bs[n>>5]|=1u<<(n&31u); }\n"
     "  unsigned acc=0; for(int i=0;i<8;i++) acc=acc*131u+(unsigned)__builtin_popcount(bs[i]);\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress255", 0},

    // Variable-width bitfield extract at a runtime offset, at -O0.
    {p+"_bextr0",
     t+" "+p+"_bextr0("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned off=h&15u, w=((h>>4)&15u)+1u; unsigned x=h^0x9e3779b9u;\n"
     "    unsigned mask=(w>=32u)?0xffffffffu:((1u<<w)-1u);\n"
     "    unsigned f=(x>>off)&mask; acc=acc*131u+f+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress255", 0},

    // Unsigned division by several constants, at -O0.
    {p+"_divc0",
     t+" "+p+"_divc0("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u; unsigned x=h^0x9e3779b9u;\n"
     "    unsigned s=x/3u+x/5u+x/7u+x/9u+x/11u+x/13u+x/255u;\n"
     "    acc=acc*131u+s+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress255", 0},

    // Unsigned modulo by several constants, at -O0.
    {p+"_modc0",
     t+" "+p+"_modc0("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u; unsigned x=h+0x7f4a7c15u;\n"
     "    unsigned s=x%3u+x%7u+x%10u+x%100u+x%255u+x%1000u;\n"
     "    acc=acc*131u+s+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress255", 0},

    // Decimal decomposition via repeated /10 and %10, at -O0.
    {p+"_digsum0",
     t+" "+p+"_digsum0("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u; unsigned x=h; unsigned dsum=0;\n"
     "    for(int d=0;d<10&&x;d++){ unsigned dig=x%10u; x/=10u; dsum+=dig; }\n"
     "    acc=acc*131u+dsum; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress255", 0},

    // Interleave test/set/toggle + popcount, at -O0.
    {p+"_bmix0",
     t+" "+p+"_bmix0("+t+" a){ unsigned h=(unsigned)a; unsigned w=0xdeadbeefu; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned n=(h>>5)&31u;\n"
     "    unsigned bit=(w>>n)&1u;\n"
     "    if(h&0x10000u) w|=1u<<n; else if(h&0x20000u) w&=~(1u<<n); else w^=1u<<n;\n"
     "    acc=acc*131u+bit+(unsigned)__builtin_popcount(w); }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress255", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress255TC("x64o255", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress255TC("x86o255", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress255TC("a64o255", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress255TC("armo255", "int");

INSTANTIATE_TEST_SUITE_P(OptStress255, X64OptStress255RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress255, X86OptStress255RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress255, A64OptStress255RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress255, ARM32OptStress255RT, ::testing::ValuesIn(kARM), rtTCName);
