//===- AllPlatform_VectorAlgo31RTTests.cpp - packed SIMD idiom probes -----===//
//
// Thirty-first batch of clang -O2 vector probes, aimed at the packed-SIMD
// idioms the docs still flag as fragile: saturating add/sub (PADDS/PSUBUS,
// SQADD/UQSUB, VQADD/VQSUB), sign-copy (PSIGN), compare-to-bitmask (PMOVMSKB),
// widening multiply-accumulate (PMADDWD, SMLAL, VMLAL), bit-clear a&~b
// (PANDN/BIC/VBIC), per-lane variable shift (USHL/VSHR), and FP horizontal
// reduction (FADDP/HADDPS).  Each kernel is an autovectorizable loop folded to
// one exact integer (FP results are returned as their bit pattern) so the
// original-vs-lifted comparison is bit-exact.
//
// x64 uses -mssse3 (covers the SSE2 saturating/PMADDWD plus SSSE3 PSIGN);
// a64/arm32 use the default NEON baseline.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo31RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo31RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo31RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo31RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo31RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo31RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec31TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Unsigned 16-bit saturating subtract reduction (PSUBUSW / UQSUB / VQSUB).
    {p+"_satsubu16",
     t+" "+p+"_satsubu16("+t+" a) {\n"
     "  unsigned short x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned short)((a*(i+1))>>3); y[i]=(unsigned short)((a*(i+7))>>5); }\n"
     "  for (int i=0;i<64;i++){ unsigned short d=(x[i]>y[i])?(unsigned short)(x[i]-y[i]):(unsigned short)0; acc=acc*131u+d; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo31", opt, fl},

    // Signed 8-bit saturating add reduction (PADDSB / SQADD / VQADD).
    {p+"_sataddi8",
     t+" "+p+"_sataddi8("+t+" a) {\n"
     "  signed char x[128], y[128]; int acc=0;\n"
     "  for (int i=0;i<128;i++){ x[i]=(signed char)((a*(i+1))>>2); y[i]=(signed char)((a*(i+3))>>4); }\n"
     "  for (int i=0;i<128;i++){ int s=(int)x[i]+(int)y[i]; if(s>127)s=127; if(s<-128)s=-128; acc=acc*131+(s&0xFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo31", opt, fl},

    // Sign-copy: r = sign(y) applied to x (PSIGND / NEON neg+cmp select).
    {p+"_vsign",
     t+" "+p+"_vsign("+t+" a) {\n"
     "  int x[64], y[64]; int acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(int)(a*(i+1)) ^ (i*0x5111); y[i]=(int)(a*(i+5)) - (i*0x3001); }\n"
     "  for (int i=0;i<64;i++){ int r = (y[i]<0)? -x[i] : ((y[i]>0)? x[i] : 0); acc=acc*131+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo31", opt, fl},

    // Compare-to-bitmask: gather sign bits of bytes into an int (PMOVMSKB).
    {p+"_movmsk",
     t+" "+p+"_movmsk("+t+" a) {\n"
     "  signed char v[128]; unsigned acc=0;\n"
     "  for (int i=0;i<128;i++) v[i]=(signed char)((a*(i+1))>>1);\n"
     "  for (int b=0;b<4;b++){ unsigned m=0; for(int i=0;i<32;i++) if(v[b*32+i]<0) m|=(1u<<i); acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo31", opt, fl},

    // Widening multiply-accumulate int16->int32 dot product (PMADDWD/SMLAL/VMLAL).
    {p+"_dotacc",
     t+" "+p+"_dotacc("+t+" a) {\n"
     "  short x[128], y[128]; long long acc=0;\n"
     "  for (int i=0;i<128;i++){ x[i]=(short)((a*(i+1))>>4); y[i]=(short)((a*(i+9))>>6); }\n"
     "  for (int i=0;i<128;i++) acc += (long long)((int)x[i]*(int)y[i]);\n"
     "  return ("+t+")(acc ^ (acc>>32));\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo31", opt, fl},

    // Bit-clear a & ~b reduction (PANDN / BIC / VBIC).
    {p+"_bicmask",
     t+" "+p+"_bicmask("+t+" a) {\n"
     "  unsigned x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned)(a*(i+1)); y[i]=(unsigned)(a*(i+11)) ^ 0x55AA33CCu; }\n"
     "  for (int i=0;i<64;i++) acc ^= (x[i] & ~y[i]);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo31", opt, fl},

    // Per-lane variable right shift reduction (USHL/VSHR with runtime amount).
    {p+"_varshr",
     t+" "+p+"_varshr("+t+" a) {\n"
     "  unsigned x[64], s[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned)(a*(i+1)) | 0x80000000u; s[i]=((unsigned)(a*(i+3))>>7)&31u; }\n"
     "  for (int i=0;i<64;i++) acc = acc*131u + (x[i] >> s[i]);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo31", opt, fl},

    // FP horizontal reduction (FADDP/HADDPS); exact integer floats, bit-exact.
    {p+"_fhoriz",
     t+" "+p+"_fhoriz("+t+" a) {\n"
     "  float f[64]; float s=0.0f;\n"
     "  for (int i=0;i<64;i++) f[i]=(float)(int)(((a*(i+1))>>9)&0x3FF);\n"
     "  for (int i=0;i<64;i++) s+=f[i];\n"
     "  unsigned u; __builtin_memcpy(&u,&s,4);\n"
     "  return ("+t+")u;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo31", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec31 =
    makeVec31TC("x64v31", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec31 =
    makeVec31TC("a64v31", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec31 =
    makeVec31TC("armv31", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo31, X64VectorAlgo31RT,
                         ::testing::ValuesIn(kX64Vec31), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo31, A64VectorAlgo31RT,
                         ::testing::ValuesIn(kA64Vec31), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo31, ARM32VectorAlgo31RT,
                         ::testing::ValuesIn(kARM32Vec31), rtTCName);
