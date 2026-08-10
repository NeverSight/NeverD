//===- AllPlatform_OptStress167RTTests.cpp - two-sum / next-perm / mode =//
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
//   * twosum   - count rodata value pairs summing to a seeded target using a
//                running complement-count table.  Pins a complement-lookup pair
//                count (distinct from any nested all-pairs scan).
//   * nextperm - repeatedly advance a rodata array to its next lexicographic
//                permutation in place (find pivot, successor swap, reverse tail).
//                Pins the next-permutation transform (distinct from any sort or
//                shuffle).
//   * mode     - statistical mode: histogram a rodata stream then take the
//                argmax bin.  Pins a frequency-argmax (distinct from the move-to
//                -front list in #143 and the counting in sorts).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress167RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress167RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress167RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress167RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress167RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress167RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress167RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress167RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress167TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // count rodata value pairs summing to a seeded target (complement counts).
    {p+"_twosum",
     "static const unsigned char "+p+"_ts[28]={5,12,3,9,1,14,7,20,11,6,4,15,8,10,13,0,19,5,12,3,7,2,8,6,11,1,14,4};\n"
     +t+" "+p+"_twosum("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned d[28]; for(int i=0;i<28;i++) d[i]=(unsigned)"+p+"_ts[i]%24u;\n"
     "    unsigned target=(s%40u)+4u; unsigned seen[24]; for(int i=0;i<24;i++) seen[i]=0u; unsigned pairs=0u;\n"
     "    for(int i=0;i<28;i++){ unsigned v=d[i]; if(v<=target){ unsigned comp=target-v; if(comp<24u) pairs+=seen[comp]; } seen[v]++; acc=acc*131u+pairs; }\n"
     "    acc=acc*131u+pairs; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Bu}, "OptStress167", 2},

    // repeatedly advance a rodata array to its next lexicographic permutation.
    {p+"_nextperm",
     "static const unsigned char "+p+"_np[12]={3,1,4,1,5,9,2,6,5,3,5,8};\n"
     +t+" "+p+"_nextperm("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned ar[12]; for(int i=0;i<12;i++) ar[i]=((unsigned)"+p+"_np[i]^((s>>(i&7))&3u))&15u;\n"
     "    for(int step=0;step<8;step++){ int i=10; while(i>=0 && ar[i]>=ar[i+1]) i--;\n"
     "      if(i<0){ int l=0,r=11; while(l<r){ unsigned tt=ar[l];ar[l]=ar[r];ar[r]=tt;l++;r--; } }\n"
     "      else { int j=11; while(j>i && ar[j]<=ar[i]) j--; unsigned tt=ar[i];ar[i]=ar[j];ar[j]=tt;\n"
     "        int l=i+1,r=11; while(l<r){ unsigned u=ar[l];ar[l]=ar[r];ar[r]=u;l++;r--; } }\n"
     "      unsigned h=0u; for(int k=0;k<12;k++) h=h*31u+ar[k]; acc=acc*131u+h; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x3Cu}, "OptStress167", 2},

    // statistical mode of a rodata stream (histogram argmax).
    {p+"_mode",
     "static const unsigned char "+p+"_mo[32]={3,7,1,7,3,0,12,7,3,1,9,7,3,0,5,7,1,3,7,0,11,3,7,1,0,3,7,14,3,1,7,0};\n"
     +t+" "+p+"_mode("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned cnt[16]; for(int i=0;i<16;i++) cnt[i]=0u;\n"
     "    for(int i=0;i<32;i++) cnt[((unsigned)"+p+"_mo[i]^((s>>(i&7))&1u))&15u]++;\n"
     "    unsigned best=0u,bestv=0u; for(int k=0;k<16;k++){ if(cnt[k]>best){ best=cnt[k]; bestv=(unsigned)k; } acc=acc*131u+cnt[k]; }\n"
     "    acc=acc*131u+best*16u+bestv; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Du}, "OptStress167", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress167TC("x64o167", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress167TC("x86o167", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress167TC("a64o167", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress167TC("armo167", "int");

INSTANTIATE_TEST_SUITE_P(OptStress167, X64OptStress167RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress167, X86OptStress167RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress167, A64OptStress167RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress167, ARM32OptStress167RT, ::testing::ValuesIn(kARM), rtTCName);
