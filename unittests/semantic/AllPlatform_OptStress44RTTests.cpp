//===- AllPlatform_OptStress44RTTests.cpp - vector + scalar tail -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for autovectorized integer reductions over prime-length
// arrays, which force clang -O2 to emit a SIMD main loop plus a scalar epilogue
// (and, for SAD/max, specific SSE/NEON ops: PSADBW, PMAXSD, PMOVSXWD + horizontal
// reduction).  The lift must reconstruct both the vector body and the scalar
// tail and merge them — a different surface than the per-lane VectorAlgo suite.
// Integer reductions only (add/xor/max are order-independent, so vectorized and
// scalar orderings agree); bounded, libcall-free, value-dependent hash.
//
//   * dotodd  - int32 dot product over length-19 arrays.
//   * ssum16  - int16 -> int32 widening sum over length-13 (PMOVSXWD / SADDLP).
//   * sad     - sum of absolute byte differences over length-17 (PSADBW / UABD).
//   * maxred  - signed max reduction over length-11 (PMAXSD + horizontal max).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress44RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress44RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress44RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress44RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress44RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress44RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress44RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress44RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress44TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // dotodd: int32 dot product over length-19 arrays (vector body + tail).
    {p+"_dotodd",
     t+" "+p+"_dotodd("+t+" a){\n"
     "  int x[19], y[19]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<19;i++){ x[i]=(int)(s>>3)-100; s=s*1103515245u+12345u;\n"
     "                         y[i]=(int)(s>>5)-100; s=s*1103515245u+12345u; }\n"
     "  long long acc=0;\n"
     "  for(int r=0;r<40;r++){ int dot=0; for(int i=0;i<19;i++) dot+=x[i]*y[i];\n"
     "    acc=acc*131+dot; x[r%19]+=r; }\n"
     "  return ("+t+")(unsigned)(acc ^ (acc>>32)); }\n",
     {0x81u}, "OptStress44", 2},

    // ssum16: int16 -> int32 widening sum over length-13 (PMOVSXWD / SADDLP).
    {p+"_ssum16",
     t+" "+p+"_ssum16("+t+" a){\n"
     "  short v[13]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<13;i++){ v[i]=(short)(s>>4); s=s*1103515245u+12345u; }\n"
     "  long long acc=0;\n"
     "  for(int r=0;r<60;r++){ int sum=0; for(int i=0;i<13;i++) sum+=(int)v[i];\n"
     "    acc=acc*131+sum; v[r%13]^=(short)r; }\n"
     "  return ("+t+")(unsigned)(acc ^ (acc>>32)); }\n",
     {0x82u}, "OptStress44", 2},

    // sad: sum of absolute byte differences over length-17 (PSADBW / UABD).
    {p+"_sad",
     t+" "+p+"_sad("+t+" a){\n"
     "  unsigned char x[17], y[17]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<17;i++){ x[i]=(unsigned char)(s>>3); s=s*1103515245u+12345u;\n"
     "                         y[i]=(unsigned char)(s>>7); s=s*1103515245u+12345u; }\n"
     "  unsigned acc=0;\n"
     "  for(int r=0;r<60;r++){ unsigned sad=0;\n"
     "    for(int i=0;i<17;i++){ int d=(int)x[i]-(int)y[i]; sad+=(unsigned)(d<0?-d:d); }\n"
     "    acc=acc*131u+sad; x[r%17]+=(unsigned char)r; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x83u}, "OptStress44", 2},

    // maxred: signed max reduction over length-11 (PMAXSD + horizontal max).
    {p+"_maxred",
     t+" "+p+"_maxred("+t+" a){\n"
     "  int v[11]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<11;i++){ v[i]=(int)(s>>2)-0x20000000; s=s*1103515245u+12345u; }\n"
     "  long long acc=0;\n"
     "  for(int r=0;r<60;r++){ int mx=-2000000000;\n"
     "    for(int i=0;i<11;i++) if(v[i]>mx) mx=v[i];\n"
     "    acc=acc*131+mx; v[r%11]-=r*7; }\n"
     "  return ("+t+")(unsigned)(acc ^ (acc>>32)); }\n",
     {0x84u}, "OptStress44", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress44TC("x64o44", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress44TC("x86o44", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress44TC("a64o44", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress44TC("armo44", "int");

INSTANTIATE_TEST_SUITE_P(OptStress44, X64OptStress44RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress44, X86OptStress44RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress44, A64OptStress44RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress44, ARM32OptStress44RT, ::testing::ValuesIn(kARM), rtTCName);
