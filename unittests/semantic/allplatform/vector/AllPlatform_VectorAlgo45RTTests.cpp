//===- AllPlatform_VectorAlgo45RTTests.cpp - bitmask-gather SIMD probes ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Forty-fifth batch of clang -O2 vector probes, focused on the compare→bitmask
// gather / bit-scatter idioms (the PMOVMSKB family) that lower to the most
// constant-pool-dense NEON sequences — bit-weight vectors, zip/shuffle index
// tables, narrowing chains.  On ARM32 these constant pools are embedded in the
// EXECUTABLE `.text` past the code; #531 rebuilt them as ONE GEP'd global
// (previously one overlapping `[addr, end]` copy per `vldr`/`adr`, O(N)).  This
// batch hardens that path across widths (byte / halfword), conditions (sign /
// zero / threshold / bit-pick / two-array compare) and gather vs count folds.
//
// Each kernel is an autovectorizable loop folded to one exact integer for a
// bit-exact original-vs-lifted compare.  x64 uses -mssse3; a64/arm32 use the
// default NEON baseline.  Integer in / integer out, no libcall on any target.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo45RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo45RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo45RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo45RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo45RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo45RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec45TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Positive-sign bitmask gather (CMGT #0 + per-lane bit weights).
    {p+"_mskpos",
     t+" "+p+"_mskpos("+t+" a) {\n"
     "  signed char v[128]; unsigned acc=0;\n"
     "  for (int i=0;i<128;i++) v[i]=(signed char)((a*(i+1))>>1);\n"
     "  for (int b=0;b<4;b++){ unsigned m=0; for(int i=0;i<32;i++) if(v[b*32+i]>0) m|=(1u<<i); acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x13579BDULL}, "VectorAlgo45", opt, fl},

    // 16-bit sign-bit gather (halfword lanes, 16 per block over 8 blocks).
    {p+"_msk16",
     t+" "+p+"_msk16("+t+" a) {\n"
     "  short v[128]; unsigned acc=0;\n"
     "  for (int i=0;i<128;i++) v[i]=(short)((a*(i+3))>>2);\n"
     "  for (int b=0;b<8;b++){ unsigned m=0; for(int i=0;i<16;i++) if(v[b*16+i]<0) m|=(1u<<i); acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2468ACEULL}, "VectorAlgo45", opt, fl},

    // Pick-bit gather: collect bit 2 of each byte into a mask.
    {p+"_mskbit2",
     t+" "+p+"_mskbit2("+t+" a) {\n"
     "  unsigned char v[128]; unsigned acc=0;\n"
     "  for (int i=0;i<128;i++) v[i]=(unsigned char)((a*(i+1))>>2);\n"
     "  for (int b=0;b<4;b++){ unsigned m=0; for(int i=0;i<32;i++) if((v[b*32+i]>>2)&1u) m|=(1u<<i); acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3579BDFULL}, "VectorAlgo45", opt, fl},

    // Per-block negative count (compare-to-mask folded to a popcount, not gather).
    {p+"_cntneg",
     t+" "+p+"_cntneg("+t+" a) {\n"
     "  signed char v[256]; unsigned acc=0;\n"
     "  for (int i=0;i<256;i++) v[i]=(signed char)((a*(i+5))>>1);\n"
     "  for (int b=0;b<8;b++){ unsigned c=0; for(int i=0;i<32;i++) if(v[b*32+i]<0) c++; acc=acc*131u+c; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4680ACEULL}, "VectorAlgo45", opt, fl},

    // Greater-than-constant-threshold bitmask gather (CMGT #imm).
    {p+"_mskgt",
     t+" "+p+"_mskgt("+t+" a) {\n"
     "  signed char v[128]; unsigned acc=0;\n"
     "  for (int i=0;i<128;i++) v[i]=(signed char)((a*(i+2))>>1);\n"
     "  for (int b=0;b<4;b++){ unsigned m=0; for(int i=0;i<32;i++) if(v[b*32+i]>10) m|=(1u<<i); acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x579BDF1ULL}, "VectorAlgo45", opt, fl},

    // Two-array per-lane compare-greater bitmask gather.
    {p+"_mskcmp",
     t+" "+p+"_mskcmp("+t+" a) {\n"
     "  signed char x[128], y[128]; unsigned acc=0;\n"
     "  for (int i=0;i<128;i++){ x[i]=(signed char)((a*(i+1))>>1); y[i]=(signed char)((a*(i+7))>>2); }\n"
     "  for (int b=0;b<4;b++){ unsigned m=0; for(int i=0;i<32;i++) if(x[b*32+i]>y[b*32+i]) m|=(1u<<i); acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x68ACE02ULL}, "VectorAlgo45", opt, fl},

    // Unsigned top-bit gather (>=0x80) — sign collection via the unsigned domain.
    {p+"_msktop",
     t+" "+p+"_msktop("+t+" a) {\n"
     "  unsigned char v[128]; unsigned acc=0;\n"
     "  for (int i=0;i<128;i++) v[i]=(unsigned char)((a*(i+1))>>1);\n"
     "  for (int b=0;b<4;b++){ unsigned m=0; for(int i=0;i<32;i++) if(v[b*32+i]>=0x80u) m|=(1u<<i); acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x79BDF13ULL}, "VectorAlgo45", opt, fl},

    // Low-nibble equals constant bitmask gather (CMEQ after AND #0xF).
    {p+"_mskeq7",
     t+" "+p+"_mskeq7("+t+" a) {\n"
     "  unsigned char v[128]; unsigned acc=0;\n"
     "  for (int i=0;i<128;i++) v[i]=(unsigned char)((a*(i+1))>>1);\n"
     "  for (int b=0;b<4;b++){ unsigned m=0; for(int i=0;i<32;i++) if((v[b*32+i]&0xFu)==7u) m|=(1u<<i); acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x8ACE024ULL}, "VectorAlgo45", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec45 =
    makeVec45TC("x64v45", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec45 =
    makeVec45TC("a64v45", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec45 =
    makeVec45TC("armv45", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo45, X64VectorAlgo45RT,
                         ::testing::ValuesIn(kX64Vec45), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo45, A64VectorAlgo45RT,
                         ::testing::ValuesIn(kA64Vec45), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo45, ARM32VectorAlgo45RT,
                         ::testing::ValuesIn(kARM32Vec45), rtTCName);
