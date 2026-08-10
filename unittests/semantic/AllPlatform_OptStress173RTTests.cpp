//===- AllPlatform_OptStress173RTTests.cpp - Stirling2 / 3-sum / consec-run ==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain base+index copies and folds a result that depends only on the
// bytes + control flow (never an absolute VA), so nothing touches the deferred
// i386/ARM32 PIC rodata *interior*-pointer model (#477/#487); every probe runs
// on all four targets.
//
//   * stirling2 - Stirling numbers of the second kind via the in-place
//                 S(n,k)=k*S(n-1,k)+S(n-1,k-1) row recurrence (descending k, mod
//                 1000003), with a rodata byte folded into each row base.  Pins a
//                 weighted triangle recurrence (distinct from Pascal #130, the
//                 Catalan fold #135 and the Bell triangle #170).
//   * threesum  - 3-sum census: count index triples i<j<k whose rodata-seeded
//                 signed values hit a seeded target.  Pins a triple-nested
//                 combination count (distinct from the two-sum complement count
//                 in #167).
//   * conseq    - longest run of consecutive integers present in a rodata multi-
//                 set, tracked through a 32-bit presence mask.  Pins a
//                 bitmask-membership consecutive-run scan (distinct from the
//                 monotonic-run scan in #163).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress173RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress173RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress173RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress173RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress173RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress173RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress173RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress173RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress173TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Stirling numbers of the 2nd kind via in-place row recurrence (mod 1000003).
    {p+"_stirling2",
     "static const unsigned char "+p+"_s2[12]={3,7,1,9,4,12,6,2,10,5,8,0};\n"
     +t+" "+p+"_stirling2("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0, M=1000003u;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned st[16]; for(int i=0;i<16;i++) st[i]=0u; st[0]=1u;\n"
     "    for(int n=1;n<12;n++){ for(int k=n;k>=1;k--) st[k]=((unsigned)k*st[k]+st[k-1])%M;\n"
     "      unsigned mix=((unsigned)"+p+"_s2[n]^((s>>(n&7))&7u)); st[0]=(st[0]+mix)%M;\n"
     "      unsigned hh=0u; for(int k=0;k<=n;k++) hh=hh*131u+st[k]; acc=acc*131u+hh; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x31u}, "OptStress173", 2},

    // 3-sum census: count triples i<j<k of signed rodata values hitting a target.
    {p+"_threesum",
     "static const unsigned char "+p+"_3s[12]={37,12,58,4,29,61,7,44,18,53,2,40};\n"
     +t+" "+p+"_threesum("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int arr[12]; for(int i=0;i<12;i++) arr[i]=(int)(((unsigned)"+p+"_3s[i]^((s>>(i&7))&7u))%25u)-12;\n"
     "    int target=(int)((s>>4)%9u)-4; unsigned cnt=0u;\n"
     "    for(int i=0;i<12;i++) for(int j=i+1;j<12;j++) for(int k=j+1;k<12;k++) if(arr[i]+arr[j]+arr[k]==target) cnt++;\n"
     "    acc=acc*131u+cnt+(unsigned)(target+100); for(int i=0;i<12;i++) acc=acc*131u+(unsigned)(arr[i]+100); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x42u}, "OptStress173", 2},

    // longest run of consecutive integers present, via a 32-bit presence mask.
    {p+"_conseq",
     "static const unsigned char "+p+"_cq[20]={5,6,7,18,9,10,2,3,4,21,11,12,13,1,0,24,8,19,14,15};\n"
     +t+" "+p+"_conseq("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned mask=0u; for(int i=0;i<20;i++) mask|=(1u<<(((unsigned)"+p+"_cq[i]^((s>>(i&7))&3u))&31u));\n"
     "    unsigned best=0u;\n"
     "    for(unsigned v=0;v<32u;v++){ if((mask>>v)&1u){ if(v==0u || !((mask>>(v-1u))&1u)){ unsigned run=0u,w=v; while(w<32u && ((mask>>w)&1u)){ run++; w++; } if(run>best) best=run; } } }\n"
     "    acc=acc*131u+best+mask; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x73u}, "OptStress173", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress173TC("x64o173", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress173TC("x86o173", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress173TC("a64o173", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress173TC("armo173", "int");

INSTANTIATE_TEST_SUITE_P(OptStress173, X64OptStress173RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress173, X86OptStress173RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress173, A64OptStress173RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress173, ARM32OptStress173RT, ::testing::ValuesIn(kARM), rtTCName);
