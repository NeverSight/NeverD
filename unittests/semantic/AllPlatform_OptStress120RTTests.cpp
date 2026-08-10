//===- AllPlatform_OptStress120RTTests.cpp - sort / MST / sieve shapes -----==//
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
//   * qsort  - iterative quicksort (Lomuto partition) of a rodata-seeded stack
//              array using an explicit (lo,hi) range stack.  Pins partition pivot
//              swaps plus a manually managed recursion stack (distinct from the
//              heap / counting sorts).
//   * prim   - Prim minimum spanning tree on a 6x6 rodata weight matrix: greedy
//              minimum-key vertex selection with key relaxation `w<key[v]`.  Pins
//              an MST key-update greedy over a rodata 2D table (distinct from the
//              Dijkstra `dist[u]+w` relaxation).
//   * sieve  - Sieve of Eratosthenes in a stack bitset with a rodata-weighted
//              prime reduce: composite marking strides `j=i*i; j+=i`.  Pins a
//              quadratic-start strided write pattern plus a rodata gather reduce.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress120RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress120RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress120RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress120RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress120RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress120RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress120RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress120RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress120TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // iterative quicksort (Lomuto) of a rodata-seeded stack array (range stack).
    {p+"_qsort",
     "static const unsigned char "+p+"_data[24]={\n"
     "0x3a,0x91,0x47,0xee,0x12,0x8d,0x5b,0xc6, 0x29,0xf0,0x74,0xa3,0x1e,0x6c,0xd8,0x05,\n"
     "0x9f,0x33,0xb7,0x4a,0xe1,0x58,0x82,0x2d};\n"
     +t+" "+p+"_qsort("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned v[24]; for(int i=0;i<24;i++) v[i]="+p+"_data[i]^((s>>(i&7))&0xFu);\n"
     "    int stk[40]; int sp=0; stk[sp++]=0; stk[sp++]=23;\n"
     "    while(sp>=2){ int hi=stk[--sp], lo=stk[--sp];\n"
     "      if(lo>=hi) continue;\n"
     "      unsigned pivot=v[hi]; int i=lo-1;\n"
     "      for(int j=lo;j<hi;j++){ if(v[j]<=pivot){ i++; unsigned tp=v[i]; v[i]=v[j]; v[j]=tp; } }\n"
     "      i++; unsigned tp=v[i]; v[i]=v[hi]; v[hi]=tp;\n"
     "      if(sp<=36){ stk[sp++]=lo; stk[sp++]=i-1; stk[sp++]=i+1; stk[sp++]=hi; } }\n"
     "    for(int i=0;i<24;i++) acc=acc*131u+v[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x71u}, "OptStress120", 2},

    // Prim minimum spanning tree over a 6x6 rodata weight matrix (min-key greedy).
    {p+"_prim",
     "static const unsigned char "+p+"_w[36]={\n"
     "0,7,99,3,99,12, 7,0,4,99,9,99, 99,4,0,6,99,2, 3,99,6,0,8,99, 99,9,99,8,0,5, 12,99,2,99,5,0};\n"
     +t+" "+p+"_prim("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<64;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned key[6], inMST[6];\n"
     "    for(int i=0;i<6;i++){ key[i]=200u; inMST[i]=0u; }\n"
     "    key[(s>>3)%6u]=0u; unsigned total=0u;\n"
     "    for(int c=0;c<6;c++){ unsigned u=6u, best=201u;\n"
     "      for(int i=0;i<6;i++) if(!inMST[i] && key[i]<best){ best=key[i]; u=(unsigned)i; }\n"
     "      if(u==6u) break; inMST[u]=1u; total+=key[u];\n"
     "      for(int v=0;v<6;v++){ unsigned w="+p+"_w[u*6u+(unsigned)v];\n"
     "        if(w<99u && !inMST[v] && w<key[v]) key[v]=w; } }\n"
     "    acc=acc*131u+total; for(int i=0;i<6;i++) acc=acc*131u+key[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x9Du}, "OptStress120", 2},

    // Sieve of Eratosthenes (stack bitset) with a rodata-weighted prime reduce.
    {p+"_sieve",
     "static const unsigned char "+p+"_wt[16]={3,9,1,7,12,5,2,14,6,10,4,8,15,0,13,11};\n"
     +t+" "+p+"_sieve("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned char comp[120]; for(int i=0;i<120;i++) comp[i]=0;\n"
     "    unsigned bias=(s>>5)&3u;\n"
     "    for(int i=2;i*i<120;i++){ if(!comp[i]){ for(int j=i*i;j<120;j+=i) comp[j]=1; } }\n"
     "    unsigned cnt=0u, mix=s;\n"
     "    for(int i=2;i<120;i++){ if(!comp[i]){ cnt++; mix=mix*131u+(unsigned)i+"+p+"_wt[i&15]+bias; } }\n"
     "    acc=acc*131u+cnt+mix;\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x53u}, "OptStress120", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress120TC("x64o120", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress120TC("x86o120", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress120TC("a64o120", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress120TC("armo120", "int");

INSTANTIATE_TEST_SUITE_P(OptStress120, X64OptStress120RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress120, X86OptStress120RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress120, A64OptStress120RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress120, ARM32OptStress120RT, ::testing::ValuesIn(kARM), rtTCName);
