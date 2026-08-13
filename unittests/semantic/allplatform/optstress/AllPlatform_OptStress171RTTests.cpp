//===- AllPlatform_OptStress171RTTests.cpp - Heap perm / mat-exp / inversions =//
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
//   * heapperm   - Heap's algorithm: enumerate every permutation of a small
//                  rodata-seeded array by the counter-driven adjacent swap
//                  schedule, hashing each permutation.  Pins Heap's iterative
//                  permutation generator (distinct from the single lexicographic
//                  next-permutation step in #167 and the gather permutations).
//   * matexp     - 2x2 matrix exponentiation by squaring (mod 40009): a binary
//                  exponent walks the squaring chain accumulating into an
//                  identity.  Pins a matrix power-by-squaring (distinct from the
//                  scalar modpow in #116).
//   * inversions - inversion count: tally the pairs i<j with arr[i]>arr[j] in a
//                  rodata-seeded array.  Pins an O(n^2) pair-order census
//                  (distinct from any sort or the Kadane scans in #136/#160).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress171RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress171RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress171RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress171RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress171RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress171RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress171RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress171RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress171TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Heap's algorithm: enumerate all permutations of a 5-element rodata array.
    {p+"_heapperm",
     "static const unsigned char "+p+"_hp[8]={3,9,1,12,6,2,14,7};\n"
     +t+" "+p+"_heapperm("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[5]; for(int i=0;i<5;i++) arr[i]=((unsigned)"+p+"_hp[i]^((s>>(i&7))&7u))&15u;\n"
     "    int c[5]; for(int i=0;i<5;i++) c[i]=0;\n"
     "    unsigned h=0u; for(int i=0;i<5;i++) h=h*31u+arr[i]; acc=acc*131u+h;\n"
     "    int i=0;\n"
     "    while(i<5){ if(c[i]<i){ int sw=(i&1)? c[i] : 0; unsigned tt=arr[sw]; arr[sw]=arr[i]; arr[i]=tt;\n"
     "        unsigned hh=0u; for(int k=0;k<5;k++) hh=hh*31u+arr[k]; acc=acc*131u+hh; c[i]++; i=0; }\n"
     "      else { c[i]=0; i++; } }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Fu}, "OptStress171", 2},

    // 2x2 matrix exponentiation by squaring, mod 40009.
    {p+"_matexp",
     "static const unsigned char "+p+"_mx[8]={5,2,7,3,9,4,6,8};\n"
     +t+" "+p+"_matexp("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0, MOD=40009u;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned m0=((unsigned)"+p+"_mx[0]^(s&7u))%MOD, m1=((unsigned)"+p+"_mx[1]^((s>>2)&7u))%MOD;\n"
     "    unsigned m2=((unsigned)"+p+"_mx[2]^((s>>4)&7u))%MOD, m3=((unsigned)"+p+"_mx[3]^((s>>6)&7u))%MOD;\n"
     "    unsigned r0=1u,r1=0u,r2=0u,r3=1u, e=((s>>8)%64u)+1u;\n"
     "    while(e){ if(e&1u){ unsigned n0=(r0*m0+r1*m2)%MOD, n1=(r0*m1+r1*m3)%MOD, n2=(r2*m0+r3*m2)%MOD, n3=(r2*m1+r3*m3)%MOD; r0=n0;r1=n1;r2=n2;r3=n3; }\n"
     "      unsigned q0=(m0*m0+m1*m2)%MOD, q1=(m0*m1+m1*m3)%MOD, q2=(m2*m0+m3*m2)%MOD, q3=(m2*m1+m3*m3)%MOD; m0=q0;m1=q1;m2=q2;m3=q3;\n"
     "      e>>=1; acc=acc*131u+r0+r1+r2+r3; }\n"
     "    acc=acc*131u+r0+r1*2u+r2*3u+r3*5u; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x40u}, "OptStress171", 2},

    // inversion count: pairs i<j with arr[i]>arr[j] in a rodata-seeded array.
    {p+"_inversions",
     "static const unsigned char "+p+"_iv[20]={37,12,58,4,29,61,7,44,18,53,2,40,25,9,49,31,16,52,3,47};\n"
     +t+" "+p+"_inversions("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[20]; for(int i=0;i<20;i++) arr[i]=((unsigned)"+p+"_iv[i]^((s>>(i&7))&7u))&63u;\n"
     "    unsigned inv=0u; for(int i=0;i<20;i++) for(int j=i+1;j<20;j++) if(arr[i]>arr[j]) inv++;\n"
     "    acc=acc*131u+inv; for(int i=0;i<20;i++) acc=acc*131u+arr[i]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x71u}, "OptStress171", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress171TC("x64o171", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress171TC("x86o171", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress171TC("a64o171", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress171TC("armo171", "int");

INSTANTIATE_TEST_SUITE_P(OptStress171, X64OptStress171RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress171, X86OptStress171RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress171, A64OptStress171RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress171, ARM32OptStress171RT, ::testing::ValuesIn(kARM), rtTCName);
