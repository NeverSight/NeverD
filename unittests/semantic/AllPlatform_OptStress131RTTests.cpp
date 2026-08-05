//===- AllPlatform_OptStress131RTTests.cpp - Floyd / matmul / counting-sort =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * floyd  - Floyd-Warshall all-pairs shortest path (min-plus closure) over a
//              6x6 rodata adjacency matrix.  Pins the `d[i][j]=min(d[i][j],
//              d[i][k]+d[k][j])` triple loop (a min-plus matrix closure, distinct
//              from any additive DP or comparison sort).
//   * matmul - dense integer matrix multiply (mod 2^16) of two 6x6 rodata
//              matrices.  Pins the classic GEMM `sum+=A[i][k]*B[k][j]` multiply-
//              accumulate triple loop over two distinct rodata tables.
//   * csort  - counting sort of rodata values into stack buckets: histogram,
//              prefix-sum the counts, then stable scatter back.  Pins a
//              histogram+prefix+scatter (distinct from the compare-exchange
//              networks of the bitonic / odd-even sorts).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress131RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress131RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress131RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress131RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress131RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress131RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress131RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress131RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress131TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Floyd-Warshall min-plus closure over a 6x6 rodata adjacency matrix.
    {p+"_floyd",
     "static const unsigned char "+p+"_adj[36]={\n"
     "  0, 7,99, 3,99,12,\n"
     "  7, 0, 4,99, 8,99,\n"
     " 99, 4, 0, 6,99, 5,\n"
     "  3,99, 6, 0, 9,99,\n"
     " 99, 8,99, 9, 0, 2,\n"
     " 12,99, 5,99, 2, 0};\n"
     +t+" "+p+"_floyd("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<64;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned d[36];\n"
     "    for(int i=0;i<36;i++){ unsigned w="+p+"_adj[i]; d[i]=(w==99u)?900u:(w+((s>>(i&7))&3u)); }\n"
     "    for(int i=0;i<6;i++) d[i*6+i]=0u;\n"
     "    for(int k=0;k<6;k++) for(int i=0;i<6;i++) for(int j=0;j<6;j++){\n"
     "      unsigned alt=d[i*6+k]+d[k*6+j]; if(alt<d[i*6+j]) d[i*6+j]=alt; }\n"
     "    for(int i=0;i<36;i++) acc=acc*131u+d[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x5Eu}, "OptStress131", 2},

    // dense integer GEMM (mod 2^16) of two 6x6 rodata matrices.
    {p+"_matmul",
     "static const unsigned char "+p+"_ma[36]={\n"
     "  2,5,1,7,3,9, 4,8,6,2,5,1, 9,3,7,4,8,6, 1,2,5,9,3,7, 6,4,8,1,2,5, 3,7,9,6,4,8};\n"
     "static const unsigned char "+p+"_mb[36]={\n"
     "  8,4,6,2,9,1, 5,7,3,8,4,6, 2,9,1,5,7,3, 6,2,8,4,9,1, 7,3,5,6,2,8, 9,1,4,7,3,5};\n"
     +t+" "+p+"_matmul("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned c[36];\n"
     "    for(int i=0;i<6;i++) for(int j=0;j<6;j++){ unsigned sum=0u;\n"
     "      for(int k=0;k<6;k++) sum+=("+p+"_ma[i*6+k]^((s>>(k&7))&1u))*(unsigned)"+p+"_mb[k*6+j];\n"
     "      c[i*6+j]=sum&0xFFFFu; }\n"
     "    for(int i=0;i<36;i++) acc=acc*131u+c[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Au}, "OptStress131", 2},

    // counting sort of rodata values: histogram, prefix-sum, stable scatter.
    {p+"_csort",
     "static const unsigned char "+p+"_vals[32]={\n"
     "37,12,55,3,28,49,16,61, 7,44,21,58,33,9,52,25, 40,5,62,18,31,47,11,56, 23,2,60,14,38,50,6,29};\n"
     +t+" "+p+"_csort("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned cnt[32]; for(int i=0;i<32;i++) cnt[i]=0u;\n"
     "    unsigned v[32];\n"
     "    for(int i=0;i<32;i++){ v[i]=("+p+"_vals[i]^((s>>(i&7))&7u))&31u; cnt[v[i]]++; }\n"
     "    for(int i=1;i<32;i++) cnt[i]+=cnt[i-1];\n"
     "    unsigned sorted[32];\n"
     "    for(int i=31;i>=0;i--){ unsigned key=v[i]; sorted[--cnt[key]]=key; }\n"
     "    for(int i=0;i<32;i++) acc=acc*131u+sorted[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x1Du}, "OptStress131", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress131TC("x64o131", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress131TC("x86o131", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress131TC("a64o131", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress131TC("armo131", "int");

INSTANTIATE_TEST_SUITE_P(OptStress131, X64OptStress131RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress131, X86OptStress131RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress131, A64OptStress131RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress131, ARM32OptStress131RT, ::testing::ValuesIn(kARM), rtTCName);
