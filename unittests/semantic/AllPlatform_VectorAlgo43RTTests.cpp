//===- AllPlatform_VectorAlgo43RTTests.cpp - packed SIMD idiom probes -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Forty-third batch of clang -O2 vector probes, covering packed-SIMD idioms
// not in batches 31-42: unsigned byte min (PMINUB / UMIN), signed 32-bit min
// (PMINSD / SMIN), per-lane compare-not-zero mask (PCMPEQ inverted / CMNEZ),
// OR-with-complement (POR+NOT / ORN), 16-bit right shift narrow (VSHRN u16),
// unsigned 32-bit max reduction (PMAXUD / UMAXV), signed 16-bit dot product
// (PMADDWD s16 reduce), and per-lane conditional triple (CMGT+ADD+SUB).  Each
// kernel is an autovectorizable loop folded to one exact integer for a
// bit-exact original-vs-lifted compare.
//
// x64 uses -mssse3 (SSE2 packed integer); a64/arm32 use the default NEON
// baseline.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo43RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo43RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo43RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo43RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo43RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo43RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec43TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Unsigned byte min (PMINUB / UMIN).
    {p+"_minu8",
     t+" "+p+"_minu8("+t+" a) {\n"
     "  unsigned char x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned char)((a*(i+1))>>2); y[i]=(unsigned char)((a*(i+5))>>3); }\n"
     "  for (int i=0;i<64;i++){ unsigned m=(unsigned)x[i]<(unsigned)y[i]?(unsigned)x[i]:(unsigned)y[i]; acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo43", opt, fl},

    // Signed 32-bit min (PMINSD / SMIN).
    {p+"_mins32",
     t+" "+p+"_mins32("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1))^(i*0x11); y[i]=(int)(a*(i+4))^(i*0x57); }\n"
     "  for (int i=0;i<32;i++){ int m=x[i]<y[i]?x[i]:y[i]; acc=acc*131u+(unsigned)m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo43", opt, fl},

    // Compare-not-zero mask AND (PCMPEQ inverted / CMNEZ).
    {p+"_nezero",
     t+" "+p+"_nezero("+t+" a) {\n"
     "  int x[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++) x[i]=(int)(a*(i+1)) & 0xF;\n"
     "  for (int i=0;i<32;i++){ int m=(x[i]!=0)?-1:0; acc=acc*131u+(unsigned)(m & x[i]); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo43", opt, fl},

    // OR-with-complement (POR+NOT / ORN).
    {p+"_orcomp",
     t+" "+p+"_orcomp("+t+" a) {\n"
     "  unsigned x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(unsigned)(a*(i+1)); y[i]=(unsigned)(a*(i+4)); }\n"
     "  for (int i=0;i<32;i++){ unsigned r=x[i]|(~y[i]); acc=acc*131u+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo43", opt, fl},

    // 16-bit right shift narrow (VSHRN u16).
    {p+"_shrnu16",
     t+" "+p+"_shrnu16("+t+" a) {\n"
     "  unsigned x[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(unsigned)(a*(i+1));\n"
     "  for (int i=0;i<64;i++){ unsigned short n=(unsigned short)(x[i]>>13); acc=acc*131u+n; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo43", opt, fl},

    // Unsigned 32-bit max reduction (PMAXUD / UMAXV).
    {p+"_maxu32red",
     t+" "+p+"_maxu32red("+t+" a) {\n"
     "  unsigned x[64]; unsigned best=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(unsigned)(a*(i+1)) ^ (i*0x99);\n"
     "  best=x[0]; for(int i=1;i<64;i++) if(x[i]>best) best=x[i];\n"
     "  return ("+t+")(best*2654435761u);\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo43", opt, fl},

    // Signed 16-bit dot product (PMADDWD s16 reduce).
    {p+"_dots16",
     t+" "+p+"_dots16("+t+" a) {\n"
     "  short x[64], y[64]; long long acc64=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(short)((a*(i+1))>>3); y[i]=(short)((a*(i+5))>>4); }\n"
     "  for (int i=0;i<64;i++) acc64 += (long long)x[i]*(long long)y[i];\n"
     "  unsigned acc=(unsigned)acc64 ^ (unsigned)(acc64>>32);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo43", opt, fl},

    // Per-lane conditional triple (CMGT+ADD+SUB).
    {p+"_condtri",
     t+" "+p+"_condtri("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1)); y[i]=(int)(a*(i+4)); }\n"
     "  for (int i=0;i<32;i++){ int v=(x[i]>y[i])?(x[i]+y[i]-1):(x[i]-y[i]+1); acc=acc*131u+(unsigned)v; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo43", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec43 =
    makeVec43TC("x64v43", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec43 =
    makeVec43TC("a64v43", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec43 =
    makeVec43TC("armv43", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo43, X64VectorAlgo43RT,
                         ::testing::ValuesIn(kX64Vec43), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo43, A64VectorAlgo43RT,
                         ::testing::ValuesIn(kA64Vec43), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo43, ARM32VectorAlgo43RT,
                         ::testing::ValuesIn(kARM32Vec43), rtTCName);
