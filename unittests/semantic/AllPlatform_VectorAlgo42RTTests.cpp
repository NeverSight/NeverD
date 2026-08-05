//===- AllPlatform_VectorAlgo42RTTests.cpp - packed SIMD idiom probes -----===//
//
// Forty-second batch of clang -O2 vector probes, covering packed-SIMD idioms
// not in batches 31-41: unsigned byte max (PMAXUB / UMAX), signed 16-bit min
// (PMINSW / SMIN), per-lane compare-equal-zero mask (PCMPEQ+zero / CMEQZ),
// AND-with-complement (PAND+NOT / BIC), 32-bit right shift narrow (VSHRN /
// shr), signed 32-bit max reduction (PMAXSD / SMAXV), unsigned 16-bit
// dot product (PMADDWD u16 reduce), and per-lane conditional halve (CMGT+SHR).
// Each kernel is an autovectorizable loop folded to one exact integer for a
// bit-exact original-vs-lifted compare.
//
// x64 uses -mssse3 (SSE2 packed integer); a64/arm32 use the default NEON
// baseline.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo42RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo42RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo42RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo42RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo42RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo42RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec42TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Unsigned byte max (PMAXUB / UMAX).
    {p+"_maxu8",
     t+" "+p+"_maxu8("+t+" a) {\n"
     "  unsigned char x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned char)((a*(i+1))>>2); y[i]=(unsigned char)((a*(i+5))>>3); }\n"
     "  for (int i=0;i<64;i++){ unsigned m=(unsigned)x[i]>(unsigned)y[i]?(unsigned)x[i]:(unsigned)y[i]; acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo42", opt, fl},

    // Signed 16-bit min (PMINSW / SMIN).
    {p+"_mins16",
     t+" "+p+"_mins16("+t+" a) {\n"
     "  short x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(short)((a*(i+1))>>3); y[i]=(short)((a*(i+5))>>4); }\n"
     "  for (int i=0;i<64;i++){ short m=x[i]<y[i]?x[i]:y[i]; acc=acc*131u+(unsigned)(m&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo42", opt, fl},

    // Compare-equal-zero mask AND (PCMPEQ+zero / CMEQZ).
    {p+"_eqzero",
     t+" "+p+"_eqzero("+t+" a) {\n"
     "  int x[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++) x[i]=(int)(a*(i+1)) & 0xF;\n"
     "  for (int i=0;i<32;i++){ int m=(x[i]==0)?-1:0; acc=acc*131u+(unsigned)(m & (i*0x111)); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo42", opt, fl},

    // AND-with-complement (PAND+NOT / BIC).
    {p+"_andcomp",
     t+" "+p+"_andcomp("+t+" a) {\n"
     "  unsigned x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(unsigned)(a*(i+1)); y[i]=(unsigned)(a*(i+4)); }\n"
     "  for (int i=0;i<32;i++){ unsigned r=x[i]&(~y[i]); acc=acc*131u+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo42", opt, fl},

    // 32-bit right shift narrow (VSHRN / shr).
    {p+"_shrn32",
     t+" "+p+"_shrn32("+t+" a) {\n"
     "  unsigned x[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++) x[i]=(unsigned)(a*(i+1));\n"
     "  for (int i=0;i<32;i++){ unsigned n=x[i]>>11; acc=acc*131u+n; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo42", opt, fl},

    // Signed 32-bit max reduction (PMAXSD / SMAXV).
    {p+"_maxs32red",
     t+" "+p+"_maxs32red("+t+" a) {\n"
     "  int x[64]; int best=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(int)(a*(i+1)) ^ (i*0x3579);\n"
     "  best=x[0]; for(int i=1;i<64;i++) if(x[i]>best) best=x[i];\n"
     "  return ("+t+")((unsigned)best*2654435761u);\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo42", opt, fl},

    // Unsigned 16-bit dot product (PMADDWD u16 reduce).
    {p+"_dotu16v2",
     t+" "+p+"_dotu16v2("+t+" a) {\n"
     "  unsigned short x[64], y[64]; unsigned sum=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned short)((a*(i+1))>>2); y[i]=(unsigned short)((a*(i+7))>>3); }\n"
     "  for (int i=0;i<64;i++) sum += (unsigned)x[i]*(unsigned)y[i];\n"
     "  return ("+t+")(sum*40503u);\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo42", opt, fl},

    // Per-lane conditional halve (CMGT+SHR).
    {p+"_condhalf",
     t+" "+p+"_condhalf("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1)); y[i]=(int)(a*(i+4)); }\n"
     "  for (int i=0;i<32;i++){ int v=(x[i]>y[i])?(x[i]>>1):x[i]; acc=acc*131u+(unsigned)v; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo42", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec42 =
    makeVec42TC("x64v42", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec42 =
    makeVec42TC("a64v42", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec42 =
    makeVec42TC("armv42", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo42, X64VectorAlgo42RT,
                         ::testing::ValuesIn(kX64Vec42), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo42, A64VectorAlgo42RT,
                         ::testing::ValuesIn(kA64Vec42), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo42, ARM32VectorAlgo42RT,
                         ::testing::ValuesIn(kARM32Vec42), rtTCName);
