//===- AllPlatform_VectorAlgo17RTTests.cpp - DSP/image algos ----*- C++ -*-===//
//
// Seventeenth batch of clang -O2 algorithm probes, targeting DSP / image /
// reduction kernels that auto-vectorize into dense, distinct SIMD across
// x64 / aarch64 / arm32 — the project's most productive bug-finding method.
// Kernels chosen for lowerings under-represented by VectorAlgo1-16:
//   * matmul4   - 4x4 int matrix multiply (MAC chains, transposed loads).
//   * conv3x3   - 3x3 signed convolution (sliding window, signed coeffs >>).
//   * rgb2gray  - fixed-point RGB->luma (packed mul-add + shift).
//   * prefixsum - inclusive scan (loop-carried add, defeats some vectorizers).
//   * minmaxred - signed min/max reduction (SMINV/UMINV / PMINSD / vmin.s).
//   * popcount  - per-element bit population count over an array.
//   * quantize  - Q15 multiply with rounding + saturation (signed clamp).
//   * transpose - 8x8 byte transpose then fold (byte shuffle / scatter).
//   * sadblock  - sum of absolute differences (SABD / PSADBW / vabd).
//   * clampscl  - per-element clamp + scale (branchy min/max + shift).
//
// All arithmetic is bounded 32-bit with constant-divisor shifts and local
// arrays, so nothing lowers to a libcall Unicorn lacks.  Returns fold to an
// exact integer so any lowering divergence (native vs lifted) surfaces.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo17RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo17RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo17RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo17RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo17RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo17RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVA17TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // 4x4 integer matrix multiply (multiply-accumulate chains).
    {p+"_matmul4",
     t+" "+p+"_matmul4("+t+" a){\n"
     "  int A[16],B[16],C[16];\n"
     "  for(int i=0;i<16;i++){ A[i]=(int)((a*(i+1))&0xFF)-128;\n"
     "    B[i]=(int)((a*7+i*3)&0xFF)-128; }\n"
     "  for(int i=0;i<4;i++) for(int j=0;j<4;j++){ int s=0;\n"
     "    for(int k=0;k<4;k++) s+=A[i*4+k]*B[k*4+j]; C[i*4+j]=s; }\n"
     "  unsigned acc=0; for(int i=0;i<16;i++) acc=acc*131u+(unsigned)C[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo17", opt, fl},

    // 3x3 signed convolution over an 8x8 int16 image.
    {p+"_conv3x3",
     t+" "+p+"_conv3x3("+t+" a){\n"
     "  short img[64];\n"
     "  for(int i=0;i<64;i++) img[i]=(short)((a*(i+1))&0x1FF)-256;\n"
     "  const int kern[9]={1,-2,1,-2,5,-2,1,-2,1};\n"
     "  unsigned acc=0;\n"
     "  for(int r=1;r<7;r++) for(int c=1;c<7;c++){ int s=0;\n"
     "    for(int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++)\n"
     "      s+=img[(r+dr)*8+(c+dc)]*kern[(dr+1)*3+(dc+1)];\n"
     "    acc=acc*131u+(unsigned)(s&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo17", opt, fl},

    // Fixed-point RGB -> luma (BT.601-ish), packed byte channels.
    {p+"_rgb2gray",
     t+" "+p+"_rgb2gray("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ unsigned px=(unsigned)(a*(i+1));\n"
     "    unsigned r=px&0xFF,g=(px>>8)&0xFF,b=(px>>16)&0xFF;\n"
     "    unsigned y=(77u*r+150u*g+29u*b)>>8;\n"
     "    acc=acc*131u+y; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo17", opt, fl},

    // Inclusive prefix sum (loop-carried dependency).
    {p+"_prefixsum",
     t+" "+p+"_prefixsum("+t+" a){\n"
     "  int x[64];\n"
     "  for(int i=0;i<64;i++) x[i]=(int)((a*(i+3))&0xFFF)-2048;\n"
     "  for(int i=1;i<64;i++) x[i]+=x[i-1];\n"
     "  unsigned acc=0; for(int i=0;i<64;i++) acc=acc*31u+(unsigned)x[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo17", opt, fl},

    // Signed min/max reduction over an array.
    {p+"_minmaxred",
     t+" "+p+"_minmaxred("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<40;k++){ int x[32];\n"
     "    for(int i=0;i<32;i++) x[i]=(int)((a*(i+1)+k*7)&0xFFFF)-32768;\n"
     "    int mn=x[0],mx=x[0];\n"
     "    for(int i=1;i<32;i++){ if(x[i]<mn)mn=x[i]; if(x[i]>mx)mx=x[i]; }\n"
     "    acc=acc*131u+(unsigned)(mx-mn); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo17", opt, fl},

    // Per-element population count summed over an array.
    {p+"_popcount",
     t+" "+p+"_popcount("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<48;k++){ unsigned x=(unsigned)(a*2654435761u+k*40503u);\n"
     "    int c=0; for(int i=0;i<32;i++) c+=(int)((x>>i)&1u);\n"
     "    acc=acc*131u+(unsigned)c; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo17", opt, fl},

    // Q15 fixed-point multiply with rounding and saturation.
    {p+"_quantize",
     t+" "+p+"_quantize("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ int x=(int)((a*(i+1))&0xFFFF)-32768;\n"
     "    int q=(int)((a*3+i)&0x7FFF);\n"
     "    int prod=(x*q+16384)>>15;\n"
     "    if(prod>32767)prod=32767; if(prod<-32768)prod=-32768;\n"
     "    acc=acc*131u+(unsigned)(prod&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo17", opt, fl},

    // 8x8 byte transpose then fold.
    {p+"_transpose",
     t+" "+p+"_transpose("+t+" a){\n"
     "  unsigned char m[64],tr[64];\n"
     "  for(int i=0;i<64;i++) m[i]=(unsigned char)(a*(i+1)+i);\n"
     "  for(int r=0;r<8;r++) for(int c=0;c<8;c++) tr[c*8+r]=m[r*8+c];\n"
     "  unsigned acc=0; for(int i=0;i<64;i++) acc=acc*131u+tr[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo17", opt, fl},

    // Sum of absolute differences (SABD/PSADBW/vabd).
    {p+"_sadblock",
     t+" "+p+"_sadblock("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<40;k++){ unsigned char p8[32],q8[32];\n"
     "    for(int i=0;i<32;i++){ p8[i]=(unsigned char)(a*(i+1)+k);\n"
     "      q8[i]=(unsigned char)(a*3+i*5+k*7); }\n"
     "    unsigned sad=0;\n"
     "    for(int i=0;i<32;i++){ int d=(int)p8[i]-(int)q8[i]; sad+=(d<0)?-d:d; }\n"
     "    acc=acc*131u+sad; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo17", opt, fl},

    // Per-element clamp + scale (branchy min/max + shift).
    {p+"_clampscl",
     t+" "+p+"_clampscl("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ int x=(int)((a*(i+1))&0x3FFFF)-131072;\n"
     "    if(x<-1000)x=-1000; else if(x>1000)x=1000;\n"
     "    x=(x*100)>>7;\n"
     "    acc=acc*131u+(unsigned)(x&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0xAABBCCDULL}, "VectorAlgo17", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeVA17TC("x64v17", "long", 2, "");
static const std::vector<RoundTripTC> kA64 = makeVA17TC("a64v17", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeVA17TC("armv17", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(VectorAlgo17, X64VectorAlgo17RT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo17, A64VectorAlgo17RT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo17, ARM32VectorAlgo17RT,
                         ::testing::ValuesIn(kARM), rtTCName);
