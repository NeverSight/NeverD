//===- AllPlatform_OptStress174RTTests.cpp - majority / RLE / binary-GCD ====//
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
//   * boyermaj  - Boyer-Moore majority vote: a single streaming pass keeps one
//                 candidate + a counter, then a verification pass tallies the
//                 candidate's true occurrences.  Pins the streaming
//                 candidate/counter idiom (distinct from the 3-sum / two-sum
//                 census counts in #173/#167).
//   * rle       - run-length encoding: walk adjacent equal runs and fold each
//                 (value,length) pair.  Pins an adjacency run-length scan
//                 (distinct from the bitmask-membership consecutive run in #173
//                 and the monotonic-run scan in #163).
//   * bingcd    - Stein's binary GCD: strip common powers of two, then a
//                 subtract-and-halve loop with no division at all.  Pins a
//                 bit-twiddling GCD (distinct from the scalar modpow in #116 and
//                 the matrix power in #171).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress174RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress174RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress174RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress174RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress174RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress174RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress174RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress174RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress174TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Boyer-Moore majority vote: streaming candidate then verification tally.
    {p+"_boyermaj",
     "static const unsigned char "+p+"_bm[24]={2,4,2,1,2,3,2,4,0,2,2,1,3,2,2,4,2,0,1,2,2,3,2,2};\n"
     +t+" "+p+"_boyermaj("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned cand=0u, cnt=0u;\n"
     "    for(int i=0;i<24;i++){ unsigned v=((unsigned)"+p+"_bm[i]^((s>>(i&7))&3u))%5u;\n"
     "      if(cnt==0u){ cand=v; cnt=1u; } else if(v==cand) cnt++; else cnt--; }\n"
     "    unsigned occ=0u; for(int i=0;i<24;i++){ unsigned v=((unsigned)"+p+"_bm[i]^((s>>(i&7))&3u))%5u; if(v==cand) occ++; }\n"
     "    acc=acc*131u+cand*100u+cnt+occ*7u; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x34u}, "OptStress174", 2},

    // run-length encoding: fold (value,length) over each adjacent equal run.
    {p+"_rle",
     "static const unsigned char "+p+"_rl[28]={1,1,1,2,2,3,3,3,3,0,0,1,2,2,2,1,1,0,3,3,2,2,2,2,0,1,1,1};\n"
     +t+" "+p+"_rle("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned runs=0u, fold=0u; int i=0;\n"
     "    while(i<28){ unsigned v=((unsigned)"+p+"_rl[i]^((s>>(i&7))&1u))&3u; unsigned len=1u; int j=i+1;\n"
     "      while(j<28 && (((unsigned)"+p+"_rl[j]^((s>>(j&7))&1u))&3u)==v){ len++; j++; }\n"
     "      runs++; fold=fold*131u+(v*37u+len); i=j; }\n"
     "    acc=acc*131u+runs+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x45u}, "OptStress174", 2},

    // Stein's binary GCD: strip common 2s, then subtract-and-halve (no divide).
    {p+"_bingcd",
     "static const unsigned char "+p+"_bg[16]={37,12,58,4,29,61,7,44,18,53,2,40,25,9,49,31};\n"
     +t+" "+p+"_bingcd("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned u=(((unsigned)"+p+"_bg[s&15u]<<3)^(s&255u))+1u, v=(((unsigned)"+p+"_bg[(s>>4)&15u]<<2)^((s>>4)&255u))+1u;\n"
     "    unsigned shift=0u;\n"
     "    while(((u|v)&1u)==0u){ u>>=1; v>>=1; shift++; }\n"
     "    while((u&1u)==0u) u>>=1;\n"
     "    do { while((v&1u)==0u) v>>=1; if(u>v){ unsigned tmp=u; u=v; v=tmp; } v=v-u; } while(v!=0u);\n"
     "    unsigned g=u<<shift; acc=acc*131u+g+u+shift; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x76u}, "OptStress174", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress174TC("x64o174", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress174TC("x86o174", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress174TC("a64o174", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress174TC("armo174", "int");

INSTANTIATE_TEST_SUITE_P(OptStress174, X64OptStress174RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress174, X86OptStress174RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress174, A64OptStress174RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress174, ARM32OptStress174RT, ::testing::ValuesIn(kARM), rtTCName);
