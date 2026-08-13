//===- AllPlatform_OptStress149RTTests.cpp - bitrev / Morton / Galois LFSR =//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * bitrev - bit-reversal permutation of a rodata-seeded array: each source
//              index has its low bits reversed to pick its destination slot
//              (the FFT reorder).  Pins a per-index bit-reversal scatter
//              (distinct from the reflected-Gray XOR-shift in #141).
//   * morton - Morton / Z-order code over rodata coordinate pairs: the bits of
//              two bytes are interleaved into one word, then de-interleaved back
//              to check the round-trip.  Pins a bit-interleave transform
//              (distinct from the bit-reversal scatter above and any byte swap).
//   * lfsr   - Galois linear-feedback shift register stepped from rodata seed and
//              tap masks: each step shifts and conditionally XORs the feedback
//              polynomial.  Pins a shift-register XOR feedback (distinct from the
//              multiply-add LCG that drives every probe and from any CRC fold).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress149RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress149RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress149RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress149RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress149RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress149RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress149RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress149RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress149TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // bit-reversal permutation of a rodata-seeded array (FFT reorder scatter).
    {p+"_bitrev",
     "static const unsigned char "+p+"_a[16]={0x3a,0x91,0x07,0xe5,0x6c,0xb8,0x4f,0x12,0xa3,0x5e,0xd0,0x29,0x7b,0xc6,0x84,0xfd};\n"
     +t+" "+p+"_bitrev("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[16],rb[16];\n"
     "    for(int i=0;i<16;i++) arr[i]=(unsigned)"+p+"_a[i]^((s>>(i&7))&7u);\n"
     "    for(int i=0;i<16;i++){ unsigned r=0u,x=(unsigned)i; for(int b=0;b<4;b++){ r=(r<<1)|(x&1u); x>>=1; } rb[r]=arr[i]; }\n"
     "    for(int i=0;i<16;i++) acc=acc*131u+rb[i]*(unsigned)(i+1);\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x19u}, "OptStress149", 2},

    // Morton / Z-order interleave of rodata coordinate pairs (+ de-interleave).
    {p+"_morton",
     "static const unsigned char "+p+"_x[16]={0x12,0x9a,0x37,0xc5,0x6e,0xb2,0x4f,0x18,0xa3,0x5d,0xd0,0x29,0x7b,0xc6,0x84,0xfd};\n"
     "static const unsigned char "+p+"_y[16]={0xab,0x60,0xdd,0x06,0x99,0x42,0xbf,0x28,0x4d,0xf2,0x81,0x3c,0xe0,0x57,0x6b,0xa9};\n"
     +t+" "+p+"_morton("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){\n"
     "      unsigned xi=((unsigned)"+p+"_x[q]^((s>>(q&7))&0xFFu))&0xFFu, yi=((unsigned)"+p+"_y[q]^((s>>((q>>1)&7))&0xFFu))&0xFFu;\n"
     "      unsigned mc=0u; for(int b=0;b<8;b++){ mc|=((xi>>b)&1u)<<(2*b); mc|=((yi>>b)&1u)<<(2*b+1); }\n"
     "      unsigned dx=0u,dy=0u; for(int b=0;b<8;b++){ dx|=((mc>>(2*b))&1u)<<b; dy|=((mc>>(2*b+1))&1u)<<b; }\n"
     "      acc=acc*131u+mc+((dx==xi&&dy==yi)?1u:0u); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Fu}, "OptStress149", 2},

    // Galois LFSR stepped from rodata seed + tap masks (XOR feedback).
    {p+"_lfsr",
     "static const unsigned char "+p+"_seed[16]={0x3a,0x91,0x07,0xe5,0x6c,0xb8,0x4f,0x12,0xa3,0x5e,0xd0,0x29,0x7b,0xc6,0x84,0xfd};\n"
     "static const unsigned char "+p+"_taps[16]={0xb4,0x1d,0x8e,0x39,0xc7,0x52,0xe1,0x76,0x9a,0x2f,0xd8,0x63,0xf5,0x40,0xac,0x1b};\n"
     +t+" "+p+"_lfsr("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){\n"
     "      unsigned st=((unsigned)"+p+"_seed[q]|1u)&0xFFu, tp=((unsigned)"+p+"_taps[q]|0x80u)&0xFFu;\n"
     "      for(int k=0;k<32;k++){ unsigned lsb=st&1u; st>>=1; if(lsb) st^=tp; st&=0xFFu; acc=acc*131u+st; } }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x5Cu}, "OptStress149", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress149TC("x64o149", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress149TC("x86o149", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress149TC("a64o149", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress149TC("armo149", "int");

INSTANTIATE_TEST_SUITE_P(OptStress149, X64OptStress149RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress149, X86OptStress149RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress149, A64OptStress149RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress149, ARM32OptStress149RT, ::testing::ValuesIn(kARM), rtTCName);
