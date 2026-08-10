//===- AllPlatform_VectorAlgo36RTTests.cpp - packed SIMD idiom probes -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Thirty-sixth batch of clang -O2 vector probes, covering packed-SIMD idioms
// not in batches 31-35: unsigned saturating byte add (PADDUSB / UQADD), signed
// saturating byte subtract (PSUBSB / SQSUB), and-not/or blend (PANDN / BIC),
// even/odd lane interleave (PUNPCK / ZIP), widening i16*i16->i32 accumulate
// (PMADDWD / SMLAL), compare-less mask AND (PCMPGTD / CMGT), packed constant
// shift-add (PSLLD+PADDD / SHL+ADD), and unsigned 16-bit min reduction
// (PMINUW / UMINV).  Each kernel is an autovectorizable loop folded to one
// exact integer for a bit-exact original-vs-lifted compare.
//
// x64 uses -mssse3 (SSE2 packed integer); a64/arm32 use the default NEON
// baseline.  Saturation bounds and widths are chosen so the native and lifted
// results match bit-for-bit.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo36RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo36RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo36RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo36RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo36RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo36RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec36TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Unsigned saturating byte add (PADDUSB / UQADD).
    {p+"_usatadd8",
     t+" "+p+"_usatadd8("+t+" a) {\n"
     "  unsigned char x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned char)((a*(i+1))>>1); y[i]=(unsigned char)((a*(i+5))>>2); }\n"
     "  for (int i=0;i<64;i++){ unsigned s=(unsigned)x[i]+(unsigned)y[i]; if(s>255u)s=255u; acc=acc*131u+s; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo36", opt, fl},

    // Signed saturating byte subtract (PSUBSB / SQSUB).
    {p+"_ssatsub8",
     t+" "+p+"_ssatsub8("+t+" a) {\n"
     "  signed char x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(signed char)((a*(i+1))>>2); y[i]=(signed char)((a*(i+7))>>3); }\n"
     "  for (int i=0;i<64;i++){ int d=(int)x[i]-(int)y[i]; if(d>127)d=127; if(d<-128)d=-128; acc=acc*131u+(unsigned)(d&0xFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo36", opt, fl},

    // And-not / or blend (PANDN / BIC).
    {p+"_andnotmix",
     t+" "+p+"_andnotmix("+t+" a) {\n"
     "  unsigned x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(unsigned)(a*(i+1)); y[i]=(unsigned)(a*(i+3))^0x5A5Au; }\n"
     "  for (int i=0;i<32;i++){ unsigned r=(x[i]&~y[i])|(~x[i]&y[i]); acc=acc*131u+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo36", opt, fl},

    // Even/odd lane interleave (PUNPCK / ZIP).
    {p+"_interleave",
     t+" "+p+"_interleave("+t+" a) {\n"
     "  unsigned x[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++) x[i]=(unsigned)(a*(i+1));\n"
     "  for (int i=0;i<16;i++){ unsigned e=x[i*2], o=x[i*2+1]; acc=acc*131u+(e^(o<<1))+(o^(e>>1)); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo36", opt, fl},

    // Widening i16*i16->i32 accumulate (PMADDWD / SMLAL).
    {p+"_wmul16",
     t+" "+p+"_wmul16("+t+" a) {\n"
     "  short x[64], y[64]; long long acc64=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(short)((a*(i+1))>>3); y[i]=(short)((a*(i+5))>>4); }\n"
     "  for (int i=0;i<64;i++) acc64 += (long long)x[i]*(long long)y[i];\n"
     "  unsigned acc=(unsigned)acc64 ^ (unsigned)(acc64>>32);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo36", opt, fl},

    // Compare-less to all-ones mask, AND with sum (PCMPGTD / CMGT).
    {p+"_cmpltmask",
     t+" "+p+"_cmpltmask("+t+" a) {\n"
     "  int x[32], y[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1)) ^ (i*0x111); y[i]=(int)(a*(i+4)) - (i*9); }\n"
     "  for (int i=0;i<32;i++){ int m=(x[i]<y[i])?-1:0; acc=acc*131u+(unsigned)(m & (x[i]+y[i])); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo36", opt, fl},

    // Packed constant shift-add: 7*x via shifts (PSLLD+PADDD / SHL+ADD).
    {p+"_shladd",
     t+" "+p+"_shladd("+t+" a) {\n"
     "  unsigned x[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++) x[i]=(unsigned)(a*(i+1));\n"
     "  for (int i=0;i<32;i++){ unsigned v=(x[i]<<3)+(x[i]<<1)-x[i]; acc=acc*131u+v; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo36", opt, fl},

    // Unsigned 16-bit min reduction + deviation (PMINUW / UMINV).
    {p+"_minu16",
     t+" "+p+"_minu16("+t+" a) {\n"
     "  unsigned short x[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(unsigned short)((a*(i+1))>>2);\n"
     "  unsigned mn=0xFFFFu; for(int i=0;i<64;i++) if(x[i]<mn) mn=x[i];\n"
     "  acc=mn; for(int i=0;i<64;i++) acc=acc*131u+(unsigned)(x[i]-mn);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo36", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec36 =
    makeVec36TC("x64v36", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec36 =
    makeVec36TC("a64v36", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec36 =
    makeVec36TC("armv36", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo36, X64VectorAlgo36RT,
                         ::testing::ValuesIn(kX64Vec36), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo36, A64VectorAlgo36RT,
                         ::testing::ValuesIn(kA64Vec36), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo36, ARM32VectorAlgo36RT,
                         ::testing::ValuesIn(kARM32Vec36), rtTCName);
