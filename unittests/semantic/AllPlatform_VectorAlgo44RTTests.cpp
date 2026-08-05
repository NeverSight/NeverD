//===- AllPlatform_VectorAlgo44RTTests.cpp - packed SIMD idiom probes -----===//
//
// Forty-fourth batch of clang -O2 vector probes, covering packed-SIMD idioms
// not in batches 31-43: unsigned byte average (PAVGB / URHADD), signed
// saturating byte subtract (PSUBSB / SQSUB), per-lane compare-greater mask OR
// (PCMPGT / CMGT), XOR-then-OR chain (PXOR+POR), 32-bit left shift by const
// (PSLLD / SHL), signed 16-bit max reduction (PMAXSW / SMAXV), unsigned 32-bit
// dot product (PMULUDQ reduce), and per-lane conditional negate (CMGT+NEG).
// Each kernel is an autovectorizable loop folded to one exact integer for a
// bit-exact original-vs-lifted compare.
//
// x64 uses -mssse3 (SSE2 packed integer); a64/arm32 use the default NEON
// baseline.  Negation stays in unsigned domain where needed.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo44RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo44RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo44RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo44RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo44RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo44RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec44TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Unsigned byte average (PAVGB / URHADD).
    {p+"_avgbyte",
     t+" "+p+"_avgbyte("+t+" a) {\n"
     "  unsigned char x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned char)((a*(i+1))>>2); y[i]=(unsigned char)((a*(i+9))>>3); }\n"
     "  for (int i=0;i<64;i++){ unsigned avg=((unsigned)x[i]+(unsigned)y[i]+1u)>>1; acc=acc*131u+avg; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo44", opt, fl},

    // Signed saturating byte subtract (PSUBSB / SQSUB).
    {p+"_subsats8",
     t+" "+p+"_subsats8("+t+" a) {\n"
     "  signed char x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(signed char)((a*(i+1))>>2); y[i]=(signed char)((a*(i+7))>>3); }\n"
     "  for (int i=0;i<64;i++){ int d=(int)x[i]-(int)y[i]; if(d>127)d=127; if(d<-128)d=-128; acc=acc*131u+(unsigned)(d&0xFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo44", opt, fl},

    // Compare-greater mask OR (PCMPGT / CMGT).
    {p+"_cmpgtor",
     t+" "+p+"_cmpgtor("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1)); y[i]=(int)(a*(i+3))-(i*7); }\n"
     "  for (int i=0;i<32;i++){ unsigned m=(x[i]>y[i])?(unsigned)x[i]:(unsigned)y[i]; acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo44", opt, fl},

    // XOR-then-OR chain (PXOR+POR).
    {p+"_xoror",
     t+" "+p+"_xoror("+t+" a) {\n"
     "  unsigned x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(unsigned)(a*(i+1)); y[i]=(unsigned)(a*(i+5)); }\n"
     "  for (int i=0;i<32;i++){ unsigned r=(x[i]^y[i])|(x[i]&y[i]); acc=acc*131u+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo44", opt, fl},

    // 32-bit left shift by const 5 (PSLLD / SHL).
    {p+"_shl5",
     t+" "+p+"_shl5("+t+" a) {\n"
     "  unsigned x[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++) x[i]=(unsigned)(a*(i+1))&0x7FFFFu;\n"
     "  for (int i=0;i<32;i++) acc=acc*131u+(x[i]<<5);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo44", opt, fl},

    // Signed 16-bit max reduction (PMAXSW / SMAXV).
    {p+"_maxs16red",
     t+" "+p+"_maxs16red("+t+" a) {\n"
     "  short x[64]; short best=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(short)(((a*(i+1))>>2) ^ (i*0x31));\n"
     "  best=x[0]; for(int i=1;i<64;i++) if(x[i]>best) best=x[i];\n"
     "  return ("+t+")((unsigned)(best&0xFFFF)*2654435761u);\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo44", opt, fl},

    // Unsigned 32-bit dot product (PMULUDQ reduce).
    {p+"_dotu32",
     t+" "+p+"_dotu32("+t+" a) {\n"
     "  unsigned x[32], y[32]; unsigned long long acc64=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(unsigned)(a*(i+1))&0xFFFFu; y[i]=(unsigned)(a*(i+7))&0xFFFFu; }\n"
     "  for (int i=0;i<32;i++) acc64 += (unsigned long long)x[i]*(unsigned long long)y[i];\n"
     "  unsigned acc=(unsigned)acc64 ^ (unsigned)(acc64>>32);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo44", opt, fl},

    // Per-lane conditional negate (CMGT+NEG, unsigned domain).
    {p+"_condneg",
     t+" "+p+"_condneg("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1)); y[i]=(int)(a*(i+4)); }\n"
     "  for (int i=0;i<32;i++){ int v=x[i]; if(v>y[i]) v=-v; unsigned uv=(unsigned)(v<0?(0u-(unsigned)v):(unsigned)v); acc=acc*131u+uv; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo44", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec44 =
    makeVec44TC("x64v44", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec44 =
    makeVec44TC("a64v44", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec44 =
    makeVec44TC("armv44", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo44, X64VectorAlgo44RT,
                         ::testing::ValuesIn(kX64Vec44), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo44, A64VectorAlgo44RT,
                         ::testing::ValuesIn(kA64Vec44), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo44, ARM32VectorAlgo44RT,
                         ::testing::ValuesIn(kARM32Vec44), rtTCName);
