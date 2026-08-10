//===- AllPlatform_OptStress247RTTests.cpp - rotate/mul-high at -O0 ======//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The OptStress245/246 rotate, funnel and cross-word multiply families rerun
// at -O0 as a sink differential (cf. #504's -O0/-O1 probes).  At -O0 clang
// emits the explicit, unfolded forms — `shld`/`shrd` instead of a folded
// rotate, a real `mul` writing EDX:EAX with a separate `shrd` extraction on
// i386, per-statement stack spill/reload of every intermediate — which is the
// exact machine code where lift bugs hide and that -O2 folding routinely
// erases.  Same well-defined inputs as 245/246 (counts masked < width).
//
//   * rotl0   - rotate-left, explicit shl/shr/or at -O0.
//   * shrd0   - two-register funnel right (explicit shrd at -O0).
//   * q16_0   - unsigned Q16.16 multiply, explicit mul+shrd extraction.
//   * mulhi0  - whole high word, explicit mul + reg pick.
//   * q15mac0 - signed Q15 MAC, explicit smull + sub-word extraction.
//   * rotbyte0- byte-multiple rotate at -O0.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O0.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress247RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress247RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress247RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress247RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress247RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress247RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress247RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress247RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress247TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Rotate-left, explicit shl/shr/or at -O0.
    {p+"_rotl0",
     t+" "+p+"_rotl0("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned n=h&31u; unsigned x=h^0x9e3779b9u;\n"
     "    unsigned r=(x<<n)|(x>>((32u-n)&31u));\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress247", 0},

    // Two-register funnel right (explicit shrd at -O0).
    {p+"_shrd0",
     t+" "+p+"_shrd0("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned hi=h*40503u, lo=h; unsigned n=(h&31u)|1u;\n"
     "    unsigned r=(lo>>n)|(hi<<(32u-n));\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress247", 0},

    // Unsigned Q16.16 multiply, explicit mul + shrd extraction at -O0.
    {p+"_q16_0",
     t+" "+p+"_q16_0("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h, y=h*2654435761u+(unsigned)i;\n"
     "    unsigned r=(unsigned)(((unsigned long long)x*y)>>16);\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress247", 0},

    // Whole high word at -O0.
    {p+"_mulhi0",
     t+" "+p+"_mulhi0("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h^0x55aa55aau, y=h+0x7f4a7c15u;\n"
     "    unsigned r=(unsigned)(((unsigned long long)x*y)>>32);\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress247", 0},

    // Signed Q15 MAC, explicit smull + sub-word extraction at -O0.
    {p+"_q15mac0",
     t+" "+p+"_q15mac0("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)(short)h, y=(int)(short)(h>>16);\n"
     "    acc += (int)(((long long)x*y)>>15); }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x56789u}, "OptStress247", 0},

    // Byte-multiple rotate at -O0.
    {p+"_rotbyte0",
     t+" "+p+"_rotbyte0("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned n=(h&3u)*8u; unsigned x=h^0x33cc33ccu;\n"
     "    unsigned r=(x<<n)|(x>>((32u-n)&31u));\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress247", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress247TC("x64o247", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress247TC("x86o247", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress247TC("a64o247", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress247TC("armo247", "int");

INSTANTIATE_TEST_SUITE_P(OptStress247, X64OptStress247RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress247, X86OptStress247RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress247, A64OptStress247RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress247, ARM32OptStress247RT, ::testing::ValuesIn(kARM), rtTCName);
