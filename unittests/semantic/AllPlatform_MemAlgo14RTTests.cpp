//===- AllPlatform_MemAlgo14RTTests.cpp - memory/array algos ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Fourteenth batch of clang -O2 algorithm probes.  Targets MEMORY-heavy
// algorithms over local stack arrays: in-place sorts, 2D (row-major) matrix
// indexing, run-length encoding, longest-increasing-subsequence, 3x3 image
// convolution, and a histogram/mode scan.  These stress paths the earlier
// (mostly element-wise vector) probes do not: stack-frame layout, mixed-width
// loads/stores to derived pointers, indexed addressing with a multiply, and
// the optimizer's memory alias analysis / frame-relative pointer handling.
//
// Arrays are initialised with an arg-derived expression (never a constant fill)
// so clang cannot lower the init to a memset/memcpy library call.  Each
// function folds to an exact integer return value; arithmetic is the parameter
// type (long on x86_64/AArch64, int on ARM32 cortex-a15 with hw divide).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64MemAlgo14RT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64MemAlgo14RT, Verify) { roundTripX64(GetParam()); }

class A64MemAlgo14RT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64MemAlgo14RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32MemAlgo14RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32MemAlgo14RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeMem14TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Insertion sort of a 24-element array, then index-weighted checksum.
    {p+"_insort",
     t+" "+p+"_insort("+t+" a) {\n"
     "  "+t+" v[24];\n"
     "  for (int i=0;i<24;i++) v[i]=(("+t+")(a*7+i*131)^(i*0x9E37u))&0xFFFF;\n"
     "  for (int i=1;i<24;i++){ "+t+" key=v[i]; int j=i-1;\n"
     "    while (j>=0 && v[j]>key){ v[j+1]=v[j]; j--; } v[j+1]=key; }\n"
     "  "+t+" s=0; for (int i=0;i<24;i++) s+=v[i]*(i+1);\n"
     "  return s;\n"
     "}\n",
     {0x1234567ULL}, "MemAlgo14", opt, fl},

    // Lomuto partition around the last element; return pivot pos and checksum.
    {p+"_qpart",
     t+" "+p+"_qpart("+t+" a) {\n"
     "  "+t+" v[24];\n"
     "  for (int i=0;i<24;i++) v[i]=(("+t+")(a+i*97)^(i*1315423911u))&0x7FFF;\n"
     "  "+t+" pivot=v[23]; int i=-1;\n"
     "  for (int j=0;j<23;j++){ if (v[j]<=pivot){ i++; "+t+" tmp=v[i]; v[i]=v[j]; v[j]=tmp; } }\n"
     "  i++; "+t+" tmp=v[i]; v[i]=v[23]; v[23]=tmp;\n"
     "  "+t+" s=i*1000; for (int k=0;k<24;k++) s+=v[k]*(k+1);\n"
     "  return s;\n"
     "}\n",
     {0x2233445ULL}, "MemAlgo14", opt, fl},

    // 6x6 row-major transpose, then diagonal/anti-diagonal weighted sum.
    {p+"_transpose",
     t+" "+p+"_transpose("+t+" a) {\n"
     "  "+t+" m[36];\n"
     "  for (int i=0;i<36;i++) m[i]=("+t+")(a*(i+1)+i*i*3);\n"
     "  for (int r=0;r<6;r++) for (int c=r+1;c<6;c++){\n"
     "    "+t+" tmp=m[r*6+c]; m[r*6+c]=m[c*6+r]; m[c*6+r]=tmp; }\n"
     "  "+t+" s=0; for (int r=0;r<6;r++) for (int c=0;c<6;c++) s+=m[r*6+c]*(r-c);\n"
     "  return s;\n"
     "}\n",
     {0x3344556ULL}, "MemAlgo14", opt, fl},

    // Run-length encode a byte stream with runs; return pair count and checksum.
    {p+"_rle",
     t+" "+p+"_rle("+t+" a) {\n"
     "  unsigned char in[48]; unsigned char val[48]; unsigned char run[48];\n"
     "  for (int i=0;i<48;i++) in[i]=(unsigned char)(((unsigned)a+i/3*5)&0x7);\n"
     "  int n=0; int i=0;\n"
     "  while (i<48){ unsigned char c=in[i]; int cnt=0;\n"
     "    while (i<48 && in[i]==c && cnt<255){ cnt++; i++; }\n"
     "    val[n]=c; run[n]=(unsigned char)cnt; n++; }\n"
     "  "+t+" s=n*10000; for (int k=0;k<n;k++) s+=("+t+")val[k]*256+run[k]*(k+1);\n"
     "  return s;\n"
     "}\n",
     {0x4455667ULL}, "MemAlgo14", opt, fl},

    // Longest increasing subsequence length (O(n^2) DP over a stack array).
    {p+"_lis",
     t+" "+p+"_lis("+t+" a) {\n"
     "  "+t+" v[28]; int dp[28];\n"
     "  for (int i=0;i<28;i++){ v[i]=("+t+")((a*13+i*577)%101); dp[i]=1; }\n"
     "  int best=0;\n"
     "  for (int i=0;i<28;i++){ for (int j=0;j<i;j++) if (v[j]<v[i] && dp[j]+1>dp[i]) dp[i]=dp[j]+1;\n"
     "    if (dp[i]>best) best=dp[i]; }\n"
     "  "+t+" s=best*1000; for (int i=0;i<28;i++) s+=dp[i]*(i+1);\n"
     "  return s;\n"
     "}\n",
     {0x5566778ULL}, "MemAlgo14", opt, fl},

    // 3x3 box-blur style convolution over an 8x8 image (interior pixels).
    // img/out are seeded with distinct arg-derived expressions in one loop so
    // clang cannot lower the seeding to a memcpy/memset library call.
    {p+"_conv2d",
     t+" "+p+"_conv2d("+t+" a) {\n"
     "  "+t+" img[64]; "+t+" out[64];\n"
     "  for (int i=0;i<64;i++){ img[i]=("+t+")((a+i*31)&0xFF); out[i]=("+t+")((a+i*17)&0xFF); }\n"
     "  for (int r=1;r<7;r++) for (int c=1;c<7;c++){\n"
     "    "+t+" acc=0;\n"
     "    for (int dr=-1;dr<=1;dr++) for (int dc=-1;dc<=1;dc++)\n"
     "      acc += img[(r+dr)*8+(c+dc)];\n"
     "    out[r*8+c]=acc/9; }\n"
     "  "+t+" s=0; for (int i=0;i<64;i++) s+=out[i]*((i&7)+1);\n"
     "  return s;\n"
     "}\n",
     {0x6677889ULL}, "MemAlgo14", opt, fl},

    // Histogram into 16 buckets, then locate the modal bucket (scatter + scan).
    {p+"_histmode",
     t+" "+p+"_histmode("+t+" a) {\n"
     "  int h[16]; for (int i=0;i<16;i++) h[i]=0;\n"
     "  for (int i=0;i<120;i++){ unsigned x=((unsigned)a*2654435761u + i*0x85EBu)>>12; h[x&15]++; }\n"
     "  int mode=0,cnt=-1; for (int i=0;i<16;i++) if (h[i]>cnt){ cnt=h[i]; mode=i; }\n"
     "  "+t+" s=mode*100000+cnt*1000; for (int i=0;i<16;i++) s+=h[i]*(i+1);\n"
     "  return s;\n"
     "}\n",
     {0x778899AULL}, "MemAlgo14", opt, fl},

    // In-place left rotation by k via the three-reversal trick.
    {p+"_revrot",
     t+" "+p+"_revrot("+t+" a) {\n"
     "  "+t+" v[30]; for (int i=0;i<30;i++) v[i]=("+t+")(a^(i*2246822519u));\n"
     "  int k=(int)(((unsigned)a)%30u);\n"
     "  int lo=0,hi=k-1; while (lo<hi){ "+t+" t2=v[lo]; v[lo]=v[hi]; v[hi]=t2; lo++; hi--; }\n"
     "  lo=k; hi=29; while (lo<hi){ "+t+" t2=v[lo]; v[lo]=v[hi]; v[hi]=t2; lo++; hi--; }\n"
     "  lo=0; hi=29; while (lo<hi){ "+t+" t2=v[lo]; v[lo]=v[hi]; v[hi]=t2; lo++; hi--; }\n"
     "  "+t+" s=0; for (int i=0;i<30;i++) s+=v[i]*(i+1);\n"
     "  return s;\n"
     "}\n",
     {0x88990ABULL}, "MemAlgo14", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Mem14 =
    makeMem14TC("x64m14", "long", 2, "");
static const std::vector<RoundTripTC> kA64Mem14 =
    makeMem14TC("a64m14", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Mem14 =
    makeMem14TC("armm14", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(MemAlgo14, X64MemAlgo14RT,
                         ::testing::ValuesIn(kX64Mem14), rtTCName);
INSTANTIATE_TEST_SUITE_P(MemAlgo14, A64MemAlgo14RT,
                         ::testing::ValuesIn(kA64Mem14), rtTCName);
INSTANTIATE_TEST_SUITE_P(MemAlgo14, ARM32MemAlgo14RT,
                         ::testing::ValuesIn(kARM32Mem14), rtTCName);
