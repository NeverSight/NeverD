//===- AllPlatform_OptStress200RTTests.cpp - switch / interior-walk edges ==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// More jump-table-recovery and rodata interior-pointer guardrails, in the
// shapes that surfaced #491's i386 bugs (peeled switch with a spilled jump
// target, interior string suffix copy).  Each fold depends only on the rodata
// bytes + control flow (never an absolute VA), so the roundtrip comparison is
// meaningful on all four targets.
//
//   * sparsesw - a switch whose case labels are non-contiguous (3,17,42,88,130);
//                clang lowers it as a sparse jump table or an if-chain, a recovery
//                path distinct from the dense masked-index switches in #491.
//   * midfwd   - FORWARD walk of an int[] from a fixed interior index (the
//                forward dual of #491's backward revstr/wcols): the interior
//                pointer must still anchor to the contiguous run global.
//   * twosw    - two switches in one loop body over the same loaded byte (`u&3`
//                then `(u>>2)&3`): two jump tables resolved per function with
//                independent selectors.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress200RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress200RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress200RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress200RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress200RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress200RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress200RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress200RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress200TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Sparse (non-contiguous) switch labels over rodata-driven values.
    {p+"_sparsesw",
     "static const unsigned char "+p+"_sp[24]={\n"
     "3,17,42,88,130,3,9,17,42,200,88,5,130,17,3,42,77,88,3,17,130,42,11,88};\n"
     +t+" "+p+"_sparsesw("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int k=0;k<24;k++){ unsigned v=(unsigned)"+p+"_sp[k];\n"
     "      switch(v){\n"
     "        case 3: acc+=v*131u; break;\n"
     "        case 17: acc^=v<<4; break;\n"
     "        case 42: acc=(acc>>5)|(acc<<27); break;\n"
     "        case 88: acc-=v*7u; break;\n"
     "        case 130: acc*=(v|1u); break;\n"
     "        default: acc^=v+1u; break; }\n"
     "      acc^=acc>>9; } out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x41u}, "OptStress200", 2},

    // FORWARD walk of an int[] from a fixed interior index, switch on value.
    {p+"_midfwd",
     "static const int "+p+"_mf[20]={\n"
     "9,2,7,4,1,8,3,6,0,5,11,14,12,17,10,18,13,16,19,15};\n"
     +t+" "+p+"_midfwd("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    const int *p="+p+"_mf+4;\n"
     "    for(int k=0;k<14;k++){ unsigned u=(unsigned)*p; p++;\n"
     "      switch(u&3u){\n"
     "        case 0: acc+=u*131u; break;\n"
     "        case 1: acc^=u<<5; break;\n"
     "        case 2: acc=(acc<<7)|(acc>>25); break;\n"
     "        default: acc-=u; break; }\n"
     "      out=out*31u+acc; } }\n"
     "  return ("+t+")out; }\n",
     {0x42u}, "OptStress200", 2},

    // Two switches in one loop body over the same loaded byte.
    {p+"_twosw",
     "static const unsigned char "+p+"_tw[28]={\n"
     "2,9,4,7,1,8,3,6,0,5,11,14,12,17,10,18,13,16,19,15,21,28,24,27,20,29,23,26};\n"
     +t+" "+p+"_twosw("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int k=0;k<28;k++){ unsigned u=(unsigned)"+p+"_tw[k];\n"
     "      switch(u&3u){\n"
     "        case 0: acc+=u*131u; break;\n"
     "        case 1: acc^=u<<3; break;\n"
     "        case 2: acc-=u; break;\n"
     "        default: acc=(acc>>2)|(acc<<30); break; }\n"
     "      switch((u>>2)&3u){\n"
     "        case 0: acc^=u*7u; break;\n"
     "        case 1: acc+=u<<2; break;\n"
     "        case 2: acc=(acc<<5)|(acc>>27); break;\n"
     "        default: acc-=u*3u; break; }\n"
     "      out=out*31u+acc; } }\n"
     "  return ("+t+")out; }\n",
     {0x43u}, "OptStress200", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress200TC("x64o200", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress200TC("x86o200", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress200TC("a64o200", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress200TC("armo200", "int");

INSTANTIATE_TEST_SUITE_P(OptStress200, X64OptStress200RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress200, X86OptStress200RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress200, A64OptStress200RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress200, ARM32OptStress200RT, ::testing::ValuesIn(kARM), rtTCName);
