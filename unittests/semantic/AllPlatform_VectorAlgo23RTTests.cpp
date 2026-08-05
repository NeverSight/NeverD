//===- AllPlatform_VectorAlgo23RTTests.cpp - cross-lane SIMD kernels -*- C++ -*-===//
//
// Twenty-third batch: clang -O2 probes stressing cross-lane reductions, saturating
// arithmetic, shuffle permutations, and flag-derived control flow.  Each kernel
// folds lane-wise results into one integer hash so any lane drop, wrong saturation,
// or shuffle miscompile surfaces.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo23RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo23RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo23RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo23RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo23RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo23RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVA23TC(const char *prefix, const char *T, int opt,
                                           const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Horizontal sum of 16-bit lanes (pshuflw-style reduction).
    {"hsum16",
     t+" "+p+"_hsum16("+t+" a){\n"
     "  short v[32]; for(int i=0;i<32;i++) v[i]=(short)((a*(i+1)+i*3)&0x7FFF);\n"
     "  int s=0; for(int i=0;i<32;i++) s+=v[i];\n"
     "  return ("+t+")s;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo23", opt, fl},

    // Saturating 16-bit add chain (vqaddq-style).
    {"satadd16",
     t+" "+p+"_satadd16("+t+" a){\n"
     "  short x[64],y[64],acc[64];\n"
     "  for(int i=0;i<64;i++){ x[i]=(short)((a+i*5)&0x7FFF); y[i]=(short)((a*3+i*7)&0x7FFF); }\n"
     "  for(int i=0;i<64;i++){ int s=(int)x[i]+(int)y[i]; acc[i]=(short)(s>32767?32767:(s<-32768?-32768:s)); }\n"
     "  unsigned h=0; for(int i=0;i<64;i++) h=h*131u+(unsigned short)acc[i];\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo23", opt, fl},

    // Byte shuffle permutation (pshufb-style index table).
    {"pshufb8",
     t+" "+p+"_pshufb8("+t+" a){\n"
     "  unsigned char s[32],idx[32],o[32];\n"
     "  for(int i=0;i<32;i++) s[i]=(unsigned char)(a+i*3+i*7);\n"
     "  for(int i=0;i<32;i++) idx[i]=(unsigned char)((a+i*5+i*3)&31);\n"
     "  for(int i=0;i<32;i++) o[i]=s[idx[i]];\n"
     "  unsigned h=0; for(int i=0;i<32;i++) h=h*131u+o[i];\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo23", opt, fl},

    // Dot product of signed 8-bit lanes (pmaddubsw-style).
    {"dot8s",
     t+" "+p+"_dot8s("+t+" a){\n"
     "  signed char x[48],y[48]; int acc=0;\n"
     "  for(int i=0;i<48;i++){ x[i]=(signed char)((a+i*3+i*5)&0x7F); y[i]=(signed char)((a*7+i*11)&0x7F); }\n"
     "  for(int i=0;i<48;i++) acc+=(int)x[i]*(int)y[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo23", opt, fl},

    // Min/max reduction across 32 bytes.
    {"minmax32",
     t+" "+p+"_minmax32("+t+" a){\n"
     "  unsigned char v[32]; for(int i=0;i<32;i++) v[i]=(unsigned char)(a*(i+1)+i*13);\n"
     "  unsigned char mn=255,mx=0; for(int i=0;i<32;i++){ if(v[i]<mn)mn=v[i]; if(v[i]>mx)mx=v[i]; }\n"
     "  return ("+t+")((unsigned)mn*256+(unsigned)mx);\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo23", opt, fl},

    // Branchy flag-derived threshold (cmp+setcc pattern).
    {"flagthresh",
     t+" "+p+"_flagthresh("+t+" a){\n"
     "  int v[64]; for(int i=0;i<64;i++) v[i]=(int)((a*(i+1)+i*17)&0xFFFF);\n"
     "  int cnt=0,sum=0;\n"
     "  for(int i=0;i<64;i++){ sum+=v[i]; if(sum>(int)a) cnt++; }\n"
     "  return ("+t+")cnt;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo23", opt, fl},

    // 2D 3x3 matrix transpose (zip columns).
    {"mat3x3t",
     t+" "+p+"_mat3x3t("+t+" a){\n"
     "  int m[9]; for(int r=0;r<3;r++) for(int c=0;c<3;c++) m[r*3+c]=(int)(a*(r+1)+c*7);\n"
     "  int t[9]; for(int c=0;c<3;c++) for(int r=0;r<3;r++) t[c*3+r]=m[r*3+c];\n"
     "  unsigned h=0; for(int i=0;i<9;i++) h=h*131u+(unsigned)t[i];\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo23", opt, fl},

    // CRC-like bit-by-bit with carry (no table, pure ALU).
    {"bitcrc",
     t+" "+p+"_bitcrc("+t+" a){\n"
     "  unsigned crc=0xFFFF;\n"
     "  for(int i=0;i<64;i++){ unsigned b=(unsigned char)(a+i); crc^=b; crc=(unsigned short)((crc>>1)^((unsigned short)b)); }\n"
     "  return ("+t+")crc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo23", opt, fl},

    // Population count per byte then sum.
    {"popcntsum",
     t+" "+p+"_popcntsum("+t+" a){\n"
     "  unsigned char v[128]; for(int i=0;i<128;i++) v[i]=(unsigned char)(a*(i+1)+i*3);\n"
     "  unsigned h=0; for(int i=0;i<128;i++) h+=__builtin_popcount(v[i]);\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo23", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64 = makeVA23TC("x64v23", "long", 2, "");
static const std::vector<RoundTripTC> kA64 = makeVA23TC("a64v23", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeVA23TC("armv23", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(VectorAlgo23, X64VectorAlgo23RT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo23, A64VectorAlgo23RT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo23, ARM32VectorAlgo23RT,
                         ::testing::ValuesIn(kARM), rtTCName);
