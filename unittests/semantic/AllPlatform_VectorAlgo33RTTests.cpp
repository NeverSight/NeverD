//===- AllPlatform_VectorAlgo33RTTests.cpp - packed SIMD idiom probes -----===//
//
// Thirty-third batch of clang -O2 vector probes, covering packed-SIMD idioms
// not exercised by batches 31/32: unsigned per-lane min+max (PMINUB/PMAXUB,
// UMIN/UMAX), rounding average (PAVGB / URHADD), sum-of-absolute-differences
// (PSADBW / UABD reduce), compare-to-mask AND (PCMPGTD/CMGT), per-lane variable
// left shift (USHL / scalarized PSLLVD), unsigned 16-bit high multiply
// (PMULHUW / UMULL+shrn), int32 clamp to u8 range (min/max / PACKUS), and
// horizontal max+min reduction (SMAXV/SMINV).  Each kernel is an
// autovectorizable loop folded to one exact integer for a bit-exact compare.
//
// x64 uses -mssse3 (SSE2 packed integer + PSADBW/PAVGB/PMULHUW + SSSE3 PSIGN);
// a64/arm32 use the default NEON baseline.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo33RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo33RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo33RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo33RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo33RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo33RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec33TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Unsigned per-lane min + max reduction (PMINUB/PMAXUB / UMIN/UMAX).
    {p+"_uminmax",
     t+" "+p+"_uminmax("+t+" a) {\n"
     "  unsigned char x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned char)((a*(i+1))>>3); y[i]=(unsigned char)((a*(i+5))>>4); }\n"
     "  for (int i=0;i<64;i++){ unsigned char mn=x[i]<y[i]?x[i]:y[i]; unsigned char mx=x[i]>y[i]?x[i]:y[i]; acc=acc*131u+(unsigned)(mn+mx); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo33", opt, fl},

    // Unsigned byte rounding average (PAVGB / URHADD).
    {p+"_avgu8",
     t+" "+p+"_avgu8("+t+" a) {\n"
     "  unsigned char x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned char)((a*(i+1))>>2); y[i]=(unsigned char)((a*(i+9))>>5); }\n"
     "  for (int i=0;i<64;i++){ unsigned avg=((unsigned)x[i]+(unsigned)y[i]+1u)>>1; acc=acc*131u+avg; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo33", opt, fl},

    // Sum of absolute differences over bytes (PSADBW / UABD reduce).
    {p+"_absdiff",
     t+" "+p+"_absdiff("+t+" a) {\n"
     "  unsigned char x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned char)((a*(i+1))>>1); y[i]=(unsigned char)((a*(i+7))>>3); }\n"
     "  for (int i=0;i<64;i++){ unsigned d=x[i]>y[i]?(unsigned)(x[i]-y[i]):(unsigned)(y[i]-x[i]); acc+=d; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo33", opt, fl},

    // Compare-greater to all-ones mask, AND with delta (PCMPGTD / CMGT).
    {p+"_cmpmask",
     t+" "+p+"_cmpmask("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1)) ^ (i*0x1111); y[i]=(int)(a*(i+3)) - (i*0x77); }\n"
     "  for (int i=0;i<32;i++){ int m=(x[i]>y[i])?-1:0; acc=acc*131u+(unsigned)(m & (x[i]^y[i])); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo33", opt, fl},

    // Per-lane variable left shift reduction (USHL / scalarized PSLLVD).
    {p+"_sllvar",
     t+" "+p+"_sllvar("+t+" a) {\n"
     "  unsigned x[32], s[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(unsigned)(a*(i+1)); s[i]=((unsigned)(a*(i+5))>>9)&31u; }\n"
     "  for (int i=0;i<32;i++) acc = acc*131u + (x[i] << s[i]);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo33", opt, fl},

    // Unsigned 16-bit high multiply (PMULHUW / UMULL + shrn).
    {p+"_mulhiu16",
     t+" "+p+"_mulhiu16("+t+" a) {\n"
     "  unsigned short x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned short)((a*(i+1))>>2); y[i]=(unsigned short)((a*(i+11))>>3); }\n"
     "  for (int i=0;i<64;i++){ unsigned hi=((unsigned)x[i]*(unsigned)y[i])>>16; acc=acc*131u+hi; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo33", opt, fl},

    // Clamp int32 to unsigned-byte range [0,255] (min/max / PACKUS).
    {p+"_clampu8",
     t+" "+p+"_clampu8("+t+" a) {\n"
     "  int x[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++) x[i]=(int)((a*(i+1))>>6)-2000;\n"
     "  for (int i=0;i<32;i++){ int v=x[i]; v=v<0?0:(v>255?255:v); acc=acc*131u+(unsigned)v; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo33", opt, fl},

    // Horizontal max + min reduction (SMAXV/SMINV), combined by multiply.
    {p+"_hmax",
     t+" "+p+"_hmax("+t+" a) {\n"
     "  int x[64]; \n"
     "  for (int i=0;i<64;i++) x[i]=(int)(a*(i+1)) ^ (i*0x3331);\n"
     "  int best=x[0], worst=x[0];\n"
     "  for (int i=1;i<64;i++){ if(x[i]>best) best=x[i]; if(x[i]<worst) worst=x[i]; }\n"
     "  unsigned acc=(unsigned)best*2654435761u + (unsigned)worst*40503u;\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo33", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec33 =
    makeVec33TC("x64v33", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec33 =
    makeVec33TC("a64v33", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec33 =
    makeVec33TC("armv33", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo33, X64VectorAlgo33RT,
                         ::testing::ValuesIn(kX64Vec33), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo33, A64VectorAlgo33RT,
                         ::testing::ValuesIn(kA64Vec33), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo33, ARM32VectorAlgo33RT,
                         ::testing::ValuesIn(kARM32Vec33), rtTCName);
