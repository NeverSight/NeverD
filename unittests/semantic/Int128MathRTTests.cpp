//===- Int128MathRTTests.cpp - native 128-bit integer math ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// clang -O2 `__int128` kernels on the two 64-bit targets (x86-64, AArch64).
// A native 128-bit value lives in a *register pair*, so these lower to the
// hardware multi-word idioms that no narrower test reaches: 128-bit add/sub
// carry chains (adc/sbb on x86-64, adds/adc on AArch64), 128-bit variable
// shifts/rotates (shld/shrd; lsl/lsr/orr pairs), 128-bit compares (cmp;sbb /
// cmp;cset chains), and the genuine 64x64->128 widening multiply
// (mulq/imulq -> RDX:RAX; mul+umulh/smulh).  These exercise the custom MedIR
// optimizer's modeling of paired/wide values and carry flags directly.
//
// Only the widening multiply form `(__int128)u64 * u64` is used; the full
// 128x128 multiply and 128-bit division/modulo are avoided because clang
// lowers them to __multi3/__udivti3 libcalls Unicorn cannot resolve.  Every
// kernel folds its full 128-bit state into a 64-bit return (`lo ^ hi`) so the
// high half is still checked even though the harness compares one result
// register.  i386/ARM32 have no __int128, so this is a 64-bit-arch pair.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64Int128RT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64Int128RT, Verify) { roundTripX64(GetParam()); }

