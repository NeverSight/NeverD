//===- AllPlatform_OptStress47RTTests.cpp - switch bit-test lowering -*-C++*-=//
//
// Roundtrip probes for clang's switch-to-bit-test lowering, a dispatch shape
// orthogonal to the jump tables the OptStress33/34/38/40 + SwitchVariety suites
// cover.  For a sparse switch over a small dense range clang emits a range check
// plus a `bt` against a constant bitmask whose carry flag drives the branch
// (`cmp $N; ja default; mov $mask; bt %idx; jae default` on x86; `lsr`+`tst` /
// `tbz` on ARM), instead of an indirect jump through a table.  This exercises
// the BT/BTL/BTQ register form with a runtime bit index feeding a conditional
// branch — distinct from the OptStress41 builtin bit-tests that feed a select.
// Each dispatch index is data-dependent so the optimizer keeps the dispatch.
//
//   * primes  - sparse switch over [0,63] (64-bit btq bitmask) returning 0/1.
//   * vowels  - small-range switch over a byte (32-bit btl bitmask).
//   * twoclus - two separated case clusters -> two bit-test branches.
//   * rangejt - contiguous-range switch (sub + jump table) for contrast.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress47RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress47RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress47RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress47RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress47RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress47RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress47RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress47RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress47TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // primes: sparse switch over [0,63] -> 64-bit bit-test bitmask + branch.
    {p+"_primes",
     t+" "+p+"_primes("+t+" a){\n"
     "  unsigned s=(unsigned)a, acc=0;\n"
     "  for(int i=0;i<300;i++){\n"
     "    unsigned c=(s>>3)&63u; int r;\n"
     "    switch(c){\n"
     "      case 2: case 3: case 5: case 7: case 11: case 13: case 17: case 19:\n"
     "      case 23: case 29: case 31: case 37: case 41: case 43: case 47:\n"
     "      case 53: case 59: case 61: r=2; break;\n"
     "      default: r=0; }\n"
     "    acc=acc*131u+(unsigned)r+c;\n"
     "    s=s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x70u}, "OptStress47", 2},

    // vowels: small-range switch over a byte -> 32-bit bit-test bitmask + branch.
    {p+"_vowels",
     t+" "+p+"_vowels("+t+" a){\n"
     "  unsigned s=(unsigned)a, acc=0;\n"
     "  for(int i=0;i<300;i++){\n"
     "    unsigned c='a'+((s>>2)%26u); int r;\n"
     "    switch(c){\n"
     "      case 'a': case 'e': case 'i': case 'o': case 'u': r=5; break;\n"
     "      case 'y': r=3; break;\n"
     "      default: r=1; }\n"
     "    acc=acc*131u+(unsigned)r+c;\n"
     "    s=s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x71u}, "OptStress47", 2},

    // twoclus: two separated case clusters -> two bit-test compares + branches.
    {p+"_twoclus",
     t+" "+p+"_twoclus("+t+" a){\n"
     "  unsigned s=(unsigned)a, acc=0;\n"
     "  for(int i=0;i<300;i++){\n"
     "    unsigned c=(s>>1)&127u; int r;\n"
     "    switch(c){\n"
     "      case 3: case 6: case 9: case 12: r=10; break;\n"
     "      case 100: case 103: case 106: case 109: case 112: r=20; break;\n"
     "      default: r=2; }\n"
     "    acc=acc*131u+(unsigned)r+c;\n"
     "    s=s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x72u}, "OptStress47", 2},

    // rangejt: contiguous-range switch (sub + jump table) for contrast.
    {p+"_rangejt",
     t+" "+p+"_rangejt("+t+" a){\n"
     "  unsigned s=(unsigned)a, acc=0;\n"
     "  for(int i=0;i<300;i++){\n"
     "    unsigned c=(s>>4)&15u; int r;\n"
     "    switch(c){\n"
     "      case 4: r=1; break; case 5: r=4; break; case 6: r=9; break;\n"
     "      case 7: r=16; break; case 8: r=25; break; case 9: r=36; break;\n"
     "      case 10: r=49; break; case 11: r=64; break;\n"
     "      default: r=0; }\n"
     "    acc=acc*131u+(unsigned)r+c;\n"
     "    s=s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x73u}, "OptStress47", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress47TC("x64o47", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress47TC("x86o47", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress47TC("a64o47", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress47TC("armo47", "int");

INSTANTIATE_TEST_SUITE_P(OptStress47, X64OptStress47RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress47, X86OptStress47RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress47, A64OptStress47RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress47, ARM32OptStress47RT, ::testing::ValuesIn(kARM), rtTCName);
