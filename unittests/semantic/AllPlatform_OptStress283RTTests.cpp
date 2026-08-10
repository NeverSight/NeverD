//===- AllPlatform_OptStress283RTTests.cpp - bit-builtin opt probe ========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O2 kernels stressing the bit-manipulation builtins clang lowers to single
// instructions (BSF/BSR/POPCNT/BSWAP on x86, CLZ/RBIT/REV on AArch64/ARM32)
// and the lift handlers / per-element semantics that have repeatedly produced
// wrong-width or wrong-value results.  Inputs are forced nonzero so clz/ctz
// stay defined; every kernel folds to one integer return, libcall-free.
//
//   * clzctz   - leading/trailing-zero counts on sub-word and full-word inputs.
//   * popmix   - population count mixed with shift/xor folding.
//   * bswaprev - byte reverse + bit reverse recombination.
//   * ffsband  - find-first-set + isolate-lowest-bit (x & -x) chain.
//   * bextract - bitfield extract/insert via shift+mask across widths.
//   * paritcnt - parity (popcount&1) accumulation with conditional toggles.
//
// All four targets, -O2.  32-bit math keeps i386/ARM32 libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress283RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress283RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress283RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress283RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress283RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress283RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress283RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress283RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress283TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // leading / trailing zero counts on sub-word and full-word inputs.
    {p+"_clzctz",
     t+" "+p+"_clzctz("+t+" a){ unsigned h=(unsigned)a|1u; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned v=h|1u; unsigned w=(h>>8)|0x100u;\n"
     "    acc+=(unsigned)__builtin_clz(v); acc+=(unsigned)__builtin_ctz(v);\n"
     "    acc+=(unsigned)__builtin_clz(w)*3u; acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress283", 2},

    // population count mixed with shift/xor folding.
    {p+"_popmix",
     t+" "+p+"_popmix("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned pc=(unsigned)__builtin_popcount(h);\n"
     "    acc+=pc; acc^=(h>>pc); acc=(acc<<2)|(acc>>30); acc+=(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress283", 2},

    // byte reverse + bit reverse recombination.
    {p+"_bswaprev",
     t+" "+p+"_bswaprev("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<88;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned b=__builtin_bswap32(h);\n"
     "    unsigned r=0; for(int k=0;k<32;k++){ r=(r<<1)|((h>>k)&1u); }\n"
     "    acc=acc*131u+(b^r)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress283", 2},

    // find-first-set + isolate-lowest-bit (x & -x) chain.
    {p+"_ffsband",
     t+" "+p+"_ffsband("+t+" a){ unsigned h=(unsigned)a|1u; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned v=h|0x80000000u; unsigned low=v&(0u-v);\n"
     "    acc+=(unsigned)__builtin_ffs((int)v); acc^=low; acc+=(v&(v-1u)); acc+=(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress283", 2},

    // bitfield extract/insert via shift+mask across widths.
    {p+"_bextract",
     t+" "+p+"_bextract("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<92;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned pos=(h>>4)&15u, len=((h>>9)&7u)+1u;\n"
     "    unsigned mask=((1u<<len)-1u)<<pos; unsigned f=(h&mask)>>pos;\n"
     "    unsigned ins=(acc & ~mask) | ((f*3u)<<pos); acc=ins+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress283", 2},

    // parity (popcount&1) accumulation with conditional toggles.
    {p+"_paritcnt",
     t+" "+p+"_paritcnt("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned par=(unsigned)__builtin_parity(h);\n"
     "    unsigned pb=(unsigned)__builtin_parity(h&0xFFFFu);\n"
     "    if(par) acc^=0xA5A5A5A5u; if(pb) acc+=0x1234u; acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress283", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress283TC("x64o283", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress283TC("x86o283", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress283TC("a64o283", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress283TC("armo283", "int");

INSTANTIATE_TEST_SUITE_P(OptStress283, X64OptStress283RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress283, X86OptStress283RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress283, A64OptStress283RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress283, ARM32OptStress283RT, ::testing::ValuesIn(kARM), rtTCName);
