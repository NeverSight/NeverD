//===- AllPlatform_VectorAlgo7RTTests.cpp - more vectorizable algos -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Seventh batch of realistic clang -O2 algorithms used as high-yield lift bug
// probes.  Targets patterns the earlier six batches did not: inclusive prefix
// sum (sequential scan), argmax (max value + index tracking), 3-tap Laplacian
// FIR (overlapping window MAC), 8-bucket histogram (indexed scatter increment),
// Horner polynomial evaluation (loop-carried mul-add), 3-element median filter
// (min/max combinations on overlapping windows), inclusive prefix max
// (loop-carried max), and stride-2 max pooling (deinterleave + max).  Each runs
// the full binary -> lift -> MedIR -> LLVM IR -> obj -> binary roundtrip and
// compares Unicorn execution of the original vs recompiled code.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo7RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo7RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo7RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo7RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo7RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo7RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec7TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Inclusive prefix sum (sequential scan), then fold -> loop-carried acc.
    {p+"_prefix_sum",
     t+" "+p+"_prefix_sum("+t+" a) {\n"
     "  int v[64]; int pre[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+1))^(i*13);\n"
     "  int acc=0; for (int i=0;i<64;i++){ acc+=v[i]; pre[i]=acc; }\n"
     "  for (int i=0;i<64;i++) s ^= pre[i] + i;\n"
     "  return s;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo7", opt, fl},

    // Argmax: track running max value and its index -> two-var conditional update.
    {p+"_argmax",
     t+" "+p+"_argmax("+t+" a) {\n"
     "  int v[64]; int best=-2147483647-1; int bi=0;\n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+3))^(i*29);\n"
     "  for (int i=0;i<64;i++) if (v[i]>best){ best=v[i]; bi=i; }\n"
     "  return best + bi*1315423911;\n"
     "}\n",
     {0xCAFE99ULL}, "VectorAlgo7", opt, fl},

    // 3-tap Laplacian FIR {1,-2,1} over overlapping windows -> vext + MAC.
    {p+"_conv3w",
     t+" "+p+"_conv3w("+t+" a) {\n"
     "  int v[66]; int s = 0;\n"
     "  for (int i=0;i<66;i++) v[i]=(int)(a*(i+1))^(i*7);\n"
     "  for (int i=1;i<65;i++) s += v[i-1] - 2*v[i] + v[i+1];\n"
     "  return s;\n"
     "}\n",
     {0x9E3779ULL}, "VectorAlgo7", opt, fl},

    // 8-bucket histogram via indexed increment -> scatter to stack array.
    {p+"_histo8",
     t+" "+p+"_histo8("+t+" a) {\n"
     "  int h[8] = {0,0,0,0,0,0,0,0};\n"
     "  for (int i=0;i<128;i++){ int x=(int)(a*(i+1))^(i*11); h[x & 7]++; }\n"
     "  int s = 0; for (int i=0;i<8;i++) s += h[i]*(i+1);\n"
     "  return s;\n"
     "}\n",
     {0x33AA55ULL}, "VectorAlgo7", opt, fl},

    // Horner polynomial evaluation -> loop-carried multiply-accumulate.
    {p+"_horner",
     t+" "+p+"_horner("+t+" a) {\n"
     "  int c[16]; \n"
     "  for (int i=0;i<16;i++) c[i]=(int)(a*(i+1))^(i*5);\n"
     "  int x = (int)a & 0xF;\n"
     "  int acc = 0; for (int i=0;i<16;i++) acc = acc*x + c[i];\n"
     "  return acc;\n"
     "}\n",
     {0x778811ULL}, "VectorAlgo7", opt, fl},

    // 3-element median filter (median of x,y,z) summed -> min/max combos.
    {p+"_median3",
     t+" "+p+"_median3("+t+" a) {\n"
     "  int v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+1))^(i*23);\n"
     "  for (int i=1;i<63;i++){ int x=v[i-1],y=v[i],z=v[i+1];\n"
     "    int mn = x<y?x:y; int mx = x>y?x:y;\n"
     "    int med = z<mn ? mn : (z>mx ? mx : z); s += med; }\n"
     "  return s;\n"
     "}\n",
     {0x246813ULL}, "VectorAlgo7", opt, fl},

    // Inclusive prefix max (loop-carried running max), summed.
    {p+"_scan_max",
     t+" "+p+"_scan_max("+t+" a) {\n"
     "  int v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+1))^(i*0x5bd1e995);\n"
     "  int m=-2147483647-1; for (int i=0;i<64;i++){ if(v[i]>m)m=v[i]; s+=m; }\n"
     "  return s;\n"
     "}\n",
     {0x5A5A33ULL}, "VectorAlgo7", opt, fl},

    // Stride-2 max pooling (deinterleave even/odd, take max), summed.
    {p+"_pool2",
     t+" "+p+"_pool2("+t+" a) {\n"
     "  int v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+1))^(i*0x9E3779B1);\n"
     "  for (int i=0;i<32;i++){ int x=v[2*i],y=v[2*i+1]; s += x>y?x:y; }\n"
     "  return s;\n"
     "}\n",
     {0x42AA42ULL}, "VectorAlgo7", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec7 =
    makeVec7TC("x64v7", "long", 2, "-msse4.2");
static const std::vector<RoundTripTC> kA64Vec7 = makeVec7TC("a64v7", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec7 = makeVec7TC("armv7v", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo7, X64VectorAlgo7RT,
                         ::testing::ValuesIn(kX64Vec7), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo7, A64VectorAlgo7RT,
                         ::testing::ValuesIn(kA64Vec7), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo7, ARM32VectorAlgo7RT,
                         ::testing::ValuesIn(kARM32Vec7), rtTCName);
