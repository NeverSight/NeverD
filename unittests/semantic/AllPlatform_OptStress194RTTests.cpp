//===- AllPlatform_OptStress194RTTests.cpp - Fletcher / Sunday / Boruvka ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
//
//   * fletcher  - Fletcher-16 checksum over rodata bytes: two running sums folded
//                 mod 255 (a compile-time-constant modulus, so a magic multiply,
//                 never a divide libcall).  Pins a two-accumulator modular
//                 checksum (distinct from Adler-32 mod-65521 and CRC table folds).
//   * sunday    - Sunday substring search using a bad-character shift keyed on the
//                 symbol just past the window.  Pins a skip-table string scan
//                 (distinct from the KMP automaton, Z-function and Rabin-Karp
//                 rolling hash, and from the Horspool window-tail variant #104).
//   * boruvka   - Boruvka minimum spanning tree: every round each component picks
//                 its cheapest outgoing edge, then components merge.  Pins a
//                 component-parallel cheapest-edge MST (distinct from Kruskal's
//                 edge sort #190 and Prim's single-frontier #190).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress194RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress194RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress194RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress194RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress194RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress194RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress194RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress194RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress194TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Fletcher-16 checksum over rodata bytes (two running sums mod 255).
    {p+"_fletcher",
     "static const unsigned char "+p+"_fl[24]={37,12,58,4,29,61,7,44,18,53,2,40,25,9,49,31,16,52,3,47,22,60,11,38};\n"
     +t+" "+p+"_fletcher("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned s1=0u, s2=0u;\n"
     "    for(int i=0;i<24;i++){ unsigned d=((unsigned)"+p+"_fl[i]^((s>>(i&7))&255u))&255u;\n"
     "      s1=(s1+d)%255u; s2=(s2+s1)%255u; }\n"
     "    unsigned cksum=(s2<<8)|s1;\n"
     "    acc=acc*131u+cksum; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xE1u}, "OptStress194", 2},

    // Sunday substring search via a bad-character shift over a small alphabet.
    {p+"_sunday",
     "static const unsigned char "+p+"_sp[5]={2,5,1,5,2};\n"
     "static const unsigned char "+p+"_st[40]={\n"
     "2,5,1,5,2,3,0,6, 4,2,5,1,5,2,7,1, 3,2,5,1,5,2,0,4, 6,2,5,1,5,2,3,7, 1,0,2,5,1,5,2,6};\n"
     +t+" "+p+"_sunday("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned pat[5]; for(int i=0;i<5;i++) pat[i]=((unsigned)"+p+"_sp[i]^(s&1u))&7u;\n"
     "    unsigned txt[40]; for(int i=0;i<40;i++) txt[i]=((unsigned)"+p+"_st[i]^((s>>(i&7))&1u))&7u;\n"
     "    int M=5, Nn=40; int shift[8]; for(int c=0;c<8;c++) shift[c]=M+1;\n"
     "    for(int i=0;i<M;i++) shift[pat[i]]=M-i;\n"
     "    unsigned hits=0u; int i=0;\n"
     "    while(i<=Nn-M){ int k=0; while(k<M && txt[i+k]==pat[k]) k++;\n"
     "      if(k==M) hits++;\n"
     "      if(i+M<Nn) i+=shift[txt[i+M]]; else break; }\n"
     "    acc=acc*131u+hits*131u+(unsigned)i; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xE2u}, "OptStress194", 2},

    // Boruvka MST: per-round cheapest outgoing edge per component, then merge.
    {p+"_boruvka",
     "static const unsigned char "+p+"_bv[27]={0,1,4, 1,2,6, 2,3,5, 3,4,3, 4,5,7, 5,6,2, 6,0,8, 0,3,9, 1,4,5};\n"
     +t+" "+p+"_boruvka("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int V=7, E=9; unsigned eu[9],ev[9],ew[9];\n"
     "    for(int i=0;i<9;i++){ eu[i]=(unsigned)"+p+"_bv[i*3]; ev[i]=(unsigned)"+p+"_bv[i*3+1];\n"
     "      ew[i]=((unsigned)"+p+"_bv[i*3+2]^((s>>(i&7))&3u))&63u; }\n"
     "    int comp[7]; for(int i=0;i<7;i++) comp[i]=i;\n"
     "    unsigned total=0u, numComp=(unsigned)V;\n"
     "    for(int round=0; round<V && numComp>1u; round++){\n"
     "      int cheap[7]; for(int i=0;i<7;i++) cheap[i]=-1;\n"
     "      for(int e=0;e<E;e++){ int cu=(int)eu[e]; while(comp[cu]!=cu) cu=comp[cu];\n"
     "        int cv=(int)ev[e]; while(comp[cv]!=cv) cv=comp[cv];\n"
     "        if(cu==cv) continue;\n"
     "        if(cheap[cu]==-1 || ew[e]<ew[cheap[cu]]) cheap[cu]=e;\n"
     "        if(cheap[cv]==-1 || ew[e]<ew[cheap[cv]]) cheap[cv]=e; }\n"
     "      for(int c=0;c<V;c++){ int e=cheap[c]; if(e==-1) continue;\n"
     "        int cu=(int)eu[e]; while(comp[cu]!=cu) cu=comp[cu];\n"
     "        int cv=(int)ev[e]; while(comp[cv]!=cv) cv=comp[cv];\n"
     "        if(cu!=cv){ comp[cu]=cv; total+=ew[e]; numComp--; } }\n"
     "      acc=acc*131u+total*7u+numComp; }\n"
     "    acc=acc*131u+total+numComp; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xE3u}, "OptStress194", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress194TC("x64o194", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress194TC("x86o194", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress194TC("a64o194", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress194TC("armo194", "int");

INSTANTIATE_TEST_SUITE_P(OptStress194, X64OptStress194RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress194, X86OptStress194RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress194, A64OptStress194RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress194, ARM32OptStress194RT, ::testing::ValuesIn(kARM), rtTCName);
