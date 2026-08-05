//===- AllPlatform_OptStress73RTTests.cpp - mutable global data -*-C++*-=//
//
// Every prior kernel keeps its state in locals / stack arrays or reads from
// `static const` rodata; none mutate a file-scope global, so the writable-data
// (.data / .bss) lift+symbolization path was never exercised.  Real binaries
// are full of mutable globals, so these probes drive them:
//
//   * gcnt   - a scalar `static` counter accumulated across the loop (a direct
//              load/store of one .data global).
//   * garr   - a `static` array READ and WRITTEN at a runtime index in the same
//              loop (indexed load + indexed store into a mutable .data array —
//              the store/load must alias the same recompiled global).
//   * gbss   - a zero-initialized `static` histogram (lands in .bss): runtime-
//              indexed increment then a full sweep (zero-init + indexed rmw).
//   * gmix   - a .data scalar and a .bss array mutated together, with the array
//              also read by a direct (constant-index) load that clang vectorizes
//              to a wide load — the direct and indexed accesses must resolve to
//              ONE cohesive global, not separate overlapping copies.
//
// All integer, arrays seeded from the LCG, fold global state into one integer
// return; no float / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress73RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress73RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress73RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress73RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress73RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress73RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress73RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress73RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress73TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Scalar .data global accumulated across the loop (direct load/store).
    {p+"_gcnt",
     "static unsigned "+p+"_gc=2166136261u;\n"
     +t+" "+p+"_gcnt("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    "+p+"_gc=("+p+"_gc^(s>>7))*16777619u; "+p+"_gc^="+p+"_gc>>13; }\n"
     "  return ("+t+")"+p+"_gc; }\n",
     {0x71u}, "OptStress73", 2},

    // .data array read AND written at a runtime index in one loop.
    {p+"_garr",
     "static int "+p+"_ga[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     +t+" "+p+"_garr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>5)&15u; "+p+"_ga[j]+=(int)(s>>11)+i;\n"
     "    sum+=" +p+"_ga[(s>>9)&15u]; sum^=sum>>7; }\n"
     "  return ("+t+")(sum+"+p+"_ga[0]+"+p+"_ga[15]); }\n",
     {0x72u}, "OptStress73", 2},

    // Zero-init .bss histogram: indexed increment then a full sweep.
    {p+"_gbss",
     "static int "+p+"_gh[32];\n"
     +t+" "+p+"_gbss("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    "+p+"_gh[(s>>6)&31u]+=(int)((s>>3)&7u)+1; }\n"
     "  int r=0; for(int k=0;k<32;k++) r=r*131+"+p+"_gh[k];\n"
     "  return ("+t+")r; }\n",
     {0x73u}, "OptStress73", 2},

    // .data scalar + .bss array mutated together; array also read by a direct
    // (constant-index) load clang may widen — direct & indexed must alias.
    {p+"_gmix",
     "static unsigned "+p+"_gm=12345u;\n"
     "static int "+p+"_gt[8];\n"
     +t+" "+p+"_gmix("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<120;i++){ s=s*1103515245u+12345u;\n"
     "    "+p+"_gt[(s>>5)&7u]+=(int)(s>>13);\n"
     "    "+p+"_gm=("+p+"_gm+(unsigned)"+p+"_gt[(s>>9)&7u])*2654435761u; }\n"
     "  int r=(int)"+p+"_gm;\n"
     "  for(int k=0;k<8;k++) r=r*131+"+p+"_gt[k];\n"
     "  return ("+t+")r; }\n",
     {0x74u}, "OptStress73", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress73TC("x64o73", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress73TC("x86o73", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress73TC("a64o73", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress73TC("armo73", "int");

INSTANTIATE_TEST_SUITE_P(OptStress73, X64OptStress73RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress73, X86OptStress73RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress73, A64OptStress73RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress73, ARM32OptStress73RT, ::testing::ValuesIn(kARM), rtTCName);
