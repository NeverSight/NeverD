//===- AllPlatform_OptStress178RTTests.cpp - isqrt / Gray code / bit-reverse =//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain base+index copies and folds a result that depends only on the
// bytes + control flow (never an absolute VA), so nothing touches the deferred
// i386/ARM32 PIC rodata *interior*-pointer model (#477/#487); every probe runs
// on all four targets.
//
//   * isqrt     - integer square root by the restoring bit-by-bit method (only
//                 shifts/adds/compares, no division).  Pins a digit-recurrence
//                 sqrt (distinct from the divide-based Newton iterations and the
//                 modpow #116).
//   * graycode  - reflected Gray-code encode (v^(v>>1)) and the shift-cascade
//                 decode back to binary, checked for round-trip identity.  Pins
//                 the prefix-xor bit cascade (distinct from popcount/parity folds
//                 and the rev-bitfield shapes).
//   * bitrev    - 4-bit bit-reversal permutation: scatter each value to its
//                 bit-reversed index (the FFT reorder).  Pins a reversal-indexed
//                 scatter (distinct from the gather permutations and Heap's
//                 enumeration #171).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress178RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress178RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress178RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress178RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress178RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress178RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress178RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress178RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress178TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // integer sqrt by the restoring bit-by-bit method (no division).
    {p+"_isqrt",
     "static const unsigned char "+p+"_iq[8]={37,12,58,4,29,61,7,44};\n"
     +t+" "+p+"_isqrt("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned n=(((unsigned)"+p+"_iq[s&7u]<<8)^(s&0xFFFFu));\n"
     "    unsigned res=0u, bit=1u<<30;\n"
     "    while(bit>n) bit>>=2;\n"
     "    while(bit){ if(n>=res+bit){ n-=res+bit; res=(res>>1)+bit; } else res>>=1; bit>>=2; }\n"
     "    acc=acc*131u+res; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x38u}, "OptStress178", 2},

    // Gray-code encode v^(v>>1) and the shift-cascade decode (round-trip check).
    {p+"_graycode",
     "static const unsigned char "+p+"_gc[16]={37,12,58,4,29,61,7,44,18,53,2,40,25,9,49,31};\n"
     +t+" "+p+"_graycode("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned hh=0u;\n"
     "    for(int i=0;i<16;i++){ unsigned v=((unsigned)"+p+"_gc[i]^((s>>(i&7))&255u));\n"
     "      unsigned g=v^(v>>1);\n"
     "      unsigned b=g; b^=b>>1; b^=b>>2; b^=b>>4; b^=b>>8; b^=b>>16;\n"
     "      hh=hh*131u+g+(b==v?7u:0u); }\n"
     "    acc=acc*131u+hh; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x49u}, "OptStress178", 2},

    // 4-bit bit-reversal permutation scatter (FFT reorder).
    {p+"_bitrev",
     "static const unsigned char "+p+"_br[16]={9,2,14,5,11,0,7,13,3,15,6,1,10,8,4,12};\n"
     +t+" "+p+"_bitrev("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned dst[16];\n"
     "    for(unsigned i=0;i<16u;i++){ unsigned r=0u, x=i; for(int b=0;b<4;b++){ r=(r<<1)|(x&1u); x>>=1; } dst[r]=((unsigned)"+p+"_br[i]^((s>>(i&7))&15u))&15u; }\n"
     "    unsigned hh=0u; for(int i=0;i<16;i++) hh=hh*131u+dst[i]; acc=acc*131u+hh; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x7Au}, "OptStress178", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress178TC("x64o178", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress178TC("x86o178", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress178TC("a64o178", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress178TC("armo178", "int");

INSTANTIATE_TEST_SUITE_P(OptStress178, X64OptStress178RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress178, X86OptStress178RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress178, A64OptStress178RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress178, ARM32OptStress178RT, ::testing::ValuesIn(kARM), rtTCName);
