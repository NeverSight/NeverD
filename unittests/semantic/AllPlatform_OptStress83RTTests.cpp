//===- AllPlatform_OptStress83RTTests.cpp - struct ptr return/write -*-C++*-=//
//
// #478 fixed a global pointer carried INTO a callee through a by-value struct.
// These probes push the dual and neighbouring shapes:
//
//   * structret - a noinline helper RETURNS a by-value struct {int*;int} whose
//                 pointer field is a global array (x86-64 RAX:RDX / AArch64
//                 X0:X1 multi-register return); the returned global pointer must
//                 be symbolized in the struct-return path, not just the caller's.
//   * structwr  - a by-value struct {int*;int} carries a global pointer to a
//                 noinline callee that STORES through the field (write-through),
//                 the caller reads the global back (must alias the same global).
//   * gprow     - a noinline helper returns a row pointer &M[k] of a 2D global
//                 (a returned computed global element address), caller indexes it.
//   * gpcswap   - a local int*[2] holds two global bases, conditionally swapped,
//                 then indexed (a stack array of global pointers, no escape).
//                 All four targets: on x86-64 / AArch64 clang materializes the
//                 `{E,F}` initializer as a `.data.rel.ro` POINTER TABLE loaded
//                 whole (i128) into the stack array.  tryResolveGlobalData now
//                 routes a non-mutable segment carrying relocated pointer slots
//                 through buildCodePtrSegmentGlobal, so each table entry is
//                 emitted as `ptrtoint(@recompiled_data)` rather than a stale VA
//                 (#483).  i386/ARM32 reach the same relocated table too.
//
// All integer, arrays seeded from the LCG, fold to one integer return.  No
// float / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress83RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress83RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress83RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress83RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress83RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress83RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress83RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress83RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress83TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // noinline returns a by-value struct {int*;int} with a global pointer field.
    {p+"_structret",
     "static int "+p+"_A[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_B[16]={2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5};\n"
     "struct "+p+"_sp{ int *q; int n; };\n"
     "static struct "+p+"_sp "+p+"_mk(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_structret("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    struct "+p+"_sp r="+p+"_mk(s); r.q[r.n]+=(int)(s>>13);\n"
     "    sum+=r.q[r.n]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_A[2]+"+p+"_B[11]); }\n"
     "static struct "+p+"_sp "+p+"_mk(unsigned s){\n"
     "  struct "+p+"_sp r; r.q=(s&8u)?"+p+"_A:"+p+"_B; r.n=(int)((s>>5)&15u); return r; }\n",
     {0xB1u}, "OptStress83", 2},

    // by-value struct {int*;int} -> noinline callee STORES through the field.
    {p+"_structwr",
     "static int "+p+"_C[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_D[16]={2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5};\n"
     "struct "+p+"_sw{ int *q; int n; };\n"
     "static void "+p+"_put(struct "+p+"_sw,int) __attribute__((noinline));\n"
     +t+" "+p+"_structwr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    struct "+p+"_sw sp; sp.q=(s&8u)?"+p+"_C:"+p+"_D; sp.n=(int)((s>>5)&15u);\n"
     "    "+p+"_put(sp,(int)(s>>13)); sum+=sp.q[sp.n]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_C[2]+"+p+"_D[11]); }\n"
     "static void "+p+"_put(struct "+p+"_sw sp,int v){ sp.q[sp.n]+=v; }\n",
     {0xB2u}, "OptStress83", 2},

    // noinline returns a row pointer &M[k] of a 2D global, caller indexes it.
    {p+"_gprow",
     "static int "+p+"_M[8][8];\n"
     "static int* "+p+"_row(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_gprow("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int h=0;\n"
     "  for(int i=0;i<8;i++) for(int j=0;j<8;j++){ s=s*1103515245u+12345u;\n"
     "    "+p+"_M[i][j]=(int)(s>>11); }\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    int *r="+p+"_row(s); unsigned c=(s>>9)&7u;\n"
     "    r[c]+=(int)(s>>13); h=h*131+r[c]; h^=h>>7; }\n"
     "  return ("+t+")h; }\n"
     "static int* "+p+"_row(unsigned s){ return "+p+"_M[(s>>5)&7u]; }\n",
     {0xB3u}, "OptStress83", 2},
  };
}

// local int*[2] holds two global bases, conditionally swapped, then indexed.
// All four targets (see file header: the wide pointer-table relocation is fixed).
static RoundTripTC makeGpcswapTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {p+"_gpcswap",
     "static int "+p+"_E[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_F[16]={2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5};\n"
     +t+" "+p+"_gpcswap("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    int *t[2]={"+p+"_E,"+p+"_F};\n"
     "    if(s&16u){ int *tmp=t[0]; t[0]=t[1]; t[1]=tmp; }\n"
     "    unsigned k=(s>>5)&1u, j=(s>>7)&15u;\n"
     "    t[k][j]+=(int)(s>>13); sum+=t[k][j]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_E[1]+"+p+"_F[9]); }\n",
     {0xB4u}, "OptStress83", 2};
}
// clang-format on

static std::vector<RoundTripTC> withSwap(std::vector<RoundTripTC> V,
                                         const char *p, const char *T) {
  V.push_back(makeGpcswapTC(p, T));
  return V;
}

static const std::vector<RoundTripTC> kX64 =
    withSwap(makeOptStress83TC("x64o83", "long"), "x64o83", "long");
static const std::vector<RoundTripTC> kX86 =
    withSwap(makeOptStress83TC("x86o83", "int"), "x86o83", "int");
static const std::vector<RoundTripTC> kA64 =
    withSwap(makeOptStress83TC("a64o83", "long"), "a64o83", "long");
static const std::vector<RoundTripTC> kARM =
    withSwap(makeOptStress83TC("armo83", "int"), "armo83", "int");

INSTANTIATE_TEST_SUITE_P(OptStress83, X64OptStress83RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress83, X86OptStress83RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress83, A64OptStress83RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress83, ARM32OptStress83RT, ::testing::ValuesIn(kARM), rtTCName);
