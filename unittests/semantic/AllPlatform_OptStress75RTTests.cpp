//===- AllPlatform_OptStress75RTTests.cpp - pointers returned/stored -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// OptStress74 symbolized a global address passed INTO a call; these probes drive
// the duals — a global address flowing OUT of a call (a returned pointer), a
// code pointer kept in a mutable global, and a strided array-of-structs global:
//
//   * retptr - a `noinline` helper returns &garr[index]; the caller dereferences
//              the returned pointer for both load and store.  The returned value
//              is &global[index] and must be symbolized on the RETURN path, the
//              mirror of OptStress74's pointer-argument fix.
//   * selret - a `noinline` helper returns one of two global arrays (cond ? A :
//              B), lowered to a branchless base blend; the caller indexes the
//              returned base.  Return-side dual of OptStress74's gptrsel.
//   * arrstr - a file-scope array of {int,unsigned} structs, runtime-indexed for
//              a strided field load/store, then swept (array-of-struct global).
//   * fnptr  - a mutable global function pointer reassigned at runtime to one of
//              two functions and called indirectly (a code pointer stored in
//              writable .data, plus its relocated static initializer).
//
// All integer; arrays seeded from the LCG; folds to one integer return.  No
// float / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress75RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress75RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress75RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress75RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress75RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress75RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress75RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress75RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress75TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // noinline returns &garr[index]; caller dereferences (load + store).
    {p+"_retptr",
     "static int "+p+"_ga[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int* "+p+"_pick(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_retptr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    int *p="+p+"_pick(s>>5); *p+=(int)(s>>11)+i;\n"
     "    sum+=*"+p+"_pick(s>>9); sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_ga[0]+"+p+"_ga[15]); }\n"
     "static int* "+p+"_pick(unsigned k){ return &"+p+"_ga[k&15u]; }\n",
     {0x91u}, "OptStress75", 2},

    // noinline returns one of two global arrays; caller indexes the base.
    {p+"_selret",
     "static int "+p+"_A[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_B[8]={2,7,1,8,2,8,1,8};\n"
     "static int* "+p+"_sel(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_selret("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    int *p="+p+"_sel(s); p[(s>>5)&7u]+=(int)(s>>13)+i;\n"
     "    sum+=p[(s>>9)&7u]; sum^=sum>>7; }\n"
     "  return ("+t+")(sum+"+p+"_A[0]+"+p+"_B[7]); }\n"
     "static int* "+p+"_sel(unsigned s){ return (s&1u)?"+p+"_A:"+p+"_B; }\n",
     {0x92u}, "OptStress75", 2},

    // File-scope array of {int,unsigned} structs: strided field load/store/sweep.
    {p+"_arrstr",
     "static struct { int k; unsigned v; } "+p+"_t[8]="
     "{{1,10u},{2,20u},{3,30u},{4,40u},{5,50u},{6,60u},{7,70u},{8,80u}};\n"
     +t+" "+p+"_arrstr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<100;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>5)&7u; "+p+"_t[j].v=("+p+"_t[j].v+s)*16777619u;\n"
     "    "+p+"_t[j].k^=(int)(s>>17)+i; sum+="+p+"_t[(s>>9)&7u].k; sum^=sum>>7; }\n"
     "  int r=sum; for(int q=0;q<8;q++) r=r*131+(int)"+p+"_t[q].v+"+p+"_t[q].k;\n"
     "  return ("+t+")r; }\n",
     {0x93u}, "OptStress75", 2},

    // Mutable global function pointer reassigned at runtime and called indirectly.
    {p+"_fnptr",
     "static int "+p+"_af(int) __attribute__((noinline));\n"
     "static int "+p+"_mf(int) __attribute__((noinline));\n"
     "static int (*"+p+"_fp)(int)="+p+"_af;\n"
     +t+" "+p+"_fnptr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int acc=0;\n"
     "  for(int i=0;i<80;i++){ s=s*1103515245u+12345u;\n"
     "    "+p+"_fp=(s&1u)?"+p+"_af:"+p+"_mf;\n"
     "    acc+="+p+"_fp((int)((s>>8)&0xff)); acc^=acc>>5; }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_af(int x){ return x*2+1; }\n"
     "static int "+p+"_mf(int x){ return x*3-2; }\n",
     {0x94u}, "OptStress75", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress75TC("x64o75", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress75TC("x86o75", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress75TC("a64o75", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress75TC("armo75", "int");

INSTANTIATE_TEST_SUITE_P(OptStress75, X64OptStress75RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress75, X86OptStress75RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress75, A64OptStress75RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress75, ARM32OptStress75RT, ::testing::ValuesIn(kARM), rtTCName);
