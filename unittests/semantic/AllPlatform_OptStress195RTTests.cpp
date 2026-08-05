//===- AllPlatform_OptStress195RTTests.cpp - ternary / jump / exp search ===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
//
//   * ternsearch - ternary search for the maximum of a unimodal quadratic whose
//                  peak is rodata/seed-derived: probe at the two third points and
//                  discard a third each step (the interval shrinks by /3, a
//                  constant divide).  Pins a unimodal max search (distinct from
//                  the binary #185 and interpolation #169 ordered searches).
//   * jumpsearch - jump search over a prefix-sum-sorted rodata array: stride by a
//                  fixed block then linear-scan the last block.  Pins a fixed
//                  block-stride ordered scan (distinct from the halving probes).
//   * expsearch  - exponential search: double the bound until it passes the key,
//                  then binary-search the bracketed window.  Pins a bound-doubling
//                  prelude before a halving search (distinct from both above).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress195RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress195RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress195RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress195RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress195RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress195RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress195RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress195RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress195TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // ternary search for the max of a unimodal quadratic (peak from rodata/seed).
    {p+"_ternsearch",
     "static const unsigned char "+p+"_ts[8]={37,12,58,4,29,61,7,44};\n"
     +t+" "+p+"_ternsearch("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int peak=(int)((((unsigned)"+p+"_ts[s&7u])^(s&63u))%100u);\n"
     "    int lo=0, hi=100;\n"
     "    while(hi-lo>2){ int m1=lo+(hi-lo)/3; int m2=hi-(hi-lo)/3;\n"
     "      int f1=5000-(m1-peak)*(m1-peak); int f2=5000-(m2-peak)*(m2-peak);\n"
     "      if(f1<f2) lo=m1+1; else hi=m2-1; }\n"
     "    int bx=lo, bf=5000-(lo-peak)*(lo-peak);\n"
     "    for(int x=lo+1;x<=hi;x++){ int f=5000-(x-peak)*(x-peak); if(f>bf){bf=f;bx=x;} }\n"
     "    acc=acc*131u+(unsigned)bx*131u+(unsigned)(bf+100000); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xF1u}, "OptStress195", 2},

    // jump search over a prefix-sum-sorted rodata array (fixed block stride).
    {p+"_jumpsearch",
     "static const unsigned char "+p+"_js[16]={3,1,6,2,5,0,7,4,2,6,1,5,3,0,4,7};\n"
     +t+" "+p+"_jumpsearch("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[16]; unsigned run=0u;\n"
     "    for(int i=0;i<16;i++){ run+=1u+((unsigned)"+p+"_js[i]&7u); arr[i]=run; }\n"
     "    unsigned target=arr[s&15u]^((s>>4)&1u);\n"
     "    int n=16, step=4, i=0;\n"
     "    while(i<n && arr[i]<target) i+=step;\n"
     "    int start=i-step; if(start<0) start=0; int found=-1;\n"
     "    for(int j=start; j<n && j<=i; j++){ if(arr[j]==target){ found=j; break; } }\n"
     "    acc=acc*131u+(unsigned)(found+1)*131u+target; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xF2u}, "OptStress195", 2},

    // exponential search: double the bound, then binary-search the window.
    {p+"_expsearch",
     "static const unsigned char "+p+"_es[16]={2,4,1,3,5,2,6,1,4,2,3,5,1,4,2,3};\n"
     +t+" "+p+"_expsearch("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[16]; unsigned run=0u;\n"
     "    for(int i=0;i<16;i++){ run+=1u+((unsigned)"+p+"_es[i]&7u); arr[i]=run; }\n"
     "    unsigned target=arr[(s>>2)&15u]+((s>>6)&1u);\n"
     "    int bound=1;\n"
     "    while(bound<16 && arr[bound]<target) bound<<=1;\n"
     "    int lo=bound>>1; int hi=(bound<16)?bound:15; int found=-1;\n"
     "    while(lo<=hi){ int mid=lo+((hi-lo)>>1);\n"
     "      if(arr[mid]==target){ found=mid; break; }\n"
     "      else if(arr[mid]<target) lo=mid+1; else hi=mid-1; }\n"
     "    acc=acc*131u+(unsigned)(found+1)*131u+target; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xF3u}, "OptStress195", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress195TC("x64o195", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress195TC("x86o195", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress195TC("a64o195", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress195TC("armo195", "int");

INSTANTIATE_TEST_SUITE_P(OptStress195, X64OptStress195RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress195, X86OptStress195RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress195, A64OptStress195RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress195, ARM32OptStress195RT, ::testing::ValuesIn(kARM), rtTCName);
