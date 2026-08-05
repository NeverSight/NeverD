//===- AllPlatform_OptStress88RTTests.cpp - ptr-table 2nd-order ---*-C++*-=//
//
// Second-order interactions of the #483 pointer-table / frame-slot pointer-array
// symbolization, to flush out boundary cases:
//
//   * tabarg - a local int*[2]={A,B} whose ADDRESS escapes to a noinline callee
//              that indexes+derefs it (frame-slot store symbolization must hold
//              even when the array escapes, since the callee reads it directly).
//   * nesttab- a file-scope int*const T[2]={A,B} copied into a local int*[2],
//              both runtime-indexed (a table feeding a table).
//   * mixidx - a local int*[2]={A,B} read BOTH at a fixed index (t[0][..]) and a
//              runtime index (t[k][..]): exercises the frameSlotHasMatchingKeyLoad
//              boundary (a fixed-offset reload coexisting with the indexed read).
//   * swp4   - a local int*[4] from four globals, switch-selected then indexed.
//
// All integer, arrays seeded from the LCG, fold to one integer return.  No
// float / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress88RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress88RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress88RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress88RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress88RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress88RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress88RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress88RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress88TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // local int*[2]={A,B} whose address escapes to a noinline indexer.
    {p+"_tabarg",
     "static int "+p+"_A[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_B[16]={2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5};\n"
     "static int "+p+"_idx(int**,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_tabarg("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<150;i++){ s=s*1103515245u+12345u;\n"
     "    int *t[2]={"+p+"_A,"+p+"_B};\n"
     "    sum+="+p+"_idx(t,s); sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_A[1]+"+p+"_B[9]); }\n"
     "static int "+p+"_idx(int**t,unsigned s){\n"
     "  unsigned k=(s>>5)&1u, j=(s>>7)&15u;\n"
     "  t[k][j]+=(int)(s>>13); return t[k][j]; }\n",
     {0x81u}, "OptStress88", 2},

    // file-scope table copied into a local table, both runtime-indexed.
    {p+"_nesttab",
     "static int "+p+"_NA[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_NB[16]={2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5};\n"
     "static int *const "+p+"_NT[2]={"+p+"_NA,"+p+"_NB};\n"
     +t+" "+p+"_nesttab("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<150;i++){ s=s*1103515245u+12345u;\n"
     "    int *t[2]={"+p+"_NT[(s>>3)&1u],"+p+"_NT[(s>>4)&1u]};\n"
     "    unsigned k=(s>>5)&1u, j=(s>>7)&15u;\n"
     "    t[k][j]+=(int)(s>>13); sum+=t[k][j]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_NA[2]+"+p+"_NB[11]); }\n",
     {0x82u}, "OptStress88", 2},

    // local int*[2]={A,B} read at BOTH a fixed index and a runtime index.
    {p+"_mixidx",
     "static int "+p+"_MA[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_MB[16]={2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5};\n"
     +t+" "+p+"_mixidx("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<150;i++){ s=s*1103515245u+12345u;\n"
     "    int *t[2]={"+p+"_MA,"+p+"_MB};\n"
     "    unsigned k=(s>>5)&1u, j=(s>>7)&15u;\n"
     "    int r0=t[0][(s>>9)&15u];\n"
     "    t[k][j]+=(int)(s>>13)+r0; sum+=t[k][j]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_MA[3]+"+p+"_MB[7]); }\n",
     {0x83u}, "OptStress88", 2},

    // local int*[4] from four globals, switch-selected then indexed.
    {p+"_swp4",
     "static int "+p+"_S0[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_S1[8]={5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_S2[8]={2,7,1,8,2,8,1,8};\n"
     "static int "+p+"_S3[8]={1,6,1,8,0,3,3,9};\n"
     +t+" "+p+"_swp4("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    int *t[4]={"+p+"_S0,"+p+"_S1,"+p+"_S2,"+p+"_S3};\n"
     "    unsigned k=(s>>4)&3u, j=(s>>7)&7u;\n"
     "    t[k][j]+=(int)(s>>13); sum+=t[k][j]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_S0[1]+"+p+"_S3[6]); }\n",
     {0x84u}, "OptStress88", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress88TC("x64o88", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress88TC("x86o88", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress88TC("a64o88", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress88TC("armo88", "int");

INSTANTIATE_TEST_SUITE_P(OptStress88, X64OptStress88RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress88, X86OptStress88RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress88, A64OptStress88RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress88, ARM32OptStress88RT, ::testing::ValuesIn(kARM), rtTCName);
