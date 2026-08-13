//===- AllPlatform_VectorAlgo26RTTests.cpp - field/DSP/codec kernels -*-C++-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Twenty-sixth batch of clang -O2 probes in domains not yet exercised: GF(2^8)
// finite-field multiply (xor/shift with conditional reduction), bit-by-bit
// integer square root (long compare/sub/shift chains), Q15 saturating multiply
// (DSP rounding + clamp), UTF-8 length classification (branchy byte scan), hex
// nibble encoding (LUT / arithmetic), 3D Morton interleave, and a median-of-5
// sorting network.  All 32-bit-internal, no runtime library calls.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo26RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo26RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo26RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo26RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo26RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo26RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVA26TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // GF(2^8) multiply (Rijndael field): xor-shift with conditional reduction.
    {p+"_gfmul",
     t+" "+p+"_gfmul("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ unsigned x=(unsigned)(a+i)&0xFF, y=(unsigned)(a*3+i*5)&0xFF;\n"
     "    unsigned pr=0;\n"
     "    for(int b=0;b<8;b++){ if(y&1) pr^=x; unsigned hi=x&0x80; x<<=1; if(hi) x^=0x11B; x&=0xFF; y>>=1; }\n"
     "    acc=acc*131u+(pr&0xFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo26", opt, fl},

    // Bit-by-bit integer square root (compare/sub/shift chain).
    {p+"_isqrt",
     t+" "+p+"_isqrt("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ unsigned n=(unsigned)(a*(i+1)+i*7);\n"
     "    unsigned res=0,bit=1u<<30;\n"
     "    while(bit>n) bit>>=2;\n"
     "    while(bit){ if(n>=res+bit){ n-=res+bit; res=(res>>1)+bit; } else res>>=1; bit>>=2; }\n"
     "    acc=acc*131u+res; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo26", opt, fl},

    // Q15 saturating multiply with rounding (DSP).
    {p+"_q15mul",
     t+" "+p+"_q15mul("+t+" a){\n"
     "  short x[80],y[80]; int acc=0;\n"
     "  for(int i=0;i<80;i++){ x[i]=(short)(a+i*13); y[i]=(short)(a*2+i*7); }\n"
     "  for(int i=0;i<80;i++){ int p=((int)x[i]*(int)y[i]+0x4000)>>15;\n"
     "    p = p>32767?32767:(p<-32768?-32768:p); acc+=p; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo26", opt, fl},

    // UTF-8 byte length classification (branchy scan, loop-carried skip).
    {p+"_utf8len",
     t+" "+p+"_utf8len("+t+" a){\n"
     "  unsigned char s[120]; for(int i=0;i<120;i++) s[i]=(unsigned char)(a+i*3);\n"
     "  unsigned chars=0; int i=0;\n"
     "  while(i<120){ unsigned c=s[i];\n"
     "    int n = c<0x80?1:(c<0xE0?2:(c<0xF0?3:4)); i+=n; chars++; }\n"
     "  return ("+t+")(chars*131u+(unsigned)i);\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo26", opt, fl},

    // Hex nibble encode (arithmetic ASCII conversion, vectorizable).
    {p+"_hexenc",
     t+" "+p+"_hexenc("+t+" a){\n"
     "  unsigned char in[48],out[96];\n"
     "  for(int i=0;i<48;i++) in[i]=(unsigned char)(a*(i+1)+i*9);\n"
     "  for(int i=0;i<48;i++){ unsigned hi=in[i]>>4, lo=in[i]&15;\n"
     "    out[2*i]=(unsigned char)(hi<10?'0'+hi:'a'+hi-10);\n"
     "    out[2*i+1]=(unsigned char)(lo<10?'0'+lo:'a'+lo-10); }\n"
     "  unsigned h=0; for(int i=0;i<96;i++) h=h*131u+out[i];\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo26", opt, fl},

    // 3D Morton interleave (spread 3 x 10-bit fields).
    {p+"_morton3",
     t+" "+p+"_morton3("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ unsigned v=(unsigned)(a+i*7)&0x3FF;\n"
     "    v=(v|(v<<16))&0xFF0000FFu; v=(v|(v<<8))&0x0F00F00Fu;\n"
     "    v=(v|(v<<4))&0xC30C30C3u; v=(v|(v<<2))&0x49249249u;\n"
     "    acc=acc*131u+v; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo26", opt, fl},

    // Median-of-5 sorting network (min/max compare-swap webs).
    {p+"_median5",
     t+" "+p+"_median5("+t+" a){\n"
     "  unsigned h=0;\n"
     "  for(int blk=0;blk<40;blk++){\n"
     "    int v0=(int)(a*(blk+1)+0), v1=(int)(a+blk*7), v2=(int)(a*3-blk*5);\n"
     "    int v3=(int)(a*5+blk*3), v4=(int)(a-blk*11);\n"
     "    int t0,t1;\n"
     "    if(v0>v1){t0=v0;v0=v1;v1=t0;} if(v3>v4){t0=v3;v3=v4;v4=t0;}\n"
     "    if(v0>v3){t0=v0;v0=v3;v3=t0;t1=v1;v1=v4;v4=t1;}\n"
     "    if(v2>v1){if(v1>v3){h=h*131u+(unsigned)v1;}else{h=h*131u+(unsigned)(v2<v3?v2:v3);}}\n"
     "    else{if(v2>v3){h=h*131u+(unsigned)v3;}else{h=h*131u+(unsigned)(v1<v2?v1:v2);}} }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo26", opt, fl},

    // Saturating accumulate to signed 16-bit with wraparound detection.
    {p+"_satacc16",
     t+" "+p+"_satacc16("+t+" a){\n"
     "  short acc=0; unsigned h=0;\n"
     "  for(int i=0;i<100;i++){ int v=(int)((a+i*13)&0xFFFF)-32768;\n"
     "    int s=(int)acc+(v>>4); s=s>32767?32767:(s<-32768?-32768:s); acc=(short)s;\n"
     "    h=h*131u+(unsigned short)acc; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo26", opt, fl},

    // Nibble popcount LUT applied to a byte stream (vpshufb-style table).
    {p+"_nibpop",
     t+" "+p+"_nibpop("+t+" a){\n"
     "  static const unsigned char lut[16]={0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4};\n"
     "  unsigned char in[128]; for(int i=0;i<128;i++) in[i]=(unsigned char)(a*(i+1)+i*3);\n"
     "  unsigned h=0; for(int i=0;i<128;i++) h += lut[in[i]&15]+lut[in[i]>>4];\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo26", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeVA26TC("x64v26", "long", 2, "");
static const std::vector<RoundTripTC> kA64 = makeVA26TC("a64v26", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeVA26TC("armv26", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(VectorAlgo26, X64VectorAlgo26RT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo26, A64VectorAlgo26RT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo26, ARM32VectorAlgo26RT,
                         ::testing::ValuesIn(kARM), rtTCName);
