//===- AllPlatform_OptStress158RTTests.cpp - anagram / brackets / dedup =//
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
//   * anagram - anagram test of two rodata strings by a shared frequency table:
//               one string increments counts, the other decrements, and a zero
//               residue means equal multisets.  Pins a count up/down multiset
//               compare (distinct from any sort-then-compare).
//   * bracket - balanced-bracket matching over a rodata token stream with an
//               explicit stack of openers, checking each closer against the top.
//               Pins a stack matcher (distinct from the RPN evaluator in #155 and
//               the quicksort recursion stack in #151).
//   * dedup   - first-occurrence de-duplication of a rodata stream using a seen
//               bitmask to keep only the earliest copy of each value.  Pins a
//               bitmask membership filter (distinct from the run-length encoder
//               in #147 and any sort).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress158RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress158RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress158RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress158RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress158RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress158RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress158RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress158RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress158TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // anagram test of two rodata strings via a shared count up/down table.
    {p+"_anagram",
     "static const unsigned char "+p+"_aa[20]={3,7,1,9,4,12,2,8,5,0,11,6,14,10,13,1,7,3,9,2};\n"
     "static const unsigned char "+p+"_ab[20]={9,3,12,1,7,2,8,4,0,5,6,11,10,14,1,13,3,9,7,2};\n"
     +t+" "+p+"_anagram("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned cnt[16]; for(int i=0;i<16;i++) cnt[i]=0u;\n"
     "    for(int i=0;i<20;i++){ cnt[((unsigned)"+p+"_aa[i]^((s>>(i&7))&1u))&15u]++; cnt[((unsigned)"+p+"_ab[i]^((s>>((i+3)&7))&1u))&15u]--; }\n"
     "    unsigned isana=1u; for(int i=0;i<16;i++){ if(cnt[i]!=0u) isana=0u; acc=acc*131u+cnt[i]; }\n"
     "    acc=acc*131u+isana; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x22u}, "OptStress158", 2},

    // balanced-bracket matching over a rodata token stream (explicit stack).
    {p+"_bracket",
     "static const unsigned char "+p+"_br[32]={1,2,12,11,3,13,1,1,11,11,2,3,13,12,1,11,2,2,12,12,3,1,11,13,1,3,13,11,2,12,1,11};\n"
     +t+" "+p+"_bracket("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned stk[32]; int sp=0; unsigned ok=1u,bad=0u;\n"
     "    for(int i=0;i<32;i++){ unsigned c=(unsigned)"+p+"_br[(i+(int)(s&7u))&31];\n"
     "      if(c>=1u && c<=3u){ if(sp<32) stk[sp++]=c; }\n"
     "      else if(c>=11u && c<=13u){ if(sp>0 && stk[sp-1]==c-10u) sp--; else { ok=0u; bad++; } }\n"
     "      acc=acc*131u+(unsigned)sp+ok; }\n"
     "    acc=acc*131u+ok+bad+(unsigned)sp; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x33u}, "OptStress158", 2},

    // first-occurrence de-duplication of a rodata stream (seen bitmask).
    {p+"_dedup",
     "static const unsigned char "+p+"_dd[24]={3,3,7,1,7,1,9,9,4,2,2,3,8,8,8,5,1,6,6,3,7,7,2,2};\n"
     +t+" "+p+"_dedup("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[24]; for(int i=0;i<24;i++) arr[i]=((unsigned)"+p+"_dd[i]^((s>>(i&7))&1u))&15u;\n"
     "    unsigned seen=0u,w=0u;\n"
     "    for(int i=0;i<24;i++){ unsigned v=arr[i]; if(!((seen>>v)&1u)){ seen|=(1u<<v); arr[w++]=v; } acc=acc*131u+v; }\n"
     "    acc=acc*131u+w+seen; for(unsigned i=0;i<w;i++) acc=acc*131u+arr[i]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x64u}, "OptStress158", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress158TC("x64o158", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress158TC("x86o158", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress158TC("a64o158", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress158TC("armo158", "int");

INSTANTIATE_TEST_SUITE_P(OptStress158, X64OptStress158RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress158, X86OptStress158RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress158, A64OptStress158RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress158, ARM32OptStress158RT, ::testing::ValuesIn(kARM), rtTCName);
