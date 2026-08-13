//===- AllPlatform_OptStress196RTTests.cpp - maxsquare / saddle / magic ====//
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
//   * maxsquare - maximal all-ones square in a rodata 6x6 bit grid by the
//                 dp[i][j]=1+min(left,up,up-left) recurrence.  Pins a 2-D min
//                 square DP (distinct from the additive grid DPs and the interval
//                 DPs #191 — here three neighbours min into a side length).
//   * saddle    - saddle-point census of a rodata 6x6 matrix: each cell that is
//                 the minimum of its row and the maximum of its column.  Pins a
//                 per-cell row/column extremal scan (distinct from any reduce).
//   * magic     - magic-square verification of a rodata 4x4 matrix: all row, all
//                 column and both diagonal sums compared to the first-row sum.
//                 Pins a multi-axis sum-equality check (distinct from a plain sum).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress196RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress196RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress196RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress196RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress196RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress196RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress196RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress196RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress196TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // maximal all-ones square in a rodata 6x6 bit grid (min-of-3 DP).
    {p+"_maxsquare",
     "static const unsigned char "+p+"_mq[36]={5,2,7,4,6,1, 3,5,2,7,4,6, 7,4,6,1,5,2, 2,7,4,6,1,5, 6,1,5,2,7,4, 4,6,1,5,2,7};\n"
     +t+" "+p+"_maxsquare("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int R=6,C=6; unsigned g[36];\n"
     "    for(int i=0;i<36;i++) g[i]=((((unsigned)"+p+"_mq[i])^((s>>(i&7))&7u))>=4u)?1u:0u;\n"
     "    unsigned dp[36]; unsigned best=0u;\n"
     "    for(int i=0;i<R;i++) for(int j=0;j<C;j++){ int id=i*C+j;\n"
     "      if(g[id]==0u) dp[id]=0u;\n"
     "      else if(i==0||j==0) dp[id]=1u;\n"
     "      else { unsigned l=dp[id-1], u=dp[id-C], d=dp[id-C-1];\n"
     "        unsigned mn=l<u?l:u; mn=mn<d?mn:d; dp[id]=mn+1u; }\n"
     "      if(dp[id]>best) best=dp[id]; }\n"
     "    acc=acc*131u+best; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x16u}, "OptStress196", 2},

    // saddle-point census of a rodata 6x6 matrix (row-min AND col-max).
    {p+"_saddle",
     "static const unsigned char "+p+"_sd[36]={40,12,55,4,29,61, 7,44,18,53,2,40, 25,9,49,31,16,52, 3,47,22,60,11,38, 6,50,27,14,58,1, 33,19,45,8,36,23};\n"
     +t+" "+p+"_saddle("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int R=6,C=6; unsigned M[36];\n"
     "    for(int i=0;i<36;i++) M[i]=((unsigned)"+p+"_sd[i])^((s>>(i&7))&7u);\n"
     "    unsigned found=0u, fold=0u;\n"
     "    for(int i=0;i<R;i++) for(int j=0;j<C;j++){ unsigned val=M[i*C+j];\n"
     "      int minRow=1; for(int c=0;c<C;c++) if(M[i*C+c]<val){ minRow=0; break; }\n"
     "      int maxCol=1; for(int r=0;r<R;r++) if(M[r*C+j]>val){ maxCol=0; break; }\n"
     "      if(minRow && maxCol){ found++; fold=fold*131u+(unsigned)(i*C+j)+val; } }\n"
     "    acc=acc*131u+found*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x17u}, "OptStress196", 2},

    // magic-square verification of a rodata 4x4 matrix (rows/cols/diagonals).
    {p+"_magic",
     "static const unsigned char "+p+"_ms[16]={16,3,2,13, 5,10,11,8, 9,6,7,12, 4,15,14,1};\n"
     +t+" "+p+"_magic("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int n=4; unsigned M[16];\n"
     "    for(int i=0;i<16;i++) M[i]=((unsigned)"+p+"_ms[i])^((s>>(i&7))&3u);\n"
     "    unsigned target=0u; for(int j=0;j<n;j++) target+=M[j];\n"
     "    unsigned ok=1u, diff=0u;\n"
     "    for(int i=0;i<n;i++){ unsigned rs=0u; for(int j=0;j<n;j++) rs+=M[i*n+j];\n"
     "      if(rs!=target) ok=0u; diff+=(rs>target?rs-target:target-rs); }\n"
     "    for(int j=0;j<n;j++){ unsigned cs=0u; for(int i=0;i<n;i++) cs+=M[i*n+j];\n"
     "      if(cs!=target) ok=0u; diff+=(cs>target?cs-target:target-cs); }\n"
     "    unsigned d1=0u,d2=0u; for(int i=0;i<n;i++){ d1+=M[i*n+i]; d2+=M[i*n+(n-1-i)]; }\n"
     "    if(d1!=target||d2!=target) ok=0u;\n"
     "    acc=acc*131u+ok*1311u+diff+target; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x18u}, "OptStress196", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress196TC("x64o196", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress196TC("x86o196", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress196TC("a64o196", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress196TC("armo196", "int");

INSTANTIATE_TEST_SUITE_P(OptStress196, X64OptStress196RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress196, X86OptStress196RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress196, A64OptStress196RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress196, ARM32OptStress196RT, ::testing::ValuesIn(kARM), rtTCName);
