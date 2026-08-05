//===- AllPlatform_OptStress334RTTests.cpp - deeper pointer symbolization ===//
//
// Extends the OptStress74-76 / #473-476 pointer-symbolization family (global
// addresses crossing memory and call boundaries) with indirection shapes those
// batches did not cover.  The lifter must keep every relocated address symbolized
// (a real global, never a frozen literal) so the recompiled program chases the
// SAME globals; a single de-symbolized level reads a stale absolute address.
// Every kernel only ever DEREFERENCES pointers — it never observes a numeric
// address — so the result is independent of the (differing) recompiled layout.
//
//   * _interior : global pointers initialized to the INTERIOR of a global array
//                 (`&arr[k]`), i.e. relocations carrying a non-zero ADDEND, then
//                 indexed relative to the interior base.
//   * _2dtab    : a global table of pointers-to-rows (int*[]), runtime-indexed in
//                 BOTH dimensions (outer picks the row pointer, inner the slot).
//   * _swfp     : a switch (jump table) that STORES one of four function
//                 addresses into a global code pointer, then calls through it —
//                 jump-table dispatch + function-address symbolization combined.
//   * _chain    : triple indirection int***→int**→int*→int[]; all three relocated
//                 levels must be chased into the recompiled globals.
//
// All-integer, power-of-two index masks (no `%`/`/` → ARM32 libcall-free), arrays
// seeded from the LCG, folds to one integer return.  -O2, all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress334RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress334RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress334RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress334RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress334RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress334RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress334RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress334RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress334TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Global pointers to the INTERIOR of a global array: addend relocations.
    {p+"_interior",
     "static int "+p+"_a[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int *"+p+"_lo=&"+p+"_a[2];\n"
     "static int *"+p+"_hi=&"+p+"_a[10];\n"
     +t+" "+p+"_interior("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    int *q=(s&1u)?"+p+"_lo:"+p+"_hi;\n"
     "    q[(s>>5)&3u]+=(int)(s>>11)+i; sum+=q[(s>>9)&3u]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_a[0]+"+p+"_a[15]); }\n",
     {0xB1u}, "OptStress334", 2},

    // Global table of row pointers, runtime-indexed in both dimensions.
    {p+"_2dtab",
     "static int "+p+"_r0[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_r1[8]={2,7,1,8,2,8,1,8};\n"
     "static int "+p+"_r2[8]={1,6,1,8,0,3,3,9};\n"
     "static int "+p+"_r3[8]={2,7,1,8,2,8,4,5};\n"
     "static int *"+p+"_rows[4]={"+p+"_r0,"+p+"_r1,"+p+"_r2,"+p+"_r3};\n"
     +t+" "+p+"_2dtab("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    int *row="+p+"_rows[(s>>5)&3u];\n"
     "    row[(s>>9)&7u]+=(int)(s>>13)+i; sum+=row[(s>>11)&7u]; sum^=sum>>5; }\n"
     "  return ("+t+")(sum+"+p+"_r0[0]+"+p+"_r3[7]); }\n",
     {0xB2u}, "OptStress334", 2},

    // Switch stores one of four function addresses into a global code pointer,
    // then calls through it: jump-table dispatch + function-address symbolize.
    {p+"_swfp",
     "static int "+p+"_g0(int) __attribute__((noinline));\n"
     "static int "+p+"_g1(int) __attribute__((noinline));\n"
     "static int "+p+"_g2(int) __attribute__((noinline));\n"
     "static int "+p+"_g3(int) __attribute__((noinline));\n"
     "static int (*"+p+"_cur)(int)="+p+"_g0;\n"
     +t+" "+p+"_swfp("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int acc=0;\n"
     "  for(int i=0;i<72;i++){ s=s*1103515245u+12345u;\n"
     "    switch((s>>5)&3u){ case 0: "+p+"_cur="+p+"_g0; break;\n"
     "      case 1: "+p+"_cur="+p+"_g1; break;\n"
     "      case 2: "+p+"_cur="+p+"_g2; break;\n"
     "      default: "+p+"_cur="+p+"_g3; break; }\n"
     "    acc+="+p+"_cur((int)((s>>9)&0xff)); acc^=acc>>5; }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_g0(int x){return x+1;}\n"
     "static int "+p+"_g1(int x){return x*2;}\n"
     "static int "+p+"_g2(int x){return x^7;}\n"
     "static int "+p+"_g3(int x){return x-3;}\n",
     {0xB3u}, "OptStress334", 2},

    // Triple indirection: int*** -> int** -> int* -> int[]; all three relocated
    // levels chased (extends OptStress76 _dblptr by one level).
    {p+"_chain",
     "static int "+p+"_arr[8]={3,1,4,1,5,9,2,6};\n"
     "static int *"+p+"_pa="+p+"_arr;\n"
     "static int **"+p+"_pp=&"+p+"_pa;\n"
     "static int ***"+p+"_ppp=&"+p+"_pp;\n"
     +t+" "+p+"_chain("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    (**"+p+"_ppp)[(s>>5)&7u]+=(int)(s>>11)+i;\n"
     "    sum+=(**"+p+"_ppp)[(s>>9)&7u]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_arr[0]+"+p+"_arr[7]); }\n",
     {0xB4u}, "OptStress334", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress334TC("x64o334", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress334TC("x86o334", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress334TC("a64o334", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress334TC("armo334", "int");

INSTANTIATE_TEST_SUITE_P(OptStress334, X64OptStress334RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress334, X86OptStress334RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress334, A64OptStress334RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress334, ARM32OptStress334RT, ::testing::ValuesIn(kARM), rtTCName);
