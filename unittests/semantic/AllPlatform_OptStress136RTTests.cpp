//===- AllPlatform_OptStress136RTTests.cpp - binsearch / rank-select / Kadane =//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * bsearch- binary search over a sorted rodata array for LCG-derived keys,
//              folding the found index and final low bound.  Pins a halving
//              divide-and-conquer probe `mid=(lo+hi)/2` (distinct from any linear
//              or hashed lookup).
//   * rank   - selection by rank: for each target rank, the element whose count
//              of strictly-smaller (tie-broken by index) elements equals it.
//              Pins an all-pairs rank-by-count select (distinct from a sort).
//   * kadane - maximum-subarray sum (Kadane) over a rodata signed stream with
//              `cur=max(0,cur+x)` running reset.  Pins a running-max with a
//              clamp-to-zero reset (distinct from a plain prefix sum).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress136RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress136RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress136RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress136RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress136RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress136RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress136RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress136RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress136TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // binary search over a sorted rodata array (halving divide-and-conquer).
    {p+"_bsearch",
     "static const unsigned char "+p+"_sorted[16]={\n"
     "2,5,9,14,20,27,35,44, 54,65,77,90,104,119,135,152};\n"
     +t+" "+p+"_bsearch("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s, hits=0u;\n"
     "    for(int q=0;q<32;q++){ unsigned key=(s>>(q&15))&0xFFu; int lo=0, hi=15, found=-1;\n"
     "      while(lo<=hi){ int mid=(lo+hi)/2; unsigned mv="+p+"_sorted[mid];\n"
     "        if(mv==key){ found=mid; break; } else if(mv<key) lo=mid+1; else hi=mid-1; }\n"
     "      if(found>=0) hits++; acc=acc*131u+(unsigned)(found+1)+(unsigned)lo; }\n"
     "    acc=acc*131u+hits; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x28u}, "OptStress136", 2},

    // selection by rank-of-count over a rodata-seeded array (all-pairs count).
    {p+"_rank",
     "static const unsigned char "+p+"_vals[16]={\n"
     "37,12,55,3,28,49,16,61, 7,44,21,58,33,9,52,25};\n"
     +t+" "+p+"_rank("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned v[16]; for(int i=0;i<16;i++) v[i]="+p+"_vals[i]^((s>>(i&7))&7u);\n"
     "    for(int k=0;k<16;k++){ unsigned tr=(s>>(k&15))&15u, chosen=0u;\n"
     "      for(int i=0;i<16;i++){ unsigned rank=0u;\n"
     "        for(int j=0;j<16;j++) if(v[j]<v[i] || (v[j]==v[i] && j<i)) rank++;\n"
     "        if(rank==tr) chosen=v[i]; }\n"
     "      acc=acc*131u+chosen; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x63u}, "OptStress136", 2},

    // maximum-subarray sum (Kadane) over a rodata signed stream.
    {p+"_kadane",
     "static const unsigned char "+p+"_seq[24]={\n"
     "12,3,15,1,9,14,2,11, 5,13,4,10,7,16,6,8, 1,15,3,12,9,2,14,5};\n"
     +t+" "+p+"_kadane("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int best=-100000, cur=0;\n"
     "    for(int i=0;i<24;i++){ int x=(int)("+p+"_seq[i]^((s>>(i&7))&3u)) - 8;\n"
     "      cur+=x; if(cur>best) best=cur; if(cur<0) cur=0;\n"
     "      acc=acc*131u+(unsigned)(cur+1000); }\n"
     "    acc=acc*131u+(unsigned)(best+100000); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x74u}, "OptStress136", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress136TC("x64o136", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress136TC("x86o136", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress136TC("a64o136", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress136TC("armo136", "int");

INSTANTIATE_TEST_SUITE_P(OptStress136, X64OptStress136RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress136, X86OptStress136RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress136, A64OptStress136RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress136, ARM32OptStress136RT, ::testing::ValuesIn(kARM), rtTCName);
