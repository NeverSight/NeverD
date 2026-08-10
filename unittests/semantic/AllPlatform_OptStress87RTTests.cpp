//===- AllPlatform_OptStress87RTTests.cpp - data pointer-table reloc -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Deeper coverage for the wide / indexed data-pointer-table relocation fixed in
// #483 (tryResolveGlobalData routes a non-mutable segment carrying relocated
// pointer slots through buildCodePtrSegmentGlobal, so each entry is emitted as
// `ptrtoint(@recompiled_data)` rather than a stale absolute VA):
//
//   * gp3tab  - a local int*[3]={A,B,C} (a 3-entry .data.rel.ro pointer table
//               loaded into the stack array), runtime-indexed and dereferenced.
//   * gpotab  - a local int*[2]={&A[3],&B[5]}: table entries are global element
//               addresses (relocations carry a non-zero addend), runtime-indexed.
//   * grotab  - a file-scope `static int *const T[3]={A,B,C}` rodata pointer
//               table indexed at runtime (indexed load, not whole load).
//   * gpstab  - a local struct holding an int*[2] pointer-array field, indexed.
//
// All integer, arrays seeded from the LCG, fold to one integer return.  No
// float / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress87RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress87RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress87RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress87RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress87RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress87RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress87RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress87RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress87TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // local int*[3]={A,B,C}: a 3-entry pointer table loaded into the stack array.
    {p+"_gp3tab",
     "static int "+p+"_A[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_B[16]={2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5};\n"
     "static int "+p+"_C[16]={1,6,1,8,0,3,3,9,8,8,7,4,9,8,9,4};\n"
     +t+" "+p+"_gp3tab("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<150;i++){ s=s*1103515245u+12345u;\n"
     "    int *tb[3]={"+p+"_A,"+p+"_B,"+p+"_C};\n"
     "    unsigned k=(s>>4)%3u, j=(s>>7)&15u;\n"
     "    tb[k][j]+=(int)(s>>13); sum+=tb[k][j]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_A[1]+"+p+"_B[9]+"+p+"_C[3]); }\n",
     {0x71u}, "OptStress87", 2},

    // local int*[2]={&A[3],&B[5]}: entries are global element addresses (the
    // relocations carry a non-zero addend), runtime-indexed.
    {p+"_gpotab",
     "static int "+p+"_OA[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_OB[16]={2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5};\n"
     +t+" "+p+"_gpotab("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<150;i++){ s=s*1103515245u+12345u;\n"
     "    int *tb[2]={&"+p+"_OA[3],&"+p+"_OB[5]};\n"
     "    unsigned k=(s>>5)&1u, j=(s>>7)&7u;\n"
     "    tb[k][j]+=(int)(s>>13); sum+=tb[k][j]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_OA[4]+"+p+"_OB[6]); }\n",
     {0x72u}, "OptStress87", 2},

    // file-scope `static int *const T[3]={A,B,C}` rodata table, runtime-indexed.
    {p+"_grotab",
     "static int "+p+"_RA[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_RB[16]={2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5};\n"
     "static int "+p+"_RC[16]={1,6,1,8,0,3,3,9,8,8,7,4,9,8,9,4};\n"
     "static int *const "+p+"_RT[3]={"+p+"_RA,"+p+"_RB,"+p+"_RC};\n"
     +t+" "+p+"_grotab("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<150;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned k=(s>>4)%3u, j=(s>>7)&15u;\n"
     "    "+p+"_RT[k][j]+=(int)(s>>13); sum+="+p+"_RT[k][j]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_RA[1]+"+p+"_RB[9]+"+p+"_RC[3]); }\n",
     {0x73u}, "OptStress87", 2},

    // local struct holding an int*[2] pointer-array field, runtime-indexed.
    {p+"_gpstab",
     "static int "+p+"_SA[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_SB[16]={2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5};\n"
     "struct "+p+"_pt{ int *e[2]; int tag; };\n"
     +t+" "+p+"_gpstab("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<150;i++){ s=s*1103515245u+12345u;\n"
     "    struct "+p+"_pt pt={{"+p+"_SA,"+p+"_SB},(int)s};\n"
     "    unsigned k=(s>>5)&1u, j=(s>>7)&15u;\n"
     "    pt.e[k][j]+=(int)(s>>13)+(pt.tag&1); sum+=pt.e[k][j]; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_SA[2]+"+p+"_SB[11]); }\n",
     {0x74u}, "OptStress87", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress87TC("x64o87", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress87TC("x86o87", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress87TC("a64o87", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress87TC("armo87", "int");

INSTANTIATE_TEST_SUITE_P(OptStress87, X64OptStress87RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress87, X86OptStress87RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress87, A64OptStress87RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress87, ARM32OptStress87RT, ::testing::ValuesIn(kARM), rtTCName);
