//===- AllPlatform_OptStress245RTTests.cpp - rotate / funnel shift =======//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Rotates and two-register funnel shifts.  clang folds the safe rotate idiom
// `(x<<n)|(x>>((32-n)&31))` into a single `rol`/`ror` (x86), `ror`/`extr`
// (AArch64) or barrel-shifter rotate (ARM32), and the two-input form into
// `shld`/`shrd` (x86) or `extr` (AArch64).  This is the same shift family that
// produced the RCL/RCR and `vmov.i64` shift-by-bitwidth UB bugs, so it probes
// the boundary amounts (n==0, byte-multiple rotates) directly.  Variable
// counts are masked to [0,31] / [1,31] so no count is ever >= the width.
//
//   * rotl_var - rotate-left by a data-derived amount (hits n==0).
//   * rotr_var - rotate-right by a data-derived amount.
//   * fnl_shld - two-register funnel left (shld), count in [1,31].
//   * fnl_shrd - two-register funnel right (shrd), count in [1,31].
//   * rot_byte - rotate by byte multiples 0/8/16/24 (folds toward rev/bswap).
//   * rothash  - rotate+xor avalanche accumulator (murmur-style finalizer).
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.  Only 32-bit ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress245RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress245RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress245RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress245RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress245RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress245RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress245RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress245RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress245TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Rotate-left by a data-derived amount (the masked count hits 0).
    {p+"_rotl_var",
     t+" "+p+"_rotl_var("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned n=h&31u; unsigned x=h^0x9e3779b9u;\n"
     "    unsigned r=(x<<n)|(x>>((32u-n)&31u));\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress245", 2},

    // Rotate-right by a data-derived amount.
    {p+"_rotr_var",
     t+" "+p+"_rotr_var("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned n=(h>>3)&31u; unsigned x=h+0x7f4a7c15u;\n"
     "    unsigned r=(x>>n)|(x<<((32u-n)&31u));\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress245", 2},

    // Two-register funnel left (shld / extr), count forced into [1,31].
    {p+"_fnl_shld",
     t+" "+p+"_fnl_shld("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned hi=h, lo=h*2654435761u; unsigned n=(h&31u)|1u;\n"
     "    unsigned r=(hi<<n)|(lo>>(32u-n));\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress245", 2},

    // Two-register funnel right (shrd), count forced into [1,31].
    {p+"_fnl_shrd",
     t+" "+p+"_fnl_shrd("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned hi=h*40503u, lo=h; unsigned n=(h&31u)|1u;\n"
     "    unsigned r=(lo>>n)|(hi<<(32u-n));\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress245", 2},

    // Rotate by byte multiples 0/8/16/24 (folds toward rev/bswap on some ISAs).
    {p+"_rot_byte",
     t+" "+p+"_rot_byte("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned n=(h&3u)*8u; unsigned x=h^0x55aa55aau;\n"
     "    unsigned r=(x<<n)|(x>>((32u-n)&31u));\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress245", 2},

    // Rotate+xor avalanche accumulator (murmur-style finalizer).
    {p+"_rothash",
     t+" "+p+"_rothash("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h; x^=x>>16; x*=0x85ebca6bu;\n"
     "    x=(x<<13)|(x>>19); x^=x>>13; x*=0xc2b2ae35u;\n"
     "    x=(x<<15)|(x>>17); x^=x>>16;\n"
     "    acc=acc*131u+x+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress245", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress245TC("x64o245", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress245TC("x86o245", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress245TC("a64o245", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress245TC("armo245", "int");

INSTANTIATE_TEST_SUITE_P(OptStress245, X64OptStress245RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress245, X86OptStress245RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress245, A64OptStress245RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress245, ARM32OptStress245RT, ::testing::ValuesIn(kARM), rtTCName);
