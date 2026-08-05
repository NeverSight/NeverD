//===- AllPlatform_OptStress257RTTests.cpp - __int128 inline arith =======//
//
// 128-bit integer arithmetic that clang keeps inline on 64-bit targets — add /
// sub / and / or / xor / shift-by-constant / compare lower to add+adc, sub+sbb,
// register-pair logic, and shld/shrd-style funnels with no libcall.  This
// exercises NeverD's i128 / register-pair (RDX:RAX, X0:X1) value modeling and
// multi-word carry the way the 64-bit-on-32-bit probes do for the 32-bit
// targets, on a width the existing probes never drive directly.
//
//   * add128   - 128-bit accumulator add, folded to the low 64 bits.
//   * sub128   - 128-bit subtract / borrow chain.
//   * logic128 - and / or / xor over 128-bit register pairs.
//   * shift128 - shift-by-constant (<<n, >>n) across the 64-bit word boundary.
//   * cmp128   - unsigned / signed 128-bit compare driving a select.
//   * mix128   - combined add + shift + compare into one accumulator.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  Only
// x86-64 / AArch64 (i386 / ARM32 turn __int128 multiply/divide AND many wide
// ops into libcalls the bare-metal harness cannot run); no __int128 multiply or
// divide here, so the 64-bit targets stay libcall-free.  -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress257RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress257RT, Verify) { roundTripX64(GetParam()); }
class A64OptStress257RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress257RT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress257TC(const char *prefix) {
  std::string p = prefix;
  return {
    // 128-bit accumulator add, folded to the low 64 bits.
    {p+"_add128",
     "long "+p+"_add128(long a){ unsigned long h=(unsigned long)a;\n"
     "  unsigned __int128 acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*6364136223846793005ull+1442695040888963407ull;\n"
     "    acc += (unsigned __int128)h + ((unsigned __int128)(h^0x9e3779b9u)<<64); }\n"
     "  return (long)((unsigned long)acc ^ (unsigned long)(acc>>64)); }\n",
     {0x12345u}, "OptStress257", 2},

    // 128-bit subtract / borrow chain.
    {p+"_sub128",
     "long "+p+"_sub128(long a){ unsigned long h=(unsigned long)a;\n"
     "  unsigned __int128 acc=((unsigned __int128)0x123456789abcdef0ull<<64)|0xfedcba9876543210ull;\n"
     "  for(int i=0;i<160;i++){ h=h*6364136223846793005ull+1442695040888963407ull;\n"
     "    acc -= (unsigned __int128)h + ((unsigned __int128)(h>>7)<<64); }\n"
     "  return (long)((unsigned long)acc + (unsigned long)(acc>>64)); }\n",
     {0x23456u}, "OptStress257", 2},

    // and / or / xor over 128-bit register pairs.
    {p+"_logic128",
     "long "+p+"_logic128(long a){ unsigned long h=(unsigned long)a;\n"
     "  unsigned __int128 acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*6364136223846793005ull+1442695040888963407ull;\n"
     "    unsigned __int128 v=(unsigned __int128)h | ((unsigned __int128)(h*2654435761u)<<64);\n"
     "    acc ^= v; acc &= ~((unsigned __int128)(h>>11)); acc |= (unsigned __int128)(h&0xffu)<<96; }\n"
     "  return (long)((unsigned long)acc ^ (unsigned long)(acc>>64)); }\n",
     {0x34567u}, "OptStress257", 2},

    // shift-by-constant across the 64-bit word boundary.
    {p+"_shift128",
     "long "+p+"_shift128(long a){ unsigned long h=(unsigned long)a;\n"
     "  unsigned __int128 acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*6364136223846793005ull+1442695040888963407ull;\n"
     "    unsigned __int128 v=(unsigned __int128)h;\n"
     "    acc += (v<<40) ^ (v<<72) ^ (((unsigned __int128)h<<64)>>24); }\n"
     "  return (long)((unsigned long)acc ^ (unsigned long)(acc>>64)); }\n",
     {0x45678u}, "OptStress257", 2},

    // unsigned / signed 128-bit compare driving a select.
    {p+"_cmp128",
     "long "+p+"_cmp128(long a){ unsigned long h=(unsigned long)a; unsigned long acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*6364136223846793005ull+1442695040888963407ull;\n"
     "    unsigned __int128 u=(unsigned __int128)h | ((unsigned __int128)(h>>9)<<64);\n"
     "    unsigned __int128 w=((unsigned __int128)(h^0x55aa55aau)<<64) | (h*3u);\n"
     "    __int128 su=(__int128)u, sw=(__int128)w;\n"
     "    acc=acc*131u + (u<w?1u:0u) + (su<sw?2u:0u) + (unsigned long)(u>w?u:w); }\n"
     "  return (long)acc; }\n",
     {0x56789u}, "OptStress257", 2},

    // combined add + shift + compare into one accumulator.
    {p+"_mix128",
     "long "+p+"_mix128(long a){ unsigned long h=(unsigned long)a;\n"
     "  unsigned __int128 acc=1;\n"
     "  for(int i=0;i<160;i++){ h=h*6364136223846793005ull+1442695040888963407ull;\n"
     "    acc += (unsigned __int128)h; acc ^= acc>>61; acc += (unsigned __int128)(h&0xffffu)<<80;\n"
     "    if((unsigned long)(acc>>64) > h) acc -= (unsigned __int128)(h>>3); }\n"
     "  return (long)((unsigned long)acc + (unsigned long)(acc>>64)); }\n",
     {0x6789Au}, "OptStress257", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress257TC("x64o257");
static const std::vector<RoundTripTC> kA64 = makeOptStress257TC("a64o257");

INSTANTIATE_TEST_SUITE_P(OptStress257, X64OptStress257RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress257, A64OptStress257RT, ::testing::ValuesIn(kA64), rtTCName);
