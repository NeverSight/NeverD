//===- AllPlatform_VectorAlgo25RTTests.cpp - saturate/MAC kernels -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Twenty-fifth batch of clang -O2 autovectorization probes, biased toward the
// lane patterns that produced the most recent lift bugs: unsigned/signed
// saturating pack and add/sub (packuswb / paddusb / paddsb / uqxtn / uqadd),
// widening 16x16->32 multiply-accumulate (pmaddwd / smlal), signed min/max
// compare-swap networks (pminsd / smin), vectorized popcount, and FP IIR/FIR
// multiply-add chains.  Each kernel derives its data from the input, folds to a
// single integer return, and uses only 32-bit (or explicitly 64-bit-safe)
// arithmetic so no path lowers to a runtime library call.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo25RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo25RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo25RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo25RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo25RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo25RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVA25TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // RGB->gray with >>8 then clamp to u8 (saturating narrow pack path).
    {p+"_rgbgray",
     t+" "+p+"_rgbgray("+t+" a){\n"
     "  unsigned char r[64],g[64],b[64],y[64];\n"
     "  for(int i=0;i<64;i++){ r[i]=(unsigned char)(a+i*3); g[i]=(unsigned char)(a*2+i);\n"
     "    b[i]=(unsigned char)(a*5+i*7); }\n"
     "  for(int i=0;i<64;i++){ unsigned v=(77u*r[i]+150u*g[i]+29u*b[i])>>8;\n"
     "    y[i]=(unsigned char)(v>255u?255u:v); }\n"
     "  unsigned h=0; for(int i=0;i<64;i++) h=h*131u+y[i];\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo25", opt, fl},

    // Unsigned saturating byte add (paddusb / uqadd).
    {p+"_usatadd",
     t+" "+p+"_usatadd("+t+" a){\n"
     "  unsigned char x[80],y[80],o[80];\n"
     "  for(int i=0;i<80;i++){ x[i]=(unsigned char)(a+i*5); y[i]=(unsigned char)(a*3+i*2); }\n"
     "  for(int i=0;i<80;i++){ unsigned s=(unsigned)x[i]+(unsigned)y[i];\n"
     "    o[i]=(unsigned char)(s>255u?255u:s); }\n"
     "  unsigned h=0; for(int i=0;i<80;i++) h=h*131u+o[i];\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo25", opt, fl},

    // Signed saturating byte sub to [-128,127] (paddsb / sqsub).
    {p+"_ssatsub",
     t+" "+p+"_ssatsub("+t+" a){\n"
     "  signed char x[80],y[80],o[80];\n"
     "  for(int i=0;i<80;i++){ x[i]=(signed char)(a+i*7); y[i]=(signed char)(a*2+i*3); }\n"
     "  for(int i=0;i<80;i++){ int s=(int)x[i]-(int)y[i];\n"
     "    s = s>127?127:(s<-128?-128:s); o[i]=(signed char)s; }\n"
     "  int h=0; for(int i=0;i<80;i++) h=h*131+o[i];\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo25", opt, fl},

    // 16x16->32 widening multiply-accumulate (pmaddwd / smlal).
    {p+"_dot16",
     t+" "+p+"_dot16("+t+" a){\n"
     "  short x[64],y[64];\n"
     "  for(int i=0;i<64;i++){ x[i]=(short)(a+i*11); y[i]=(short)(a*3+i*5); }\n"
     "  int acc=0; for(int i=0;i<64;i++) acc += (int)x[i]*(int)y[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo25", opt, fl},

    // Signed min/max compare-swap reduction (pminsd/pmaxsd / smin/smax).
    {p+"_minmax",
     t+" "+p+"_minmax("+t+" a){\n"
     "  int v[96]; for(int i=0;i<96;i++) v[i]=(int)((a*(i+1))^(i*2654435761u));\n"
     "  int mn=v[0],mx=v[0];\n"
     "  for(int i=1;i<96;i++){ if(v[i]<mn) mn=v[i]; if(v[i]>mx) mx=v[i]; }\n"
     "  return ("+t+")((unsigned)mn*131u+(unsigned)mx);\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo25", opt, fl},

    // Bitonic-ish compare-swap network over fixed groups of 8.
    {p+"_sortnet",
     t+" "+p+"_sortnet("+t+" a){\n"
     "  unsigned h=0;\n"
     "  for(int blk=0;blk<12;blk++){\n"
     "    int w[8]; for(int i=0;i<8;i++) w[i]=(int)(a*(blk+1)+i*17-blk*5);\n"
     "    for(int s=0;s<3;s++) for(int i=0;i+1<8;i+=2){\n"
     "      if(w[i]>w[i+1]){ int tmp=w[i]; w[i]=w[i+1]; w[i+1]=tmp; } }\n"
     "    for(int i=0;i<8;i++) h=h*131u+(unsigned)w[i]; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo25", opt, fl},

    // Vectorized popcount over a 32-bit array (SWAR / vpshufb LUT / cnt).
    {p+"_popcnt",
     t+" "+p+"_popcnt("+t+" a){\n"
     "  unsigned v[96]; for(int i=0;i<96;i++) v[i]=(unsigned)(a*(i+3)+i*2654435761u);\n"
     "  unsigned h=0; for(int i=0;i<96;i++) h += (unsigned)__builtin_popcount(v[i]);\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo25", opt, fl},

    // Biquad IIR filter (FP loop-carried multiply-add chain).
    {p+"_biquad",
     t+" "+p+"_biquad("+t+" a){\n"
     "  double b0=0.2,b1=0.3,b2=0.1,a1=-0.4,a2=0.15;\n"
     "  double x1=0,x2=0,y1=0,y2=0,acc=0;\n"
     "  for(int i=0;i<48;i++){ double x=(double)((a+i*13)%97)-48.0;\n"
     "    double y=b0*x+b1*x1+b2*x2-a1*y1-a2*y2;\n"
     "    x2=x1; x1=x; y2=y1; y1=y; acc+=y; }\n"
     "  unsigned lo,hi; __builtin_memcpy(&lo,&acc,4); __builtin_memcpy(&hi,((char*)&acc)+4,4);\n"
     "  return ("+t+")(lo*131u+hi);\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo25", opt, "-fno-math-errno"},

    // FIR convolution (FP multiply-add reduction).
    {p+"_fir",
     t+" "+p+"_fir("+t+" a){\n"
     "  float in[72]; for(int i=0;i<72;i++) in[i]=(float)((a+i*5)%53)-26.0f;\n"
     "  float taps[5]={0.1f,0.2f,0.4f,0.2f,0.1f}; float acc=0;\n"
     "  for(int i=4;i<72;i++){ float s=0; for(int k=0;k<5;k++) s+=taps[k]*in[i-k]; acc+=s; }\n"
     "  unsigned bits; __builtin_memcpy(&bits,&acc,4); return ("+t+")bits;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo25", opt, "-fno-math-errno"},

    // Threshold-and-count: compare to a per-element threshold, accumulate mask.
    {p+"_thresh",
     t+" "+p+"_thresh("+t+" a){\n"
     "  int v[128]; for(int i=0;i<128;i++) v[i]=(int)(a*(i+1)+i*97);\n"
     "  int thr=(int)a; unsigned cnt=0,sum=0;\n"
     "  for(int i=0;i<128;i++){ if(v[i]>thr){ cnt++; sum+=(unsigned)v[i]; } }\n"
     "  return ("+t+")(cnt*131u+sum);\n"
     "}\n",
     {0xAABBCCDULL}, "VectorAlgo25", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeVA25TC("x64v25", "long", 2, "");
static const std::vector<RoundTripTC> kA64 = makeVA25TC("a64v25", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeVA25TC("armv25", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(VectorAlgo25, X64VectorAlgo25RT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo25, A64VectorAlgo25RT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo25, ARM32VectorAlgo25RT,
                         ::testing::ValuesIn(kARM), rtTCName);