class A64Int128RT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64Int128RT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeI128(const char *prefix) {
  std::string p = prefix;
  return {
    // Unsigned 64x64->128 widening multiply-accumulate (mulq / mul+umulh).
    {p+"_uwmul",
     "long "+p+"_uwmul(long a){\n"
     "  unsigned __int128 acc=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned long x=(unsigned long)(a*(i+1))*2654435761UL+(unsigned)i;\n"
     "    unsigned long y=(unsigned long)(a+i*131)*40503UL+7UL;\n"
     "    acc += (unsigned __int128)x * y; }\n"
     "  return (long)((unsigned long)acc ^ (unsigned long)(acc>>64));\n}\n",
     {0x1234567ULL}, "Int128", 2, ""},

    // Signed 64x64->128 widening multiply-accumulate (imulq / mul+smulh).
    {p+"_swmul",
     "long "+p+"_swmul(long a){\n"
     "  __int128 acc=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    long x=(long)(a*(i+1)) - (long)i*131;\n"
     "    long y=(long)(a - i*7) + 13;\n"
     "    acc += (__int128)x * y; }\n"
     "  unsigned long lo=(unsigned long)acc;\n"
     "  unsigned long hi=(unsigned long)((unsigned __int128)acc>>64);\n"
     "  return (long)(lo ^ hi);\n}\n",
     {0x2233445ULL}, "Int128", 2, ""},

    // 128-bit add/sub carry chains (adc/sbb) with a built-up high half.
    {p+"_addsub",
     "long "+p+"_addsub(long a){\n"
     "  unsigned __int128 acc=0;\n"
     "  for(int i=0;i<100;i++){\n"
     "    unsigned __int128 x=((unsigned __int128)(unsigned long)(a*(i+1))<<64)\n"
     "                        | (unsigned long)(a+i*7+13);\n"
     "    acc += x; acc -= (x>>1);\n"
     "    acc ^= (acc<<3); }\n"
     "  return (long)((unsigned long)acc ^ (unsigned long)(acc>>64));\n}\n",
     {0x3344556ULL}, "Int128", 2, ""},

    // 128-bit variable rotate via shift pair + or (shld/shrd; lsl/lsr/orr).
    {p+"_varrot",
     "long "+p+"_varrot(long a){\n"
     "  unsigned __int128 x=((unsigned __int128)(unsigned long)a<<64)\n"
     "                      |0x123456789ABCDEFULL, acc=0;\n"
     "  for(int i=0;i<90;i++){\n"
     "    int k=(a+i)&127;\n"
     "    unsigned __int128 r = k? ((x<<k)|(x>>(128-k))) : x;\n"
     "    acc += r; x = r ^ (x + 0x9E3779B97F4A7C15ULL); }\n"
     "  return (long)((unsigned long)acc ^ (unsigned long)(acc>>64));\n}\n",
     {0x4455667ULL}, "Int128", 2, ""},

    // 128-bit unsigned/signed compare + min/max + sign-bit test.
    {p+"_cmpminmax",
     "long "+p+"_cmpminmax(long a){\n"
     "  unsigned __int128 acc=((unsigned __int128)0xF0F0F0F0F0F0F0F0ULL<<64)\n"
     "                        |0x0F0F0F0F0F0F0F0FULL;\n"
     "  for(int i=0;i<100;i++){\n"
     "    unsigned __int128 v=((unsigned __int128)(unsigned long)(a*(i+3))<<64)\n"
     "                        |((unsigned long)(a+i)*2654435761UL);\n"
     "    unsigned __int128 mn=(v<acc)?v:acc, mx=(v>acc)?v:acc;\n"
     "    acc=(mx-mn) ^ (mn+(mx<<1));\n"
     "    if((__int128)acc < 0) acc=~acc; }\n"
     "  return (long)((unsigned long)acc ^ (unsigned long)(acc>>64));\n}\n",
     {0x5566778ULL}, "Int128", 2, ""},

    // SplitMix-style 128-bit mixing built on 64x64->128 widening multiplies
    // and shift/xor (no full 128x128 multiply -> no __multi3 libcall).
    {p+"_mix",
     "long "+p+"_mix(long a){\n"
     "  unsigned __int128 acc=0;\n"
     "  unsigned long s=(unsigned long)a + 0x123456789ABCDEFULL;\n"
     "  for(int i=0;i<80;i++){\n"
     "    s += 0x9E3779B97F4A7C15ULL;\n"
     "    unsigned __int128 z=(unsigned __int128)s * 0xBF58476D1CE4E5B9ULL;\n"
     "    z ^= z>>64;\n"
     "    z += (unsigned __int128)(s^(unsigned)i) * 0x94D049BB133111EBULL;\n"
     "    acc += z; acc ^= acc<<13; acc ^= acc>>27; }\n"
     "  return (long)((unsigned long)acc ^ (unsigned long)(acc>>64));\n}\n",
     {0x6677889ULL}, "Int128", 2, ""},

    // 128-bit logical masks + fixed double-word shifts (SWAR-style on 128 bits).
    {p+"_logic",
     "long "+p+"_logic(long a){\n"
     "  unsigned __int128 acc=0;\n"
     "  for(int k=0;k<96;k++){\n"
     "    unsigned __int128 x=((unsigned __int128)(unsigned long)(a*(k+1))<<64)\n"
     "                        |(unsigned long)(a*40503UL+k);\n"
     "    x=((x&((unsigned __int128)0x5555555555555555ULL<<64\n"
     "          |0x5555555555555555ULL))<<1)\n"
     "      |((x>>1)&((unsigned __int128)0x5555555555555555ULL<<64\n"
     "          |0x5555555555555555ULL));\n"
     "    x ^= (x<<16)|(x>>112);\n"
     "    acc = (acc^x) + (acc<<7); }\n"
     "  return (long)((unsigned long)acc ^ (unsigned long)(acc>>64));\n}\n",
     {0x778899AULL}, "Int128", 2, ""},
  };
}

static const std::vector<RoundTripTC> kX64I128 = makeI128("x64i128");
static const std::vector<RoundTripTC> kA64I128 = makeI128("a64i128");
// clang-format on

INSTANTIATE_TEST_SUITE_P(Int128, X64Int128RT, ::testing::ValuesIn(kX64I128),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(Int128, A64Int128RT, ::testing::ValuesIn(kA64I128),
                         rtTCName);
