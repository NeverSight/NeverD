//===- AllPlatform_OptStress105RTTests.cpp - automaton / 2D rodata shapes --==//
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
//   * kmp    - Knuth-Morris-Pratt match: a failure function built in a stack
//              array from a rodata pattern, then a backtracking scan of a rodata
//              text (`q=fail[q-1]`).  Pins an index-backtracking automaton whose
//              transitions read a runtime table + a rodata compare.
//   * bilin  - 2D bilinear interpolation: the four-neighbour quad `img[r*8+c]`,
//              `[r*8+c+1]`, `[(r+1)*8+c]`, `[(r+1)*8+c+1]` of a rodata 8x8 image
//              blended with two integer fractions.  Pins `base+row*stride+col`
//              adjacency in BOTH dimensions plus an all-integer blend.
//   * rgb2y  - fixed-point luma: consecutive RGB byte triples of a rodata buffer
//              combined `(77*R+150*G+29*B)>>8`.  Pins an adjacent-triple forward
//              walk with an asymmetric weighted reduce.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress105RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress105RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress105RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress105RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress105RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress105RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress105RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress105RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress105TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // KMP match: failure function in a stack array, backtracking rodata scan.
    {p+"_kmp",
     "static const unsigned char "+p+"_pat[8]={2,5,2,5,2,1,2,5};\n"
     "static const unsigned char "+p+"_txt[80]={\n"
     "2,5,2,5,2,1,2,5, 3,2,5,2,5,2,1,2, 5,0,4,2,5,2,5,2,\n"
     "1,2,5,6,1,2,5,2, 5,2,1,2,5,7,3,2, 5,2,5,2,1,2,5,0,\n"
     "2,5,2,5,2,1,2,5, 4,6,2,5,2,5,2,1, 2,5,1,3,2,5,2,5, 2,1,2,5,0,6,4,7};\n"
     +t+" "+p+"_kmp("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<64;it++){ s=s*1103515245u+12345u;\n"
     "    int fail[8]; fail[0]=0; int kk=0;\n"
     "    for(int i=1;i<8;i++){ while(kk>0 && ("+p+"_pat[i]&7u)!=("+p+"_pat[kk]&7u)) kk=fail[kk-1];\n"
     "      if(("+p+"_pat[i]&7u)==("+p+"_pat[kk]&7u)) kk++; fail[i]=kk; }\n"
     "    int q=0; unsigned acc=s, hits=0;\n"
     "    for(int i=0;i<80;i++){ unsigned c=("+p+"_txt[i]^(s>>(i&7)))&7u;\n"
     "      while(q>0 && c!=(unsigned)("+p+"_pat[q]&7u)) q=fail[q-1];\n"
     "      if(c==(unsigned)("+p+"_pat[q]&7u)) q++;\n"
     "      if(q==8){ hits++; q=fail[q-1]; acc=acc*131u+(unsigned)i; } }\n"
     "    out=out*1311u+acc+hits; }\n"
     "  return ("+t+")out; }\n",
     {0x4Bu}, "OptStress105", 2},

    // 2D bilinear interpolation over a rodata 8x8 image (four-neighbour quad).
    {p+"_bilin",
     "static const unsigned char "+p+"_img[64]={\n"
     "10,20,35,50,60,75,85,95, 25,40,55,70,80,90,100,110,\n"
     "30,45,65,85,95,105,115,120, 35,55,75,95,110,125,130,140,\n"
     "40,60,80,100,115,130,140,150, 45,65,85,105,120,135,145,155,\n"
     "50,70,90,110,125,140,150,160, 55,75,95,115,130,145,155,165};\n"
     +t+" "+p+"_bilin("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=0;\n"
     "    for(int k=0;k<48;k++){ s=s*1103515245u+12345u;\n"
     "      unsigned r=(s>>4)&6u, c=(s>>12)&6u;\n"
     "      unsigned fx=s&0xFFu, fy=(s>>8)&0xFFu;\n"
     "      unsigned p00="+p+"_img[r*8u+c], p01="+p+"_img[r*8u+c+1u];\n"
     "      unsigned p10="+p+"_img[(r+1u)*8u+c], p11="+p+"_img[(r+1u)*8u+c+1u];\n"
     "      unsigned top=(p00*(256u-fx)+p01*fx)>>8;\n"
     "      unsigned bot=(p10*(256u-fx)+p11*fx)>>8;\n"
     "      unsigned v=(top*(256u-fy)+bot*fy)>>8;\n"
     "      acc=acc*131u+v; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xB1u}, "OptStress105", 2},

    // fixed-point luma over consecutive rodata RGB byte triples.
    {p+"_rgb2y",
     "static const unsigned char "+p+"_px[48]={\n"
     "0x10,0x80,0x30,0xC0,0x20,0x90, 0x40,0x55,0xF0,0x07,0x77,0xAA, 0x33,0x66,0x99,0xCC,\n"
     "0xEE,0x11,0x22,0x44,0x88,0xBB,0xDD,0xFF, 0x05,0x50,0xA5,0x5A,0x0F,0xF0,0x3C,0xC3,\n"
     "0x6E,0xE6,0x1B,0xB1,0x4D,0xD4,0x2F,0xF2, 0x09,0x90,0x7E,0xE7,0x58,0x85,0x36,0x63};\n"
     +t+" "+p+"_rgb2y("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i+2<48;i+=3){\n"
     "      unsigned R="+p+"_px[i]^(s&0xFFu), G="+p+"_px[i+1]^((s>>8)&0xFFu), B="+p+"_px[i+2]^((s>>16)&0xFFu);\n"
     "      unsigned y=(77u*R+150u*G+29u*B)>>8;\n"
     "      acc=acc*131u+y; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x59u}, "OptStress105", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress105TC("x64o105", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress105TC("x86o105", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress105TC("a64o105", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress105TC("armo105", "int");

INSTANTIATE_TEST_SUITE_P(OptStress105, X64OptStress105RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress105, X86OptStress105RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress105, A64OptStress105RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress105, ARM32OptStress105RT, ::testing::ValuesIn(kARM), rtTCName);
