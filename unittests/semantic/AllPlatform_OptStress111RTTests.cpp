//===- AllPlatform_OptStress111RTTests.cpp - DP / graph rodata shapes ------==//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * floyd  - Floyd-Warshall all-pairs shortest paths on a 6x6 rodata weight
//              matrix copied to a stack scratch, relaxed by the triple loop
//              `d[i][j]=min(d[i][j],d[i][k]+d[k][j])`.  Pins a 2D matrix DP with
//              three independent strided indices over a rodata-seeded table.
//   * leven  - Levenshtein edit distance between two rodata strings via a rolling
//              one-row DP (`min(del,ins,sub)`).  Pins a min-of-three DP
//              recurrence reading two rodata arrays.
//   * base36 - base-36 digit expansion: repeated CONSTANT modulo/divide of a
//              runtime value indexing a rodata digit alphabet (`digits[v%36]`).
//              Pins a divmod-by-constant chain feeding an alphabet gather.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress111RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress111RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress111RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress111RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress111RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress111RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress111RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress111RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress111TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Floyd-Warshall all-pairs shortest paths over a 6x6 rodata weight matrix.
    {p+"_floyd",
     "static const unsigned char "+p+"_adj[36]={\n"
     "0,7,99,3,99,12, 7,0,4,99,9,99, 99,4,0,6,99,2, 3,99,6,0,8,99, 99,9,99,8,0,5, 12,99,2,99,5,0};\n"
     +t+" "+p+"_floyd("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<64;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned d[36];\n"
     "    for(int i=0;i<36;i++) d[i]="+p+"_adj[i]+((s>>(i&15))&1u);\n"
     "    for(int k=0;k<6;k++) for(int i=0;i<6;i++) for(int j=0;j<6;j++){\n"
     "      unsigned nd=d[i*6+k]+d[k*6+j]; if(nd<d[i*6+j]) d[i*6+j]=nd; }\n"
     "    for(int i=0;i<36;i++) acc=acc*131u+d[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xF1u}, "OptStress111", 2},

    // Levenshtein edit distance between two rodata strings (rolling-row DP).
    {p+"_leven",
     "static const unsigned char "+p+"_sa[12]={3,9,1,7,12,5,2,14,6,10,4,8};\n"
     "static const unsigned char "+p+"_sb[12]={9,1,7,3,5,12,14,2,10,6,8,4};\n"
     +t+" "+p+"_leven("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned prev[13], cur[13];\n"
     "    for(int j=0;j<=12;j++) prev[j]=(unsigned)j;\n"
     "    for(int i=1;i<=12;i++){ cur[0]=(unsigned)i;\n"
     "      unsigned ca=("+p+"_sa[i-1]^(s&3u))&0xFu;\n"
     "      for(int j=1;j<=12;j++){ unsigned cb=("+p+"_sb[j-1]^((s>>2)&3u))&0xFu;\n"
     "        unsigned cost=(ca==cb)?0u:1u;\n"
     "        unsigned del=prev[j]+1u, ins=cur[j-1]+1u, sub=prev[j-1]+cost;\n"
     "        unsigned m=del<ins?del:ins; if(sub<m) m=sub; cur[j]=m; }\n"
     "      for(int j=0;j<=12;j++) prev[j]=cur[j]; }\n"
     "    acc=acc*131u+prev[12];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x1Eu}, "OptStress111", 2},

    // base-36 digit expansion: divmod-by-constant chain + rodata alphabet gather.
    {p+"_base36",
     "static const unsigned char "+p+"_digits[36]={\n"
     "48,49,50,51,52,53,54,55,56,57, 65,66,67,68,69,70,71,72,73,74,\n"
     "75,76,77,78,79,80,81,82,83,84, 85,86,87,88,89,90};\n"
     +t+" "+p+"_base36("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<32;q++){ unsigned v=(s>>(q&15))^((unsigned)q*2654435761u); unsigned h=0;\n"
     "      for(int d=0;d<7;d++){ unsigned r=v%36u; v/=36u; h=h*131u+"+p+"_digits[r]; }\n"
     "      acc=acc*131u+h; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x36u}, "OptStress111", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress111TC("x64o111", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress111TC("x86o111", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress111TC("a64o111", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress111TC("armo111", "int");

INSTANTIATE_TEST_SUITE_P(OptStress111, X64OptStress111RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress111, X86OptStress111RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress111, A64OptStress111RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress111, ARM32OptStress111RT, ::testing::ValuesIn(kARM), rtTCName);
