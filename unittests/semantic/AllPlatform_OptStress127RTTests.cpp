//===- AllPlatform_OptStress127RTTests.cpp - Huffman / treap / Gray shapes -=//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * huff   - canonical Huffman code-length assignment from rodata symbol
//              frequencies: a leaf-queue merge building code lengths, then a
//              rodata symbol gather by assigned length.  Pins a priority-queue
//              style pairwise combine over rodata weights.
//   * treap  - implicit treap in-order walk keyed by rodata priorities: each step
//              compares the runtime key against a rodata priority and branches
//              left/right via rodata child pointers.  Pins a BST walk driven by
//              rodata key/priority/child triples.
//   * gray   - Gray-code generation and rodata remap: for each runtime index i
//              emit `i^(i>>1)` then gather `tab[gray]`.  Pins a bit-transform
//              feeding a permuted rodata gather (distinct from bit-reversal).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress127RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress127RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress127RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress127RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress127RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress127RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress127RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress127RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress127TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // canonical Huffman code-length build from rodata symbol frequencies.
    {p+"_huff",
     "static const unsigned char "+p+"_freq[16]={\n"
     "5,12,3,18,7,2,9,1, 14,4,8,6,11,2,10,3};\n"
     +t+" "+p+"_huff("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned nodes[32]; int n=0;\n"
     "    for(int i=0;i<16;i++){ unsigned f="+p+"_freq[i]+((s>>(i&7))&1u); if(f) nodes[n++]=f; }\n"
     "    if(n<2){ nodes[n++]=1u; nodes[n++]=1u; }\n"
     "    unsigned depth[16]; for(int i=0;i<16;i++) depth[i]=0u;\n"
     "    while(n>1){ unsigned m0=0xFFFFu,m1=0xFFFFu, i0=0,i1=0;\n"
     "      for(int i=0;i<n;i++){ if(nodes[i]<m0){ m1=m0; i1=i0; m0=nodes[i]; i0=i; }\n"
     "        else if(nodes[i]<m1){ m1=nodes[i]; i1=i; } }\n"
     "      unsigned sum=m0+m1; nodes[i0]=sum; nodes[i1]=0xFFFFu; n--;\n"
     "      for(int i=0;i<16;i++) if("+p+"_freq[i]) depth[i]++; acc=acc*131u+sum; }\n"
     "    for(int i=0;i<16;i++) acc=acc*131u+depth[i]+"+p+"_freq[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x7Du}, "OptStress127", 2},

    // implicit treap in-order walk keyed by rodata priorities (BST descent).
    {p+"_treap",
     "static const unsigned char "+p+"_key[16]={\n"
     "50,25,75,12,37,62,87,6, 18,31,43,56,68,81,93,3};\n"
     "static const unsigned char "+p+"_pri[16]={\n"
     "7,3,9,1,5,8,2,0, 4,6,10,11,12,13,14,15};\n"
     "static const unsigned char "+p+"_ch[32]={\n"
     "1,2,3,4,5,6,7,8, 9,10,11,12,13,14,15,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0};\n"
     +t+" "+p+"_treap("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<24;q++){ unsigned target=(s>>(q&15))&0xFFu; int node=0; unsigned steps=0u;\n"
     "      while(node>=0 && node<16 && steps<32u){ unsigned k="+p+"_key[node]^((s>>2)&3u);\n"
     "        if(target==k){ acc=acc*131u+k+"+p+"_pri[node]; break; }\n"
     "        node=(target<k)?(int)"+p+"_ch[node*2]:(int)"+p+"_ch[node*2+1]; steps++; }\n"
     "      acc=acc*131u+steps; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x42u}, "OptStress127", 2},

    // Gray-code generation then rodata gather tab[gray(i)].
    {p+"_gray",
     "static const unsigned char "+p+"_tab[16]={\n"
     "0x3a,0x91,0x47,0xee,0x12,0x8d,0x5b,0xc6, 0x29,0xf0,0x74,0xa3,0x1e,0x6c,0xd8,0x05};\n"
     +t+" "+p+"_gray("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i<16;i++){ unsigned g=(unsigned)i^((unsigned)i>>1);\n"
     "      unsigned v="+p+"_tab[g&15u]^((s>>(i&7))&0xFu); acc=acc*131u+v+g; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x19u}, "OptStress127", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress127TC("x64o127", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress127TC("x86o127", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress127TC("a64o127", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress127TC("armo127", "int");

INSTANTIATE_TEST_SUITE_P(OptStress127, X64OptStress127RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress127, X86OptStress127RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress127, A64OptStress127RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress127, ARM32OptStress127RT, ::testing::ValuesIn(kARM), rtTCName);
