//===- AllPlatform_OptStress82RTTests.cpp - ptr compare / wrap -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The #473-#477 fixes symbolized a global address that becomes a VALUE wherever
// it is consumed as an address (LOAD/STORE/CALL-arg/RETURN/STORE-into-global).
// These probes push the same DNA into shapes where a global address flows into
// a pointer COMPARISON and a compare-driven reset PHI (the strroll wrap shape,
// but compare-based instead of null-terminated):
//
//   * ptrcmp   - a pointer walks a global int[]; `if(p>=A+N) p=A` is a compare
//                against &A[N] plus a reset PHI to &A[0] (two symbolized global
//                addresses merged at a PHI with the +1 induction).
//   * gcondptr - `p = cond ? &A[i] : &B[j]` selects between two *computed
//                element* addresses (not gptrsel's bare bases), deref + store.
//   * dblidx   - `A[B[i]]` : the index loaded from global B drives a store into
//                global A (two globals, one feeding the other's address).
//   * structval- a by-value struct {int*;int} carrying a global pointer passed
//                to a noinline callee that dereferences the field.
//
// All integer, arrays seeded from the LCG, fold to one integer return.  No
// float / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress82RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress82RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress82RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress82RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress82RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress82RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress82RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress82RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress82TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Pointer walks a global int[]; compare against &A[N] + reset PHI to &A[0].
    {p+"_ptrcmp",
     "static int "+p+"_A[16];\n"
     +t+" "+p+"_ptrcmp("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int h=0;\n"
     "  for(int i=0;i<16;i++){ s=s*1103515245u+12345u; "+p+"_A[i]=(int)(s>>9); }\n"
     "  int *p="+p+"_A+((s>>4)&15u);\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    *p+=(int)(s>>11); h=h*131+*p; p++;\n"
     "    if(p>="+p+"_A+16) p="+p+"_A; }\n"
     "  return ("+t+")(h+"+p+"_A[0]+"+p+"_A[15]); }\n",
     {0x91u}, "OptStress82", 2},

    // p = cond ? &A[i] : &B[j] selects between two computed element addresses.
    {p+"_gcondptr",
     "static int "+p+"_A[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_B[16]={2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5};\n"
     +t+" "+p+"_gcondptr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    int *p=(s&16u)?"+p+"_A+((s>>5)&15u):"+p+"_B+((s>>9)&15u);\n"
     "    *p+=(int)(s>>13); sum+=*p; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_A[3]+"+p+"_B[12]); }\n",
     {0x92u}, "OptStress82", 2},

    // A[B[i]] : index loaded from global B drives a store into global A.
    {p+"_dblidx",
     "static int "+p+"_A[16];\n"
     "static const unsigned char "+p+"_B[32]={"
     "3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3,2,3,8,4,6,2,6,4,3,3,8,3,2,7,9,5};\n"
     +t+" "+p+"_dblidx("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int h=0;\n"
     "  for(int i=0;i<16;i++) "+p+"_A[i]=0;\n"
     "  for(int i=0;i<240;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned k="+p+"_B[(s>>7)&31u]&15u;\n"
     "    "+p+"_A[k]+=(int)(s>>12); h=h*131+"+p+"_A[k]; h^=h>>9; }\n"
     "  return ("+t+")h; }\n",
     {0x93u}, "OptStress82", 2},

    // By-value struct {int*;int} carrying a global pointer to a noinline callee.
    {p+"_structval",
     "static int "+p+"_A[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_C[16]={2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5};\n"
     "struct "+p+"_sp{ int *q; int n; };\n"
     "static int "+p+"_use(struct "+p+"_sp) __attribute__((noinline));\n"
     +t+" "+p+"_structval("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    struct "+p+"_sp sp; sp.q=(s&8u)?"+p+"_A:"+p+"_C; sp.n=(int)((s>>5)&15u);\n"
     "    sum+="+p+"_use(sp); sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_A[1]+"+p+"_C[9]); }\n"
     "static int "+p+"_use(struct "+p+"_sp sp){ sp.q[sp.n]+=sp.n; return sp.q[sp.n]; }\n",
     {0x94u}, "OptStress82", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress82TC("x64o82", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress82TC("x86o82", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress82TC("a64o82", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress82TC("armo82", "int");

INSTANTIATE_TEST_SUITE_P(OptStress82, X64OptStress82RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress82, X86OptStress82RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress82, A64OptStress82RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress82, ARM32OptStress82RT, ::testing::ValuesIn(kARM), rtTCName);
