//===- AllPlatform_VectorAlgo20RTTests.cpp - codec/vision algos -*- C++ -*-===//
//
// Twentieth batch of clang -O2 algorithm probes.  Kernels steer the
// auto-vectorizer into instruction mixes still under-represented by
// VectorAlgo1-19 — block SAD, byte-matrix transpose, morphological min/max
// filters, pooling, prefix scan, leading-zero normalization, branchy Paeth
// prediction, chroma subsampling, integral image and bit-plane extraction:
//   * sad4x4    - 4x4 block sum-of-absolute-differences (UABD/PSADBW + reduce).
//   * transp8   - 8x8 byte matrix transpose (ZIP/TRN/UZP / PUNPCK shuffles).
//   * erodil    - 1D erosion+dilation (running window UMIN / UMAX reductions).
//   * maxpool2  - 2x2 max pooling over a byte grid (pairwise max + stride).
//   * prefixsum - inclusive prefix scan (loop-carried partial-sum recurrence).
//   * clznorm   - leading-zero normalize (CLZ/LZCNT + variable-count shift).
//   * paeth     - PNG Paeth predictor (abs/compare/select, heavily branchy).
//   * chroma420 - 4:2:0 chroma subsample (2x2 averaging + pack).
//   * integral  - 2D integral image partial (row+col running sums).
//   * bitplane  - bit-plane extraction (per-bit shift/mask/pack reassembly).
//
// All arithmetic is bounded 32-bit with local arrays and constant-divisor
// shifts, so nothing lowers to a libcall Unicorn lacks.  Returns fold to an
// exact integer so any native-vs-lifted lowering divergence surfaces.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo20RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo20RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo20RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo20RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo20RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo20RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVA20TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Sliding sum-of-absolute-differences (motion-estimation SAD search).
    {p+"_sadsearch",
     t+" "+p+"_sadsearch("+t+" a){\n"
     "  unsigned char A[64],B[64];\n"
     "  for(int i=0;i<64;i++){ A[i]=(unsigned char)(a*(i+1)+i*3);\n"
     "    B[i]=(unsigned char)(a*5+i*7); }\n"
     "  unsigned acc=0;\n"
     "  for(int off=0;off<8;off++){ unsigned s=0;\n"
     "    for(int i=0;i<56;i++){ int d=A[i]-B[i+off]; s+=(unsigned)(d<0?-d:d); }\n"
     "    acc=acc*131u+s; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo20", opt, fl},

    // 8x8 byte matrix transpose then weighted reduce.
    {p+"_transp8",
     t+" "+p+"_transp8("+t+" a){\n"
     "  unsigned char m[64],tr[64];\n"
     "  for(int i=0;i<64;i++) m[i]=(unsigned char)(a*(i+1)+i*5);\n"
     "  for(int r=0;r<8;r++) for(int c=0;c<8;c++) tr[c*8+r]=m[r*8+c];\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<64;i++) acc=acc*131u+(unsigned)(tr[i]+i);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo20", opt, fl},

    // 1D erosion (window min) then dilation (window max).
    {p+"_erodil",
     t+" "+p+"_erodil("+t+" a){\n"
     "  int x[128];\n"
     "  for(int i=0;i<128;i++) x[i]=(int)((a*(i+1)+i*9)&0xFFFF);\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<124;i++){ int mn=x[i],mx=x[i];\n"
     "    for(int k=1;k<5;k++){ if(x[i+k]<mn)mn=x[i+k]; if(x[i+k]>mx)mx=x[i+k]; }\n"
     "    acc=acc*131u+(unsigned)(mx-mn); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo20", opt, fl},

    // 2x2 max pooling over a byte grid.
    {p+"_maxpool2",
     t+" "+p+"_maxpool2("+t+" a){\n"
     "  unsigned char g[256];\n"
     "  for(int i=0;i<256;i++) g[i]=(unsigned char)(a*(i+1)+i*11);\n"
     "  unsigned acc=0;\n"
     "  for(int r=0;r<16;r+=2) for(int c=0;c<16;c+=2){\n"
     "    unsigned m=g[r*16+c];\n"
     "    if(g[r*16+c+1]>m)m=g[r*16+c+1];\n"
     "    if(g[(r+1)*16+c]>m)m=g[(r+1)*16+c];\n"
     "    if(g[(r+1)*16+c+1]>m)m=g[(r+1)*16+c+1];\n"
     "    acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo20", opt, fl},

    // Inclusive prefix scan (loop-carried partial-sum recurrence).
    {p+"_prefixsum",
     t+" "+p+"_prefixsum("+t+" a){\n"
     "  int x[160];\n"
     "  for(int i=0;i<160;i++) x[i]=(int)((a*(i+1))&0x3FF)-512;\n"
     "  unsigned acc=0; int run=0;\n"
     "  for(int i=0;i<160;i++){ run+=x[i]; acc=acc*131u+(unsigned)(run&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo20", opt, fl},

    // Leading-zero normalize: shift each value left until top bit set.
    {p+"_clznorm",
     t+" "+p+"_clznorm("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ unsigned v=(unsigned)(a*(i+1)+i*3)|1u;\n"
     "    int sh=0; while(!(v&0x80000000u)){ v<<=1; sh++; }\n"
     "    acc=acc*131u+(unsigned)(v^(unsigned)sh); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo20", opt, fl},

    // PNG Paeth predictor (abs distance + select; heavily branchy).
    {p+"_paeth",
     t+" "+p+"_paeth("+t+" a){\n"
     "  unsigned char row[130];\n"
     "  for(int i=0;i<130;i++) row[i]=(unsigned char)(a*(i+1)+i*7);\n"
     "  unsigned acc=0;\n"
     "  for(int i=2;i<130;i++){ int A=row[i-1],B=row[i-2],C=row[i-2];\n"
     "    int pp=A+B-C, pa=pp-A,pb=pp-B,pc=pp-C;\n"
     "    pa=pa<0?-pa:pa; pb=pb<0?-pb:pb; pc=pc<0?-pc:pc;\n"
     "    int pr=(pa<=pb&&pa<=pc)?A:(pb<=pc?B:C);\n"
     "    acc=acc*131u+(unsigned)((row[i]-pr)&0xFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo20", opt, fl},

    // 4:2:0 chroma subsample (2x2 average + pack).
    {p+"_chroma420",
     t+" "+p+"_chroma420("+t+" a){\n"
     "  unsigned char c[256];\n"
     "  for(int i=0;i<256;i++) c[i]=(unsigned char)(a*(i+1)+i*13);\n"
     "  unsigned acc=0;\n"
     "  for(int r=0;r<16;r+=2) for(int x=0;x<16;x+=2){\n"
     "    unsigned s=c[r*16+x]+c[r*16+x+1]+c[(r+1)*16+x]+c[(r+1)*16+x+1];\n"
     "    acc=acc*131u+((s+2)>>2); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo20", opt, fl},

    // 2D integral image partial (row then column running sums).
    {p+"_integral",
     t+" "+p+"_integral("+t+" a){\n"
     "  unsigned img[100],ii[100];\n"
     "  for(int i=0;i<100;i++) img[i]=(unsigned)(a*(i+1)+i)&0xFF;\n"
     "  for(int r=0;r<10;r++){ unsigned run=0;\n"
     "    for(int c=0;c<10;c++){ run+=img[r*10+c];\n"
     "      unsigned up=r?ii[(r-1)*10+c]:0; ii[r*10+c]=run+up; } }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<100;i++) acc=acc*131u+ii[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo20", opt, fl},

    // Bit-plane extraction then reassembly (per-bit shift/mask/pack).
    {p+"_bitplane",
     t+" "+p+"_bitplane("+t+" a){\n"
     "  unsigned char px[64]; for(int i=0;i<64;i++) px[i]=(unsigned char)(a*(i+1)+i*3);\n"
     "  unsigned acc=0;\n"
     "  for(int b=0;b<8;b++){ unsigned plane=0;\n"
     "    for(int i=0;i<64;i++) plane=(plane<<1)|((px[i]>>b)&1u);\n"
     "    acc=acc*131u+plane; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0xAABBCCDULL}, "VectorAlgo20", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeVA20TC("x64v20", "long", 2, "");
static const std::vector<RoundTripTC> kA64 = makeVA20TC("a64v20", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeVA20TC("armv20", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(VectorAlgo20, X64VectorAlgo20RT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo20, A64VectorAlgo20RT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo20, ARM32VectorAlgo20RT,
                         ::testing::ValuesIn(kARM), rtTCName);
