//===- AllPlatform_OptStress183RTTests.cpp - Kernighan / base64 / xor-basis =//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
//
//   * kernighan - Hamming weight by Kernighan's clear-lowest-set (v &= v-1)
//                 loop.  Pins the set-bit stripping count (distinct from the
//                 Gray-code cascade #178 and any table popcount).
//   * base64    - 3-byte to 4-sextet regrouping (the base64 packing arithmetic)
//                 folded over groups.  Pins a cross-byte bit regrouping (distinct
//                 from the bit-reversal #178 and the byte histograms).
//   * xorbasis  - GF(2) linear basis (xor span) insertion: reduce each value
//                 against the basis and grow the rank.  Pins a greedy linear-
//                 algebra reduction (distinct from every arithmetic reduction).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress183RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress183RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress183RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress183RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress183RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress183RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress183RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress183RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress183TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Hamming weight by Kernighan's clear-lowest-set loop.
    {p+"_kernighan",
     "static const unsigned char "+p+"_kn[16]={37,12,58,4,29,61,7,44,18,53,2,40,25,9,49,31};\n"
     +t+" "+p+"_kernighan("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned total=0u, fold=0u;\n"
     "    for(int i=0;i<16;i++){ unsigned v=((unsigned)"+p+"_kn[i]^((s>>(i&7))&255u))|(((s>>(i&15))&255u)<<8);\n"
     "      unsigned c=0u; while(v){ v&=v-1u; c++; } total+=c; fold=fold*131u+c; }\n"
     "    acc=acc*131u+fold+total*7u; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x3Du}, "OptStress183", 2},

    // 3-byte to 4-sextet regrouping (base64 packing arithmetic).
    {p+"_base64",
     "static const unsigned char "+p+"_b6[24]={71,12,200,4,159,61,7,244,18,153,2,40,225,9,49,131,88,17,99,250,33,140,6,177};\n"
     +t+" "+p+"_base64("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned fold=0u;\n"
     "    for(int g=0;g<8;g++){ unsigned b0=((unsigned)"+p+"_b6[g*3]^((s>>(g&7))&255u)); unsigned b1=((unsigned)"+p+"_b6[g*3+1]^((s>>((g+1)&7))&255u)); unsigned b2=((unsigned)"+p+"_b6[g*3+2]^((s>>((g+2)&7))&255u));\n"
     "      unsigned s0=b0>>2, s1=((b0&3u)<<4)|(b1>>4), s2=((b1&15u)<<2)|(b2>>6), s3=b2&63u;\n"
     "      fold=fold*131u+s0; fold=fold*131u+s1; fold=fold*131u+s2; fold=fold*131u+s3; }\n"
     "    acc=acc*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Eu}, "OptStress183", 2},

    // GF(2) linear basis (xor span) insertion; fold the running rank + basis.
    {p+"_xorbasis",
     "static const unsigned char "+p+"_xb[20]={37,12,58,4,29,61,7,44,18,53,2,40,25,9,49,31,16,52,3,47};\n"
     +t+" "+p+"_xorbasis("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned basis[16]; for(int i=0;i<16;i++) basis[i]=0u;\n"
     "    unsigned rank=0u, fold=0u;\n"
     "    for(int i=0;i<20;i++){ unsigned v=((unsigned)"+p+"_xb[i]^((s>>(i&7))&255u))|(((s>>(i&7))&255u)<<8);\n"
     "      for(int b=15;b>=0;b--){ if(!((v>>b)&1u)) continue; if(!basis[b]){ basis[b]=v; rank++; break; } v^=basis[b]; }\n"
     "      fold=fold*131u+rank; }\n"
     "    for(int b=0;b<16;b++) fold=fold*131u+basis[b]; acc=acc*131u+fold+rank*7u; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x7Fu}, "OptStress183", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress183TC("x64o183", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress183TC("x86o183", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress183TC("a64o183", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress183TC("armo183", "int");

INSTANTIATE_TEST_SUITE_P(OptStress183, X64OptStress183RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress183, X86OptStress183RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress183, A64OptStress183RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress183, ARM32OptStress183RT, ::testing::ValuesIn(kARM), rtTCName);
