//===- AllPlatform_OptStress45RTTests.cpp - shift-count-zero edges -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for variable-count shifts whose runtime count sweeps through
// zero.  The existing OptStress36 `shld32` funnel kernel deliberately dodges a
// zero count (`if(n==0u) n=1u;`), so the count==0 path of the double-precision
// shifts (i386 lowers a 64-bit variable shift to SHLD/SHRD pairs) was never
// exercised.  x86 SHLD/SHRD with a post-mask count of 0 is a no-op on hardware,
// but a naive lift computes `src >> (width - count)` = `src >> width`, a
// shift-by-bitwidth that is poison in LLVM IR (the same UB class as the RCL/RCR
// bug).  These kernels make the count data-dependent (so clang emits a real
// variable shift rather than constant-folding) and add the loop index to a
// runtime seed so the count provably hits every residue 0..63 including 0.
//
//   * vshl64  - 64-bit variable left shift (i386 -> SHLD/SHRD; count hits 0).
//   * vshr64  - 64-bit variable logical right shift.
//   * vsar64  - 64-bit variable arithmetic right shift (signed).
//   * vrot64  - 64-bit rotate built from two shifts (funnel) over a swept count.
//
// 64-bit-only internal math (forces the wide shift even on 32-bit targets),
// bounded loop, libcall-free at -O2, value-dependent hash.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress45RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress45RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress45RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress45RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress45RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress45RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress45RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress45RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress45TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // vshl64: 64-bit variable left shift, count = (seed+i)&63 sweeps 0..63.
    {p+"_vshl64",
     t+" "+p+"_vshl64("+t+" a){\n"
     "  unsigned long long v=(unsigned long long)a|0x9E3779B97F4A7C15ULL, acc=0;\n"
     "  unsigned seed=(unsigned)a;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned n=(seed+(unsigned)i)&63u;\n"
     "    unsigned long long s=v<<n;\n"
     "    acc=acc*131+s+(s>>17);\n"
     "    v+=0x123456789ULL; }\n"
     "  return ("+t+")(unsigned)(acc^(acc>>32)); }\n",
     {0x40u}, "OptStress45", 2},

    // vshr64: 64-bit variable logical right shift over the swept count.
    {p+"_vshr64",
     t+" "+p+"_vshr64("+t+" a){\n"
     "  unsigned long long v=(unsigned long long)a|0xC2B2AE3D27D4EB4FULL, acc=0;\n"
     "  unsigned seed=(unsigned)a;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned n=(seed+(unsigned)i)&63u;\n"
     "    unsigned long long s=v>>n;\n"
     "    acc=acc*131+s+(s<<13);\n"
     "    v+=0x9E3779B1ULL; }\n"
     "  return ("+t+")(unsigned)(acc^(acc>>32)); }\n",
     {0x41u}, "OptStress45", 2},

    // vsar64: signed 64-bit variable arithmetic right shift over the swept count.
    {p+"_vsar64",
     t+" "+p+"_vsar64("+t+" a){\n"
     "  long long v=(long long)((unsigned long long)a|0x8000000000000001ULL);\n"
     "  unsigned long long acc=0; unsigned seed=(unsigned)a;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned n=(seed+(unsigned)i)&63u;\n"
     "    long long s=v>>n;\n"
     "    acc=acc*131+(unsigned long long)s;\n"
     "    v=v*6364136223846793005LL+1442695040888963407LL; }\n"
     "  return ("+t+")(unsigned)(acc^(acc>>32)); }\n",
     {0x42u}, "OptStress45", 2},

    // vrot64: 64-bit funnel (left shift OR'd with right shift) over a swept count
    // built so the C is well-defined at n==0 (both partials reduce to v / 0).
    {p+"_vrot64",
     t+" "+p+"_vrot64("+t+" a){\n"
     "  unsigned long long v=(unsigned long long)a|0xD1B54A32D192ED03ULL, acc=0;\n"
     "  unsigned seed=(unsigned)a;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned n=(seed+(unsigned)i)&63u;\n"
     "    unsigned long long hi=v<<n;\n"
     "    unsigned long long lo=(n==0)?0ull:(v>>(64u-n));\n"
     "    unsigned long long s=hi|lo;\n"
     "    acc=acc*131+s;\n"
     "    v+=0xABCDEF0123ULL; }\n"
     "  return ("+t+")(unsigned)(acc^(acc>>32)); }\n",
     {0x43u}, "OptStress45", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress45TC("x64o45", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress45TC("x86o45", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress45TC("a64o45", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress45TC("armo45", "int");

INSTANTIATE_TEST_SUITE_P(OptStress45, X64OptStress45RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress45, X86OptStress45RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress45, A64OptStress45RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress45, ARM32OptStress45RT, ::testing::ValuesIn(kARM), rtTCName);
