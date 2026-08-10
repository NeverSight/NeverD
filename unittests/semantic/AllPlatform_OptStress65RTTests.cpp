//===- AllPlatform_OptStress65RTTests.cpp - select-address deref -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Data-dependent table addresses that flow through a conditional move / select
// / predicated instruction before the load: x86 CMOV, AArch64 CSEL, ARM32
// IT/predication.  #468 fixed an ARM32 predicated table index
// (`SELECT(cond, base+i+3, base+i)`); these widen the surface to conditional
// indices, selected strides and table-valued predicated accumulation so any
// remaining "constant-pool address through a select" miscompile surfaces as a
// return mismatch.
//
//   * condidx  - one rodata table, conditional runtime index `T[c?i:j]`.
//   * selstride- selected stride/scale into one table `T[i*(c?2:3)]`.
//   * predacc  - predicated accumulate driven by a table-valued condition.
//
// (Select between two distinct rodata table *bases* — `(c?A:B)[i]` — is tracked
// as an open bug: see the Unicorn unsupported-instructions doc #469 open-issues.)
//
// All integer, fold to one return, no float / 64-bit divide helper.
// All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress65RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress65RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress65RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress65RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress65RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress65RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress65RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress65RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress65TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // One rodata table, conditional runtime index T[c?i:j].
    {p+"_condidx",
     "static const unsigned short Q[16]={101,103,107,109,113,127,131,137,"
     "139,149,151,157,163,167,173,179};\n"
     +t+" "+p+"_condidx("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned i1=(s>>5)&15u, j1=(s>>9)&15u;\n"
     "    unsigned idx=((s>>3)&1u)? i1 : j1;\n"
     "    h=h*131u+Q[idx]+Q[(i1>j1)?i1:j1]; h^=h>>11; }\n"
     "  return ("+t+")h; }\n",
     {0xD2u}, "OptStress65", 2},

    // Selected stride/scale into one table T[i*(c?2:3)].
    {p+"_selstride",
     "static const unsigned char R[30]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,"
     "16,17,18,19,20,21,22,23,24,25,26,27,28,29,30};\n"
     +t+" "+p+"_selstride("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned stride=((s>>4)&1u)?2u:3u; unsigned base=(s>>6)%10u;\n"
     "    h=h*131u+R[base*stride]; h^=h>>9; }\n"
     "  return ("+t+")h; }\n",
     {0xD5u}, "OptStress65", 2},

    // Predicated accumulate driven by a table-valued condition.
    {p+"_predacc",
     "static const unsigned char M[16]={0,1,0,1,1,0,1,0,1,1,0,0,1,0,1,1};\n"
     "static const unsigned char V[16]={5,9,2,7,3,8,1,6,4,11,12,10,13,15,14,0};\n"
     +t+" "+p+"_predacc("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0; int acc=0;\n"
     "  for(int i=0;i<400;i++){ s=s*1103515245u+12345u; unsigned k=(s>>5)&15u;\n"
     "    if(M[k]) acc += V[k]; else acc -= V[k];\n"
     "    acc = (acc>200)? acc-200 : (acc<-200? acc+200 : acc);\n"
     "    h=h*131u+(unsigned)(acc+512); h^=h>>8; }\n"
     "  return ("+t+")h; }\n",
     {0xD6u}, "OptStress65", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress65TC("x64o65", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress65TC("x86o65", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress65TC("a64o65", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress65TC("armo65", "int");

INSTANTIATE_TEST_SUITE_P(OptStress65, X64OptStress65RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress65, X86OptStress65RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress65, A64OptStress65RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress65, ARM32OptStress65RT, ::testing::ValuesIn(kARM), rtTCName);
