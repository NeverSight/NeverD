//===- AllPlatform_ScalarMixRTTests.cpp - narrow-width scalar ---*- C++ -*-===//
//
// clang -O2 scalar kernels dominated by 8/16-bit arithmetic, signed/unsigned
// division+modulo, and char<->short<->int promotion chains.  These lower very
// differently per target: on i386/x86-64 to partial-register ops (al/ax with
// movzx/movsx) and hardware idiv/div, on AArch64/ARM32 to sxtb/uxtb/sxth/uxth
// plus sdiv/udiv.  i386 in particular has the thinnest algorithm-level coverage
// (most AllPlatform batches only run x64/a64/arm32), and its AL/AX partial
// registers are exactly where sub-register aliasing bugs hid before -- so these
// run on all four targets to flag any i386-specific lowering gap.  Each folds to
// an exact integer return.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64ScalarMixRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64ScalarMixRT, Verify) { roundTripX64(GetParam()); }

class X86ScalarMixRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86ScalarMixRT, Verify) { roundTripX86(GetParam()); }

class A64ScalarMixRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64ScalarMixRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32ScalarMixRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ScalarMixRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeScalarMix(const char *prefix) {
  std::string p = prefix;
  return {
    // CRC-8 bit loop: the accumulator is an unsigned char (al / w-reg byte) with
    // a sign-bit branch and an 8-bit (<<1)^poly each step.
    {p+"_crc8",
     "int "+p+"_crc8(int a){ unsigned char crc=0xFF; int acc=0;\n"
     "  for(int i=0;i<200;i++){ unsigned char b=(unsigned char)(a*(i+1)+i*7);\n"
     "    crc ^= b;\n"
     "    for(int k=0;k<8;k++){\n"
     "      if(crc&0x80) crc=(unsigned char)((crc<<1)^0x1D);\n"
     "      else crc=(unsigned char)(crc<<1); }\n"
     "    acc += (int)crc - (i&3); }\n"
     "  return acc; }\n",
     {0x1234567ULL}, "ScalarMix", 2, ""},

    // 16-bit saturating accumulator: short truncation each step + clamp.
    {p+"_short_sat",
     "int "+p+"_short_sat(int a){ short acc=0; int sum=0;\n"
     "  for(int i=0;i<300;i++){ int v=a*i - i*131;\n"
     "    int t=(int)acc + v;\n"
     "    if(t>32767) t=32767; else if(t<-32768) t=-32768;\n"
     "    acc=(short)t; sum += (int)acc; }\n"
     "  return sum; }\n",
     {0x2233445ULL}, "ScalarMix", 2, ""},

    // Signed division + modulo by a runtime divisor (idiv / sdiv).
    {p+"_idivmod",
     "int "+p+"_idivmod(int a){ int acc=0;\n"
     "  for(int i=1;i<=200;i++){ int d=((a^i)&31)+1; int n=a*i - i*7;\n"
     "    int q=n/d, r=n%d;\n"
     "    acc += q - r + ((q*r) & 0xFF); }\n"
     "  return acc; }\n",
     {0x3344556ULL}, "ScalarMix", 2, ""},

    // Unsigned division + modulo (div / udiv) with byte/halfword feedstock.
    {p+"_udivmod",
     "int "+p+"_udivmod(int a){ unsigned acc=0;\n"
     "  for(int i=1;i<=200;i++){ unsigned d=((unsigned)(a+i)&63u)+1u;\n"
     "    unsigned n=(unsigned)(a*i)*2654435761u;\n"
     "    acc += (n/d) ^ (n%d); }\n"
     "  return (int)acc; }\n",
     {0x4455667ULL}, "ScalarMix", 2, ""},

    // char/short promotion chain: signed+unsigned narrow truncation and
    // sign/zero extension feeding wider arithmetic (movsx/movzx, sxtb/uxth...).
    {p+"_promote",
     "int "+p+"_promote(int a){ int acc=0;\n"
     "  for(int i=0;i<200;i++){ signed char c=(signed char)(a+i);\n"
     "    unsigned char u=(unsigned char)(a*i);\n"
     "    short s=(short)(c*u - i);\n"
     "    unsigned short us=(unsigned short)(s + a);\n"
     "    acc += (int)c + (int)u - (int)s + (int)us; }\n"
     "  return acc; }\n",
     {0x5566778ULL}, "ScalarMix", 2, ""},

    // FNV-1a 32-bit byte hash: 8-bit load + xor + 32-bit multiply.
    {p+"_fnvhash",
     "int "+p+"_fnvhash(int a){ unsigned h=2166136261u;\n"
     "  for(int i=0;i<256;i++){ unsigned char b=(unsigned char)(a*(i*3+1)+i);\n"
     "    h ^= b; h *= 16777619u; }\n"
     "  return (int)(h ^ (h>>16)); }\n",
     {0x6677889ULL}, "ScalarMix", 2, ""},

    // Mixed signed/unsigned byte compares feeding a small histogram in a local
    // array (stack slots + byte loads/stores).
    {p+"_histbyte",
     "int "+p+"_histbyte(int a){ int h[8]={0,0,0,0,0,0,0,0}; int acc=0;\n"
     "  for(int i=0;i<240;i++){ unsigned char b=(unsigned char)(a*(i+1)+i*13);\n"
     "    h[b&7] += (int)(signed char)b; }\n"
     "  for(int k=0;k<8;k++) acc += h[k]*(k+1);\n"
     "  return acc; }\n",
     {0x778899AULL}, "ScalarMix", 2, ""},
  };
}

static const std::vector<RoundTripTC> kX64SM   = makeScalarMix("x64sm");
static const std::vector<RoundTripTC> kX86SM   = makeScalarMix("x86sm");
static const std::vector<RoundTripTC> kA64SM   = makeScalarMix("a64sm");
static const std::vector<RoundTripTC> kARM32SM = makeScalarMix("armsm");
// clang-format on

INSTANTIATE_TEST_SUITE_P(ScalarMix, X64ScalarMixRT, ::testing::ValuesIn(kX64SM),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(ScalarMix, X86ScalarMixRT, ::testing::ValuesIn(kX86SM),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(ScalarMix, A64ScalarMixRT, ::testing::ValuesIn(kA64SM),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(ScalarMix, ARM32ScalarMixRT,
                         ::testing::ValuesIn(kARM32SM), rtTCName);
