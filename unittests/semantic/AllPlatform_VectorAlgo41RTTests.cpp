//===- AllPlatform_VectorAlgo41RTTests.cpp - packed SIMD idiom probes -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Forty-first batch of clang -O2 vector probes, covering packed-SIMD idioms
// not in batches 31-40: signed 32-bit absolute value (PABSD / ABS), unsigned
// 16-bit max (PMAXUW / UMAX), per-lane compare-less-or-equal mask (PCMPLE /
// CMLE), NAND logic (PAND then invert / BIC chain), 8-bit left shift widen
// (VSHLL / u8 shl), unsigned 32-bit min reduction (PMINUD / UMINV), signed
// byte dot product (PMADDSBW reduce), and per-lane conditional multiply-by-2
// (CMGT+ADD self).  Each kernel is an autovectorizable loop folded to one
// exact integer for a bit-exact original-vs-lifted compare.
//
// x64 uses -mssse3 (SSE2 packed integer); a64/arm32 use the default NEON
// baseline.  All abs is taken in the unsigned domain.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo41RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo41RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo41RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo41RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo41RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo41RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec41TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Signed 32-bit absolute value (PABSD / ABS).
    {p+"_absd",
     t+" "+p+"_absd("+t+" a) {\n"
     "  int x[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++) x[i]=(int)(a*(i+1)) ^ (i*0x2468);\n"
     "  for (int i=0;i<32;i++){ int v=x[i]; unsigned av=(unsigned)(v<0?(0u-(unsigned)v):(unsigned)v); acc=acc*131u+av; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo41", opt, fl},

    // Unsigned 16-bit max (PMAXUW / UMAX).
    {p+"_maxu16",
     t+" "+p+"_maxu16("+t+" a) {\n"
     "  unsigned short x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned short)((a*(i+1))>>2); y[i]=(unsigned short)((a*(i+5))>>3); }\n"
     "  for (int i=0;i<64;i++){ unsigned m=(unsigned)x[i]>(unsigned)y[i]?(unsigned)x[i]:(unsigned)y[i]; acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo41", opt, fl},

    // Compare-less-or-equal mask AND (PCMPLE / CMLE).
    {p+"_cmple",
     t+" "+p+"_cmple("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1)); y[i]=(int)(a*(i+3))+(i*3); }\n"
     "  for (int i=0;i<32;i++){ int m=(x[i]<=y[i])?-1:0; acc=acc*131u+(unsigned)(m & (x[i]-y[i])); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo41", opt, fl},

    // NAND logic (PAND then invert / BIC chain).
    {p+"_nand",
     t+" "+p+"_nand("+t+" a) {\n"
     "  unsigned x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(unsigned)(a*(i+1)); y[i]=(unsigned)(a*(i+4))^0xAAAAu; }\n"
     "  for (int i=0;i<32;i++){ unsigned r=~((x[i]&y[i])); acc=acc*131u+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo41", opt, fl},

    // 8-bit left shift widen (VSHLL / u8 shl).
    {p+"_shll8",
     t+" "+p+"_shll8("+t+" a) {\n"
     "  unsigned char x[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(unsigned char)((a*(i+1))>>2);\n"
     "  for (int i=0;i<64;i++){ unsigned w=(unsigned)x[i]<<3; acc=acc*131u+(w&0xFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo41", opt, fl},

    // Unsigned 32-bit min reduction (PMINUD / UMINV).
    {p+"_minu32red",
     t+" "+p+"_minu32red("+t+" a) {\n"
     "  unsigned x[64]; unsigned mn=0xFFFFFFFFu;\n"
     "  for (int i=0;i<64;i++) x[i]=(unsigned)(a*(i+1)) ^ (i*0x77);\n"
     "  for (int i=0;i<64;i++) if(x[i]<mn) mn=x[i];\n"
     "  return ("+t+")(mn*2654435761u);\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo41", opt, fl},

    // Signed byte dot product (PMADDSBW reduce).
    {p+"_dots8",
     t+" "+p+"_dots8("+t+" a) {\n"
     "  signed char x[64], y[64]; long long acc64=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(signed char)((a*(i+1))>>2); y[i]=(signed char)((a*(i+5))>>3); }\n"
     "  for (int i=0;i<64;i++) acc64 += (long long)x[i]*(long long)y[i];\n"
     "  unsigned acc=(unsigned)acc64 ^ (unsigned)(acc64>>32);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo41", opt, fl},

    // Per-lane conditional multiply-by-2 (CMGT+ADD self).
    {p+"_conddbl",
     t+" "+p+"_conddbl("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1)); y[i]=(int)(a*(i+4)); }\n"
     "  for (int i=0;i<32;i++){ int v=x[i]+((x[i]>y[i])?x[i]:0); acc=acc*131u+(unsigned)v; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo41", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec41 =
    makeVec41TC("x64v41", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec41 =
    makeVec41TC("a64v41", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec41 =
    makeVec41TC("armv41", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo41, X64VectorAlgo41RT,
                         ::testing::ValuesIn(kX64Vec41), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo41, A64VectorAlgo41RT,
                         ::testing::ValuesIn(kA64Vec41), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo41, ARM32VectorAlgo41RT,
                         ::testing::ValuesIn(kARM32Vec41), rtTCName);
