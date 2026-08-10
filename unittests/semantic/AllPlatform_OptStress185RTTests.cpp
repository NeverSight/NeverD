//===- AllPlatform_OptStress185RTTests.cpp - binsearch / ternary / Horner ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
//
//   * binsearch - insertion-sort then lower_bound / upper_bound binary searches
//                 for several targets.  Pins the halving search loop (distinct
//                 from the linear order-statistic select #179).
//   * ternary   - ternary search for the maximum of a unimodal fold over an index
//                 window.  Pins a three-way interval narrowing (distinct from the
//                 binary halving above).
//   * horner    - Horner polynomial evaluation modulo a constant over rodata
//                 coefficients.  Pins a fused multiply-add reduction (distinct
//                 from the rolling hash folds and the modpow #116).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress185RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress185RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress185RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress185RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress185RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress185RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress185RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress185RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress185TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // insertion-sort, then lower_bound / upper_bound binary searches.
    {p+"_binsearch",
     "static const unsigned char "+p+"_bs[20]={37,12,58,4,29,61,7,44,18,53,2,40,25,9,49,31,16,52,3,47};\n"
     +t+" "+p+"_binsearch("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[20]; for(int i=0;i<20;i++) arr[i]=((unsigned)"+p+"_bs[i]^((s>>(i&7))&7u))&63u;\n"
     "    for(int i=1;i<20;i++){ unsigned k=arr[i]; int j=i-1; while(j>=0 && arr[j]>k){ arr[j+1]=arr[j]; j--; } arr[j+1]=k; }\n"
     "    unsigned fold=0u;\n"
     "    for(int q=0;q<8;q++){ unsigned target=(s>>q)&63u;\n"
     "      int lo=0,hi=20; while(lo<hi){ int mid=(lo+hi)>>1; if(arr[mid]<target) lo=mid+1; else hi=mid; } int lb=lo;\n"
     "      lo=0; hi=20; while(lo<hi){ int mid=(lo+hi)>>1; if(arr[mid]<=target) lo=mid+1; else hi=mid; } int ub=lo;\n"
     "      fold=fold*131u+(unsigned)lb*32u+(unsigned)ub+target; }\n"
     "    acc=acc*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x41u}, "OptStress185", 2},

    // ternary search for the max of a unimodal fold over an index window.
    {p+"_ternary",
     "static const unsigned char "+p+"_tr[16]={2,5,9,14,20,27,33,38,40,37,31,24,16,9,4,1};\n"
     +t+" "+p+"_ternary("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[16]; for(int i=0;i<16;i++) arr[i]=((unsigned)"+p+"_tr[i]^((s>>(i&3))&1u));\n"
     "    int lo=0, hi=15; while(hi-lo>2){ int m1=lo+(hi-lo)/3, m2=hi-(hi-lo)/3; if(arr[m1]<arr[m2]) lo=m1+1; else hi=m2-1; }\n"
     "    unsigned best=0u; int at=lo; for(int i=lo;i<=hi;i++) if(arr[i]>best){ best=arr[i]; at=i; }\n"
     "    acc=acc*131u+best*32u+(unsigned)at; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x50u}, "OptStress185", 2},

    // Horner polynomial evaluation modulo a constant over rodata coefficients.
    {p+"_horner",
     "static const unsigned char "+p+"_hn[16]={7,3,9,2,5,8,1,6,4,0,9,3,7,2,5,8};\n"
     +t+" "+p+"_horner("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0, M=1000003u;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned x=(s>>5)%97u; unsigned r=0u;\n"
     "    for(int i=0;i<16;i++){ unsigned c=((unsigned)"+p+"_hn[i]^((s>>(i&7))&3u)); r=(r*x+c)%M; }\n"
     "    acc=acc*131u+r; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x81u}, "OptStress185", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress185TC("x64o185", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress185TC("x86o185", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress185TC("a64o185", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress185TC("armo185", "int");

INSTANTIATE_TEST_SUITE_P(OptStress185, X64OptStress185RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress185, X86OptStress185RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress185, A64OptStress185RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress185, ARM32OptStress185RT, ::testing::ValuesIn(kARM), rtTCName);
