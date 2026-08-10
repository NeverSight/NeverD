//===- AllPlatform_VectorAlgo37RTTests.cpp - packed SIMD idiom probes -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Thirty-seventh batch of clang -O2 vector probes, covering packed-SIMD idioms
// not in batches 31-36: unsigned 16-bit rounding average (PAVGW / URHADD),
// signed saturating 16-bit add (PADDSW / SQADD), rounding shift-right
// (PSRLD after add-half / URSHR), even-lane 32x32->64 multiply
// (PMULUDQ / UMULL), unsigned 16-bit dot product (PMADDWD / UMLAL), per-lane
// max of three arrays (PMAXSD tree / SMAX), XOR reduction (PXOR tree / EOR),
// and per-16-bit population count (CNT / scalarized popcount).  Each kernel is
// an autovectorizable loop folded to one exact integer for a bit-exact
// original-vs-lifted compare.
//
// x64 uses -mssse3 (SSE2 packed integer + PAVGW); a64/arm32 use the default
// NEON baseline.  64-bit folds use only +/* and constant shifts (no libcalls).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo37RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo37RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo37RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo37RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo37RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo37RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec37TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Unsigned 16-bit rounding average (PAVGW / URHADD).
    {p+"_avgu16",
     t+" "+p+"_avgu16("+t+" a) {\n"
     "  unsigned short x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned short)((a*(i+1))>>2); y[i]=(unsigned short)((a*(i+9))>>3); }\n"
     "  for (int i=0;i<64;i++){ unsigned avg=((unsigned)x[i]+(unsigned)y[i]+1u)>>1; acc=acc*131u+avg; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo37", opt, fl},

    // Signed saturating 16-bit add (PADDSW / SQADD).
    {p+"_sqadd16",
     t+" "+p+"_sqadd16("+t+" a) {\n"
     "  short x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(short)((a*(i+1))>>2); y[i]=(short)((a*(i+5))>>3); }\n"
     "  for (int i=0;i<64;i++){ int s=(int)x[i]+(int)y[i]; if(s>32767)s=32767; if(s<-32768)s=-32768; acc=acc*131u+(unsigned)(s&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo37", opt, fl},

    // Rounding shift-right by 7 (PSRLD after add-half / URSHR).
    {p+"_shrround",
     t+" "+p+"_shrround("+t+" a) {\n"
     "  unsigned x[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++) x[i]=(unsigned)(a*(i+1));\n"
     "  for (int i=0;i<32;i++){ unsigned r=(x[i]+(1u<<6))>>7; acc=acc*131u+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo37", opt, fl},

    // Even-lane 32x32->64 multiply accumulate (PMULUDQ / UMULL).
    {p+"_muleven",
     t+" "+p+"_muleven("+t+" a) {\n"
     "  unsigned x[32]; unsigned long long acc64=0;\n"
     "  for (int i=0;i<32;i++) x[i]=(unsigned)(a*(i+1));\n"
     "  for (int i=0;i<16;i++) acc64 += (unsigned long long)x[i*2]*(unsigned long long)x[i*2+1];\n"
     "  unsigned acc=(unsigned)acc64 ^ (unsigned)(acc64>>32);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo37", opt, fl},

    // Unsigned 16-bit dot product (PMADDWD / UMLAL).
    {p+"_dotu16",
     t+" "+p+"_dotu16("+t+" a) {\n"
     "  unsigned short x[64], y[64]; unsigned long long acc64=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned short)((a*(i+1))>>2); y[i]=(unsigned short)((a*(i+5))>>3); }\n"
     "  for (int i=0;i<64;i++) acc64 += (unsigned long long)((unsigned)x[i]*(unsigned)y[i]);\n"
     "  unsigned acc=(unsigned)acc64 ^ (unsigned)(acc64>>32);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo37", opt, fl},

    // Per-lane max of three arrays (PMAXSD tree / SMAX).
    {p+"_selmax3",
     t+" "+p+"_selmax3("+t+" a) {\n"
     "  int x[32], y[32], z[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1))^(i*0x13); y[i]=(int)(a*(i+3))^(i*0x57); z[i]=(int)(a*(i+5))^(i*0x9b); }\n"
     "  for (int i=0;i<32;i++){ int m=x[i]>y[i]?x[i]:y[i]; m=m>z[i]?m:z[i]; acc=acc*131u+(unsigned)m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo37", opt, fl},

    // XOR reduction (PXOR tree / EOR).
    {p+"_xorreduce",
     t+" "+p+"_xorreduce("+t+" a) {\n"
     "  unsigned x[64];\n"
     "  for (int i=0;i<64;i++) x[i]=(unsigned)(a*(i+1)) ^ (i*0x2bd);\n"
     "  unsigned r=0; for(int i=0;i<64;i++) r^=x[i];\n"
     "  return ("+t+")(r*2654435761u);\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo37", opt, fl},

    // Per-16-bit population count reduction (CNT / scalarized popcount).
    {p+"_cnt16",
     t+" "+p+"_cnt16("+t+" a) {\n"
     "  unsigned short x[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(unsigned short)((a*(i+1))>>1);\n"
     "  for (int i=0;i<64;i++) acc += (unsigned)__builtin_popcount((unsigned)x[i]);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo37", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec37 =
    makeVec37TC("x64v37", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec37 =
    makeVec37TC("a64v37", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec37 =
    makeVec37TC("armv37", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo37, X64VectorAlgo37RT,
                         ::testing::ValuesIn(kX64Vec37), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo37, A64VectorAlgo37RT,
                         ::testing::ValuesIn(kA64Vec37), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo37, ARM32VectorAlgo37RT,
                         ::testing::ValuesIn(kARM32Vec37), rtTCName);
