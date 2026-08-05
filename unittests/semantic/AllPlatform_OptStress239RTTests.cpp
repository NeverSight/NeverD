//===- AllPlatform_OptStress239RTTests.cpp - vectorized byte manipulation =//
//
// Byte-granular array transforms that clang vectorizes into NEON byte ops
// (`vrev`, `vmax.u8`, `vabd`+`vaddl`, `vqmovn`, `vmull.u8`, `vzip`) and the
// x86 equivalents (`pshufb`/`bswap`, `pmaxub`, `psadbw`, `packuswb`,
// `pmullw`, `punpck`).  Byte-width vector lanes plus widen/narrow crossings
// are where lane-width tracking is most error-prone (cf. #505).
//
//   * byterev - reverse bytes within each u32 word.
//   * bmax    - per-element unsigned byte max of two arrays.
//   * sad     - sum of absolute byte differences.
//   * narrow  - saturating u16 -> u8 narrow, then sum.
//   * widen8  - u8 -> u16 widen, multiply by const, accumulate.
//   * zipsum  - interleave two byte arrays, then sum.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress239RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress239RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress239RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress239RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress239RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress239RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress239RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress239RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress239TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Reverse byte order within each u32 word.
    {p+"_byterev",
     t+" "+p+"_byterev("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned x[16]; for(int k=0;k<16;k++){ h=h*1664525u+1013904223u; x[k]=h; }\n"
     "    unsigned s=0; for(int k=0;k<16;k++) s+=__builtin_bswap32(x[k]);\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress239", 2},

    // Per-element unsigned byte max of two arrays.
    {p+"_bmax",
     t+" "+p+"_bmax("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned char u[32],v[32];\n"
     "    for(int k=0;k<32;k++){ h=h*1664525u+1013904223u; u[k]=(unsigned char)(h>>5); v[k]=(unsigned char)(h>>17); }\n"
     "    unsigned s=0; for(int k=0;k<32;k++){ unsigned char m=u[k]>v[k]?u[k]:v[k]; s+=m; }\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress239", 2},

    // Sum of absolute byte differences.
    {p+"_sad",
     t+" "+p+"_sad("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned char u[32],v[32];\n"
     "    for(int k=0;k<32;k++){ h=h*1664525u+1013904223u; u[k]=(unsigned char)(h>>5); v[k]=(unsigned char)(h>>17); }\n"
     "    unsigned s=0; for(int k=0;k<32;k++){ int d=(int)u[k]-(int)v[k]; s+=(unsigned)(d<0?-d:d); }\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress239", 2},

    // Saturating u16 -> u8 narrow, then sum.
    {p+"_narrow",
     t+" "+p+"_narrow("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned short x[32]; for(int k=0;k<32;k++){ h=h*1664525u+1013904223u; x[k]=(unsigned short)(h>>10); }\n"
     "    unsigned s=0; for(int k=0;k<32;k++){ unsigned v=x[k]; unsigned char b=v>255u?255u:(unsigned char)v; s+=b; }\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress239", 2},

    // u8 -> u16 widen, multiply by a constant, accumulate.
    {p+"_widen8",
     t+" "+p+"_widen8("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned char x[32]; for(int k=0;k<32;k++){ h=h*1664525u+1013904223u; x[k]=(unsigned char)(h>>6); }\n"
     "    unsigned s=0; for(int k=0;k<32;k++){ unsigned short w=(unsigned short)((unsigned short)x[k]*251u); s+=w; }\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress239", 2},

    // Interleave two byte arrays, then sum the interleaved result.
    {p+"_zipsum",
     t+" "+p+"_zipsum("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned char u[16],v[16],z[32];\n"
     "    for(int k=0;k<16;k++){ h=h*1664525u+1013904223u; u[k]=(unsigned char)(h>>5); v[k]=(unsigned char)(h>>13); }\n"
     "    for(int k=0;k<16;k++){ z[2*k]=u[k]; z[2*k+1]=v[k]; }\n"
     "    unsigned s=0; for(int k=0;k<32;k++) s+=(unsigned)z[k]*(unsigned)(k+1);\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress239", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress239TC("x64o239", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress239TC("x86o239", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress239TC("a64o239", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress239TC("armo239", "int");

INSTANTIATE_TEST_SUITE_P(OptStress239, X64OptStress239RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress239, X86OptStress239RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress239, A64OptStress239RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress239, ARM32OptStress239RT, ::testing::ValuesIn(kARM), rtTCName);
