//===- AllPlatform_OptStress238RTTests.cpp - vectorized reductions =======//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Array reductions that clang turns into vector horizontal reductions
// (NEON `addv`/`vpadd`/`smax`/`umaxv`, x86 `paddd`+shuffle folds, `pmaxsd`,
// `vcnt`+`vpaddl` popcount).  Reduction lift mixes per-lane vector arithmetic
// with a final cross-lane fold — a different failure surface than #505's
// element-wise vector immediates.
//
//   * sumarr  - sum of a u32 array.
//   * mmarr   - signed max AND unsigned min in one pass.
//   * logred  - AND-reduction and OR-reduction combined.
//   * xorred  - XOR-reduction.
//   * dotp    - 16-bit dot product accumulated into 32 bits.
//   * popcnt  - total popcount over a byte array.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress238RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress238RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress238RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress238RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress238RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress238RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress238RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress238RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress238TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Sum reduction of a u32 array.
    {p+"_sumarr",
     t+" "+p+"_sumarr("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned x[32]; for(int k=0;k<32;k++){ h=h*1664525u+1013904223u; x[k]=h>>3; }\n"
     "    unsigned s=0; for(int k=0;k<32;k++) s+=x[k];\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress238", 2},

    // Signed max AND unsigned min in one pass.
    {p+"_mmarr",
     t+" "+p+"_mmarr("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    int x[32]; for(int k=0;k<32;k++){ h=h*1664525u+1013904223u; x[k]=(int)h; }\n"
     "    int mx=x[0]; unsigned mn=(unsigned)x[0];\n"
     "    for(int k=1;k<32;k++){ if(x[k]>mx)mx=x[k]; if((unsigned)x[k]<mn)mn=(unsigned)x[k]; }\n"
     "    acc=acc*131u+(unsigned)mx+mn+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress238", 2},

    // AND-reduction and OR-reduction combined.
    {p+"_logred",
     t+" "+p+"_logred("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned x[32]; for(int k=0;k<32;k++){ h=h*1664525u+1013904223u; x[k]=h; }\n"
     "    unsigned za=~0u, zo=0; for(int k=0;k<32;k++){ za&=x[k]; zo|=x[k]; }\n"
     "    acc=acc*131u+(za^zo)+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress238", 2},

    // XOR-reduction.
    {p+"_xorred",
     t+" "+p+"_xorred("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned x[32]; for(int k=0;k<32;k++){ h=h*1664525u+1013904223u; x[k]=h; }\n"
     "    unsigned z=0; for(int k=0;k<32;k++) z^=x[k]*2654435761u;\n"
     "    acc=acc*131u+z+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress238", 2},

    // 16-bit dot product accumulated into 32 bits.
    {p+"_dotp",
     t+" "+p+"_dotp("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    short u[32],v[32];\n"
     "    for(int k=0;k<32;k++){ h=h*1664525u+1013904223u; u[k]=(short)(h>>11); v[k]=(short)(h>>19); }\n"
     "    int s=0; for(int k=0;k<32;k++) s+=(int)u[k]*(int)v[k];\n"
     "    acc=acc*131u+(unsigned)s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress238", 2},

    // Total popcount over a byte array.
    {p+"_popcnt",
     t+" "+p+"_popcnt("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned char x[48]; for(int k=0;k<48;k++){ h=h*1664525u+1013904223u; x[k]=(unsigned char)(h>>9); }\n"
     "    unsigned s=0; for(int k=0;k<48;k++) s+=(unsigned)__builtin_popcount(x[k]);\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress238", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress238TC("x64o238", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress238TC("x86o238", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress238TC("a64o238", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress238TC("armo238", "int");

INSTANTIATE_TEST_SUITE_P(OptStress238, X64OptStress238RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress238, X86OptStress238RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress238, A64OptStress238RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress238, ARM32OptStress238RT, ::testing::ValuesIn(kARM), rtTCName);
