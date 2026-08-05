//===- AllPlatform_OptStress184RTTests.cpp - heapsort / quicksort / combsort =//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
//
//   * heapsort  - in-place heapsort: build a max-heap by sift-down, then pop the
//                 root repeatedly.  Pins binary-heap sift-down (distinct from the
//                 distribution counting sort #177).
//   * quicksort - iterative quicksort with a Lomuto partition and an explicit
//                 work stack.  Pins pivot partitioning + manual recursion stack
//                 (distinct from the Dutch-flag 3-way partition #175).
//   * combsort  - comb sort: a shrinking-gap bubble pass until sorted.  Pins a
//                 gap-strided exchange sort (distinct from both above).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress184RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress184RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress184RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress184RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress184RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress184RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress184RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress184RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress184TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // in-place heapsort: build max-heap by sift-down, then pop the root.
    {p+"_heapsort",
     "static const unsigned char "+p+"_hs[20]={37,12,58,4,29,61,7,44,18,53,2,40,25,9,49,31,16,52,3,47};\n"
     +t+" "+p+"_heapsort("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[20]; for(int i=0;i<20;i++) arr[i]=((unsigned)"+p+"_hs[i]^((s>>(i&7))&7u))&63u;\n"
     "    for(int start=9;start>=0;start--){ int root=start;\n"
     "      while(root*2+1<20){ int ch=root*2+1; if(ch+1<20 && arr[ch]<arr[ch+1]) ch++; if(arr[root]<arr[ch]){ unsigned tt=arr[root]; arr[root]=arr[ch]; arr[ch]=tt; root=ch; } else break; } }\n"
     "    for(int end=19;end>0;end--){ unsigned tt=arr[0]; arr[0]=arr[end]; arr[end]=tt; int root=0;\n"
     "      while(root*2+1<end){ int ch=root*2+1; if(ch+1<end && arr[ch]<arr[ch+1]) ch++; if(arr[root]<arr[ch]){ unsigned u=arr[root]; arr[root]=arr[ch]; arr[ch]=u; root=ch; } else break; } }\n"
     "    unsigned fold=0u; for(int i=0;i<20;i++) fold=fold*131u+arr[i]; acc=acc*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x40u}, "OptStress184", 2},

    // iterative quicksort with Lomuto partition and an explicit work stack.
    {p+"_quicksort",
     "static const unsigned char "+p+"_qs[20]={47,3,52,16,31,49,9,25,40,2,53,18,44,7,61,29,4,58,12,37};\n"
     +t+" "+p+"_quicksort("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[20]; for(int i=0;i<20;i++) arr[i]=((unsigned)"+p+"_qs[i]^((s>>(i&7))&7u))&63u;\n"
     "    int st[128]; int sp=0; st[sp++]=0; st[sp++]=19;\n"
     "    while(sp>0){ int hi=st[--sp]; int lo=st[--sp]; if(lo>=hi) continue;\n"
     "      unsigned pivot=arr[hi]; int i=lo-1;\n"
     "      for(int j=lo;j<hi;j++){ if(arr[j]<=pivot){ i++; unsigned tt=arr[i]; arr[i]=arr[j]; arr[j]=tt; } }\n"
     "      unsigned tt=arr[i+1]; arr[i+1]=arr[hi]; arr[hi]=tt; int pp=i+1;\n"
     "      if(sp<=124){ st[sp++]=lo; st[sp++]=pp-1; st[sp++]=pp+1; st[sp++]=hi; } }\n"
     "    unsigned fold=0u; for(int i=0;i<20;i++) fold=fold*131u+arr[i]; acc=acc*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Fu}, "OptStress184", 2},

    // comb sort: a shrinking-gap bubble pass until sorted.
    {p+"_combsort",
     "static const unsigned char "+p+"_cs[20]={9,49,2,40,25,7,61,29,4,58,12,37,52,16,31,53,18,44,3,47};\n"
     +t+" "+p+"_combsort("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[20]; for(int i=0;i<20;i++) arr[i]=((unsigned)"+p+"_cs[i]^((s>>(i&7))&7u))&63u;\n"
     "    int gap=20, swapped=1;\n"
     "    while(gap>1 || swapped){ gap=(gap*10)/13; if(gap<1) gap=1; swapped=0;\n"
     "      for(int i=0;i+gap<20;i++){ if(arr[i]>arr[i+gap]){ unsigned tt=arr[i]; arr[i]=arr[i+gap]; arr[i+gap]=tt; swapped=1; } } }\n"
     "    unsigned fold=0u; for(int i=0;i<20;i++) fold=fold*131u+arr[i]; acc=acc*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x80u}, "OptStress184", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress184TC("x64o184", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress184TC("x86o184", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress184TC("a64o184", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress184TC("armo184", "int");

INSTANTIATE_TEST_SUITE_P(OptStress184, X64OptStress184RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress184, X86OptStress184RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress184, A64OptStress184RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress184, ARM32OptStress184RT, ::testing::ValuesIn(kARM), rtTCName);
