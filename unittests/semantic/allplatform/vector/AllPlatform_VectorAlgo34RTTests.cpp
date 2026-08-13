//===- AllPlatform_VectorAlgo34RTTests.cpp - packed SIMD idiom probes -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Thirty-fourth batch of clang -O2 vector probes, covering packed-SIMD idioms
// not in batches 31/32/33: signed saturating narrow i32->i16 (PACKSSDW/SQXTN),
// pairwise horizontal add (PHADDD/ADDP), rounding multiply-high i16
// (PMULHRSW / SQRDMULH), shift-right-narrow (VSHRN / PSRLD+pack), per-byte
// population count (CNT / scalarized on x86), per-element byte swap
// (REV32 / PSHUFB), unsigned min reduction (UMINV), and unsigned byte dot
// product (UDOT / PMADDUBSW reduce).  Each kernel is an autovectorizable loop
// folded to one exact integer for a bit-exact original-vs-lifted compare.
//
// x64 uses -mssse3 (SSSE3 PHADD/PMULHRSW/PSHUFB/PMADDUBSW); a64/arm32 use the
// default NEON baseline.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo34RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo34RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo34RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo34RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo34RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo34RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec34TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Signed saturating narrow i32 -> i16 (PACKSSDW / SQXTN).
    {p+"_satnarrow",
     t+" "+p+"_satnarrow("+t+" a) {\n"
     "  int x[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++) x[i]=(int)(a*(i+1)) ^ (i*0x2221);\n"
     "  for (int i=0;i<32;i++){ int v=x[i]; if(v>32767)v=32767; if(v<-32768)v=-32768; acc=acc*131u+(unsigned)(v&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo34", opt, fl},

    // Pairwise horizontal add of adjacent int lanes (PHADDD / ADDP).
    {p+"_pairadd",
     t+" "+p+"_pairadd("+t+" a) {\n"
     "  int x[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++) x[i]=(int)(a*(i+1)) - (i*7);\n"
     "  for (int i=0;i<64;i+=2){ int pp=x[i]+x[i+1]; acc=acc*131u+(unsigned)pp; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo34", opt, fl},

    // Rounding multiply-high i16: (x*y + 0x4000) >> 15 (PMULHRSW / SQRDMULH).
    {p+"_mulhrs",
     t+" "+p+"_mulhrs("+t+" a) {\n"
     "  short x[64], y[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(short)((a*(i+1))>>3); y[i]=(short)((a*(i+5))>>4); }\n"
     "  for (int i=0;i<64;i++){ int pp=((int)x[i]*(int)y[i]+0x4000)>>15; acc=acc*131u+(unsigned)(pp&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo34", opt, fl},

    // Shift-right narrow u32 -> u16 (VSHRN / PSRLD + pack).
    {p+"_shrn",
     t+" "+p+"_shrn("+t+" a) {\n"
     "  unsigned x[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++) x[i]=(unsigned)(a*(i+1));\n"
     "  for (int i=0;i<32;i++){ unsigned short n=(unsigned short)(x[i]>>9); acc=acc*131u+n; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo34", opt, fl},

    // Per-byte population count reduction (CNT / scalarized popcount).
    {p+"_cnt8",
     t+" "+p+"_cnt8("+t+" a) {\n"
     "  unsigned char buf[64]; unsigned acc=0;\n"
     "  for (int i=0;i<64;i++) buf[i]=(unsigned char)((a*(i+1))>>2);\n"
     "  for (int i=0;i<64;i++) acc += (unsigned)__builtin_popcount(buf[i]);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo34", opt, fl},

    // Per-element byte swap (REV32 / PSHUFB constant shuffle).
    {p+"_revbytes",
     t+" "+p+"_revbytes("+t+" a) {\n"
     "  unsigned x[32]; unsigned acc=0;\n"
     "  for (int i=0;i<32;i++) x[i]=(unsigned)(a*(i+1)) ^ (i*0x1357);\n"
     "  for (int i=0;i<32;i++){ unsigned r=__builtin_bswap32(x[i]); acc=acc*131u+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo34", opt, fl},

    // Unsigned min reduction, then deviation reduction (UMINV / PMINUD tree).
    {p+"_minred",
     t+" "+p+"_minred("+t+" a) {\n"
     "  unsigned x[64];\n"
     "  for (int i=0;i<64;i++) x[i]=(unsigned)(a*(i+1)) ^ (i*0x99);\n"
     "  unsigned mn=x[0]; for(int i=1;i<64;i++) if(x[i]<mn) mn=x[i];\n"
     "  unsigned acc=mn; for(int i=0;i<64;i++) acc=acc*131u+(x[i]-mn);\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo34", opt, fl},

    // Unsigned byte dot product (UDOT / PMADDUBSW reduce).
    {p+"_dotu8",
     t+" "+p+"_dotu8("+t+" a) {\n"
     "  unsigned char x[64], y[64]; unsigned sum=0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned char)((a*(i+1))>>1); y[i]=(unsigned char)((a*(i+7))>>2); }\n"
     "  for (int i=0;i<64;i++) sum += (unsigned)x[i]*(unsigned)y[i];\n"
     "  return ("+t+")(sum*2654435761u);\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo34", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec34 =
    makeVec34TC("x64v34", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec34 =
    makeVec34TC("a64v34", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec34 =
    makeVec34TC("armv34", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo34, X64VectorAlgo34RT,
                         ::testing::ValuesIn(kX64Vec34), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo34, A64VectorAlgo34RT,
                         ::testing::ValuesIn(kA64Vec34), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo34, ARM32VectorAlgo34RT,
                         ::testing::ValuesIn(kARM32Vec34), rtTCName);
