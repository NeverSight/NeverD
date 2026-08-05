//===- AllPlatform_VectorAlgo38RTTests.cpp - packed SIMD idiom probes -----===//
//
// Thirty-eighth batch of clang -O2 vector probes, covering packed-SIMD idioms
// not in batches 31-37: unsigned saturating byte subtract (PSUBUSB / UQSUB),
// unsigned saturating 16-bit add (PADDUSW / UQADD), 32-bit compare-equal mask
// (PCMPEQD / CMEQ), sign-mask variable blend (PBLENDVB / BSL), pairwise
// horizontal subtract (PHSUBD / SUB of deinterleaved lanes), signed 16-bit
// high multiply (PMULHW / shrn), 32-bit absolute difference (PABSD of diff /
// UABD), and unsigned 16->8 pack with clamp (PACKUSWB / UQXTN).  Each kernel is
// an autovectorizable loop folded to one exact integer for a bit-exact
// original-vs-lifted compare.
//
// x64 uses -mssse3 (SSE2 packed integer); a64/arm32 use the default NEON
// baseline.  All subtraction that could overflow is done in the unsigned domain.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo38RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo38RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo38RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo38RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo38RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo38RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec38TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Unsigned saturating byte subtract (PSUBUSB / UQSUB).
    {p+"_subu8sat",
     t+" "+p+"_subu8sat("+t+" a) {\n"
     "  unsigned char x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned char)((a*(i+1))>>1); y[i]=(unsigned char)((a*(i+5))>>2); }\n"
     "  for (int i=0;i<64;i++){ int d=(int)x[i]-(int)y[i]; if(d<0)d=0; acc=acc*131u+(unsigned)d; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo38", opt, fl},

    // Unsigned saturating 16-bit add (PADDUSW / UQADD).
    {p+"_addsat16u",
     t+" "+p+"_addsat16u("+t+" a) {\n"
     "  unsigned short x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned short)((a*(i+1))>>1); y[i]=(unsigned short)((a*(i+5))>>2); }\n"
     "  for (int i=0;i<64;i++){ unsigned s=(unsigned)x[i]+(unsigned)y[i]; if(s>65535u)s=65535u; acc=acc*131u+s; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo38", opt, fl},

    // 32-bit compare-equal mask AND (PCMPEQD / CMEQ).
    {p+"_cmpeq32",
     t+" "+p+"_cmpeq32("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1)) & 0xF; y[i]=(int)(a*(i+2)) & 0xF; }\n"
     "  for (int i=0;i<32;i++){ int m=(x[i]==y[i])?-1:0; acc=acc*131u+(unsigned)(m & (x[i]+1)); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo38", opt, fl},

    // Sign-mask variable blend (PBLENDVB / BSL).
    {p+"_blendv",
     t+" "+p+"_blendv("+t+" a) {\n"
     "  int sel[32], x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ sel[i]=(int)(a*(i+1))^(i*0x55); x[i]=(int)(a*(i+3)); y[i]=(int)(a*(i+7)); }\n"
     "  for (int i=0;i<32;i++){ int r=(sel[i]<0)?x[i]:y[i]; acc=acc*131u+(unsigned)r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo38", opt, fl},

    // Pairwise horizontal subtract (PHSUBD / deinterleave-sub).
    {p+"_hsub",
     t+" "+p+"_hsub("+t+" a) {\n"
     "  int x[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(int)(a*(i+1)) - (i*3);\n"
     "  for (int i=0;i<64;i+=2){ int d=x[i]-x[i+1]; acc=acc*131u+(unsigned)d; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo38", opt, fl},

    // Signed 16-bit high multiply (PMULHW / shrn).
    {p+"_mulhss16",
     t+" "+p+"_mulhss16("+t+" a) {\n"
     "  short x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(short)((a*(i+1))>>3); y[i]=(short)((a*(i+5))>>4); }\n"
     "  for (int i=0;i<64;i++){ int hi=((int)x[i]*(int)y[i])>>16; acc=acc*131u+(unsigned)(hi&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo38", opt, fl},

    // 32-bit absolute difference accumulate (PABSD of diff / UABD).
    {p+"_absdiff32",
     t+" "+p+"_absdiff32("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1))^(i*0x13); y[i]=(int)(a*(i+4))^(i*0x57); }\n"
     "  for (int i=0;i<32;i++){ unsigned ux=(unsigned)x[i],uy=(unsigned)y[i]; unsigned d=(x[i]>y[i])?(ux-uy):(uy-ux); acc=acc*131u+d; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo38", opt, fl},

    // Unsigned 16->8 pack with clamp (PACKUSWB / UQXTN).
    {p+"_packus16",
     t+" "+p+"_packus16("+t+" a) {\n"
     "  short x[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(short)(((a*(i+1))>>2) - 300);\n"
     "  for (int i=0;i<64;i++){ int v=x[i]; if(v<0)v=0; if(v>255)v=255; acc=acc*131u+(unsigned)v; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo38", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec38 =
    makeVec38TC("x64v38", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec38 =
    makeVec38TC("a64v38", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec38 =
    makeVec38TC("armv38", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo38, X64VectorAlgo38RT,
                         ::testing::ValuesIn(kX64Vec38), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo38, A64VectorAlgo38RT,
                         ::testing::ValuesIn(kA64Vec38), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo38, ARM32VectorAlgo38RT,
                         ::testing::ValuesIn(kARM32Vec38), rtTCName);
