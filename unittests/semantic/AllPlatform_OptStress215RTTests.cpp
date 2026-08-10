//===- AllPlatform_OptStress215RTTests.cpp - multi-arg leading bit-scan ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Hardens the #500 fix (i386 BSR/BSF zero-source preserve must not be mistaken
// for a regparm argument) on MULTI-argument functions: a leading 64-bit clz/ctz
// makes `bsr`/`bsf` the first write to a regparm register (ECX/EDX) while the
// function ALSO takes several genuine cdecl stack arguments.  If a phantom
// leading parameter were re-introduced, every real stack argument would shift
// to the wrong offset, so these multi-arg probes pin the argument layout end to
// end (the single-arg OptStress211 cannot catch an offset that only the second
// or third argument would expose).
//
//   * clz2  - int(int,int): clz of arg0 leads; both args fold into the result.
//   * ctz3  - int(int,int,int): ctz of arg0 leads; all three args consumed.
//   * clzsum- int(int,int,int,int): clz leads; four args summed under control flow.
//   * mixab - int(int,int): clz drives a select between the two arguments.
//
// Integer in / integer out, LCG-seeded, folded to one integer return; no float
// / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress215RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress215RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress215RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress215RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress215RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress215RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress215RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress215RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress215TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // int(int,int): clz of arg0 leads; both arguments must land at correct slots.
    {p+"_clz2",
     t+" "+p+"_clz2("+t+" a, "+t+" b){\n"
     "  unsigned long long h=(unsigned long long)a^0xABCDEF0123456789ULL;\n"
     "  unsigned acc=(unsigned)b, out=0;\n"
     "  for(int i=0;i<8;i++){ unsigned c=(unsigned)__builtin_clzll(h|1ULL);\n"
     "    out=out*131u+c+(unsigned)b*3u; acc+=c+(unsigned)a;\n"
     "    h=h*6364136223846793005ULL+1u+acc; }\n"
     "  return ("+t+")(out+acc); }\n",
     {0xC4u, 0x37u}, "OptStress215", 2},

    // int(int,int,int): ctz of arg0 leads; all three arguments consumed.
    {p+"_ctz3",
     t+" "+p+"_ctz3("+t+" a, "+t+" b, "+t+" c){\n"
     "  unsigned long long h=(unsigned long long)a^0x123456789ABCDEF0ULL;\n"
     "  unsigned acc=(unsigned)b^(unsigned)c, out=0;\n"
     "  for(int i=0;i<8;i++){ unsigned z=(unsigned)__builtin_ctzll(h|0x8000000000000000ULL);\n"
     "    out=out*131u+z+(unsigned)b*5u+(unsigned)c*7u; acc+=z+(unsigned)a;\n"
     "    h=h*6364136223846793005ULL+1442695040888963407ULL+acc; }\n"
     "  return ("+t+")(out^acc); }\n",
     {0x51u, 0x29u, 0x6Du}, "OptStress215", 2},

    // int(int,int,int,int): clz leads; four args summed under control flow.
    {p+"_clzsum",
     t+" "+p+"_clzsum("+t+" a, "+t+" b, "+t+" c, "+t+" d){\n"
     "  unsigned long long h=(unsigned long long)a^0xF0E1D2C3B4A59687ULL;\n"
     "  unsigned out=(unsigned)b+(unsigned)c+(unsigned)d;\n"
     "  for(int i=0;i<8;i++){ unsigned k=(unsigned)__builtin_clzll(h|1ULL);\n"
     "    unsigned add=(k&1u)?(unsigned)b:(unsigned)c; if(k>20u) add+=(unsigned)d;\n"
     "    out=out*131u+k+add+(unsigned)a;\n"
     "    h=h*6364136223846793005ULL+1u+out; }\n"
     "  return ("+t+")out; }\n",
     {0x9Bu, 0x11u, 0x22u, 0x33u}, "OptStress215", 2},

    // int(int,int): clz drives a select between the two arguments.
    {p+"_mixab",
     t+" "+p+"_mixab("+t+" a, "+t+" b){\n"
     "  unsigned long long h=(unsigned long long)a^((unsigned long long)b<<32);\n"
     "  unsigned out=0;\n"
     "  for(int i=0;i<8;i++){ h|=1ULL; unsigned c=(unsigned)__builtin_clzll(h);\n"
     "    unsigned v=(c&1u)?(unsigned)a:(unsigned)b;\n"
     "    out=out*131u+(v^c); h=h*6364136223846793005ULL+1u+out; }\n"
     "  return ("+t+")out; }\n",
     {0x7Eu, 0x42u}, "OptStress215", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress215TC("x64o215", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress215TC("x86o215", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress215TC("a64o215", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress215TC("armo215", "int");

INSTANTIATE_TEST_SUITE_P(OptStress215, X64OptStress215RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress215, X86OptStress215RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress215, A64OptStress215RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress215, ARM32OptStress215RT, ::testing::ValuesIn(kARM), rtTCName);
