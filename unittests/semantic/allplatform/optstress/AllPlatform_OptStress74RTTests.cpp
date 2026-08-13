//===- AllPlatform_OptStress74RTTests.cpp - globals across calls -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// OptStress73 drove mutable .data/.bss globals but only from one function and
// never across a call boundary; StructAbi drove the call ABI but never touched
// a global.  Real binaries constantly do both at once, so these probes cross
// the two axes — mutable global state shared through calls and pointers:
//
//   * gshare  - a scalar .data global read-modify-written by a `noinline` helper
//               that the caller invokes in a loop, then read back.  The lifted
//               caller and callee must symbolize the SAME recompiled global, or
//               the helper's writes are invisible to the caller's read.
//   * gptrsel - a writable global pointer (its .data initializer is a relocation
//               to a global array) reassigned at runtime to one of two global
//               arrays, then indexed for both load and store — a mutable data
//               pointer whose value is a global address.
//   * gstrfld - a file-scope struct mixing a scalar counter, a hashed scalar and
//               an array field, every field mutated and folded in one loop (field
//               offsets inside one global).
//   * gcallar - the address of a global array element passed to a `noinline`
//               mutator that reads/writes through the pointer (pointer-argument
//               recovery + global aliasing across the call).
//
// All integer; arrays seeded from the LCG; folds global state to one integer
// return.  No float / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress74RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress74RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress74RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress74RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress74RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress74RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress74RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress74RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress74TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Scalar .data global RMW'd by a noinline helper, shared with the caller.
    {p+"_gshare",
     "static unsigned "+p+"_gs=2166136261u;\n"
     "static void "+p+"_bump(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_gshare("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<100;i++){ s=s*1103515245u+12345u; "+p+"_bump(s>>9); }\n"
     "  return ("+t+")"+p+"_gs; }\n"
     "static void "+p+"_bump(unsigned x){\n"
     "  "+p+"_gs=("+p+"_gs^x)*16777619u; "+p+"_gs^="+p+"_gs>>13; }\n",
     {0x81u}, "OptStress74", 2},

    // Writable global pointer reassigned among two global arrays, indexed RW.
    {p+"_gptrsel",
     "static int "+p+"_a1[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_a2[8]={2,7,1,8,2,8,1,8};\n"
     "static int *"+p+"_gp="+p+"_a1;\n"
     +t+" "+p+"_gptrsel("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    "+p+"_gp=(s&1u)?"+p+"_a1:"+p+"_a2;\n"
     "    sum+="+p+"_gp[(s>>5)&7u]; "+p+"_gp[(s>>9)&7u]+=(int)(s>>13)+i;\n"
     "    sum^=sum>>7; }\n"
     "  return ("+t+")(sum+"+p+"_a1[0]+"+p+"_a2[7]); }\n",
     {0x82u}, "OptStress74", 2},

    // File-scope struct: scalar counter + hashed scalar + array field, all RMW.
    {p+"_gstrfld",
     "static struct { int n; unsigned acc; int hist[4]; } "+p+"_st="
     "{0,2166136261u,{1,2,3,4}};\n"
     +t+" "+p+"_gstrfld("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<120;i++){ s=s*1103515245u+12345u;\n"
     "    "+p+"_st.n++; "+p+"_st.acc=("+p+"_st.acc+s)*16777619u;\n"
     "    "+p+"_st.hist[(s>>6)&3u]+=(int)(s>>20)+i; }\n"
     "  int r="+p+"_st.n+(int)"+p+"_st.acc;\n"
     "  for(int k=0;k<4;k++) r=r*131+"+p+"_st.hist[k];\n"
     "  return ("+t+")r; }\n",
     {0x83u}, "OptStress74", 2},

    // &global[j] passed to a noinline mutator that RWs through the pointer.
    {p+"_gcallar",
     "static int "+p+"_buf[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static void "+p+"_acc(int*,int) __attribute__((noinline));\n"
     +t+" "+p+"_gcallar("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    "+p+"_acc(&"+p+"_buf[(s>>5)&15u],(int)(s>>11)+i);\n"
     "    sum+="+p+"_buf[(s>>9)&15u]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_buf[0]+"+p+"_buf[15]); }\n"
     "static void "+p+"_acc(int*q,int v){ *q+=v; *q^=*q>>9; }\n",
     {0x84u}, "OptStress74", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress74TC("x64o74", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress74TC("x86o74", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress74TC("a64o74", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress74TC("armo74", "int");

INSTANTIATE_TEST_SUITE_P(OptStress74, X64OptStress74RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress74, X86OptStress74RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress74, A64OptStress74RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress74, ARM32OptStress74RT, ::testing::ValuesIn(kARM), rtTCName);
