//===- AllPlatform_VectorAlgo24RTTests.cpp - FP/rodata/PHI kernels -*- C++ -*-===//
//
// Twenty-fourth batch: reverse-index rodata gather, dual LUT fusion, float
// conditional blends, double horizontal reductions, and branchy accumulators
// that keep PHI nodes alive through the optimizer.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo24RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo24RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo24RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo24RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo24RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo24RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVA24TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Reverse-index LUT (vectorizer often emits INT_SUB from table end).
    {p+"_revlut",
     t+" "+p+"_revlut("+t+" a){\n"
     "  unsigned char in[48];\n"
     "  for(int i=0;i<48;i++) in[i]=(unsigned char)((a*(i+1)+i*11)>>1);\n"
     "  static const unsigned tab[16]={3,7,11,15,19,23,27,31,35,39,43,47,51,55,59,63};\n"
     "  unsigned long acc=0;\n"
     "  for(int i=0;i<48;i++) acc=acc*131u+tab[15-(in[i]&15)];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo24", opt, fl},

    // Two independent tables fused (rodata base disambiguation).
    {p+"_dualtab",
     t+" "+p+"_dualtab("+t+" a){\n"
     "  unsigned char in[40];\n"
     "  for(int i=0;i<40;i++) in[i]=(unsigned char)((a+i*3)>>2);\n"
     "  static const unsigned t0[8]={1,3,5,7,9,11,13,15};\n"
     "  static const unsigned t1[8]={2,4,6,8,10,12,14,16};\n"
     "  unsigned long acc=0;\n"
     "  for(int i=0;i<40;i++){ unsigned k=in[i]&7;\n"
     "    acc=acc*131u+t0[k]^t1[(7-k)]; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo24", opt, fl},

    // Float conditional blend (cmp+and+or / blendv pattern).
    {p+"_fblend",
     t+" "+p+"_fblend("+t+" a){\n"
     "  float x[32],y[32],o[32];\n"
     "  for(int i=0;i<32;i++){ x[i]=(float)((a+i)%41)-20.0f;\n"
     "    y[i]=(float)((a*3+i*5)%37)-18.0f; }\n"
     "  for(int i=0;i<32;i++) o[i]=(x[i]>y[i])?x[i]:y[i];\n"
     "  float s=0; for(int i=0;i<32;i++) s+=o[i];\n"
     "  unsigned bits; __builtin_memcpy(&bits,&s,4); return ("+t+")bits;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo24", opt, "-fno-math-errno"},

    // Double horizontal reduction chain.
    {p+"_dhoriz",
     t+" "+p+"_dhoriz("+t+" a){\n"
     "  double v[24]; for(int i=0;i<24;i++) v[i]=(double)((a+i*9)%67)-33.0;\n"
     "  double s=0; for(int i=0;i<24;i++) s+=v[i]*v[i];\n"
     "  unsigned lo,hi; __builtin_memcpy(&lo,&s,4); __builtin_memcpy(&hi,((char*)&s)+4,4);\n"
     "  return ("+t+")(lo*131u+hi);\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo24", opt, fl},

    // Branchy signed accumulate (PHI across if/else in the loop).
    {p+"_phiacc",
     t+" "+p+"_phiacc("+t+" a){\n"
     "  int v[64]; for(int i=0;i<64;i++) v[i]=(int)((a*(i+1)+i*13)&0xFFFF);\n"
     "  int acc=0;\n"
     "  for(int i=0;i<64;i++){ if(v[i]>(int)a) acc+=v[i]; else acc-=v[i]; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo24", opt, fl},

    // Rotate-left hash (rol chains).
    {p+"_rolhash",
     t+" "+p+"_rolhash("+t+" a){\n"
     "  unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<96;i++){ unsigned b=(unsigned)((a+i*17)&0xFF);\n"
     "    h=(h<<5)|(h>>(32-5)); h^=b; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo24", opt, fl},

    // 32-bit to byte pack via shift/mask.
    {p+"_pack32",
     t+" "+p+"_pack32("+t+" a){\n"
     "  unsigned v[16],o[64];\n"
     "  for(int i=0;i<16;i++) v[i]=(unsigned)(a*(i+1)+i*3);\n"
     "  for(int i=0;i<16;i++){ unsigned x=v[i];\n"
     "    o[4*i+0]=(x>>0)&0xFF; o[4*i+1]=(x>>8)&0xFF;\n"
     "    o[4*i+2]=(x>>16)&0xFF; o[4*i+3]=(x>>24)&0xFF; }\n"
     "  unsigned h=0; for(int i=0;i<64;i++) h=h*131u+o[i];\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo24", opt, fl},

    // SAD-style byte absolute difference sum.
    {p+"_sad8",
     t+" "+p+"_sad8("+t+" a){\n"
     "  unsigned char x[48],y[48]; unsigned s=0;\n"
     "  for(int i=0;i<48;i++){ x[i]=(unsigned char)(a+i*3); y[i]=(unsigned char)(a*7+i); }\n"
     "  for(int i=0;i<48;i++){ int d=(int)x[i]-(int)y[i]; s+=(unsigned)(d<0?-d:d); }\n"
     "  return ("+t+")s;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo24", opt, fl},

    // Descending-index gather tab[(N-i)&mask].
    {p+"_descidx",
     t+" "+p+"_descidx("+t+" a){\n"
     "  unsigned char in[32];\n"
     "  for(int i=0;i<32;i++) in[i]=(unsigned char)((a+i*5)&31);\n"
     "  static const unsigned short tab[32]={\n"
     "    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,\n"
     "    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32};\n"
     "  unsigned long acc=0;\n"
     "  for(int i=0;i<32;i++) acc=acc*131u+tab[(31-(in[i]&31))&31];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0xAABBCCDLL}, "VectorAlgo24", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64 = makeVA24TC("x64v24", "long", 2, "");
static const std::vector<RoundTripTC> kA64 = makeVA24TC("a64v24", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeVA24TC("armv24", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(VectorAlgo24, X64VectorAlgo24RT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo24, A64VectorAlgo24RT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo24, ARM32VectorAlgo24RT,
                         ::testing::ValuesIn(kARM), rtTCName);
