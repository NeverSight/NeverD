//===- AllPlatform_VectorAlgo35RTTests.cpp - packed SIMD idiom probes -----===//
//
// Thirty-fifth batch of clang -O2 vector probes, covering packed-SIMD idioms
// not in batches 31/32/33/34: per-signed-byte absolute value (PABSB / ABS),
// compare-select blend (PCMPGTD+blend / CMGT+BSL), bitwise logic chain
// (PAND/POR/PXOR/PANDN), per-lane variable right shift (USHR / scalarized
// PSRLVD), packed 32-bit low multiply (PMULUDQ sequence / MUL), equal-byte
// count (PCMPEQB + popcount), widening unsigned 16->32 accumulate
// (UADDL / unpack+add), and max-absolute reduction (ABS + UMAX).  Each kernel
// is an autovectorizable loop folded to one exact integer for a bit-exact
// original-vs-lifted compare.
//
// x64 uses -mssse3 (SSE2 packed integer + SSSE3 PABSB); a64/arm32 use the
// default NEON baseline.  All abs is taken in the unsigned domain so INT_MIN /
// -128 inputs stay UB-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo35RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo35RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo35RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo35RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo35RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo35RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec35TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Per-signed-byte absolute value (PABSB / ABS).
    {p+"_absb",
     t+" "+p+"_absb("+t+" a) {\n"
     "  signed char x[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(signed char)((a*(i+1))>>2);\n"
     "  for (int i=0;i<64;i++){ int v=x[i]; unsigned av=(unsigned)(v<0?-v:v); acc=acc*131u+av; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo35", opt, fl},

    // Compare-select blend: pick adjusted x or y by (x>y) (PCMPGTD+blend / CMGT+BSL).
    {p+"_blendmask",
     t+" "+p+"_blendmask("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1)) ^ (i*0x1111); y[i]=(int)(a*(i+3)) - (i*0x55); }\n"
     "  for (int i=0;i<32;i++){ int v=(x[i]>y[i])?(x[i]+1):(y[i]^7); acc=acc*131u+(unsigned)v; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo35", opt, fl},

    // Bitwise logic chain (PAND / POR / PXOR / PANDN).
    {p+"_logicchain",
     t+" "+p+"_logicchain("+t+" a) {\n"
     "  unsigned x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(unsigned)(a*(i+1)); y[i]=(unsigned)(a*(i+5))^0xA5A5u; }\n"
     "  for (int i=0;i<32;i++){ unsigned r=((x[i]&y[i])|(x[i]^y[i]))&~(y[i]>>3); acc=acc*131u+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo35", opt, fl},

    // Per-lane variable right shift reduction (USHR / scalarized PSRLVD).
    {p+"_srlvar",
     t+" "+p+"_srlvar("+t+" a) {\n"
     "  unsigned x[32], s[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(unsigned)(a*(i+1)); s[i]=((unsigned)(a*(i+5))>>9)&31u; }\n"
     "  for (int i=0;i<32;i++) acc = acc*131u + (x[i] >> s[i]);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo35", opt, fl},

    // Packed 32-bit low multiply (PMULUDQ sequence / MUL).
    {p+"_mullo32",
     t+" "+p+"_mullo32("+t+" a) {\n"
     "  unsigned x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(unsigned)(a*(i+1)); y[i]=(unsigned)(a*(i+7)); }\n"
     "  for (int i=0;i<32;i++) acc = acc*131u + (x[i]*y[i]);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo35", opt, fl},

    // Count equal bytes between two arrays (PCMPEQB + popcount).
    {p+"_eqcount",
     t+" "+p+"_eqcount("+t+" a) {\n"
     "  unsigned char x[64], y[64]; unsigned cnt=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned char)((a*(i+1))>>3); y[i]=(unsigned char)((a*(i+2))>>3); }\n"
     "  for (int i=0;i<64;i++) cnt += (x[i]==y[i])?1u:0u;\n"
     "  return ("+t+")(cnt*2654435761u);\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo35", opt, fl},

    // Widening unsigned 16->32 accumulate (UADDL / unpack+add).
    {p+"_waddu16",
     t+" "+p+"_waddu16("+t+" a) {\n"
     "  unsigned short x[64]; unsigned sum=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(unsigned short)((a*(i+1))>>1);\n"
     "  for (int i=0;i<64;i++) sum += (unsigned)x[i];\n"
     "  return ("+t+")(sum*40503u);\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo35", opt, fl},

    // Max-absolute reduction (ABS + UMAX), unsigned-domain abs.
    {p+"_maxabs",
     t+" "+p+"_maxabs("+t+" a) {\n"
     "  int x[64]; unsigned best=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(int)(a*(i+1)) ^ (i*0x2468);\n"
     "  for (int i=0;i<64;i++){ unsigned av=(unsigned)(x[i]<0?(0u-(unsigned)x[i]):(unsigned)x[i]); if(av>best) best=av; }\n"
     "  return ("+t+")(best*2654435761u);\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo35", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec35 =
    makeVec35TC("x64v35", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec35 =
    makeVec35TC("a64v35", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec35 =
    makeVec35TC("armv35", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo35, X64VectorAlgo35RT,
                         ::testing::ValuesIn(kX64Vec35), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo35, A64VectorAlgo35RT,
                         ::testing::ValuesIn(kA64Vec35), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo35, ARM32VectorAlgo35RT,
                         ::testing::ValuesIn(kARM32Vec35), rtTCName);
