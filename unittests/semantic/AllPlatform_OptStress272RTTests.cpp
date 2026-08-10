//===- AllPlatform_OptStress272RTTests.cpp - variable div/mod at -O0 =====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Runtime-variable signed/unsigned division and modulo at -O0.  Earlier probes
// covered constant division (magic-multiply, OptStress253/255); this one forces
// real divide instructions (i386/x64 idiv/div, ARM32/AArch64 sdiv/udiv) with
// negative dividends and divisors, the div+mod-of-same-operands pairing (must
// recover a single divide), and division results steering control flow.  All
// dividends are bounded away from INT_MIN and all divisors forced nonzero so the
// ORIGINAL never traps (#DE) — the test must measure lift fidelity, not a trap.
//
//   * sdivmod  - signed quotient + remainder, small signed divisor.
//   * udivmod  - unsigned quotient + remainder.
//   * divident - (x/d)*d + x%d == x identity (div/mod must stay consistent).
//   * negdiv   - negative divisors (truncation toward zero).
//   * divdig   - decimal digit-sum via repeated /10 %10.
//   * divbkt   - quotient parity steers a branch; remainder bucketed.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit divides, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress272RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress272RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress272RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress272RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress272RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress272RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress272RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress272RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress272TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // signed quotient + remainder, small signed divisor (never -1 with INT_MIN).
    {p+"_sdivmod",
     t+" "+p+"_sdivmod("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)(h&0x3fffffu)-0x200000; int d=(int)((h>>3)&0x3fu)-32; if(d==0) d=1;\n"
     "    int q=x/d; int r=x%d; acc=acc*131 + q + r*7; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress272", 0},

    // unsigned quotient + remainder.
    {p+"_udivmod",
     t+" "+p+"_udivmod("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h; unsigned d=((h>>5)&0xffu)+1u;\n"
     "    acc=acc*131u + x/d + (x%d)*7u; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress272", 0},

    // (x/d)*d + x%d == x identity (div and mod must stay mutually consistent).
    {p+"_divident",
     t+" "+p+"_divident("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)(h&0x3fffffu)-0x200000; int d=(int)((h>>4)&0x1fu)+1;\n"
     "    int rebuilt=(x/d)*d + x%d; acc=acc*131 + (rebuilt - x) + x/d; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress272", 0},

    // negative divisors (truncation toward zero).
    {p+"_negdiv",
     t+" "+p+"_negdiv("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)(h&0xfffffu)-0x80000; int d=-(int)(((h>>6)&0xfu)+2);\n"
     "    int q=x/d; int r=x%d; acc=acc*131 + q*3 + r; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress272", 0},

    // decimal digit-sum via repeated /10 %10 (data-dependent loop trip count).
    {p+"_divdig",
     t+" "+p+"_divdig("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned v=h%1000000u; unsigned s=0;\n"
     "    while(v){ s+=v%10u; v/=10u; }\n"
     "    acc=acc*131u + s; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress272", 0},

    // quotient parity steers a branch; remainder bucketed into the accumulator.
    {p+"_divbkt",
     t+" "+p+"_divbkt("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)(h&0x1fffffu)-0x100000; int d=(int)((h>>3)&7u)+2;\n"
     "    int q=x/d; if(q&1) acc+=x%d; else acc-=x%d; acc=acc*3 + q; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress272", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress272TC("x64o272", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress272TC("x86o272", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress272TC("a64o272", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress272TC("armo272", "int");

INSTANTIATE_TEST_SUITE_P(OptStress272, X64OptStress272RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress272, X86OptStress272RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress272, A64OptStress272RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress272, ARM32OptStress272RT, ::testing::ValuesIn(kARM), rtTCName);
