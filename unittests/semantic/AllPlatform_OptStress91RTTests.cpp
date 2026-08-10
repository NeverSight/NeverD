//===- AllPlatform_OptStress91RTTests.cpp - global pointer difference -C++-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// #473-#485 symbolized a global address when it is USED as a pointer (load /
// store address, call arg, return, stored-through).  A pointer *difference*
// `p - A` is the dual: the symbolized walking pointer and the array base must
// cancel so the recovered index is base-independent.  If the emitter
// symbolizes one side (e.g. the `lea`/`adrp` of the base to `ptrtoint(@A)`)
// but leaves the induction pointer at its original VA — or vice versa — the
// subtraction no longer cancels and the index is off by (newVA - origVA): the
// same mixed-addressing-model hazard the strroll specializations chase, here
// driven by an integer pointer-diff rather than a load.
//
//   * pdsearch - linear search; index of the hit recovered via `q - A`.
//   * pdwalk   - pointer walks a global with a data-dependent stride + wrap;
//                `p - A` index drives the accumulation (no strength-reduction
//                to a plain counter).
//   * pdtwo    - two pointers into the SAME global; their difference `q - p`
//                steers the access.
//   * pdstore  - search for a slot, then store back at the `q - A` index.
//
// Every difference stays within a single global (cross-global pointer
// differences are implementation-defined and would legitimately diverge).
// All integer, file-scope globals, trip counts defeat unrolling.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress91RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress91RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress91RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress91RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress91RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress91RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress91RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress91RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress91TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Linear search; the index of the first hit is recovered via `q - A`.  clang
    // keeps q a pointer through the search and emits a real `sub;sar` at the end.
    {p+"_pdsearch",
     "static int "+p+"_A[48];\n"
     +t+" "+p+"_pdsearch("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long acc=0;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u; "+p+"_A[i]=(int)(s>>8); }\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    int target=(int)((s>>6)&7u);\n"
     "    int *q="+p+"_A;\n"
     "    while(q<"+p+"_A+48 && ((*q)&7)!=target) q++;\n"
     "    long pos=(long)(q-"+p+"_A);\n"
     "    acc=acc*131+pos; acc^=acc>>5; }\n"
     "  return ("+t+")acc; }\n",
     {0xA1u}, "OptStress91", 2},

    // Pointer walks the global with a data-dependent stride + wrap; the index is
    // recovered each step via `p - A` (not a plain loop counter).
    {p+"_pdwalk",
     "static int "+p+"_A[32];\n"
     +t+" "+p+"_pdwalk("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long h=0;\n"
     "  for(int i=0;i<32;i++){ s=s*1103515245u+12345u; "+p+"_A[i]=(int)(s>>9); }\n"
     "  int *p="+p+"_A+((s>>4)&31u);\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    long idx=(long)(p-"+p+"_A);\n"
     "    h=h*131+(long)"+p+"_A[idx]+idx;\n"
     "    p+=1+(int)(s&3u); while(p>="+p+"_A+32) p-=32; }\n"
     "  return ("+t+")(h+"+p+"_A[0]+"+p+"_A[31]); }\n",
     {0xA2u}, "OptStress91", 2},

    // Two pointers into the same global; their signed difference steers access.
    {p+"_pdtwo",
     "static int "+p+"_A[64];\n"
     +t+" "+p+"_pdtwo("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long h=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u; "+p+"_A[i]=(int)(s>>10); }\n"
     "  int *p="+p+"_A+((s>>3)&31u), *q="+p+"_A+32+((s>>7)&31u);\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    long d=(long)(q-p); if(d<0) d=-d;\n"
     "    h=h*131+(long)"+p+"_A[d&63]+d;\n"
     "    p+=1+(int)(s&1u); if(p>="+p+"_A+32) p="+p+"_A;\n"
     "    q-=1+(int)((s>>1)&1u); if(q<"+p+"_A+32) q="+p+"_A+63; }\n"
     "  return ("+t+")(h+"+p+"_A[5]+"+p+"_A[60]); }\n",
     {0xA3u}, "OptStress91", 2},

    // Search for a slot, then store back at the recovered `q - A` index.
    {p+"_pdstore",
     "static int "+p+"_A[40];\n"
     +t+" "+p+"_pdstore("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long h=0;\n"
     "  for(int i=0;i<40;i++){ s=s*1103515245u+12345u; "+p+"_A[i]=(int)(s>>9); }\n"
     "  for(int i=0;i<220;i++){ s=s*1103515245u+12345u;\n"
     "    int target=(int)((s>>5)&3u);\n"
     "    int *q="+p+"_A;\n"
     "    while(q<"+p+"_A+40 && ((*q)&3)!=target) q++;\n"
     "    long idx=(long)(q-"+p+"_A);\n"
     "    if(idx<40){ "+p+"_A[idx]+=(int)(s>>13); h=h*131+"+p+"_A[idx]; }\n"
     "    else h=h*131+7; h^=h>>7; }\n"
     "  return ("+t+")(h+"+p+"_A[0]+"+p+"_A[39]); }\n",
     {0xA4u}, "OptStress91", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress91TC("x64o91", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress91TC("x86o91", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress91TC("a64o91", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress91TC("armo91", "int");

INSTANTIATE_TEST_SUITE_P(OptStress91, X64OptStress91RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress91, X86OptStress91RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress91, A64OptStress91RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress91, ARM32OptStress91RT, ::testing::ValuesIn(kARM), rtTCName);
