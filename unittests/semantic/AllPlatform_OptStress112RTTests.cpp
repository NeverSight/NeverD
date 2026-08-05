//===- AllPlatform_OptStress112RTTests.cpp - stencil / sqrt / prefix shapes =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * sobel  - 3x3 Sobel gradient over a rodata 8x8 image: a full nine-neighbour
//              stencil (`img[(r-1)*8+c-1]` ... `img[(r+1)*8+c+1]`) reduced to
//              `|gx|+|gy|`.  Pins a 2D 3x3 stencil with row stride +-8 and column
//              +-1 over rodata plus an absolute-value reduce.
//   * isqrt  - bitwise integer square root of rodata-seeded operands (the
//              digit-by-digit `bit` recurrence, no divide).  Pins a variable-trip
//              shift/compare sqrt over rodata-derived inputs.
//   * rngsum - prefix-sum table built from a rodata array, then random range
//              queries answered by the difference `pre[hi]-pre[lo]`.  Pins a
//              scan-build over rodata feeding indexed range-difference reads.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress112RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress112RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress112RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress112RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress112RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress112RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress112RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress112RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress112TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 3x3 Sobel gradient over a rodata 8x8 image (nine-neighbour 2D stencil).
    {p+"_sobel",
     "static const unsigned char "+p+"_img[64]={\n"
     "10,20,35,50,60,75,85,95, 25,40,55,70,80,90,100,110, 30,45,65,85,95,105,115,120,\n"
     "35,55,75,95,110,125,130,140, 40,60,80,100,115,130,140,150, 45,65,85,105,120,135,145,155,\n"
     "50,70,90,110,125,140,150,160, 55,75,95,115,130,145,155,165};\n"
     +t+" "+p+"_sobel("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int r=1;r<7;r++) for(int c=1;c<7;c++){\n"
     "      int p00="+p+"_img[(r-1)*8+(c-1)], p01="+p+"_img[(r-1)*8+c], p02="+p+"_img[(r-1)*8+(c+1)];\n"
     "      int p10="+p+"_img[r*8+(c-1)], p12="+p+"_img[r*8+(c+1)];\n"
     "      int p20="+p+"_img[(r+1)*8+(c-1)], p21="+p+"_img[(r+1)*8+c], p22="+p+"_img[(r+1)*8+(c+1)];\n"
     "      int gx=(p02+2*p12+p22)-(p00+2*p10+p20);\n"
     "      int gy=(p20+2*p21+p22)-(p00+2*p01+p02);\n"
     "      unsigned mag=(unsigned)((gx<0?-gx:gx)+(gy<0?-gy:gy));\n"
     "      mag^=(s>>((r+c)&15))&3u; acc=acc*131u+mag; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x5Bu}, "OptStress112", 2},

    // bitwise integer square root of rodata-seeded operands (no divide).
    {p+"_isqrt",
     "static const unsigned char "+p+"_n[24]={\n"
     "0x12,0x9a,0x47,0xe3,0x05,0xbd,0x72,0x18, 0x8f,0x23,0xd6,0x4a,0x91,0x0c,0xfe,0x57,\n"
     "0x6b,0xa3,0x2e,0xd0,0x14,0x88,0x3d,0xc9};\n"
     +t+" "+p+"_isqrt("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i<24;i++){\n"
     "      unsigned n=((unsigned)"+p+"_n[i]<<8)|((s>>(i&7))&0xFFu);\n"
     "      unsigned res=0, bit=1u<<14;\n"
     "      while(bit>n) bit>>=2;\n"
     "      while(bit){ if(n>=res+bit){ n-=res+bit; res=(res>>1)+bit; } else res>>=1; bit>>=2; }\n"
     "      acc=acc*131u+res; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x71u}, "OptStress112", 2},

    // prefix-sum table from a rodata array + range-difference queries.
    {p+"_rngsum",
     "static const unsigned char "+p+"_arr[40]={\n"
     "7,3,11,2,9,5,14,1, 6,12,4,10,8,15,0,13, 5,9,2,7,11,3,14,6, 1,12,8,4,10,15,0,13,\n"
     "9,2,7,11,3,14,6,5};\n"
     +t+" "+p+"_rngsum("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned pre[41]; pre[0]=0;\n"
     "    for(int i=0;i<40;i++) pre[i+1]=pre[i]+("+p+"_arr[i]^((s>>(i&7))&3u));\n"
     "    for(int q=0;q<32;q++){ unsigned lo=((s>>(q&15))&0x7FFFu)%40u, hi=((s>>((q+3)&15))&0x7FFFu)%41u;\n"
     "      if(hi<lo){ unsigned t2=lo; lo=hi; hi=t2; }\n"
     "      acc=acc*131u+(pre[hi]-pre[lo]); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x52u}, "OptStress112", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress112TC("x64o112", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress112TC("x86o112", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress112TC("a64o112", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress112TC("armo112", "int");

INSTANTIATE_TEST_SUITE_P(OptStress112, X64OptStress112RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress112, X86OptStress112RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress112, A64OptStress112RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress112, ARM32OptStress112RT, ::testing::ValuesIn(kARM), rtTCName);
