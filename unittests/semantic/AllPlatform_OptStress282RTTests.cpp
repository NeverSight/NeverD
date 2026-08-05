//===- AllPlatform_OptStress282RTTests.cpp - wide-64-on-32 opt probe ======//
//
// -O2 kernels built entirely from 64-bit `unsigned long long` arithmetic that
// clang lowers libcall-free (add/sub/mul/shift/compare/select).  On x64/a64
// these are native 64-bit ops; on i386/ARM32 they exercise the Wide64 split
// lowering (lo/hi register pairs, carry/borrow propagation, variable 64-bit
// shifts assembled from 32-bit shifts + masks) — a historically fragile path.
//
//   * wmix     - 64-bit multiply/xor/rotate hash mixing.
//   * wcarry   - 64-bit add/sub carry & borrow chain.
//   * wshift   - variable 64-bit logical/arith shifts assembled from 32-bit.
//   * wselect  - 64-bit conditional select on 64-bit comparisons.
//   * wsplit   - high/low 32-bit halves recombined across the 64-bit boundary.
//   * wcmp     - signed/unsigned 64-bit comparison chains folded to flags.
//
// 64-bit in / 64-bit out (x64/a64) or truncated to 32-bit (i386/ARM32).  No
// division anywhere, so every target stays libcall-free.  All four, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress282RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress282RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress282RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress282RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress282RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress282RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress282RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress282RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress282TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 64-bit multiply/xor/rotate hash mixing (native 64 vs split 64).
    {p+"_wmix",
     t+" "+p+"_wmix("+t+" a){ unsigned long long h=(unsigned long long)a^0x9E3779B97F4A7C15ull;\n"
     "  for(int i=0;i<64;i++){ h^=h>>30; h*=0xBF58476D1CE4E5B9ull; h^=h<<13;\n"
     "    h+=0x94D049BB133111EBull+(unsigned long long)i; h=(h<<27)|(h>>37); }\n"
     "  return ("+t+")(h ^ (h>>32)); }\n",
     {0x12345u}, "OptStress282", 2},

    // 64-bit add/sub carry & borrow chain across the 32-bit boundary.
    {p+"_wcarry",
     t+" "+p+"_wcarry("+t+" a){ unsigned long long acc=0; unsigned long long x=(unsigned long long)a|0x100000000ull;\n"
     "  for(int i=0;i<96;i++){ x=x*6364136223846793005ull+1442695040888963407ull;\n"
     "    acc+=x; acc-=(x>>17); acc+=(x<<11); acc^=(acc>>29); }\n"
     "  return ("+t+")(acc + (acc>>32)); }\n",
     {0x23456u}, "OptStress282", 2},

    // variable 64-bit logical/arith shifts assembled from 32-bit shifts.
    {p+"_wshift",
     t+" "+p+"_wshift("+t+" a){ unsigned long long h=(unsigned long long)a+1; long long acc=0;\n"
     "  for(int i=0;i<80;i++){ h=h*6364136223846793005ull+1442695040888963407ull;\n"
     "    unsigned s=(unsigned)((h>>3)&63u); unsigned long long u=h>>s;\n"
     "    long long v=((long long)h)>>(s|1u); acc+=(long long)u ^ v; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress282", 2},

    // 64-bit conditional select driven by 64-bit comparisons.
    {p+"_wselect",
     t+" "+p+"_wselect("+t+" a){ unsigned long long h=(unsigned long long)a; unsigned long long acc=0;\n"
     "  for(int i=0;i<88;i++){ h=h*6364136223846793005ull+1442695040888963407ull;\n"
     "    unsigned long long m=(h>i*7)?(h^0xFULL):(h+0x100ULL);\n"
     "    long long sv=((long long)h<0)? -(long long)(h>>20):(long long)(h>>21);\n"
     "    acc=acc*1099511628211ull + m + (unsigned long long)sv; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress282", 2},

    // high/low 32-bit halves recombined across the 64-bit boundary.
    {p+"_wsplit",
     t+" "+p+"_wsplit("+t+" a){ unsigned long long h=(unsigned long long)a*0x100000001ull; unsigned long long acc=0;\n"
     "  for(int i=0;i<80;i++){ h=h*6364136223846793005ull+1442695040888963407ull;\n"
     "    unsigned lo=(unsigned)h, hi=(unsigned)(h>>32);\n"
     "    unsigned long long r=((unsigned long long)(lo^hi)<<32)|(unsigned long long)(lo+hi);\n"
     "    acc^=r; acc=(acc<<3)|(acc>>61); }\n"
     "  return ("+t+")(acc^(acc>>32)); }\n",
     {0x56789u}, "OptStress282", 2},

    // signed/unsigned 64-bit comparison chains folded to flags.
    {p+"_wcmp",
     t+" "+p+"_wcmp("+t+" a){ unsigned long long h=(unsigned long long)a; unsigned long long acc=0;\n"
     "  for(int i=0;i<88;i++){ h=h*6364136223846793005ull+1442695040888963407ull;\n"
     "    long long s=(long long)h; unsigned long long u=h;\n"
     "    unsigned r=0; if(s<-1000000000LL) r|=1u; if(s>1000000000LL) r|=2u;\n"
     "    if(u<0x4000000000000000ull) r|=4u; if(u>=0xC000000000000000ull) r|=8u;\n"
     "    acc=acc*131u + r + ((unsigned long long)i<<2); }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress282", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress282TC("x64o282", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress282TC("x86o282", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress282TC("a64o282", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress282TC("armo282", "int");

INSTANTIATE_TEST_SUITE_P(OptStress282, X64OptStress282RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress282, X86OptStress282RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress282, A64OptStress282RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress282, ARM32OptStress282RT, ::testing::ValuesIn(kARM), rtTCName);
