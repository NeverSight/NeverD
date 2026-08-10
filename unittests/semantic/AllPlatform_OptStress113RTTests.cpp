//===- AllPlatform_OptStress113RTTests.cpp - fractal / bitrev / GF shapes --==//
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
//   * mandel - fixed-point Mandelbrot escape iteration seeded from rodata
//              (c,ci) pairs: a coupled `z=z*z+c` recurrence with `>>` scaling
//              (no divide) and a data-dependent escape count.  Pins a two-state
//              multiply-accumulate iteration with a value-driven trip count.
//   * bitrev - bit-reversal permutation gather: a rodata array read at the
//              5-bit-reversed loop index (`arr[reverse5(i)]`).  Pins a permuted
//              (non-linear but address-independent) index gather over rodata.
//   * gf256  - GF(2^8) Rijndael multiply of rodata operand pairs by the
//              shift-and-reduce `xtime` loop (reduce poly 0x11B, no table).  Pins
//              a carryless multiply / conditional-xor reduction over rodata.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress113RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress113RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress113RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress113RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress113RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress113RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress113RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress113RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress113TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // fixed-point Mandelbrot escape iteration seeded from rodata (c,ci) pairs.
    {p+"_mandel",
     "static const unsigned char "+p+"_seed[32]={\n"
     "60,128,90,150,30,200,170,70, 110,40,210,95,15,180,140,55, 75,160,25,120,190,50,100,135,\n"
     "20,175,145,65,205,85,35,115};\n"
     +t+" "+p+"_mandel("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q+1<32;q+=2){\n"
     "      int cr=(int)"+p+"_seed[q]-128+(int)((s>>4)&7u);\n"
     "      int ci=(int)"+p+"_seed[q+1]-128+(int)((s>>8)&7u);\n"
     "      int zr=0, zi=0; unsigned iter=0;\n"
     "      for(;iter<48;iter++){ int zr2=(zr*zr)>>6, zi2=(zi*zi)>>6;\n"
     "        if(zr2+zi2>256) break;\n"
     "        int nzr=zr2-zi2+cr; int nzi=((zr*zi)>>5)+ci; zr=nzr; zi=nzi; }\n"
     "      acc=acc*131u+iter; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Du}, "OptStress113", 2},

    // bit-reversal permutation gather of a rodata array (arr[reverse5(i)]).
    {p+"_bitrev",
     "static const unsigned char "+p+"_arr[32]={\n"
     "0x3a,0x91,0x47,0xee,0x12,0x8d,0x5b,0xc6, 0x29,0xf0,0x74,0xa3,0x1e,0x6c,0xd8,0x05,\n"
     "0x9f,0x33,0xb7,0x4a,0xe1,0x58,0x82,0x2d, 0xc9,0x60,0xf5,0x17,0xab,0x3e,0x70,0x9c};\n"
     +t+" "+p+"_bitrev("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i<32;i++){ unsigned r=0, x=(unsigned)i;\n"
     "      for(int b=0;b<5;b++){ r=(r<<1)|(x&1u); x>>=1; }\n"
     "      unsigned v="+p+"_arr[r]^((s>>(i&7))&0xFu);\n"
     "      acc=acc*131u+v; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Bu}, "OptStress113", 2},

    // GF(2^8) Rijndael multiply of rodata operand pairs via xtime reduction.
    {p+"_gf256",
     "static const unsigned char "+p+"_gx[16]={\n"
     "0x57,0x83,0x1f,0xc4,0x6a,0x9e,0x2d,0xb8, 0x05,0xf1,0x4c,0xa7,0x39,0xd0,0x6e,0x92};\n"
     "static const unsigned char "+p+"_gy[16]={\n"
     "0x13,0xca,0x88,0x2f,0xe6,0x40,0xbd,0x71, 0x9c,0x35,0xa0,0x5b,0xf7,0x0e,0x68,0xd3};\n"
     +t+" "+p+"_gf256("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i<16;i++){\n"
     "      unsigned x=("+p+"_gx[i]^(s&0xFFu))&0xFFu, y=("+p+"_gy[i]^((s>>8)&0xFFu))&0xFFu;\n"
     "      unsigned prod=0, aa=x, bb=y;\n"
     "      for(int b=0;b<8;b++){ if(bb&1u) prod^=aa;\n"
     "        unsigned hi=aa&0x80u; aa=(aa<<1)&0xFFu; if(hi) aa^=0x1Bu; bb>>=1; }\n"
     "      acc=acc*131u+(prod&0xFFu); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x9Au}, "OptStress113", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress113TC("x64o113", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress113TC("x86o113", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress113TC("a64o113", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress113TC("armo113", "int");

INSTANTIATE_TEST_SUITE_P(OptStress113, X64OptStress113RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress113, X86OptStress113RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress113, A64OptStress113RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress113, ARM32OptStress113RT, ::testing::ValuesIn(kARM), rtTCName);
