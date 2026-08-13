//===- AllPlatform_LongLong64RTTests.cpp - 64-bit on 32-bit ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// clang -O2 64-bit `unsigned long long` kernels run across all four targets.
// On i386 and ARM32 a 64-bit value lives in a *register pair*, so every kernel
// lowers to multi-word add/sub carry chains (adc/sbb, adds/adc), double-width
// shifts (shld/shrd, lsl+lsr+orr), and inlined 64x64->64 multiplies — exactly
// the carry-chain / partial-register lowering that previously hid lifter and
// optimizer bugs, but never exercised on the 32-bit register-pair path until
// i386 became a roundtrip target.  On x86-64 / AArch64 the same source is a
// native 64-bit guardrail.
//
// Every kernel folds its full 64-bit state into a 32-bit `int` return
// (`(int)(h ^ (h>>32))`) so the high half matters even though the harness only
// compares the 32-bit result register.  No 64-bit division/modulo is used, so
// nothing lowers to a __udivdi3/__divdi3 libcall Unicorn lacks.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64LL64RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64LL64RT, Verify) { roundTripX64(GetParam()); }

class X86LL64RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86LL64RT, Verify) { roundTripX86(GetParam()); }

class A64LL64RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64LL64RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32LL64RT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32LL64RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeLL64(const char *prefix) {
  std::string p = prefix;
  return {
    // 64-bit FNV-1a: inlined 64x64 multiply + xor over a synthetic byte stream.
    {p+"_fnv1a64",
     "int "+p+"_fnv1a64(int a){\n"
     "  unsigned long long h=1469598103934665603ULL;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned char b=(unsigned char)(a*(i+1)+i*7);\n"
     "    h ^= b; h *= 1099511628211ULL; }\n"
     "  return (int)(h ^ (h>>32));\n}\n",
     {0x1234567ULL}, "LongLong64", 2, ""},

    // xorshift64*: constant-amount double-width shifts + xor + accumulate.
    {p+"_xorshift64",
     "int "+p+"_xorshift64(int a){\n"
     "  unsigned long long x=(unsigned long long)(unsigned)a*2654435761u\n"
     "                       +0x9E3779B97F4A7C15ULL, acc=0;\n"
     "  for(int i=0;i<80;i++){\n"
     "    x ^= x<<13; x ^= x>>7; x ^= x<<17;\n"
     "    acc += x; acc ^= acc>>11; }\n"
     "  return (int)(acc ^ (acc>>32));\n}\n",
     {0x2233445ULL}, "LongLong64", 2, ""},

    // SplitMix64: 64-bit add (carry chain) + multiply + shift mixing.
    {p+"_splitmix64",
     "int "+p+"_splitmix64(int a){\n"
     "  unsigned long long s=(unsigned long long)a+0x123456789ABCDEFULL, acc=0;\n"
     "  for(int i=0;i<70;i++){\n"
     "    s += 0x9E3779B97F4A7C15ULL;\n"
     "    unsigned long long z=s;\n"
     "    z=(z^(z>>30))*0xBF58476D1CE4E5B9ULL;\n"
     "    z=(z^(z>>27))*0x94D049BB133111EBULL;\n"
     "    z=z^(z>>31);\n"
     "    acc=acc*131u+z; }\n"
     "  return (int)(acc ^ (acc>>32));\n}\n",
     {0x3344556ULL}, "LongLong64", 2, ""},

    // Murmur3 64-bit finalizer chain: shift-xor + two 64-bit multiplies.
    {p+"_mix64",
     "int "+p+"_mix64(int a){\n"
     "  unsigned long long acc=0;\n"
     "  for(int k=0;k<96;k++){\n"
     "    unsigned long long h=(unsigned long long)(a+k*0x1000193);\n"
     "    h ^= h>>33; h *= 0xff51afd7ed558ccdULL;\n"
     "    h ^= h>>33; h *= 0xc4ceb9fe1a85ec53ULL;\n"
     "    h ^= h>>33; acc += h; }\n"
     "  return (int)(acc ^ (acc>>32));\n}\n",
     {0x4455667ULL}, "LongLong64", 2, ""},

    // 64-bit unsigned compare / min / max chain + signed sign-bit test.
    {p+"_cmpminmax64",
     "int "+p+"_cmpminmax64(int a){\n"
     "  unsigned long long acc=0xF0F0F0F0F0F0F0F0ULL;\n"
     "  for(int i=0;i<100;i++){\n"
     "    unsigned long long v=(unsigned long long)(unsigned)(a*(i+3))\n"
     "                          *2654435761ULL + i;\n"
     "    unsigned long long mn=(v<acc)?v:acc, mx=(v>acc)?v:acc;\n"
     "    acc=(mx-mn) ^ (mn+(mx<<1));\n"
     "    if((long long)acc<0) acc=~acc; }\n"
     "  return (int)(acc ^ (acc>>32));\n}\n",
     {0x5566778ULL}, "LongLong64", 2, ""},

    // Explicit multi-word carry: detect 64-bit add overflow (nlo<lo).
    {p+"_carryacc64",
     "int "+p+"_carryacc64(int a){\n"
     "  unsigned long long lo=0, hi=0;\n"
     "  for(int i=0;i<120;i++){\n"
     "    unsigned long long x=(unsigned long long)(unsigned)(a*(i+1)+i*131);\n"
     "    unsigned long long nlo=lo+x;\n"
     "    if(nlo<lo) hi++;\n"
     "    lo=nlo; hi += x>>3; }\n"
     "  unsigned long long m=lo^hi;\n"
     "  return (int)(m ^ (m>>32) ^ lo ^ hi);\n}\n",
     {0x6677889ULL}, "LongLong64", 2, ""},

    // Variable-amount 64-bit rotate (shld/shrd by CL + zero-shift guard).
    {p+"_varrot64",
     "int "+p+"_varrot64(int a){\n"
     "  unsigned long long x=(unsigned long long)(unsigned)a*0x100000001ULL\n"
     "                       +0xABCDEFULL, acc=0;\n"
     "  for(int i=0;i<90;i++){\n"
     "    int k=(a+i)&63;\n"
     "    unsigned long long r=k?((x<<k)|(x>>(64-k))):x;\n"
     "    acc += r; x = r ^ (x*3+1); }\n"
     "  return (int)(acc ^ (acc>>32));\n}\n",
     {0x778899AULL}, "LongLong64", 2, ""},

    // 64-bit SWAR bit reversal: wide AND masks + fixed shifts + 32-bit swap.
    {p+"_bitrev64",
     "int "+p+"_bitrev64(int a){\n"
     "  unsigned long long acc=0;\n"
     "  for(int k=0;k<64;k++){\n"
     "    unsigned long long x=(unsigned long long)(a*(k+1))\n"
     "                          *0x9E3779B97F4A7C15ULL;\n"
     "    x=((x&0x5555555555555555ULL)<<1)|((x>>1)&0x5555555555555555ULL);\n"
     "    x=((x&0x3333333333333333ULL)<<2)|((x>>2)&0x3333333333333333ULL);\n"
     "    x=((x&0x0F0F0F0F0F0F0F0FULL)<<4)|((x>>4)&0x0F0F0F0F0F0F0F0FULL);\n"
     "    x=((x&0x00FF00FF00FF00FFULL)<<8)|((x>>8)&0x00FF00FF00FF00FFULL);\n"
     "    x=((x&0x0000FFFF0000FFFFULL)<<16)|((x>>16)&0x0000FFFF0000FFFFULL);\n"
     "    x=(x<<32)|(x>>32);\n"
     "    acc ^= x; acc = acc*131u+k; }\n"
     "  return (int)(acc ^ (acc>>32));\n}\n",
     {0x1020304ULL}, "LongLong64", 2, ""},
  };
}

static const std::vector<RoundTripTC> kX64LL   = makeLL64("x64ll");
static const std::vector<RoundTripTC> kX86LL   = makeLL64("x86ll");
static const std::vector<RoundTripTC> kA64LL   = makeLL64("a64ll");
static const std::vector<RoundTripTC> kARM32LL = makeLL64("armll");
// clang-format on

INSTANTIATE_TEST_SUITE_P(LongLong64, X64LL64RT, ::testing::ValuesIn(kX64LL),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(LongLong64, X86LL64RT, ::testing::ValuesIn(kX86LL),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(LongLong64, A64LL64RT, ::testing::ValuesIn(kA64LL),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(LongLong64, ARM32LL64RT, ::testing::ValuesIn(kARM32LL),
                         rtTCName);
