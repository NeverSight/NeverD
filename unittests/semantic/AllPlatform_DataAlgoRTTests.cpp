//===- AllPlatform_DataAlgoRTTests.cpp - codec/checksum/transpose -*- C++-*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// clang -O2 algorithm-level "high-yield probing", third batch: checksum / codec
// / data-movement kernels over LOCAL STACK arrays (no rodata lookup tables, no
// runtime division — constant moduli only).  Distinct lowerings vs the prior
// batches: 2D index math, in-place compare/shift, run-length control flow,
// prefix accumulation, and zigzag delta coding.
//   * adler   - Adler-32 checksum (two running sums, constant modulus 65521).
//   * transp  - 8x8 integer matrix transpose (swapped 2D index store/load).
//   * insort  - insertion sort (in-place compare/shift/store inner loop).
//   * rle     - run-length encode into a stack buffer (nested data-dependent
//               run counting).
//   * prefix  - prefix sum + running maximum scan.
//   * delta   - delta + zigzag encoding (signed sub, arithmetic-shift sign map).
//
// Bounded 16/32-bit, deterministic; the harness compares native vs lifted folded
// returns, so any lowering divergence surfaces as a mismatch.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64DataAlgoRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64DataAlgoRT, Verify) { roundTripX64(GetParam()); }

class A64DataAlgoRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64DataAlgoRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32DataAlgoRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32DataAlgoRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeDataTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Adler-32 checksum (constant modulus -> magic multiply, no div libcall).
    {p+"_adler",
     t+" "+p+"_adler("+t+" a){\n"
     "  unsigned char buf[120];\n"
     "  for(int i=0;i<120;i++) buf[i]=(unsigned char)(a*(i+1)+i*7);\n"
     "  unsigned A=1u,B=0u;\n"
     "  for(int i=0;i<120;i++){ A=(A+buf[i])%65521u; B=(B+A)%65521u; }\n"
     "  return ("+t+")((B<<16)|A);\n"
     "}\n",
     {0x1234567ULL}, "DataAlgo", 2, ""},

    // 8x8 matrix transpose.
    {p+"_transp",
     t+" "+p+"_transp("+t+" a){\n"
     "  int m[64], tr[64];\n"
     "  for(int i=0;i<64;i++) m[i]=(int)((a*(i+1)+i*13)&0xFFFF);\n"
     "  for(int i=0;i<8;i++) for(int j=0;j<8;j++) tr[j*8+i]=m[i*8+j];\n"
     "  unsigned acc=0; for(int i=0;i<64;i++) acc=acc*131u+(unsigned)tr[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "DataAlgo", 2, ""},

    // Insertion sort.
    {p+"_insort",
     t+" "+p+"_insort("+t+" a){\n"
     "  int v[32];\n"
     "  for(int i=0;i<32;i++) v[i]=(int)(((unsigned)(a*7u+i*131u))%1000u)-500;\n"
     "  for(int i=1;i<32;i++){ int x=v[i],j=i-1;\n"
     "    while(j>=0 && v[j]>x){ v[j+1]=v[j]; j--; } v[j+1]=x; }\n"
     "  unsigned acc=0; for(int i=0;i<32;i++) acc=acc*131u+(unsigned)v[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "DataAlgo", 2, ""},

    // Run-length encode into a stack buffer.
    {p+"_rle",
     t+" "+p+"_rle("+t+" a){\n"
     "  unsigned char in[64];\n"
     "  for(int i=0;i<64;i++) in[i]=(unsigned char)(((unsigned)(a*(i/4+1)))&3u);\n"
     "  unsigned char out[128]; int n=0, i=0;\n"
     "  while(i<64){ unsigned char c=in[i]; int run=1;\n"
     "    while(i+run<64 && in[i+run]==c && run<255) run++;\n"
     "    out[n++]=(unsigned char)run; out[n++]=c; i+=run; }\n"
     "  unsigned acc=(unsigned)n; for(int k=0;k<n;k++) acc=acc*131u+out[k];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "DataAlgo", 2, ""},

    // Prefix sum + running maximum.
    {p+"_prefix",
     t+" "+p+"_prefix("+t+" a){\n"
     "  int v[64], pf[64];\n"
     "  for(int i=0;i<64;i++) v[i]=(int)((a*(i+1)+i*5)&0xFF)-128;\n"
     "  int s=0, mx=-1000000;\n"
     "  for(int i=0;i<64;i++){ s+=v[i]; pf[i]=s; if(s>mx) mx=s; }\n"
     "  unsigned acc=(unsigned)mx; for(int i=0;i<64;i++) acc=acc*131u+(unsigned)pf[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "DataAlgo", 2, ""},

    // Delta + zigzag encoding.
    {p+"_delta",
     t+" "+p+"_delta("+t+" a){\n"
     "  int v[48];\n"
     "  for(int i=0;i<48;i++) v[i]=(int)((a*(i+1)+i*9)&0xFFFF)-32768;\n"
     "  unsigned acc=0; int prev=0;\n"
     "  for(int i=0;i<48;i++){ int d=v[i]-prev; prev=v[i];\n"
     "    unsigned zz=((unsigned)d<<1)^((unsigned)(d>>31)); acc=acc*131u+zz; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "DataAlgo", 2, ""},
  };
}

static const std::vector<RoundTripTC> kX64D   = makeDataTC("x64d", "long");
static const std::vector<RoundTripTC> kA64D   = makeDataTC("a64d", "long");
static const std::vector<RoundTripTC> kARM32D = makeDataTC("armd", "int");
// clang-format on

INSTANTIATE_TEST_SUITE_P(DataAlgo, X64DataAlgoRT,
                         ::testing::ValuesIn(kX64D), rtTCName);
INSTANTIATE_TEST_SUITE_P(DataAlgo, A64DataAlgoRT,
                         ::testing::ValuesIn(kA64D), rtTCName);
INSTANTIATE_TEST_SUITE_P(DataAlgo, ARM32DataAlgoRT,
                         ::testing::ValuesIn(kARM32D), rtTCName);
