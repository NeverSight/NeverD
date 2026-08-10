//===- AllPlatform_VectorAlgo18RTTests.cpp - DSP/filter algos ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Eighteenth batch of clang -O2 algorithm probes.  Kernels chosen for SIMD
// lowerings under-represented by VectorAlgo1-17 — sliding-window MAC filters,
// strided de-interleave loads, widening int8 dot products, loop-carried IIR
// feedback, and sorting-network min/max — each of which steers the
// auto-vectorizer into a distinct instruction mix across x64 / aarch64 / arm32:
//   * fir8       - 8-tap FIR (MAC chains, sliding window, signed coeffs).
//   * yuv2rgb    - fixed-point YUV->RGB (signed mul-add + clamp to byte).
//   * median3x3  - 3x3 median via sorting network (dense min/max + swaps).
//   * dot_i8     - signed int8 widening dot product (SDOT / PMADDWD / SMLAL).
//   * iir1       - first-order IIR (loop-carried feedback, defeats vectorizer).
//   * deinterlv  - RGB channel de-interleave (vld3 / strided gather + recombine).
//   * threshold  - per-element threshold count + masked sum (compare + select).
//   * xcorr      - cross-correlation at several lags (nested MAC, shifted reads).
//   * boxblur    - 5-tap running box blur (window sum + constant divide).
//   * satacc     - running signed saturating accumulate (clamp idiom).
//
// All arithmetic is bounded 32-bit with constant-divisor shifts/divides and
// local arrays, so nothing lowers to a libcall Unicorn lacks.  Returns fold to
// an exact integer so any native-vs-lifted lowering divergence surfaces.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo18RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo18RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo18RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo18RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo18RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo18RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVA18TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // 8-tap FIR filter: sliding-window multiply-accumulate with signed taps.
    {p+"_fir8",
     t+" "+p+"_fir8("+t+" a){\n"
     "  int x[80];\n"
     "  for(int i=0;i<80;i++) x[i]=(int)((a*(i+1))&0x1FF)-256;\n"
     "  const int c[8]={3,-5,11,-9,7,-2,4,1};\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<72;i++){ int s=0;\n"
     "    for(int k=0;k<8;k++) s+=x[i+k]*c[k];\n"
     "    acc=acc*131u+(unsigned)(s&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo18", opt, fl},

    // Fixed-point YUV -> RGB (BT.601), signed mul-add with byte clamp.
    {p+"_yuv2rgb",
     t+" "+p+"_yuv2rgb("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ int Y=(int)((a*(i+1))&0xFF);\n"
     "    int U=(int)((a*3+i)&0xFF)-128, V=(int)((a*5+i*2)&0xFF)-128;\n"
     "    int r=Y+((91881*V)>>16);\n"
     "    int g=Y-((22554*U+46802*V)>>16);\n"
     "    int b=Y+((116130*U)>>16);\n"
     "    if(r<0)r=0; if(r>255)r=255;\n"
     "    if(g<0)g=0; if(g>255)g=255;\n"
     "    if(b<0)b=0; if(b>255)b=255;\n"
     "    acc=acc*131u+(unsigned)((r<<16)|(g<<8)|b); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo18", opt, fl},

    // 3x3 median filter via sorting network (dense min/max + conditional swap).
    {p+"_median3x3",
     t+" "+p+"_median3x3("+t+" a){\n"
     "  unsigned char img[100];\n"
     "  for(int i=0;i<100;i++) img[i]=(unsigned char)(a*(i+1)+i*7);\n"
     "  unsigned acc=0;\n"
     "  for(int r=1;r<9;r++) for(int c=1;c<9;c++){\n"
     "    int w[9],n=0;\n"
     "    for(int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++)\n"
     "      w[n++]=img[(r+dr)*10+(c+dc)];\n"
     "    for(int i=0;i<9;i++) for(int j=i+1;j<9;j++)\n"
     "      if(w[j]<w[i]){ int tmp=w[i]; w[i]=w[j]; w[j]=tmp; }\n"
     "    acc=acc*131u+(unsigned)w[4]; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo18", opt, fl},

    // Signed int8 widening dot product (SDOT / PMADDWD / SMLAL chains).
    {p+"_dot_i8",
     t+" "+p+"_dot_i8("+t+" a){\n"
     "  signed char x[128],y[128];\n"
     "  for(int i=0;i<128;i++){ x[i]=(signed char)(a*(i+1)+i);\n"
     "    y[i]=(signed char)(a*3+i*5); }\n"
     "  int acc=0;\n"
     "  for(int i=0;i<128;i++) acc+=x[i]*y[i];\n"
     "  return ("+t+")(unsigned)acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo18", opt, fl},

    // First-order IIR filter: loop-carried feedback (scalar recurrence).
    {p+"_iir1",
     t+" "+p+"_iir1("+t+" a){\n"
     "  unsigned acc=0; int y=0;\n"
     "  for(int i=0;i<200;i++){ int x=(int)((a*(i+1))&0xFF)-128;\n"
     "    y=(3*y+x*4)>>2; acc=acc*131u+(unsigned)(y&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo18", opt, fl},

    // RGB de-interleave (strided loads -> vld3) then weighted recombine.
    {p+"_deinterlv",
     t+" "+p+"_deinterlv("+t+" a){\n"
     "  unsigned char rgb[96];\n"
     "  for(int i=0;i<96;i++) rgb[i]=(unsigned char)(a*(i+1)+i*3);\n"
     "  unsigned char r[32],g[32],b[32];\n"
     "  for(int i=0;i<32;i++){ r[i]=rgb[i*3]; g[i]=rgb[i*3+1]; b[i]=rgb[i*3+2]; }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<32;i++) acc=acc*131u+(unsigned)(r[i]+2*g[i]+3*b[i]);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo18", opt, fl},

    // Per-element threshold: masked sum + count (compare-select / CMGT + reduce).
    {p+"_threshold",
     t+" "+p+"_threshold("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<40;k++){ int x[64]; int th=(int)((a+k)&0xFF);\n"
     "    for(int i=0;i<64;i++) x[i]=(int)((a*(i+1)+k*3)&0xFF);\n"
     "    int cnt=0,sum=0;\n"
     "    for(int i=0;i<64;i++){ if(x[i]>th){ sum+=x[i]; cnt++; } }\n"
     "    acc=acc*131u+(unsigned)(sum+cnt); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo18", opt, fl},

    // Cross-correlation at several lags (nested MAC over shifted windows).
    {p+"_xcorr",
     t+" "+p+"_xcorr("+t+" a){\n"
     "  int x[64],y[64];\n"
     "  for(int i=0;i<64;i++){ x[i]=(int)((a*(i+1))&0xFF)-128;\n"
     "    y[i]=(int)((a*3+i)&0xFF)-128; }\n"
     "  unsigned acc=0;\n"
     "  for(int lag=0;lag<16;lag++){ int s=0;\n"
     "    for(int i=0;i<48;i++) s+=x[i]*y[i+lag];\n"
     "    acc=acc*131u+(unsigned)(s&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo18", opt, fl},

    // 5-tap running box blur (window sum + constant divide by 5).
    {p+"_boxblur",
     t+" "+p+"_boxblur("+t+" a){\n"
     "  int x[128];\n"
     "  for(int i=0;i<128;i++) x[i]=(int)((a*(i+1))&0x3FF);\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<=123;i++){ int s=0;\n"
     "    for(int k=0;k<5;k++) s+=x[i+k];\n"
     "    acc=acc*131u+(unsigned)(s/5); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo18", opt, fl},

    // Running signed saturating accumulate (clamp to +-1e6).
    {p+"_satacc",
     t+" "+p+"_satacc("+t+" a){\n"
     "  unsigned acc=0; int s=0;\n"
     "  for(int i=0;i<128;i++){ int x=(int)((a*(i+1))&0xFFFF)-32768;\n"
     "    s+=x; if(s>1000000)s=1000000; if(s<-1000000)s=-1000000;\n"
     "    acc=acc*131u+(unsigned)(s&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0xAABBCCDULL}, "VectorAlgo18", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeVA18TC("x64v18", "long", 2, "");
static const std::vector<RoundTripTC> kA64 = makeVA18TC("a64v18", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeVA18TC("armv18", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(VectorAlgo18, X64VectorAlgo18RT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo18, A64VectorAlgo18RT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo18, ARM32VectorAlgo18RT,
                         ::testing::ValuesIn(kARM), rtTCName);
