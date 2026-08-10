//===- AllPlatform_OptStress89RTTests.cpp - optimizer-corner probes -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Five aggressive corners aimed squarely at the self-written MedIR optimizer /
// type-inference / memory-alias model rather than at global-pointer relocation
// (which #473-#488 already hammered):
//
//   * punchain - float<->int bit type-punning through a union in a loop: stresses
//                MedTypePass (a stack slot used as both float and int) plus the
//                store/load memory model (the punned bits must survive the FP op).
//   * aliasstk - two address-taken locals, a runtime-selected pointer stored
//                through, then read back: the optimizer must NOT assume the two
//                stack slots never alias.
//   * ovf32    - __builtin_add/mul_overflow chains whose overflow bit drives
//                control flow: native seto/jo-style flag production consumed
//                across a branch (MedFlags + DCE must keep the flag live).
//   * selmix   - deeply nested ternaries fed by DIFFERENT comparison ops: cmov /
//                csel flag folding where each select's predicate is a distinct
//                flag computation.
//   * reasm2   - byte/word extraction and reinsertion at high bit positions with
//                an 8x16 multiply: loop-carried sub-register aliasing (the
//                RAX/AL family) in a shape distinct from OptStress79 mixacc.
//
// All integer in / integer out, self-contained, no libcall (32-bit int<->float
// only, no 64-bit divide, no 128-bit mul).  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress89RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress89RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress89RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress89RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress89RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress89RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress89RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress89RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress89TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // float<->int bit punning through a union (bounded floats, no NaN/inf).
    {p+"_punchain",
     t+" "+p+"_punchain("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    union { float f; unsigned u; } v;\n"
     "    v.f=(float)(int)((acc^s)&0xffffu)*1.5f+2.0f;\n"
     "    acc=v.u+(acc>>3);\n"
     "    union { float f; unsigned u; } w;\n"
     "    w.f=(float)(int)((s>>9)&0x7fffu);\n"
     "    acc^=w.u+(s>>11); }\n"
     "  return ("+t+")acc; }\n",
     {0xE1u}, "OptStress89", 2},

    // Two address-taken locals, runtime-selected pointer stored through.
    {p+"_aliasstk",
     t+" "+p+"_aliasstk("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int acc=0;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    int x=(int)(s>>7), y=(int)(s<<3);\n"
     "    int *pp=(s&1u)?&x:&y; *pp+=acc;\n"
     "    int *qq=(s&2u)?&x:&y; acc+=*qq-(x^y);\n"
     "    acc^=acc>>6; }\n"
     "  return ("+t+")acc; }\n",
     {0xE2u}, "OptStress89", 2},

    // __builtin_add/mul_overflow chains driving control flow (32-bit, no libcall).
    {p+"_ovf32",
     t+" "+p+"_ovf32("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int acc=(int)a;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    int v=(int)(s>>5), r;\n"
     "    if(__builtin_add_overflow(acc,v,&r)) acc=(acc>>1)^v; else acc=r;\n"
     "    if(__builtin_mul_overflow(acc,3,&r)) acc^=0x5A5A5A5A; else acc=r;\n"
     "    acc-=(acc>>9); }\n"
     "  return ("+t+")acc; }\n",
     {0xE3u}, "OptStress89", 2},

    // Deeply nested ternaries fed by distinct comparison ops (cmov/csel chains).
    {p+"_selmix",
     t+" "+p+"_selmix("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int acc=(int)a;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    int x=(int)(s>>3), y=(int)(s>>11);\n"
     "    int m=(x<y)?(x>0?x:-x):(y&0x7fff);\n"
     "    int n=((x^y)<0)?(acc+m):(acc-m);\n"
     "    acc=(n>1000000)?(n>>2):((n<-1000000)?(n<<1):n);\n"
     "    acc^=acc>>5; }\n"
     "  return ("+t+")acc; }\n",
     {0xE4u}, "OptStress89", 2},

    // Byte/word extraction + high-position reinsertion with 8x16 multiply.
    {p+"_reasm2",
     t+" "+p+"_reasm2("+t+" a){\n"
     "  unsigned long long acc=(unsigned long long)a^0xCAFEBABEDEADBEEFULL;\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned char lo=(unsigned char)acc;\n"
     "    unsigned char hi=(unsigned char)(acc>>56);\n"
     "    unsigned short mid=(unsigned short)(acc>>24);\n"
     "    acc=(acc>>8)|((unsigned long long)lo<<56);\n"
     "    acc^=(unsigned long long)((unsigned)hi*(unsigned)mid)<<16;\n"
     "    acc+=lo; acc^=acc>>17; }\n"
     "  return ("+t+")(acc^(acc>>32)); }\n",
     {0xE5u}, "OptStress89", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress89TC("x64o89", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress89TC("x86o89", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress89TC("a64o89", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress89TC("armo89", "int");

INSTANTIATE_TEST_SUITE_P(OptStress89, X64OptStress89RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress89, X86OptStress89RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress89, A64OptStress89RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress89, ARM32OptStress89RT, ::testing::ValuesIn(kARM), rtTCName);
