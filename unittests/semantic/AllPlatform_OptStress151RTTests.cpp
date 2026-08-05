//===- AllPlatform_OptStress151RTTests.cpp - quicksort / Manacher / histeq =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * quicksrt - iterative quicksort of a rodata-seeded array with an explicit
//                stack of sub-range bounds and a Lomuto partition.  Pins a
//                manual recursion-stack divide-and-conquer (distinct from the
//                in-place heap/insert sorts in #144, the radix buckets in #147,
//                and the partial quickselect in #124).
//   * manacher - Manacher linear palindrome radii over a separator-expanded
//                rodata string: the [c,r] window mirrors an already-known radius
//                before extending.  Pins a mirror-reuse palindrome scan (distinct
//                from the naive center expansion in #142 and the Z-array window).
//   * histeq   - histogram equalization of a rodata image: a bin histogram, its
//                running CDF, and a min-anchored rescale remap each sample.  Pins
//                a histogram-CDF remap (distinct from the counting/radix sorts
//                and from the move-to-front frequency list in #143).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress151RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress151RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress151RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress151RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress151RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress151RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress151RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress151RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress151TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // iterative quicksort with an explicit bounds stack (Lomuto partition).
    {p+"_quicksrt",
     "static const unsigned char "+p+"_q[24]={211,17,140,88,3,199,72,150,46,8,131,2,97,55,188,30,118,77,14,200,41,160,123,5};\n"
     +t+" "+p+"_quicksrt("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[24]; for(int i=0;i<24;i++) arr[i]=(unsigned)"+p+"_q[i]^((s>>(i&7))&7u);\n"
     "    int stk[128]; int top=0; stk[top++]=0; stk[top++]=23;\n"
     "    while(top>0){ int hi=stk[--top]; int lo=stk[--top]; if(lo>=hi) continue;\n"
     "      unsigned pivot=arr[hi]; int i=lo-1;\n"
     "      for(int j=lo;j<hi;j++){ if(arr[j]<=pivot){ i++; unsigned tt=arr[i]; arr[i]=arr[j]; arr[j]=tt; } }\n"
     "      unsigned tt=arr[i+1]; arr[i+1]=arr[hi]; arr[hi]=tt; int pp=i+1;\n"
     "      int ln=pp-1-lo, rn=hi-(pp+1);\n"
     "      if(ln>rn){ if(top+2<=128){stk[top++]=lo;stk[top++]=pp-1;} if(top+2<=128){stk[top++]=pp+1;stk[top++]=hi;} }\n"
     "      else { if(top+2<=128){stk[top++]=pp+1;stk[top++]=hi;} if(top+2<=128){stk[top++]=lo;stk[top++]=pp-1;} }\n"
     "      acc=acc*131u+(unsigned)pp; }\n"
     "    for(int i=0;i<24;i++) acc=acc*131u+arr[i]*(unsigned)(i+1);\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x1Bu}, "OptStress151", 2},

    // Manacher palindrome radii over a separator-expanded rodata string.
    {p+"_manacher",
     "static const unsigned char "+p+"_s[15]={1,2,3,2,1,4,1,2,1,5,1,2,1,2,1};\n"
     +t+" "+p+"_manacher("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned t[31];\n"
     "    for(int i=0;i<31;i++) t[i]=(i&1)?(unsigned)"+p+"_s[(i-1)>>1]^((s>>(((i-1)>>1)&7))&1u):1000u;\n"
     "    int pr[31]; for(int i=0;i<31;i++) pr[i]=0;\n"
     "    int c=0,r=0;\n"
     "    for(int i=0;i<31;i++){ int mir=2*c-i;\n"
     "      if(i<r && mir>=0 && mir<31){ int rm=r-i; pr[i]=(pr[mir]<rm)?pr[mir]:rm; } else pr[i]=0;\n"
     "      while(i-pr[i]-1>=0 && i+pr[i]+1<31 && t[i-pr[i]-1]==t[i+pr[i]+1]) pr[i]++;\n"
     "      if(i+pr[i]>r){ c=i; r=i+pr[i]; } acc=acc*131u+(unsigned)pr[i]; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Cu}, "OptStress151", 2},

    // histogram equalization of a rodata image (histogram + CDF + remap).
    {p+"_histeq",
     "static const unsigned char "+p+"_img[40]={\n"
     "5,9,12,5,10,3,5,9, 13,5,9,12,1,1,3,3, 10,8,5,15,6,8,8,8, 5,5,5,5,0,0,14,2, 2,2,2,1,9,12,5,9};\n"
     +t+" "+p+"_histeq("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned hist[16]; for(int k=0;k<16;k++) hist[k]=0u;\n"
     "    for(int i=0;i<40;i++){ unsigned v=((unsigned)"+p+"_img[i]+((s>>(i&7))&1u))&15u; hist[v]++; }\n"
     "    unsigned cdf[16]; unsigned run=0u; for(int k=0;k<16;k++){ run+=hist[k]; cdf[k]=run; }\n"
     "    unsigned cmin=41u; for(int k=0;k<16;k++) if(hist[k]!=0u){ cmin=cdf[k]; break; }\n"
     "    unsigned denom=(40u>cmin)?(40u-cmin):1u;\n"
     "    for(int i=0;i<40;i++){ unsigned v=((unsigned)"+p+"_img[i]+((s>>(i&7))&1u))&15u; unsigned nv=((cdf[v]-cmin)*15u)/denom; acc=acc*131u+nv; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x5Fu}, "OptStress151", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress151TC("x64o151", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress151TC("x86o151", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress151TC("a64o151", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress151TC("armo151", "int");

INSTANTIATE_TEST_SUITE_P(OptStress151, X64OptStress151RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress151, X86OptStress151RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress151, A64OptStress151RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress151, ARM32OptStress151RT, ::testing::ValuesIn(kARM), rtTCName);
