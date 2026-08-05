//===- AllPlatform_OptStress129RTTests.cpp - KMP / union-find / Fenwick ===//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * kmp    - Knuth-Morris-Pratt failure-function build over a rodata pattern
//              then a single-pass scan of a rodata text counting matches.  Pins
//              a prefix-automaton with `k=fail[k-1]` fallback (distinct from the
//              LCS / edit-distance DP tables, which never backtrack an index).
//   * dsu    - union-find over rodata edge pairs with full path compression and
//              a component count.  Pins a pointer-chasing forest walk + in-place
//              parent rewrite (distinct from any matrix/array DP shape).
//   * fenwick- binary-indexed (Fenwick) tree built from rodata deltas with
//              low-bit-stride `idx += idx & -idx` updates and prefix queries.
//              Pins a low-bit traversal lattice (distinct from a flat prefix sum).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress129RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress129RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress129RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress129RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress129RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress129RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress129RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress129RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress129TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Knuth-Morris-Pratt failure function + single-pass match count over rodata.
    {p+"_kmp",
     "static const unsigned char "+p+"_pat[12]={1,2,1,2,3,1,2,1,2,3,4,1};\n"
     "static const unsigned char "+p+"_txt[48]={\n"
     "1,2,1,2,3,1,2,1,2,3,4,1, 5,1,2,1,2,3,1,2,1,2,3,4, 1,3,2,1,2,1,2,3,1,2,1,2, 3,4,1,7,1,2,3,1,2,1,2,3};\n"
     +t+" "+p+"_kmp("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned pat[12], fail[12];\n"
     "    for(int i=0;i<12;i++) pat[i]="+p+"_pat[i]^((s>>(i&7))&1u);\n"
     "    fail[0]=0u; unsigned k=0u;\n"
     "    for(int i=1;i<12;i++){ while(k>0u && pat[i]!=pat[k]) k=fail[k-1u];\n"
     "      if(pat[i]==pat[k]) k++; fail[i]=k; }\n"
     "    unsigned q=0u, matches=0u;\n"
     "    for(int i=0;i<48;i++){ unsigned c="+p+"_txt[i]^((s>>(i&7))&1u);\n"
     "      while(q>0u && c!=pat[q]) q=fail[q-1u];\n"
     "      if(c==pat[q]) q++;\n"
     "      if(q==12u){ matches++; q=fail[q-1u]; } acc=acc*131u+q; }\n"
     "    acc=acc*131u+matches; for(int i=0;i<12;i++) acc=acc*131u+fail[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x71u}, "OptStress129", 2},

    // union-find with path compression over rodata edge pairs, count components.
    {p+"_dsu",
     "static const unsigned char "+p+"_eu[24]={\n"
     "0,1, 2,3, 1,2, 4,5, 6,7, 5,6, 8,9, 3,8, 10,11, 7,10, 12,13, 11,12};\n"
     +t+" "+p+"_dsu("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned par[16]; for(int i=0;i<16;i++) par[i]=(unsigned)i;\n"
     "    for(int e=0;e<12;e++){ if(((s>>(e&7))&1u)==0u) continue;\n"
     "      unsigned x="+p+"_eu[e*2]&15u, y="+p+"_eu[e*2+1]&15u;\n"
     "      unsigned rx=x; while(par[rx]!=rx) rx=par[rx];\n"
     "      unsigned ry=y; while(par[ry]!=ry) ry=par[ry];\n"
     "      while(par[x]!=rx){ unsigned nx=par[x]; par[x]=rx; x=nx; }\n"
     "      while(par[y]!=ry){ unsigned ny=par[y]; par[y]=ry; y=ny; }\n"
     "      if(rx!=ry) par[rx]=ry; }\n"
     "    unsigned comps=0u;\n"
     "    for(int i=0;i<16;i++){ if(par[i]==(unsigned)i) comps++; acc=acc*131u+par[i]; }\n"
     "    acc=acc*131u+comps; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Du}, "OptStress129", 2},

    // Fenwick (binary indexed) tree build + prefix queries over rodata deltas.
    {p+"_fenwick",
     "static const unsigned char "+p+"_d[16]={3,7,2,9,5,1,8,4,6,11,2,7,3,10,5,9};\n"
     +t+" "+p+"_fenwick("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned bit[17]; for(int i=0;i<17;i++) bit[i]=0u;\n"
     "    for(int i=0;i<16;i++){ unsigned v="+p+"_d[i]+((s>>(i&7))&3u); unsigned idx=(unsigned)i+1u;\n"
     "      while(idx<=16u){ bit[idx]+=v; idx+=idx&(0u-idx); } }\n"
     "    for(int q=0;q<16;q++){ unsigned idx=((s>>(q&7))&15u)+1u, sum=0u;\n"
     "      while(idx>0u){ sum+=bit[idx]; idx-=idx&(0u-idx); } acc=acc*131u+sum; }\n"
     "    for(int i=1;i<=16;i++) acc=acc*131u+bit[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x55u}, "OptStress129", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress129TC("x64o129", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress129TC("x86o129", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress129TC("a64o129", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress129TC("armo129", "int");

INSTANTIATE_TEST_SUITE_P(OptStress129, X64OptStress129RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress129, X86OptStress129RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress129, A64OptStress129RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress129, ARM32OptStress129RT, ::testing::ValuesIn(kARM), rtTCName);
