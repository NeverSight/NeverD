//===- AllPlatform_OptStress76RTTests.cpp - pointer indirection -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// OptStress74/75 covered a single global address crossing a call boundary in or
// out.  These probes push the indirection one level deeper — pointer tables and
// pointer-to-pointer state held in mutable globals:
//
//   * fptab  - a writable global ARRAY of function pointers, runtime-indexed for
//              the indirect call AND with entries reassigned at runtime (a
//              writable code-pointer table: relocated initializer + indexed load
//              + indexed store).
//   * dblptr - an `int**` global that points at a global `int*` which points at a
//              global array; the double dereference must chase both relocated
//              levels into the recompiled globals.
//   * strptr - a file-scope struct `{int *p; int n}` whose pointer field is
//              reassigned among two global arrays and dereferenced (a data
//              pointer inside a writable struct).
//   * outptr - a `noinline` helper takes `int**` and writes a global address
//              through it (an output-pointer parameter); the caller then
//              dereferences its updated local pointer.
//
// All integer; arrays seeded from the LCG; folds to one integer return.  No
// float / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress76RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress76RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress76RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress76RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress76RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress76RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress76RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress76RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress76TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Writable global function-pointer table: indexed call + runtime reassign.
    {p+"_fptab",
     "static int "+p+"_f0(int) __attribute__((noinline));\n"
     "static int "+p+"_f1(int) __attribute__((noinline));\n"
     "static int "+p+"_f2(int) __attribute__((noinline));\n"
     "static int "+p+"_f3(int) __attribute__((noinline));\n"
     "static int (*"+p+"_ft[4])(int)={"+p+"_f0,"+p+"_f1,"+p+"_f2,"+p+"_f3};\n"
     +t+" "+p+"_fptab("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int acc=0;\n"
     "  for(int i=0;i<80;i++){ s=s*1103515245u+12345u;\n"
     "    if((s&7u)==0u) "+p+"_ft[(s>>5)&3u]="+p+"_f3;\n"
     "    acc+="+p+"_ft[(s>>9)&3u]((int)((s>>13)&0xff)); acc^=acc>>5; }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_f0(int x){return x+1;}\n"
     "static int "+p+"_f1(int x){return x*2;}\n"
     "static int "+p+"_f2(int x){return x^7;}\n"
     "static int "+p+"_f3(int x){return x-3;}\n",
     {0xA1u}, "OptStress76", 2},

    // int** global -> int* global -> int[] global: double dereference.
    {p+"_dblptr",
     "static int "+p+"_arr[8]={3,1,4,1,5,9,2,6};\n"
     "static int *"+p+"_pa="+p+"_arr;\n"
     "static int **"+p+"_pp=&"+p+"_pa;\n"
     +t+" "+p+"_dblptr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    (*"+p+"_pp)[(s>>5)&7u]+=(int)(s>>11)+i;\n"
     "    sum+=(*"+p+"_pp)[(s>>9)&7u]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_arr[0]+"+p+"_arr[7]); }\n",
     {0xA2u}, "OptStress76", 2},

    // File-scope struct with a pointer field reassigned among two globals.
    {p+"_strptr",
     "static int "+p+"_t1[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_t2[8]={2,7,1,8,2,8,1,8};\n"
     "static struct { int *p; int n; } "+p+"_sp={"+p+"_t1,0};\n"
     +t+" "+p+"_strptr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    "+p+"_sp.p=(s&1u)?"+p+"_t1:"+p+"_t2; "+p+"_sp.n++;\n"
     "    "+p+"_sp.p[(s>>5)&7u]+=(int)(s>>11); sum+="+p+"_sp.p[(s>>9)&7u];\n"
     "    sum^=sum>>7; }\n"
     "  return ("+t+")(sum+"+p+"_sp.n+"+p+"_t1[0]+"+p+"_t2[7]); }\n",
     {0xA3u}, "OptStress76", 2},

    // Output-pointer parameter: noinline writes a global address through int**.
    {p+"_outptr",
     "static int "+p+"_x[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_y[8]={2,7,1,8,2,8,1,8};\n"
     "static void "+p+"_setp(int**,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_outptr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0; int *q="+p+"_x;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    "+p+"_setp(&q,s); q[(s>>5)&7u]+=(int)(s>>11)+i;\n"
     "    sum+=q[(s>>9)&7u]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_x[0]+"+p+"_y[7]); }\n"
     "static void "+p+"_setp(int**pp,unsigned s){ *pp=(s&1u)?"+p+"_x:"+p+"_y; }\n",
     {0xA4u}, "OptStress76", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress76TC("x64o76", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress76TC("x86o76", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress76TC("a64o76", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress76TC("armo76", "int");

INSTANTIATE_TEST_SUITE_P(OptStress76, X64OptStress76RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress76, X86OptStress76RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress76, A64OptStress76RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress76, ARM32OptStress76RT, ::testing::ValuesIn(kARM), rtTCName);
