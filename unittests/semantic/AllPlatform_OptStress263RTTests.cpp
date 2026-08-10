//===- AllPlatform_OptStress263RTTests.cpp - long long on 32-bit at -O0 ==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// 64-bit (long long) arithmetic at -O0 — on i386/ARM32 every value is a genuine
// register pair and -O0 emits the add/adc, sub/sbb, and shld/shrd-style word
// traffic explicitly (no folding), which is exactly where register-pair value
// modeling and multi-word carry can break.  On x64/AArch64 these are native
// 64-bit ops, so the probe also guards the 64-bit path at -O0.
//
//   * lladd   - 64-bit add/sub carry/borrow chain.
//   * lllogic - 64-bit and/or/xor + shift-by-const across the word boundary.
//   * llcmp   - signed/unsigned 64-bit compare driving a select.
//   * llwiden - 32x32->64 widening multiply accumulate.
//   * llshift - 64-bit shift-by-constant spanning the 32-bit word boundary.
//   * llmix   - combined add + shift + compare into one accumulator.
//
// Integer in / integer out, LCG-seeded, the 64-bit accumulator folded to one
// integer return.  Only +,-,&,|,^,shift-by-const,compare and 32x32->64 mul, so
// i386/ARM32 stay libcall-free (no 64-bit divide / variable 64-bit shift).  -O0.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress263RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress263RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress263RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress263RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress263RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress263RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress263RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress263RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress263TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 64-bit add/sub carry/borrow chain.
    {p+"_lladd",
     t+" "+p+"_lladd("+t+" a){ unsigned h=(unsigned)a;\n"
     "  unsigned long long acc=0x0123456789abcdefULL;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned long long v=(unsigned long long)h | ((unsigned long long)(h^0x9e3779b9u)<<32);\n"
     "    acc += v; acc -= (unsigned long long)(h>>3); }\n"
     "  return ("+t+")((unsigned)acc ^ (unsigned)(acc>>32)); }\n",
     {0x12345u}, "OptStress263", 0},

    // 64-bit and/or/xor + shift-by-const across the word boundary.
    {p+"_lllogic",
     t+" "+p+"_lllogic("+t+" a){ unsigned h=(unsigned)a;\n"
     "  unsigned long long acc=0xfedcba9876543210ULL;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned long long v=(unsigned long long)h | ((unsigned long long)(h*2654435761u)<<32);\n"
     "    acc ^= v; acc &= ~((unsigned long long)(h>>11)); acc |= ((unsigned long long)(h&0xffu)<<56); }\n"
     "  return ("+t+")((unsigned)acc + (unsigned)(acc>>32)); }\n",
     {0x23456u}, "OptStress263", 0},

    // signed/unsigned 64-bit compare driving a select.
    {p+"_llcmp",
     t+" "+p+"_llcmp("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned long long u=(unsigned long long)h | ((unsigned long long)(h>>9)<<32);\n"
     "    unsigned long long w=((unsigned long long)(h^0x55aa55aau)<<32) | (h*3u);\n"
     "    long long su=(long long)u, sw=(long long)w;\n"
     "    acc=acc*131u + (u<w?1u:0u) + (su<sw?2u:0u) + (unsigned)(u>w?u:w); }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress263", 0},

    // 32x32->64 widening multiply accumulate.
    {p+"_llwiden",
     t+" "+p+"_llwiden("+t+" a){ unsigned h=(unsigned)a;\n"
     "  unsigned long long acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h, y=h^0xdeadbeefu;\n"
     "    acc += (unsigned long long)x * (unsigned long long)y;\n"
     "    long long sx=(int)h, sy=(int)(h>>1); acc += (unsigned long long)(sx*sy); }\n"
     "  return ("+t+")((unsigned)acc ^ (unsigned)(acc>>32)); }\n",
     {0x45678u}, "OptStress263", 0},

    // 64-bit shift-by-constant spanning the 32-bit word boundary.
    {p+"_llshift",
     t+" "+p+"_llshift("+t+" a){ unsigned h=(unsigned)a;\n"
     "  unsigned long long acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned long long v=(unsigned long long)h;\n"
     "    acc += (v<<40) ^ (v<<13) ^ (((unsigned long long)h<<32)>>20) ^ (acc>>7); }\n"
     "  return ("+t+")((unsigned)acc ^ (unsigned)(acc>>32)); }\n",
     {0x56789u}, "OptStress263", 0},

    // combined add + shift + compare into one accumulator.
    {p+"_llmix",
     t+" "+p+"_llmix("+t+" a){ unsigned h=(unsigned)a;\n"
     "  unsigned long long acc=1;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    acc += (unsigned long long)h; acc ^= acc>>29; acc += ((unsigned long long)(h&0xffffu)<<40);\n"
     "    if((unsigned)(acc>>32) > h) acc -= (unsigned long long)(h>>3); }\n"
     "  return ("+t+")((unsigned)acc + (unsigned)(acc>>32)); }\n",
     {0x6789Au}, "OptStress263", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress263TC("x64o263", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress263TC("x86o263", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress263TC("a64o263", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress263TC("armo263", "int");

INSTANTIATE_TEST_SUITE_P(OptStress263, X64OptStress263RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress263, X86OptStress263RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress263, A64OptStress263RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress263, ARM32OptStress263RT, ::testing::ValuesIn(kARM), rtTCName);
