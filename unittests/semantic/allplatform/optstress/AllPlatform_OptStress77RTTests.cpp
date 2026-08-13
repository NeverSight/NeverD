//===- AllPlatform_OptStress77RTTests.cpp - pointer arrays/PHI --*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// OptStress75/76 covered a global address returned, stored through a parameter,
// or kept in a scalar/struct global.  These probes push it into a global ARRAY
// of pointers built at runtime and into branch-merged pointer values:
//
//   * ptrarr - a `.bss` array of `int*` populated at runtime with the addresses
//              of two global arrays, then runtime-indexed and dereferenced (the
//              stored pointers go INTO a global array slot — the IsGlobalData
//              store-value case OptStress76's fix deliberately excluded).
//   * ptrphi - a local `int*` chosen by an if/else (a cross-block PHI of two
//              global bases, not the branchless blend gptrsel produced) then
//              dereferenced for load and store.
//   * ptrinc - a file-scope `int*` advanced across iterations (gp += k, stored
//              back to the global each step) and dereferenced — a global
//              induction pointer.
//   * gpcall - a `noinline` helper returns &A/&B, the caller stores it into a
//              global pointer, reads it back, and dereferences (a returned
//              pointer round-tripped through a global).
//
// All integer; arrays seeded from the LCG; folds to one integer return.  No
// float / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress77RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress77RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress77RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress77RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress77RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress77RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress77RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress77RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress77TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // .bss array of int* built at runtime with global-array addresses.
    {p+"_ptrarr",
     "static int "+p+"_A[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_B[8]={2,7,1,8,2,8,1,8};\n"
     "static int *"+p+"_gp[4];\n"
     +t+" "+p+"_ptrarr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<4;i++) "+p+"_gp[i]=(i&1)?"+p+"_A:"+p+"_B;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned k=(s>>5)&3u; "+p+"_gp[k][(s>>9)&7u]+=(int)(s>>11)+i;\n"
     "    sum+="+p+"_gp[(s>>13)&3u][(s>>17)&7u]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_A[0]+"+p+"_B[7]); }\n",
     {0xB1u}, "OptStress77", 2},

    // Local int* chosen by if/else (cross-block PHI) then dereferenced.
    {p+"_ptrphi",
     "static int "+p+"_a[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_b[8]={2,7,1,8,2,8,1,8};\n"
     +t+" "+p+"_ptrphi("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    int *q; if(s&0x10000u) q="+p+"_a; else q="+p+"_b;\n"
     "    q[(s>>5)&7u]+=(int)(s>>11); sum+=q[(s>>9)&7u]; sum^=sum>>7; }\n"
     "  return ("+t+")(sum+"+p+"_a[0]+"+p+"_b[7]); }\n",
     {0xB2u}, "OptStress77", 2},

    // File-scope int* advanced across iterations (global induction pointer).
    {p+"_ptrinc",
     "static int "+p+"_buf[32]={0};\n"
     "static int *"+p+"_cur="+p+"_buf;\n"
     +t+" "+p+"_ptrinc("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0; "+p+"_cur="+p+"_buf;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    *"+p+"_cur+=(int)(s>>11); sum+=*"+p+"_cur;\n"
     "    "+p+"_cur+=1+((s>>5)&1u); if("+p+"_cur>="+p+"_buf+30) "+p+"_cur="+p+"_buf;\n"
     "    sum^=sum>>6; }\n"
     "  int r=sum; for(int k=0;k<32;k++) r=r*131+"+p+"_buf[k];\n"
     "  return ("+t+")r; }\n",
     {0xB3u}, "OptStress77", 2},

    // noinline returns &A/&B; caller stores it to a global ptr, reads back, derefs.
    {p+"_gpcall",
     "static int "+p+"_x[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_y[8]={2,7,1,8,2,8,1,8};\n"
     "static int *"+p+"_slot;\n"
     "static int* "+p+"_pick(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_gpcall("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    "+p+"_slot="+p+"_pick(s); "+p+"_slot[(s>>5)&7u]+=(int)(s>>11)+i;\n"
     "    sum+="+p+"_slot[(s>>9)&7u]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_x[0]+"+p+"_y[7]); }\n"
     "static int* "+p+"_pick(unsigned s){ return (s&1u)?"+p+"_x:"+p+"_y; }\n",
     {0xB4u}, "OptStress77", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress77TC("x64o77", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress77TC("x86o77", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress77TC("a64o77", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress77TC("armo77", "int");

INSTANTIATE_TEST_SUITE_P(OptStress77, X64OptStress77RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress77, X86OptStress77RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress77, A64OptStress77RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress77, ARM32OptStress77RT, ::testing::ValuesIn(kARM), rtTCName);
