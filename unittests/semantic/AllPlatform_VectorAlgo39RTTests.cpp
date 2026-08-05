//===- AllPlatform_VectorAlgo39RTTests.cpp - packed SIMD idiom probes -----===//
//
// Thirty-ninth batch of clang -O2 vector probes, covering packed-SIMD idioms
// not in batches 31-38: signed per-byte negate (PSIGNB / NEG), unsigned 32-bit
// min (PMINUD / UMIN), per-lane compare-not-equal mask (PCMPEQ inverted /
// CMNE), OR-with-not (POR+PANDN / ORN), 16-bit left shift narrow (VSHLL /
// widen+shl), signed 32-bit max reduction (PMAXSD / SMAXV), byte sum-of-squares
// (PMADDUBSW reduce), and per-lane conditional increment (CMGT+ADD).  Each kernel
// is an autovectorizable loop folded to one exact integer for a bit-exact
// original-vs-lifted compare.
//
// x64 uses -mssse3 (SSE2 packed integer + SSSE3 PSIGN); a64/arm32 use the
// default NEON baseline.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo39RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo39RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo39RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo39RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo39RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo39RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec39TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Signed per-byte negate (PSIGNB / NEG).
    {p+"_negb",
     t+" "+p+"_negb("+t+" a) {\n"
     "  signed char x[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(signed char)((a*(i+1))>>2);\n"
     "  for (int i=0;i<64;i++){ int v=-(int)x[i]; acc=acc*131u+(unsigned)(v&0xFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo39", opt, fl},

    // Unsigned 32-bit min (PMINUD / UMIN).
    {p+"_minu32",
     t+" "+p+"_minu32("+t+" a) {\n"
     "  unsigned x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(unsigned)(a*(i+1)); y[i]=(unsigned)(a*(i+5))^0x7777u; }\n"
     "  for (int i=0;i<32;i++){ unsigned m=x[i]<y[i]?x[i]:y[i]; acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo39", opt, fl},

    // Compare-not-equal mask AND (PCMPEQ inverted / CMNE).
    {p+"_cmpne",
     t+" "+p+"_cmpne("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1)) & 0xFF; y[i]=(int)(a*(i+2)) & 0xFF; }\n"
     "  for (int i=0;i<32;i++){ int m=(x[i]!=y[i])?-1:0; acc=acc*131u+(unsigned)(m & (x[i]^y[i])); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo39", opt, fl},

    // OR-with-not (POR+PANDN / ORN).
    {p+"_orn",
     t+" "+p+"_orn("+t+" a) {\n"
     "  unsigned x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(unsigned)(a*(i+1)); y[i]=(unsigned)(a*(i+3)); }\n"
     "  for (int i=0;i<32;i++){ unsigned r=x[i]|(~y[i]); acc=acc*131u+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo39", opt, fl},

    // 16-bit left shift narrow (VSHLL / widen+shl).
    {p+"_shll16",
     t+" "+p+"_shll16("+t+" a) {\n"
     "  unsigned short x[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(unsigned short)((a*(i+1))>>3);\n"
     "  for (int i=0;i<64;i++){ unsigned w=(unsigned)x[i]<<4; acc=acc*131u+(w&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo39", opt, fl},

    // Signed 32-bit max reduction (PMAXSD / SMAXV).
    {p+"_maxs32",
     t+" "+p+"_maxs32("+t+" a) {\n"
     "  int x[64]; int best=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(int)(a*(i+1)) ^ (i*0x1357);\n"
     "  best=x[0]; for(int i=1;i<64;i++) if(x[i]>best) best=x[i];\n"
     "  return ("+t+")((unsigned)best*2654435761u);\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo39", opt, fl},

    // Byte sum-of-squares (PMADDUBSW reduce).
    {p+"_sqsum8",
     t+" "+p+"_sqsum8("+t+" a) {\n"
     "  unsigned char x[64]; unsigned sum=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(unsigned char)((a*(i+1))>>2);\n"
     "  for (int i=0;i<64;i++) sum += (unsigned)x[i]*(unsigned)x[i];\n"
     "  return ("+t+")(sum*40503u);\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo39", opt, fl},

    // Per-lane conditional increment (CMGT+ADD).
    {p+"_condinc",
     t+" "+p+"_condinc("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1)); y[i]=(int)(a*(i+4)); }\n"
     "  for (int i=0;i<32;i++){ int v=x[i]+((x[i]>y[i])?1:0); acc=acc*131u+(unsigned)v; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo39", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec39 =
    makeVec39TC("x64v39", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec39 =
    makeVec39TC("a64v39", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec39 =
    makeVec39TC("armv39", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo39, X64VectorAlgo39RT,
                         ::testing::ValuesIn(kX64Vec39), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo39, A64VectorAlgo39RT,
                         ::testing::ValuesIn(kA64Vec39), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo39, ARM32VectorAlgo39RT,
                         ::testing::ValuesIn(kARM32Vec39), rtTCName);
