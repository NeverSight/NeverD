//===- AllPlatform_OptStress84RTTests.cpp - indirect call ptr flow -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// #473-#479 symbolized global addresses flowing through DIRECT calls (args,
// returns, struct fields).  These push the same DNA through INDIRECT calls (a
// function pointer selected from a table) and recursion:
//
//   * icallptr - indirect call passing a computed global element address
//                `&G[i]` as a pointer argument; callee writes through it.
//   * icallret - indirect call RETURNS a global element address `&H[i]`; the
//                caller dereferences the returned pointer.
//   * icallout - indirect call writes a global address through an `int**`
//                output-pointer argument (the #475 outptr shape, but indirect).
//   * recacc   - a recursive function threads a global pointer down its calls
//                and writes/reads through it at each depth.
//
// All integer, arrays seeded from the LCG, fold to one integer return.  No
// float / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress84RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress84RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress84RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress84RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress84RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress84RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress84RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress84RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress84TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Indirect call passing a computed global element address as a pointer arg.
    {p+"_icallptr",
     "static int "+p+"_G[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_addv(int*,int) __attribute__((noinline));\n"
     "static int "+p+"_xorv(int*,int) __attribute__((noinline));\n"
     "static int (*const "+p+"_FT[2])(int*,int)={"+p+"_addv,"+p+"_xorv};\n"
     +t+" "+p+"_icallptr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    int (*fp)(int*,int)="+p+"_FT[(s>>4)&1u];\n"
     "    sum+=fp(&"+p+"_G[(s>>5)&15u],(int)(s>>9)); sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_G[2]+"+p+"_G[13]); }\n"
     "static int "+p+"_addv(int*q,int v){ *q+=v; return *q; }\n"
     "static int "+p+"_xorv(int*q,int v){ *q^=v; return *q; }\n",
     {0xC1u}, "OptStress84", 2},

    // Indirect call returns a computed global element address.
    {p+"_icallret",
     "static int "+p+"_H[16]={2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5};\n"
     "static int* "+p+"_pick(unsigned) __attribute__((noinline));\n"
     "static int* (*const "+p+"_RT[2])(unsigned)={"+p+"_pick,"+p+"_pick};\n"
     +t+" "+p+"_icallret("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    int* (*rp)(unsigned)="+p+"_RT[(s>>4)&1u];\n"
     "    int *q=rp(s); *q+=(int)(s>>11); sum+=*q; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_H[3]+"+p+"_H[12]); }\n"
     "static int* "+p+"_pick(unsigned s){ return &"+p+"_H[(s>>7)&15u]; }\n",
     {0xC2u}, "OptStress84", 2},

    // Indirect call writes a global address through an int** output parameter.
    {p+"_icallout",
     "static int "+p+"_X[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_Y[16]={2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5};\n"
     "static void "+p+"_setq(int**,unsigned) __attribute__((noinline));\n"
     "static void (*const "+p+"_OT[2])(int**,unsigned)={"+p+"_setq,"+p+"_setq};\n"
     +t+" "+p+"_icallout("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    void (*op)(int**,unsigned)="+p+"_OT[(s>>4)&1u]; int *q;\n"
     "    op(&q,s); q[(s>>5)&15u]+=(int)(s>>11); sum+=q[(s>>5)&15u]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_X[1]+"+p+"_Y[9]); }\n"
     "static void "+p+"_setq(int**pp,unsigned s){ *pp=(s&1u)?"+p+"_X:"+p+"_Y; }\n",
     {0xC3u}, "OptStress84", 2},

    // Recursive function threads a global pointer down its calls.
    {p+"_recacc",
     "static int "+p+"_A[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_rec(int*,int,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_recacc("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<60;i++){ s=s*1103515245u+12345u;\n"
     "    sum+="+p+"_rec("+p+"_A,6,s); sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_A[4]+"+p+"_A[10]); }\n"
     "static int "+p+"_rec(int*q,int d,unsigned s){\n"
     "  if(d<=0) return 0;\n"
     "  unsigned k=(s>>(unsigned)d)&15u; q[k]+=d;\n"
     "  return q[k]+"+p+"_rec(q,d-1,s*1103515245u+12345u); }\n",
     {0xC4u}, "OptStress84", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress84TC("x64o84", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress84TC("x86o84", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress84TC("a64o84", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress84TC("armo84", "int");

INSTANTIATE_TEST_SUITE_P(OptStress84, X64OptStress84RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress84, X86OptStress84RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress84, A64OptStress84RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress84, ARM32OptStress84RT, ::testing::ValuesIn(kARM), rtTCName);
