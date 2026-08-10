//===- AllPlatform_VectorAlgo30RTTests.cpp - vectorized search ---*- C++-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Thirtieth batch of clang -O2 algorithm probes, aimed squarely at the
// vectorized *index / search* shape that surfaced the #431 memcmp miscompile
// (clang turns a full-array scan into a per-block `pcmpeqb`/`pmovmskb` reduction
// that tracks a block base plus an in-block offset, then recombines them into a
// register that is later read wide — the exact spot the cross-block partial-
// write merge had to get right).  argmax / argmin / reverse-memcmp / first-match
// all share that "block base + in-block lane index, recombined and returned"
// structure, so they re-exercise the same lift machinery from new angles.
//
// Every algorithm folds to an exact integer for bit-exact original-vs-lifted
// comparison.  x64 uses -msse4.2 (matching the memcmp probe); a64/arm32 use the
// default NEON.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo30RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo30RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo30RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo30RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo30RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo30RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec30TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Index of the maximum element (full scan, block-base + in-block argmax).
    {p+"_argmax",
     t+" "+p+"_argmax("+t+" a) {\n"
     "  int v[128]; \n"
     "  for (int i=0;i<128;i++) v[i]=(int)(a*(i+1)) ^ (i*0x33AA55);\n"
     "  int best=v[0], bi=0;\n"
     "  for (int i=1;i<128;i++) if (v[i]>best){ best=v[i]; bi=i; }\n"
     "  return bi;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo30", opt, fl},

    // Index of the minimum element (mirror of argmax, different compare polarity).
    {p+"_argmin",
     t+" "+p+"_argmin("+t+" a) {\n"
     "  int v[128]; \n"
     "  for (int i=0;i<128;i++) v[i]=(int)(a*(i+1)) ^ (i*0x55CC77);\n"
     "  int best=v[0], bi=0;\n"
     "  for (int i=1;i<128;i++) if (v[i]<best){ best=v[i]; bi=i; }\n"
     "  return bi;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo30", opt, fl},

    // Index of the max-absolute-value element (abs then argmax).
    {p+"_maxidxabs",
     t+" "+p+"_maxidxabs("+t+" a) {\n"
     "  int v[128]; \n"
     "  for (int i=0;i<128;i++) v[i]=(int)(a*(i+1)) - (i*0x4321);\n"
     "  int best=-1, bi=0;\n"
     "  for (int i=0;i<128;i++){ int x=v[i]; if(x<0)x=-x; if (x>best){ best=x; bi=i; } }\n"
     "  return bi;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo30", opt, fl},

    // Reverse memcmp: LAST index where two arrays differ (full scan, no break).
    {p+"_lastne",
     t+" "+p+"_lastne("+t+" a) {\n"
     "  unsigned char x[128], y[128]; \n"
     "  for (int i=0;i<128;i++){ x[i]=(unsigned char)(a*(i+1)); y[i]=x[i]; }\n"
     "  y[40]=(unsigned char)(x[40]+2); y[97]=(unsigned char)(x[97]+1);\n"
     "  int idx=-1;\n"
     "  for (int i=0;i<128;i++) if (x[i]!=y[i]) idx=i;\n"
     "  return idx;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo30", opt, fl},

    // First element strictly greater than a threshold (early-exit scan, memchr-ish).
    {p+"_firstgt",
     t+" "+p+"_firstgt("+t+" a) {\n"
     "  int v[128]; \n"
     "  for (int i=0;i<128;i++) v[i]=(int)(a*(i+1)) & 0x3FFFFF;\n"
     "  int idx=128;\n"
     "  for (int i=0;i<128;i++) if (v[i]>0x300000){ idx=i; break; }\n"
     "  return idx;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo30", opt, fl},

    // Count elements above a threshold (compare + popcount reduction).
    {p+"_counttop",
     t+" "+p+"_counttop("+t+" a) {\n"
     "  unsigned v[128]; int s=0;\n"
     "  for (int i=0;i<128;i++) v[i]=(unsigned)(a*(i+1)) ^ (unsigned)(i*0x9E3779B1u);\n"
     "  for (int i=0;i<128;i++) if (v[i]>0x80000000u) s++;\n"
     "  return s;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo30", opt, fl},

    // Max element minus min element, with the index of the max folded in.
    {p+"_spanidx",
     t+" "+p+"_spanidx("+t+" a) {\n"
     "  int v[96]; \n"
     "  for (int i=0;i<96;i++) v[i]=(int)(a*(i+1)) ^ (i*0x271828);\n"
     "  int mx=v[0], mn=v[0], bi=0;\n"
     "  for (int i=1;i<96;i++){ if(v[i]>mx){mx=v[i];bi=i;} if(v[i]<mn)mn=v[i]; }\n"
     "  return (mx-mn) + bi*131;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo30", opt, fl},

    // Longest run of equal adjacent bytes (loop-carried run length + max reduce).
    {p+"_runmax",
     t+" "+p+"_runmax("+t+" a) {\n"
     "  unsigned char b[160]; \n"
     "  for (int i=0;i<160;i++) b[i]=(unsigned char)((a*(i+1))>>5);\n"
     "  int run=1, best=1;\n"
     "  for (int i=1;i<160;i++){ if(b[i]==b[i-1]) run++; else run=1; if(run>best)best=run; }\n"
     "  return best;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo30", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec30 =
    makeVec30TC("x64v30", "long", 2, "-msse4.2");
static const std::vector<RoundTripTC> kA64Vec30 =
    makeVec30TC("a64v30", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec30 =
    makeVec30TC("armv30", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo30, X64VectorAlgo30RT,
                         ::testing::ValuesIn(kX64Vec30), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo30, A64VectorAlgo30RT,
                         ::testing::ValuesIn(kA64Vec30), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo30, ARM32VectorAlgo30RT,
                         ::testing::ValuesIn(kARM32Vec30), rtTCName);
