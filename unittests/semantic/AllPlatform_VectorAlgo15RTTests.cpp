//===- AllPlatform_VectorAlgo15RTTests.cpp - widen/narrow MAC ---*- C++ -*-===//
//
// Fifteenth batch of clang -O2 algorithm probes.  Stresses widening/narrowing
// and pairwise variants adjacent to the #265e/f bugs: unsigned i8 widening dot
// product (udot/umull), signed pairwise-add-long reduction (saddlp), halving
// and rounding-halving add (vhadd/vrhadd), shift-left-long (sshll), unsigned
// widening multiply-accumulate (umlal), add-high-narrow (addhn) and signed
// saturating narrow high.
//
// Every algorithm folds to an exact integer for bit-exact comparison.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo15RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo15RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo15RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo15RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo15RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo15RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec15TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Unsigned i8 widening dot product (udot / umull+uaddw).
    {p+"_udotw8",
     t+" "+p+"_udotw8("+t+" a) {\n"
     "  unsigned char v[128], w[128]; unsigned s = 0;\n"
     "  for (int i=0;i<128;i++){ v[i]=(unsigned char)(a*(i+1)); w[i]=(unsigned char)(a*(i+3)+i); }\n"
     "  for (int i=0;i<128;i++) s += (unsigned)v[i]*(unsigned)w[i];\n"
     "  return (int)s;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo15", opt, fl},

    // Signed widening pairwise add reduction of an i16 array (saddlp chains).
    {p+"_saddlp",
     t+" "+p+"_saddlp("+t+" a) {\n"
     "  short v[96]; long long s = 0;\n"
     "  for (int i=0;i<96;i++) v[i]=(short)(a*(i+1) - i*271);\n"
     "  for (int i=0;i<96;i++) s += v[i];\n"
     "  return (int)s;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo15", opt, fl},

    // Halving add of two i32 arrays: (a+b)>>1 with no overflow (vhadd).
    {p+"_hadd",
     t+" "+p+"_hadd("+t+" a) {\n"
     "  int x[64], y[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(int)(a*(i+1)); y[i]=(int)(a*(i+3)); }\n"
     "  for (int i=0;i<64;i++) s += (int)(((long long)x[i]+(long long)y[i])>>1);\n"
     "  return s;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo15", opt, fl},

    // Rounding halving add of two u16 arrays: (a+b+1)>>1 (vrhadd).
    {p+"_rhadd16",
     t+" "+p+"_rhadd16("+t+" a) {\n"
     "  unsigned short x[64], y[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned short)(a*(i+1)); y[i]=(unsigned short)(a*(i+5)+i); }\n"
     "  for (int i=0;i<64;i++) s += ((int)x[i]+(int)y[i]+1)>>1;\n"
     "  return s;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo15", opt, fl},

    // Shift-left long: widen i16 -> i32 then <<4, summed (sshll).
    {p+"_shll",
     t+" "+p+"_shll("+t+" a) {\n"
     "  short v[64]; long long s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(short)(a*(i+1) - i*97);\n"
     "  for (int i=0;i<64;i++) s += ((long long)v[i])<<4;\n"
     "  return (int)s;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo15", opt, fl},

    // Unsigned widening multiply-accumulate i16 -> i32 (umlal).
    {p+"_umlal",
     t+" "+p+"_umlal("+t+" a) {\n"
     "  unsigned short v[64], w[64]; unsigned acc = 0;\n"
     "  for (int i=0;i<64;i++){ v[i]=(unsigned short)(a*(i+1)); w[i]=(unsigned short)(a*(i+3)+i); }\n"
     "  for (int i=0;i<64;i++) acc += (unsigned)v[i]*(unsigned)w[i];\n"
     "  return (int)acc;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo15", opt, fl},

    // Add then high-narrow: ((a+b)>>16) truncated to i16, summed (addhn).
    {p+"_addhn",
     t+" "+p+"_addhn("+t+" a) {\n"
     "  int x[64], y[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(int)(a*(i+1)); y[i]=(int)(a*(i+3)+i); }\n"
     "  for (int i=0;i<64;i++) s += (short)(((unsigned)x[i]+(unsigned)y[i])>>16);\n"
     "  return s;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo15", opt, fl},

    // Signed min across an i16 array (sminv / pminsw reduce).
    {p+"_sminv16",
     t+" "+p+"_sminv16("+t+" a) {\n"
     "  short v[64]; int mn = 32767;\n"
     "  for (int i=0;i<64;i++) v[i]=(short)(a*(i+1) ^ (i*0x1357));\n"
     "  for (int i=0;i<64;i++) if (v[i]<mn) mn=v[i];\n"
     "  return mn;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo15", opt, fl},

    // Unsigned widening abs-diff accumulate i8 (uabal / psadbw).
    {p+"_uabd8",
     t+" "+p+"_uabd8("+t+" a) {\n"
     "  unsigned char x[128], y[128]; int s = 0;\n"
     "  for (int i=0;i<128;i++){ x[i]=(unsigned char)(a*(i+1)); y[i]=(unsigned char)(a*(i+7)+i); }\n"
     "  for (int i=0;i<128;i++){ int d=(int)x[i]-(int)y[i]; s += d<0?-d:d; }\n"
     "  return s;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo15", opt, fl},

    // Widen i8 -> i32 with sign, scale and reduce (sxtl / pmovsxbd).
    {p+"_sxtl8",
     t+" "+p+"_sxtl8("+t+" a) {\n"
     "  signed char v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(signed char)(a*(i+1) + i*11);\n"
     "  for (int i=0;i<64;i++) s += (int)v[i]*5;\n"
     "  return s;\n"
     "}\n",
     {0xAABBCCDULL}, "VectorAlgo15", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec15 =
    makeVec15TC("x64v15", "long", 2, "-msse4.2");
static const std::vector<RoundTripTC> kA64Vec15 =
    makeVec15TC("a64v15", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec15 =
    makeVec15TC("armv15v", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo15, X64VectorAlgo15RT,
                         ::testing::ValuesIn(kX64Vec15), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo15, A64VectorAlgo15RT,
                         ::testing::ValuesIn(kA64Vec15), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo15, ARM32VectorAlgo15RT,
                         ::testing::ValuesIn(kARM32Vec15), rtTCName);
