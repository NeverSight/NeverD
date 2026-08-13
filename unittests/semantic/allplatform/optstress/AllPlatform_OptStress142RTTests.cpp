//===- AllPlatform_OptStress142RTTests.cpp - cycle / palindrome / Z-array =//
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
//   * cycle  - Floyd tortoise-and-hare cycle detection over a rodata successor
//              table (a functional graph): a one-step pointer and a two-step
//              pointer chase to the meeting node, then a lap to measure the
//              cycle length.  Pins a two-speed pointer chase (distinct from any
//              linear walk or the BFS level order in #132).
//   * palin  - palindrome census by center expansion: the rodata string is read
//              forward into a stack copy (base+index, no interior pointer), then
//              for every odd and even center a symmetric two-pointer window
//              expands while the mirrored characters match.  Pins a symmetric
//              expand-around-center scan (distinct from any forward DP).
//   * zalgo  - Z-array construction over a rodata string: the [l,r] match window
//              that re-uses an already-computed prefix length before extending
//              by direct comparison.  Pins the Z-algorithm window bookkeeping
//              (distinct from the Rabin-Karp rolling hash in #123/#132 and the
//              KMP failure table in #129).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress142RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress142RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress142RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress142RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress142RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress142RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress142RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress142RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress142TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Floyd tortoise-and-hare cycle detection over a rodata successor table.
    {p+"_cycle",
     "static const unsigned char "+p+"_next[32]={\n"
     "5,12,7,19,2,23,14,0, 9,30,4,17,28,1,21,8, 25,3,11,27,6,16,31,13, 20,10,29,15,24,18,22,26};\n"
     +t+" "+p+"_cycle("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int start=0;start<32;start++){\n"
     "      unsigned slow=(unsigned)start, fast=(unsigned)start, steps=0u;\n"
     "      do { slow=(unsigned)"+p+"_next[slow&31]^((s>>(slow&7))&1u);\n"
     "        unsigned f1=(unsigned)"+p+"_next[fast&31]^((s>>(fast&7))&1u);\n"
     "        fast=(unsigned)"+p+"_next[f1&31]^((s>>(f1&7))&1u); steps++;\n"
     "      } while(slow!=fast && steps<64u);\n"
     "      unsigned len=1u, cur=(unsigned)"+p+"_next[slow&31]^((s>>(slow&7))&1u);\n"
     "      while(cur!=slow && len<64u){ cur=(unsigned)"+p+"_next[cur&31]^((s>>(cur&7))&1u); len++; }\n"
     "      acc=acc*131u+slow+len*7u+steps; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Cu}, "OptStress142", 2},

    // palindrome census by symmetric center expansion over a rodata-seeded copy.
    {p+"_palin",
     "static const unsigned char "+p+"_str[24]={\n"
     "3,1,4,1,5,9,2,6, 5,3,5,8,9,7,9,3, 2,3,8,4,6,2,6,4};\n"
     +t+" "+p+"_palin("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s, total=0u, best=0u;\n"
     "    unsigned cs[24];\n"
     "    for(int i=0;i<24;i++) cs[i]=(unsigned)"+p+"_str[i]^((s>>(i&7))&1u);\n"
     "    for(int c=0;c<24;c++){\n"
     "      int l=c, r=c; unsigned rad=0u;\n"
     "      while(l>=0 && r<24 && cs[l]==cs[r]){ rad++; l--; r++; }\n"
     "      total+=rad; if(rad>best) best=rad; acc=acc*131u+rad;\n"
     "      int l2=c, r2=c+1; unsigned rad2=0u;\n"
     "      while(l2>=0 && r2<24 && cs[l2]==cs[r2]){ rad2++; l2--; r2++; }\n"
     "      total+=rad2; acc=acc*131u+rad2; }\n"
     "    acc=acc*131u+total+best; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x55u}, "OptStress142", 2},

    // Z-array construction (the [l,r] prefix-reuse match window) over rodata.
    {p+"_zalgo",
     "static const unsigned char "+p+"_t[28]={\n"
     "1,2,1,2,3,1,2,1,2,3,1,2, 4,1,2,1,2,3,1,2,1,2,3,1,2,4,1,2};\n"
     +t+" "+p+"_zalgo("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s, sumz=0u, maxz=0u;\n"
     "    unsigned z[28]; z[0]=28u; int l=0, r=0;\n"
     "    for(int i=1;i<28;i++){ int zi=0;\n"
     "      if(i<r){ int k=i-l, rem=r-i; zi=((int)z[k]<rem)?(int)z[k]:rem; }\n"
     "      while(i+zi<28 && ((unsigned)("+p+"_t[zi]^((s>>(zi&7))&1u))==(unsigned)("+p+"_t[i+zi]^((s>>((i+zi)&7))&1u)))) zi++;\n"
     "      if(i+zi>r){ l=i; r=i+zi; }\n"
     "      z[i]=(unsigned)zi; sumz+=(unsigned)zi; if((unsigned)zi>maxz) maxz=(unsigned)zi; acc=acc*131u+(unsigned)zi; }\n"
     "    acc=acc*131u+sumz+maxz; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x7Au}, "OptStress142", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress142TC("x64o142", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress142TC("x86o142", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress142TC("a64o142", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress142TC("armo142", "int");

INSTANTIATE_TEST_SUITE_P(OptStress142, X64OptStress142RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress142, X86OptStress142RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress142, A64OptStress142RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress142, ARM32OptStress142RT, ::testing::ValuesIn(kARM), rtTCName);
