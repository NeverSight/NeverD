//===- AllPlatform_OptStress199RTTests.cpp - rodata interior-pointer walks =//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Guardrails for the #490 "rodata interior-pointer addressing" fix: each probe
// materializes a FIXED interior rodata address (`&tab[k]`, k a compile-time
// constant) and walks it, so the address must stay anchored to the contiguous
// run global (i386 emitted a standalone per-address global before; ARM32's
// PC-relative literal-pool base was never embedded).  A value-driven switch /
// data dependency keeps the read from being constant-folded away, and every
// fold depends only on the rodata bytes + control flow (never an absolute VA),
// so the roundtrip comparison is meaningful on all four targets.
//
//   * revstr - backward walk of a rodata STRING from a fixed interior near-end
//              char, each char driving a small switch (string analog of #490's
//              byte-array revwalk; exercises the interior C-string pointer path).
//   * wcols  - backward walk of an int[] (4-byte elements) from a fixed interior
//              index, switch on each value (wider-element interior pointer).
//   * dual   - forward + backward pointers into the SAME rodata run advanced in
//              opposite directions and combined (two interior cursors at once).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress199RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress199RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress199RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress199RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress199RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress199RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress199RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress199RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress199TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Backward walk of a rodata STRING from a fixed interior near-end char.
    {p+"_revstr",
     "static const char "+p+"_sw[]=\"net_quick_brown_fox_42_jumps_over\";\n"
     +t+" "+p+"_revstr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    const char *p="+p+"_sw+30;\n"
     "    for(int k=0;k<28;k++){ unsigned c=(unsigned)(unsigned char)*p; p--;\n"
     "      switch(c&7u){\n"
     "        case 0: acc+=c*131u; break;\n"
     "        case 1: acc^=c<<3; break;\n"
     "        case 2: acc=(acc>>3)|(acc<<29); break;\n"
     "        case 3: acc-=c; break;\n"
     "        default: acc=acc*31u+c; break; }\n"
     "      acc^=acc>>11; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x33u}, "OptStress199", 2},

    // Backward walk of an int[] (4-byte elements) from a fixed interior index.
    {p+"_wcols",
     "static const int "+p+"_wt[20]={\n"
     "5,71,13,42,8,90,23,17,64,3,88,31,55,7,29,46,12,99,20,61};\n"
     +t+" "+p+"_wcols("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    const int *p="+p+"_wt+17;\n"
     "    for(int k=0;k<16;k++){ unsigned u=(unsigned)*p; p--;\n"
     "      switch(u&3u){\n"
     "        case 0: acc+=u*131u; break;\n"
     "        case 1: acc^=u<<5; break;\n"
     "        case 2: acc=(acc<<7)|(acc>>25); break;\n"
     "        default: acc-=u; break; }\n"
     "      out=out*31u+acc; } }\n"
     "  return ("+t+")out; }\n",
     {0x34u}, "OptStress199", 2},

    // Forward + backward cursors into the SAME rodata run, combined per step.
    {p+"_dual",
     "static const unsigned char "+p+"_dt[40]={\n"
     "2,9,4,7,1,8,3,6,0,5, 11,14,12,17,10,18,13,16,19,15,\n"
     "21,28,24,27,20,29,23,26,22,25, 31,34,32,37,30,38,33,36,39,35};\n"
     +t+" "+p+"_dual("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    const unsigned char *f="+p+"_dt;\n"
     "    const unsigned char *b="+p+"_dt+39;\n"
     "    for(int k=0;k<20;k++){ unsigned x=*f++, y=*b--;\n"
     "      acc=acc*131u+(x^(y<<3)); acc^=acc>>13; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x35u}, "OptStress199", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress199TC("x64o199", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress199TC("x86o199", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress199TC("a64o199", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress199TC("armo199", "int");

INSTANTIATE_TEST_SUITE_P(OptStress199, X64OptStress199RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress199, X86OptStress199RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress199, A64OptStress199RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress199, ARM32OptStress199RT, ::testing::ValuesIn(kARM), rtTCName);
