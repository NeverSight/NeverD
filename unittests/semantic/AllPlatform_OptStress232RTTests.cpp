//===- AllPlatform_OptStress232RTTests.cpp - O1 lifting stress ===========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Companion to OptStress231 (-O0) at -O1: clang does register allocation and
// basic folding but skips the heavier -O2 vectorization / idiom recognition,
// so it emits yet another distinct instruction mix (scalar setcc + cmov,
// fewer SIMD substitutions, simpler addressing) -- a third lowering of the
// same bug-prone families to widen differential coverage.
//
//   * usubb  - loop-carried unsigned sub-with-borrow.
//   * addov  - signed add-overflow saturate (branch + select reuse of OF).
//   * swap16 - swap the two 16-bit halves and recombine.
//   * mix16  - 16-bit multiply/xor discarding the carry out of 16 bits.
//   * sel64  - 64-bit select on the sign bit, loop-carried (constant shifts).
//   * cmp64  - signed AND unsigned 64-bit compare, select + branch.
//
// Only 32-bit math plus 64-bit add / constant-shift / compare is used so no
// 64-bit shift/div libcall is emitted.  Integer in / integer out, LCG-seeded,
// folded to one integer return.  All four targets, -O1.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress232RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress232RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress232RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress232RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress232RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress232RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress232RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress232RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress232TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Loop-carried unsigned sub-with-borrow.
    {p+"_usubb",
     t+" "+p+"_usubb("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0, borrow=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned d; unsigned b=__builtin_sub_overflow(h, borrow, &d);\n"
     "    unsigned d2; b |= __builtin_sub_overflow(d, (h>>9)&0xffu, &d2);\n"
     "    borrow=b; acc=acc*131u+d2-b+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress232", 1},

    // Signed add-overflow saturate (branch + select reuse of OF).
    {p+"_addov",
     t+" "+p+"_addov("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)h, y=(int)(h*2654435761u); int s;\n"
     "    unsigned v = __builtin_add_overflow(x,y,&s)\n"
     "      ? (unsigned)((x<0)?(-2147483647-1):2147483647) : (unsigned)s;\n"
     "    acc=acc*131u+v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress232", 1},

    // Swap the two 16-bit halves and recombine.
    {p+"_swap16",
     t+" "+p+"_swap16("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned short lo=(unsigned short)h, hi=(unsigned short)(h>>16);\n"
     "    unsigned r=((unsigned)lo<<16)|(unsigned)hi;\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress232", 1},

    // 16-bit multiply/xor discarding the carry out of 16 bits.
    {p+"_mix16",
     t+" "+p+"_mix16("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned short x=(unsigned short)h, y=(unsigned short)(h>>13);\n"
     "    unsigned short s=(unsigned short)((unsigned)x*(unsigned)y + ((unsigned)x^(unsigned)y));\n"
     "    acc=acc*131u+(unsigned)s+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress232", 1},

    // 64-bit select on the sign bit, loop-carried (constant shifts only).
    {p+"_sel64",
     t+" "+p+"_sel64("+t+" a){ unsigned long long h=(unsigned long long)a^0xDEADBEEFCAFEBABEULL;\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL;\n"
     "    unsigned long long m = (h&0x8000000000000000ULL) ? (h>>3) : (h<<3);\n"
     "    acc=acc*131u+(unsigned)(m^(m>>32))+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress232", 1},

    // Signed AND unsigned 64-bit compare; select then branch.
    {p+"_cmp64",
     t+" "+p+"_cmp64("+t+" a){ unsigned long long x=(unsigned long long)a^0x1234567890ABCDEFULL;\n"
     "  unsigned long long y=x*2654435761u+1u; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ x=x*6364136223846793005ULL+1u; y=y*1103515245u+12345u;\n"
     "    unsigned v = (x<y) ? (unsigned)(x>>20) : (unsigned)(y>>12);\n"
     "    if((long long)x < (long long)y) v+=7u; else v+=3u;\n"
     "    acc=acc*131u+v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress232", 1},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress232TC("x64o232", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress232TC("x86o232", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress232TC("a64o232", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress232TC("armo232", "int");

INSTANTIATE_TEST_SUITE_P(OptStress232, X64OptStress232RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress232, X86OptStress232RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress232, A64OptStress232RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress232, ARM32OptStress232RT, ::testing::ValuesIn(kARM), rtTCName);
