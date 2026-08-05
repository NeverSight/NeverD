//===- AllPlatform_RodataInductionRTTests.cpp - rodata walk pointers -*-C++-*-=//
//
// Regression probes for "constant-pool mapping" reached through a *rodata-
// walking induction pointer* rather than `base + index`.  Under register
// pressure (a const array of >=6 elements consumed by a long hash that also
// holds an integer-compare select live) clang lowers the array read as
// `p = &arr; ...; load *p; p += stride` instead of `load arr[i]`.  The base-
// plus-index table resolvers cannot redirect that induction pointer, so the
// recompiled object kept reading the original VA — unmapped / stale at run time:
//   * i386   : low-VA GOTOFF `.rodata` base materialized into the pointer
//   * ARM32  : literal-pool `ldr[pc] + pc` base materialized into the pointer
// Both now redirect the induction pointer onto the embedded rodata global.
// x86-64 / AArch64 reach the same array via rip/adrp `base + index` and already
// worked; they are included so the pattern is locked on every target.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64RodataIndRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64RodataIndRT, Verify) { roundTripX64(GetParam()); }

class X86RodataIndRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86RodataIndRT, Verify) { roundTripX86(GetParam()); }

class A64RodataIndRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64RodataIndRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32RodataIndRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32RodataIndRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeRodataIndTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Float const array consumed by a hash that also keeps an integer-compare
    // select live — the exact shape that pushed clang to the induction pointer.
    {p+"_fhash",
     t+" "+p+"_fhash("+t+" a){\n"
     "  unsigned s[6]={0x3F800000u,0x40000000u,0x40400000u,0x40800000u,0x40A00000u,0x40C00000u};\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<6;i++){ float x; __builtin_memcpy(&x,&s[i],4);\n"
     "    for(int j=0;j<6;j++){ float y; __builtin_memcpy(&y,&s[j],4);\n"
     "      unsigned ic=((int)(a+i)<(int)(a+j))?100u:200u;\n"
     "      unsigned c3=(x>y)?13u:17u; unsigned c4=(x!=y)?19u:23u;\n"
     "      acc=acc*3u+ic+c3+c4; } }\n"
     "  return ("+t+")(unsigned long)(acc+(unsigned)(a&0)); }\n",
     {0ULL}, "RodataInd", 2},

    // Integer const table walked the same way (no FP), still a long hash + an
    // index compare to force the induction pointer over a plain indexed load.
    {p+"_iwalk",
     t+" "+p+"_iwalk("+t+" a){\n"
     "  unsigned s[8]={2654435761u,40503u,2246822519u,3266489917u,\n"
     "                 668265263u,374761393u,3332679571u,2147483647u};\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<8;i++)\n"
     "    for(int j=0;j<8;j++){\n"
     "      unsigned v=s[j]+(unsigned)(i<j?7:11);\n"
     "      acc=acc*31u+v+(s[i]^v); }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x1234ULL}, "RodataInd", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeRodataIndTC("x64ri", "long");
static const std::vector<RoundTripTC> kX86 = makeRodataIndTC("x86ri", "int");
static const std::vector<RoundTripTC> kA64 = makeRodataIndTC("a64ri", "long");
static const std::vector<RoundTripTC> kARM = makeRodataIndTC("armri", "int");

INSTANTIATE_TEST_SUITE_P(RodataInd, X64RodataIndRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(RodataInd, X86RodataIndRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(RodataInd, A64RodataIndRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(RodataInd, ARM32RodataIndRT, ::testing::ValuesIn(kARM), rtTCName);
