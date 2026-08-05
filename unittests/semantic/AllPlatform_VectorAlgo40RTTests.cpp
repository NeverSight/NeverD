//===- AllPlatform_VectorAlgo40RTTests.cpp - packed SIMD idiom probes -----===//
//
// Fortieth batch of clang -O2 vector probes, covering packed-SIMD idioms not
// in batches 31-39: signed 16-bit absolute value (PABSW / ABS), unsigned 32-bit
// max (PMAXUD / UMAX), per-lane compare-greater-or-equal mask (PCMPGE / CMGE),
// XOR-then-AND mask chain (PXOR+PAND), 32-bit left shift narrow (VSHLL / shl),
// signed 16-bit min reduction (PMINSW / SMINV), unsigned byte multiply-accumulate
// (PMADDUBSW / UMLAL reduce), and per-lane conditional decrement (CMGE+SUB).
// Each kernel is an autovectorizable loop folded to one exact integer for a
// bit-exact original-vs-lifted compare.
//
// x64 uses -mssse3 (SSE2 packed integer); a64/arm32 use the default NEON
// baseline.  All abs is taken in the unsigned domain.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo40RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo40RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo40RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo40RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo40RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo40RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec40TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Signed 16-bit absolute value (PABSW / ABS).
    {p+"_absw",
     t+" "+p+"_absw("+t+" a) {\n"
     "  short x[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(short)((a*(i+1))>>3);\n"
     "  for (int i=0;i<64;i++){ int v=x[i]; unsigned av=(unsigned)(v<0?-v:v); acc=acc*131u+(av&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo40", opt, fl},

    // Unsigned 32-bit max (PMAXUD / UMAX).
    {p+"_maxu32",
     t+" "+p+"_maxu32("+t+" a) {\n"
     "  unsigned x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(unsigned)(a*(i+1)); y[i]=(unsigned)(a*(i+5))^0x3333u; }\n"
     "  for (int i=0;i<32;i++){ unsigned m=x[i]>y[i]?x[i]:y[i]; acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo40", opt, fl},

    // Compare-greater-or-equal mask AND (PCMPGE / CMGE).
    {p+"_cmpge",
     t+" "+p+"_cmpge("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1)); y[i]=(int)(a*(i+3))-(i*5); }\n"
     "  for (int i=0;i<32;i++){ int m=(x[i]>=y[i])?-1:0; acc=acc*131u+(unsigned)(m & (x[i]+y[i])); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo40", opt, fl},

    // XOR-then-AND mask chain (PXOR+PAND).
    {p+"_xormask",
     t+" "+p+"_xormask("+t+" a) {\n"
     "  unsigned x[32], y[32], m[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(unsigned)(a*(i+1)); y[i]=(unsigned)(a*(i+4)); m[i]=0xFF00FF00u; }\n"
     "  for (int i=0;i<32;i++){ unsigned r=(x[i]^y[i])&m[i]; acc=acc*131u+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo40", opt, fl},

    // 32-bit left shift narrow (VSHLL / shl).
    {p+"_shll32",
     t+" "+p+"_shll32("+t+" a) {\n"
     "  unsigned x[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++) x[i]=(unsigned)(a*(i+1))&0xFFFFu;\n"
     "  for (int i=0;i<32;i++){ unsigned w=x[i]<<5; acc=acc*131u+w; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo40", opt, fl},

    // Signed 16-bit min reduction (PMINSW / SMINV).
    {p+"_mins16",
     t+" "+p+"_mins16("+t+" a) {\n"
     "  short x[64]; short best=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(short)(((a*(i+1))>>2) ^ (i*0x11));\n"
     "  best=x[0]; for(int i=1;i<64;i++) if(x[i]<best) best=x[i];\n"
     "  return ("+t+")((unsigned)(best&0xFFFF)*2654435761u);\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo40", opt, fl},

    // Unsigned byte multiply-accumulate (PMADDUBSW / UMLAL reduce).
    {p+"_macu8",
     t+" "+p+"_macu8("+t+" a) {\n"
     "  unsigned char x[64], y[64]; unsigned long long acc64=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned char)((a*(i+1))>>1); y[i]=(unsigned char)((a*(i+7))>>2); }\n"
     "  for (int i=0;i<64;i++) acc64 += (unsigned long long)x[i]*(unsigned long long)y[i];\n"
     "  unsigned acc=(unsigned)acc64 ^ (unsigned)(acc64>>32);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo40", opt, fl},

    // Per-lane conditional decrement (CMGE+SUB).
    {p+"_conddec",
     t+" "+p+"_conddec("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1)); y[i]=(int)(a*(i+4)); }\n"
     "  for (int i=0;i<32;i++){ int v=x[i]-((x[i]>=y[i])?1:0); acc=acc*131u+(unsigned)v; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo40", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec40 =
    makeVec40TC("x64v40", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec40 =
    makeVec40TC("a64v40", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec40 =
    makeVec40TC("armv40", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo40, X64VectorAlgo40RT,
                         ::testing::ValuesIn(kX64Vec40), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo40, A64VectorAlgo40RT,
                         ::testing::ValuesIn(kA64Vec40), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo40, ARM32VectorAlgo40RT,
                         ::testing::ValuesIn(kARM32Vec40), rtTCName);
