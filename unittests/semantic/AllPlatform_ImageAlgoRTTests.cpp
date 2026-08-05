//===- AllPlatform_ImageAlgoRTTests.cpp - Image kernels ---------*- C++ -*-===//
//
// clang -O2 image-processing kernel probes (grayscale weighting, threshold,
// box blur, Sobel gradient, brightness with saturation, RGB565 pack/unpack,
// 1D median filter).  These are byte-heavy and auto-vectorize into widening
// multiplies, saturating narrows, byte compares/selects, min/max chains and
// pack/unpack shuffles across all three architectures — broad coverage that
// also exercises the optimizer pipeline at -O2.
//
// All arithmetic is bounded 8/16/32-bit with constant divisors only, so nothing
// lowers to a libcall Unicorn lacks.  Original vs recompiled are compared.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64ImageAlgoRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64ImageAlgoRT, Verify) { roundTripX64(GetParam()); }

class A64ImageAlgoRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64ImageAlgoRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32ImageAlgoRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ImageAlgoRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeImgTC(const char *prefix, const char *T,
                                          int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Luminance: gray = (r*77 + g*150 + b*29) >> 8 over an RGB array.  Widening
    // byte*const multiply + add + narrowing shift.
    {p+"_grayscale",
     t+" "+p+"_grayscale("+t+" a) {\n"
     "  unsigned char img[60]; unsigned acc=0;\n"
     "  for(int i=0;i<60;i++) img[i]=(unsigned char)(a*(i+1)+i*13);\n"
     "  for(int i=0;i<20;i++){\n"
     "    unsigned r=img[i*3],g=img[i*3+1],b=img[i*3+2];\n"
     "    unsigned gray=(r*77u+g*150u+b*29u)>>8;\n"
     "    acc=acc*131+gray; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "ImageAlgo", opt, fl},

    // Threshold to 0/255 then accumulate + count.  Byte compare + select (vbsl /
    // pcmpgtb+blend) and a branchless saturate-to-mask.
    {p+"_threshold",
     t+" "+p+"_threshold("+t+" a) {\n"
     "  unsigned char img[64]; unsigned acc=0; int cnt=0;\n"
     "  for(int i=0;i<64;i++) img[i]=(unsigned char)(a*(i+1)+i*7);\n"
     "  unsigned char th=(unsigned char)(a&0x7F);\n"
     "  for(int i=0;i<64;i++){ unsigned char v=img[i]>th?255:0;\n"
     "    acc=acc*31+v; if(v)cnt++; }\n"
     "  return ("+t+")(acc+(unsigned)cnt);\n"
     "}\n",
     {0x2345678ULL}, "ImageAlgo", opt, fl},

    // 1D box blur (3-tap average with rounding): (a+b+c+1)/3.  Sum + divide by
    // constant (magic multiply).
    {p+"_boxblur",
     t+" "+p+"_boxblur("+t+" a) {\n"
     "  unsigned char img[66]; unsigned acc=0;\n"
     "  for(int i=0;i<66;i++) img[i]=(unsigned char)(a*(i+1)+i*5);\n"
     "  for(int i=1;i<65;i++){\n"
     "    unsigned s=((unsigned)img[i-1]+img[i]+img[i+1]+1u)/3u;\n"
     "    acc=acc*131+(s&0xFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3456789ULL}, "ImageAlgo", opt, fl},

    // Sobel horizontal gradient: clamp(|img[i+1]-img[i-1]|, 0, 255).  Absolute
    // difference + saturation.
    {p+"_sobel",
     t+" "+p+"_sobel("+t+" a) {\n"
     "  unsigned char img[64]; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++) img[i]=(unsigned char)(a*(i+1)+i*i);\n"
     "  for(int i=1;i<63;i++){\n"
     "    int g=(int)img[i+1]-(int)img[i-1]; if(g<0)g=-g; if(g>255)g=255;\n"
     "    acc=acc*131+(unsigned)g; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x456789AULL}, "ImageAlgo", opt, fl},

    // Brightness/contrast with saturation: clamp(in*5/4 + bias, 0, 255).
    {p+"_brightness",
     t+" "+p+"_brightness("+t+" a) {\n"
     "  unsigned char img[64]; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++) img[i]=(unsigned char)(a*(i+1));\n"
     "  int bias=(int)(a&0x3F)-32;\n"
     "  for(int i=0;i<64;i++){ int v=(int)img[i]*5/4+bias;\n"
     "    if(v<0)v=0; if(v>255)v=255;\n"
     "    acc=(acc<<5)+(acc>>27)+(unsigned)v; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x56789ABULL}, "ImageAlgo", opt, fl},

    // RGB565 pack then unpack: bit-field pack/extract round-trip.
    {p+"_rgb565",
     t+" "+p+"_rgb565("+t+" a) {\n"
     "  unsigned char img[60]; unsigned acc=0;\n"
     "  for(int i=0;i<60;i++) img[i]=(unsigned char)(a*(i+1)+i*3);\n"
     "  for(int i=0;i<20;i++){\n"
     "    unsigned r=img[i*3],g=img[i*3+1],b=img[i*3+2];\n"
     "    unsigned short pk=(unsigned short)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));\n"
     "    unsigned r2=((pk>>11)&0x1F)<<3, g2=((pk>>5)&0x3F)<<2, b2=(pk&0x1F)<<3;\n"
     "    acc=acc*131+r2+g2*7+b2*13; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6789ABCULL}, "ImageAlgo", opt, fl},

    // 1D median-of-3 filter: min/max chain (umin/umax / pminub/pmaxub).
    {p+"_median3",
     t+" "+p+"_median3("+t+" a) {\n"
     "  unsigned char img[64]; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++) img[i]=(unsigned char)(a*(i+1)+((i*i)&0xFF));\n"
     "  for(int i=1;i<63;i++){\n"
     "    int x=img[i-1],y=img[i],z=img[i+1];\n"
     "    int mx=x>y?x:y, mn=x<y?x:y;\n"
     "    int med=z>mx?mx:(z<mn?mn:z);\n"
     "    acc=acc*131+(unsigned)med; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x789ABCDULL}, "ImageAlgo", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Img = makeImgTC("x64i", "long", 2, "");
static const std::vector<RoundTripTC> kA64Img = makeImgTC("a64i", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Img = makeImgTC("armi", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(ImageAlgo, X64ImageAlgoRT,
                         ::testing::ValuesIn(kX64Img), rtTCName);
INSTANTIATE_TEST_SUITE_P(ImageAlgo, A64ImageAlgoRT,
                         ::testing::ValuesIn(kA64Img), rtTCName);
INSTANTIATE_TEST_SUITE_P(ImageAlgo, ARM32ImageAlgoRT,
                         ::testing::ValuesIn(kARM32Img), rtTCName);
