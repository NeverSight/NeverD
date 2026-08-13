//===- AllPlatform_OptStress85RTTests.cpp - memcpy / kv ptr flow ---*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// More emission paths a global address flows through, beyond the direct/indirect
// call args of #473-#480:
//
//   * memcpyg  - a small struct copied between two global objects (clang inlines
//                or emits a memcpy intrinsic): both src and dst global addresses
//                must be symbolized.
//   * gpkv     - a global table of {key,int*} entries walked at runtime; the
//                matching entry's pointer field is dereferenced.
//
// A VARIADIC global-pointer argument (`vfn(n, &G[i])`) is the next open bug,
// precisely root-caused (see docs #481): the variadic call's args are recovered
// as fixed integer parameters, so neither the declared-pointer-param nor the
// LLVM-vararg symbolization path fires — the global element address is passed as
// a raw absolute VA the callee's va_arg dereferences.  Excluded here (all four
// targets) pending the variadic ABI-recovery fix.
//
// All integer, arrays seeded from the LCG, fold to one integer return.  No
// float / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress85RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress85RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress85RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress85RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress85RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress85RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress85RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress85RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress85TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // A small struct copied between two global objects (memcpy / inlined).
    {p+"_memcpyg",
     "struct "+p+"_blk{ int v[6]; };\n"
     "static struct "+p+"_blk "+p+"_S={{3,1,4,1,5,9}};\n"
     "static struct "+p+"_blk "+p+"_D={{0,0,0,0,0,0}};\n"
     +t+" "+p+"_memcpyg("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    "+p+"_S.v[(s>>5)&5u]+=(int)(s>>13);\n"
     "    "+p+"_D="+p+"_S;\n"
     "    sum+="+p+"_D.v[(s>>9)&5u]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_D.v[0]+"+p+"_D.v[5]); }\n",
     {0xD3u}, "OptStress85", 2},

    // Global table of {key,int*} entries walked at runtime; matching entry's
    // pointer field is dereferenced.
    {p+"_gpkv",
     "static int "+p+"_K0[4]={3,1,4,1};\n"
     "static int "+p+"_K1[4]={5,9,2,6};\n"
     "static int "+p+"_K2[4]={5,3,5,8};\n"
     "static int "+p+"_K3[4]={9,7,9,3};\n"
     "static const struct { unsigned key; int *p; } "+p+"_KV[4]={\n"
     "  {10,"+p+"_K0},{20,"+p+"_K1},{30,"+p+"_K2},{40,"+p+"_K3} };\n"
     +t+" "+p+"_gpkv("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned want=10u+10u*((s>>5)&3u);\n"
     "    for(int j=0;j<4;j++) if("+p+"_KV[j].key==want){\n"
     "      sum+="+p+"_KV[j].p[(s>>9)&3u]; break; }\n"
     "    sum^=sum>>6; }\n"
     "  return ("+t+")sum; }\n",
     {0xD4u}, "OptStress85", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress85TC("x64o85", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress85TC("x86o85", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress85TC("a64o85", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress85TC("armo85", "int");

INSTANTIATE_TEST_SUITE_P(OptStress85, X64OptStress85RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress85, X86OptStress85RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress85, A64OptStress85RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress85, ARM32OptStress85RT, ::testing::ValuesIn(kARM), rtTCName);
