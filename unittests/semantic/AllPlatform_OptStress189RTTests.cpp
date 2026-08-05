//===- AllPlatform_OptStress189RTTests.cpp - shell / gnome / cocktail sorts =//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
//
//   * shellsort - Shell sort: gapped insertion passes over a halving gap
//                 sequence.  Pins a strided insertion shuffle (distinct from the
//                 contiguous insertion sort #144, the shrinking-gap comb sort
//                 #184 and the heap/quick sorts #114/#151).
//   * gnomesort - Gnome sort: a single index that walks back one step on every
//                 inversion and forward otherwise.  Pins the back-step bubble
//                 idiom (distinct from the bidirectional cocktail pass below and
//                 every gapped sort).
//   * cocktail  - Cocktail-shaker sort: a bubble pass forward then backward with
//                 shrinking bounds.  Pins a two-way sweep with swap-flag early
//                 exit (distinct from the one-way bubble/comb passes).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress189RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress189RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress189RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress189RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress189RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress189RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress189RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress189RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress189TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Shell sort: gapped insertion passes over a halving gap sequence.
    {p+"_shellsort",
     "static const unsigned char "+p+"_ss[16]={37,12,58,4,29,61,7,44,18,53,2,40,25,9,49,31};\n"
     +t+" "+p+"_shellsort("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned v[16]; for(int i=0;i<16;i++) v[i]=((unsigned)"+p+"_ss[i]^((s>>(i&7))&15u))&63u;\n"
     "    for(int gap=8; gap>0; gap>>=1){\n"
     "      for(int i=gap;i<16;i++){ unsigned tmp=v[i]; int j=i;\n"
     "        while(j>=gap && v[j-gap]>tmp){ v[j]=v[j-gap]; j-=gap; } v[j]=tmp; } }\n"
     "    unsigned fold=0u; for(int i=0;i<16;i++) fold=fold*131u+v[i];\n"
     "    acc=acc*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x91u}, "OptStress189", 2},

    // Gnome sort: single index walks back on an inversion, forward otherwise.
    {p+"_gnomesort",
     "static const unsigned char "+p+"_gn[16]={50,7,33,12,61,4,28,55,2,40,19,9,47,25,16,38};\n"
     +t+" "+p+"_gnomesort("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned v[16]; for(int i=0;i<16;i++) v[i]=((unsigned)"+p+"_gn[i]^((s>>(i&7))&15u))&63u;\n"
     "    int i=0;\n"
     "    while(i<16){ if(i==0 || v[i-1]<=v[i]) i++;\n"
     "      else { unsigned tmp=v[i]; v[i]=v[i-1]; v[i-1]=tmp; i--; } }\n"
     "    unsigned fold=0u; for(int k=0;k<16;k++) fold=fold*131u+v[k];\n"
     "    acc=acc*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x92u}, "OptStress189", 2},

    // Cocktail-shaker sort: forward + backward bubble passes with swap flag.
    {p+"_cocktail",
     "static const unsigned char "+p+"_ck[16]={9,44,3,57,21,38,6,49,15,60,2,33,27,11,52,40};\n"
     +t+" "+p+"_cocktail("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned v[16]; for(int i=0;i<16;i++) v[i]=((unsigned)"+p+"_ck[i]^((s>>(i&7))&15u))&63u;\n"
     "    int lo=0, hi=15; unsigned swapped=1u;\n"
     "    while(swapped){ swapped=0u;\n"
     "      for(int i=lo;i<hi;i++) if(v[i]>v[i+1]){ unsigned t=v[i]; v[i]=v[i+1]; v[i+1]=t; swapped=1u; }\n"
     "      hi--;\n"
     "      for(int i=hi;i>lo;i--) if(v[i-1]>v[i]){ unsigned t=v[i]; v[i]=v[i-1]; v[i-1]=t; swapped=1u; }\n"
     "      lo++; }\n"
     "    unsigned fold=0u; for(int i=0;i<16;i++) fold=fold*131u+v[i];\n"
     "    acc=acc*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x93u}, "OptStress189", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress189TC("x64o189", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress189TC("x86o189", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress189TC("a64o189", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress189TC("armo189", "int");

INSTANTIATE_TEST_SUITE_P(OptStress189, X64OptStress189RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress189, X86OptStress189RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress189, A64OptStress189RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress189, ARM32OptStress189RT, ::testing::ValuesIn(kARM), rtTCName);
