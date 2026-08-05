//===- AllPlatform_OptStress80RTTests.cpp - ptr indirection ---*-C++*-=//
//
// The #473-#477 fixes all chased one DNA: a global address that becomes a
// VALUE, flows through some path, and is read back as an address without being
// re-symbolized at the use.  These probes push that DNA into shapes the prior
// ones did not reach:
//
//   * swptr     - a switch(k) selects which of FOUR global arrays a pointer
//                 points to (a 4-way base, not gptrsel's branchless blend nor
//                 ptrphi's 2-way PHI); clang may build a rodata table of the
//                 four base addresses indexed by k.
//   * structptab- a pointer array nested inside a global STRUCT (G.t[k][j]),
//                 not the bare global array #476 covered.
//   * dptrret   - a noinline helper returns an `int**` (&slot, a writable
//                 global int**); the caller stores a global address THROUGH it
//                 and double-dereferences (returned-addr + store-through +
//                 double indirection composed).
//   * gptrshare - a global `int*` written in one helper and dereferenced for a
//                 store in another (the shared global pointer drives an indirect
//                 store across the call boundary).
//
// All integer, arrays seeded from the LCG, fold to one integer return.  No
// float / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress80RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress80RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress80RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress80RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress80RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress80RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress80RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress80RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress80TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // switch(k) picks one of four global arrays as the pointer base.
    {p+"_swptr",
     "static int "+p+"_A[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_B[8]={2,7,1,8,2,8,1,8};\n"
     "static int "+p+"_C[8]={1,6,1,8,0,3,3,9};\n"
     "static int "+p+"_D[8]={5,7,7,2,1,5,6,6};\n"
     +t+" "+p+"_swptr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    int *q;\n"
     "    switch((s>>5)&3u){ case 0:q="+p+"_A;break; case 1:q="+p+"_B;break;\n"
     "      case 2:q="+p+"_C;break; default:q="+p+"_D; }\n"
     "    q[(s>>9)&7u]+=(int)(s>>11); sum+=q[(s>>13)&7u]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_A[0]+"+p+"_B[7]+"+p+"_C[3]+"+p+"_D[5]); }\n",
     {0xE1u}, "OptStress80", 2},

    // Pointer array nested inside a global struct: G.t[k][j].
    {p+"_structptab",
     "static int "+p+"_P[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_Q[8]={2,7,1,8,2,8,1,8};\n"
     "static struct { int *t[4]; } "+p+"_G;\n"
     +t+" "+p+"_structptab("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<4;i++) "+p+"_G.t[i]=(i&1)?"+p+"_P:"+p+"_Q;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned k=(s>>5)&3u; "+p+"_G.t[k][(s>>9)&7u]+=(int)(s>>11);\n"
     "    sum+="+p+"_G.t[(s>>13)&3u][(s>>17)&7u]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_P[0]+"+p+"_Q[7]); }\n",
     {0xE2u}, "OptStress80", 2},

    // noinline returns int** (&slot); caller stores a global addr through it,
    // then double-dereferences.
    {p+"_dptrret",
     "static int "+p+"_X[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_Y[8]={2,7,1,8,2,8,1,8};\n"
     "static int *"+p+"_slot;\n"
     "static int** "+p+"_getpp(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_dptrret("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    int **pp="+p+"_getpp(s); *pp=(s&1u)?"+p+"_X:"+p+"_Y;\n"
     "    (*pp)[(s>>9)&7u]+=(int)(s>>11);\n"
     "    sum+=(*pp)[(s>>13)&7u]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_X[0]+"+p+"_Y[7]); }\n"
     "static int** "+p+"_getpp(unsigned s){ (void)s; return &"+p+"_slot; }\n",
     {0xE3u}, "OptStress80", 2},

    // A shared global int* written in one helper, dereferenced for a store in
    // another, across the call boundary.
    {p+"_gptrshare",
     "static int "+p+"_U[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_V[8]={2,7,1,8,2,8,1,8};\n"
     "static int *"+p+"_cur;\n"
     "static void "+p+"_setcur(unsigned) __attribute__((noinline));\n"
     "static void "+p+"_bump(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_gptrshare("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    "+p+"_setcur(s); "+p+"_bump(s);\n"
     "    sum+="+p+"_cur[(s>>13)&7u]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_U[0]+"+p+"_V[7]); }\n"
     "static void "+p+"_setcur(unsigned s){ "+p+"_cur=(s&1u)?"+p+"_U:"+p+"_V; }\n"
     "static void "+p+"_bump(unsigned s){ "+p+"_cur[(s>>9)&7u]+=(int)(s>>11); }\n",
     {0xE4u}, "OptStress80", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress80TC("x64o80", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress80TC("x86o80", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress80TC("a64o80", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress80TC("armo80", "int");

INSTANTIATE_TEST_SUITE_P(OptStress80, X64OptStress80RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress80, X86OptStress80RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress80, A64OptStress80RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress80, ARM32OptStress80RT, ::testing::ValuesIn(kARM), rtTCName);
