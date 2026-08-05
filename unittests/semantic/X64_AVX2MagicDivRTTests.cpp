//===- X64_AVX2MagicDivRTTests.cpp - 256-bit magic-division frontier --*-C++*-=//
//
// x86_64-only probes for the AVX2-256 frontier carried forward from #432/#433:
// clang vectorizes `unsigned % constant` (and `/ constant`) into a magic-number
// sequence over YMM — VPSHUFD $0xf5 (odd-dword gather), VPBROADCASTD, two
// VPMULUDQ (u32xu32->u64 even-lane multiply), VPBLENDD recombine, VPSRLD,
// VPMULLD, VPSUBD, then a VEXTRACTI128 + horizontal-add reduction.  Forced via
// GCC vector extensions (vector_size(32)) at -O3 -mavx2 and folded to a single
// integer for bit-exact original-vs-lifted comparison.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64AVX2MagicDivRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64AVX2MagicDivRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeMagicDivTC() {
  std::string p = "x64mdiv";
  return {
    // i32 x8 unsigned modulo by a constant -> VPMULUDQ/VPBLENDD/VPMULLD chain.
    {p+"_modu1000",
     "long "+p+"_modu1000(long a){\n"
     "  typedef unsigned v8u __attribute__((vector_size(32)));\n"
     "  v8u x; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=s; }\n"
     "  v8u z=x%1000u;\n"
     "  unsigned r=0; for(int i=0;i<8;i++) r+=z[i];\n"
     "  return (long)r; }\n",
     {0xb7ULL}, "AVX2MagicDiv", 3, "-mavx2"},

    // i32 x8 unsigned divide by a constant -> magic multiply + shift only.
    {p+"_divu1000",
     "long "+p+"_divu1000(long a){\n"
     "  typedef unsigned v8u __attribute__((vector_size(32)));\n"
     "  v8u x; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=s; }\n"
     "  v8u z=x/1000u;\n"
     "  unsigned r=0; for(int i=0;i<8;i++) r+=z[i];\n"
     "  return (long)r; }\n",
     {0xc3ULL}, "AVX2MagicDiv", 3, "-mavx2"},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kMDIV = makeMagicDivTC();
INSTANTIATE_TEST_SUITE_P(AVX2MagicDiv, X64AVX2MagicDivRT,
                         ::testing::ValuesIn(kMDIV), rtTCName);
