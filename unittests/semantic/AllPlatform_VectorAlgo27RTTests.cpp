//===- AllPlatform_VectorAlgo27RTTests.cpp - interleave/strided kernels C++===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Twenty-seventh batch of clang -O2 probes targeting de-interleaving structure
// access and saturating narrow chains — the patterns that lower to AArch64
// LD2/LD3/LD4 + ST2/ST3/ST4, ARM32 VLDn/VSTn, and x86 unpack/shuffle webs:
// interleaved RGB->luma (LD3), complex array multiply (LD2), 3-lane stride sum,
// saturating int32->int16->int8 pack chain, vectorized 32-bit byte swap (REV),
// running min/max reduction, and fixed-point clamp-scale.  All 32-bit-internal,
// no runtime library calls, run across all four targets including i386.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo27RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo27RT, Verify) { roundTripX64(GetParam()); }

class X86VectorAlgo27RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86VectorAlgo27RT, Verify) { roundTripX86(GetParam()); }

class A64VectorAlgo27RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo27RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo27RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo27RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVA27TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Interleaved RGB -> luma (LD3 de-interleave on ARM, unpack/shuffle on x86).
    {p+"_rgb2y",
     t+" "+p+"_rgb2y("+t+" a){\n"
     "  unsigned char rgb[3*64]; for(int i=0;i<3*64;i++) rgb[i]=(unsigned char)(a+i*7);\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ unsigned r=rgb[3*i],g=rgb[3*i+1],b=rgb[3*i+2];\n"
     "    unsigned y=(r*77u+g*150u+b*29u)>>8; acc=acc*131u+(y&0xFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo27", opt, fl},

    // Complex array multiply (interleaved re/im -> LD2 de-interleave).
    {p+"_cmul",
     t+" "+p+"_cmul("+t+" a){\n"
     "  short re[48],im[48]; for(int i=0;i<48;i++){ re[i]=(short)(a+i*5); im[i]=(short)(a*2+i*3); }\n"
     "  int sr=0,si=0;\n"
     "  for(int i=0;i<48;i++){ int xr=re[i],xi=im[i],yr=re[47-i],yi=im[47-i];\n"
     "    sr += xr*yr - xi*yi; si += xr*yi + xi*yr; }\n"
     "  return ("+t+")((unsigned)sr*131u+(unsigned)si);\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo27", opt, fl},

    // 4-field struct stride sum (LD4 de-interleave / strided gather).
    {p+"_stride4",
     t+" "+p+"_stride4("+t+" a){\n"
     "  int v[4*40]; for(int i=0;i<4*40;i++) v[i]=(int)(a+i*13);\n"
     "  int s0=0,s1=0,s2=0,s3=0;\n"
     "  for(int i=0;i<40;i++){ s0+=v[4*i]; s1+=v[4*i+1]; s2+=v[4*i+2]; s3+=v[4*i+3]; }\n"
     "  return ("+t+")((unsigned)s0*7u+(unsigned)s1*5u+(unsigned)s2*3u+(unsigned)s3);\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo27", opt, fl},

    // Saturating narrow chain int32 -> int16 -> int8 (SQXTN / packsswb webs).
    {p+"_satpack",
     t+" "+p+"_satpack("+t+" a){\n"
     "  int w[64]; for(int i=0;i<64;i++) w[i]=(int)((a+i*131)*(i-32));\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<64;i++){ int x=w[i];\n"
     "    int s16 = x>32767?32767:(x<-32768?-32768:x);\n"
     "    int s8 = s16>127?127:(s16<-128?-128:s16);\n"
     "    h=h*131u+(unsigned char)s8; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo27", opt, fl},

    // Vectorized 32-bit byte swap (REV32 / bswap / movbe-style).
    {p+"_bswap",
     t+" "+p+"_bswap("+t+" a){\n"
     "  unsigned w[48]; for(int i=0;i<48;i++) w[i]=(unsigned)(a*(i+1)+i*2654435761u);\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ unsigned x=w[i];\n"
     "    x=((x>>24)&0xFF)|((x>>8)&0xFF00)|((x<<8)&0xFF0000)|((x<<24)&0xFF000000u);\n"
     "    h=h*131u+x; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo27", opt, fl},

    // Running min/max reduction (vectorizable horizontal min+max).
    {p+"_minmax",
     t+" "+p+"_minmax("+t+" a){\n"
     "  int v[80]; for(int i=0;i<80;i++) v[i]=(int)((a*(i+1))^(i*40503));\n"
     "  int mn=v[0],mx=v[0];\n"
     "  for(int i=1;i<80;i++){ if(v[i]<mn) mn=v[i]; if(v[i]>mx) mx=v[i]; }\n"
     "  return ("+t+")((unsigned)mn*131u+(unsigned)mx);\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo27", opt, fl},

    // Fixed-point clamp-scale (multiply, shift, saturating clamp to [0,255]).
    {p+"_clampscale",
     t+" "+p+"_clampscale("+t+" a){\n"
     "  unsigned char in[96]; for(int i=0;i<96;i++) in[i]=(unsigned char)(a+i*9);\n"
     "  int g=(int)((a&0xFF)+64);\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<96;i++){ int v=((int)in[i]*g)>>6; v=v>255?255:(v<0?0:v);\n"
     "    h=h*131u+(unsigned)v; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo27", opt, fl},

    // Interleaved store: scatter a value into 2-channel output then re-sum (ST2).
    {p+"_zip2",
     t+" "+p+"_zip2("+t+" a){\n"
     "  short out[2*48]; \n"
     "  for(int i=0;i<48;i++){ out[2*i]=(short)(a+i*3); out[2*i+1]=(short)(a*2-i*5); }\n"
     "  int s=0; for(int i=0;i<2*48;i++) s+=(short)out[i]*(i&1?-1:1);\n"
     "  return ("+t+")(unsigned)s;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo27", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeVA27TC("x64v27", "long", 2, "");
static const std::vector<RoundTripTC> kX86 = makeVA27TC("x86v27", "int", 2, "");
static const std::vector<RoundTripTC> kA64 = makeVA27TC("a64v27", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeVA27TC("armv27", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(VectorAlgo27, X64VectorAlgo27RT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo27, X86VectorAlgo27RT,
                         ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo27, A64VectorAlgo27RT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo27, ARM32VectorAlgo27RT,
                         ::testing::ValuesIn(kARM), rtTCName);
