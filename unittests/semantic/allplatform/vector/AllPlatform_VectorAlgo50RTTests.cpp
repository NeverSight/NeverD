//===- AllPlatform_VectorAlgo50RTTests.cpp - SAD block-match / fused -O3 --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Fiftieth batch of clang vector probes, escalating VectorAlgo49's -O2 128-bit
// SAD to -O3 and to real-world / fused shapes where a vector horizontal
// reduction feeds (or shares registers with) a second reduction or a data-
// dependent decision — exactly the boundary that recently broke (#532 bug②:
// vectorized body stalling jump-table base folding; #532 bug①: ARM32 NEON
// mixed-half wide-read tracking):
//   * _sadb3   : -O3 256-element byte SAD (heavier unrolling than v49's -O2).
//   * _dotsad  : a PSADBW byte SAD and a PMADDWD-style widening dot product in
//                one function — two distinct vector reductions under register
//                pressure (sad in u32, dot in an i64 accumulator).
//   * _sadmin  : motion-estimation block matching — inner 16-byte PSADBW SAD,
//                outer scalar min-with-index search (vector reduction feeding a
//                data-dependent argmin branch).
//   * _sadw3   : two interleaved 16-bit abs-diff streams (SABDL / VABAL path).
//
// Each kernel folds to one exact integer for a bit-exact compare; u32 SAD
// accumulators and i64 dot accumulator with constant-only i64 shifts keep
// ARM32 libcall-free (no i64 divide / variable i64 shift).  x64 uses -mssse3;
// a64/arm32 use the default NEON baseline.  Three targets (i386 skipped).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo50RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo50RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo50RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo50RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo50RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo50RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec50TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // -O3 256-element byte SAD: heavier unrolling / wider reduction tree than the
    // -O2 128-element v49 form.
    {p+"_sadb3",
     t+" "+p+"_sadb3("+t+" a){\n"
     "  unsigned char x[256], y[256]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<256;i++){ s=s*1664525u+1013904223u; x[i]=(unsigned char)(s>>16);\n"
     "    s=s*1664525u+1013904223u; y[i]=(unsigned char)(s>>16); }\n"
     "  unsigned sum=0;\n"
     "  for(int i=0;i<256;i++){ int d=(int)x[i]-(int)y[i]; sum += (unsigned)(d<0?-d:d); }\n"
     "  return ("+t+")sum;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo50", opt, fl},

    // Byte SAD (PSADBW) and a widening dot product (PMADDWD-style) in one body:
    // two vector reductions, one u32 + one i64 accumulator, under reg pressure.
    {p+"_dotsad",
     t+" "+p+"_dotsad("+t+" a){\n"
     "  unsigned char p[128], q[128]; short x[128], y[128];\n"
     "  unsigned s=(unsigned)a^0x9e3779b9u;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u; p[i]=(unsigned char)(s>>16);\n"
     "    q[i]=(unsigned char)(s>>8); x[i]=(short)(s>>3); y[i]=(short)((s>>11)^(s<<1)); }\n"
     "  unsigned sad=0;\n"
     "  for(int i=0;i<128;i++){ int d=(int)p[i]-(int)q[i]; sad += (unsigned)(d<0?-d:d); }\n"
     "  long long dot=0;\n"
     "  for(int i=0;i<128;i++) dot += (long long)((int)x[i]*(int)y[i]);\n"
     "  return ("+t+")((long long)sad*131 + (dot ^ (dot>>32)));\n"
     "}\n",
     {0x2345678ULL}, "VectorAlgo50", opt, fl},

    // Motion-estimation block matching: inner 16-byte PSADBW SAD, outer argmin
    // search — a vector reduction feeding a data-dependent min-with-index branch.
    {p+"_sadmin",
     t+" "+p+"_sadmin("+t+" a){\n"
     "  unsigned char buf[160], ref[16]; unsigned s=(unsigned)a|3u;\n"
     "  for(int i=0;i<160;i++){ s=s*1664525u+1013904223u; buf[i]=(unsigned char)(s>>16); }\n"
     "  for(int i=0;i<16;i++){ s=s*1664525u+1013904223u; ref[i]=(unsigned char)(s>>16); }\n"
     "  unsigned best=0xffffffffu; int bestpos=0;\n"
     "  for(int pos=0;pos<=160-16;pos++){ unsigned sad=0;\n"
     "    for(int i=0;i<16;i++){ int d=(int)buf[pos+i]-(int)ref[i]; sad += (unsigned)(d<0?-d:d); }\n"
     "    if(sad<best){ best=sad; bestpos=pos; } }\n"
     "  return ("+t+")(best*1000u + (unsigned)bestpos);\n"
     "}\n",
     {0x3456789ULL}, "VectorAlgo50", opt, fl},

    // Two interleaved 16-bit abs-diff streams (no PSADBW for words → SABDL /
    // VABAL widening accumulate), both folded into the result.
    {p+"_sadw3",
     t+" "+p+"_sadw3("+t+" a){\n"
     "  unsigned short x[128], y[128], z[128]; unsigned s=(unsigned)a+0x55u;\n"
     "  for(int i=0;i<128;i++){ s=s*22695477u+1u; x[i]=(unsigned short)(s>>8);\n"
     "    s=s*22695477u+1u; y[i]=(unsigned short)(s>>8);\n"
     "    s=s*22695477u+1u; z[i]=(unsigned short)(s>>8); }\n"
     "  unsigned s1=0, s2=0;\n"
     "  for(int i=0;i<128;i++){ int d1=(int)x[i]-(int)y[i]; s1 += (unsigned)(d1<0?-d1:d1);\n"
     "    int d2=(int)y[i]-(int)z[i]; s2 += (unsigned)(d2<0?-d2:d2); }\n"
     "  return ("+t+")(s1*131u + s2);\n"
     "}\n",
     {0x456789AULL}, "VectorAlgo50", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec50 =
    makeVec50TC("x64v50", "long", 3, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec50 =
    makeVec50TC("a64v50", "long", 3, "");
static const std::vector<RoundTripTC> kARM32Vec50 =
    makeVec50TC("armv50", "int", 3, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo50, X64VectorAlgo50RT,
                         ::testing::ValuesIn(kX64Vec50), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo50, A64VectorAlgo50RT,
                         ::testing::ValuesIn(kA64Vec50), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo50, ARM32VectorAlgo50RT,
                         ::testing::ValuesIn(kARM32Vec50), rtTCName);
