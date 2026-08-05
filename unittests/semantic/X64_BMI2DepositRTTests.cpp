//===- X64_BMI2DepositRTTests.cpp - BMI2 PDEP/PEXT/BZHI/MULX -----*- C++ -*-=//
//
// Roundtrip coverage for the BMI2 parallel bit deposit/extract family, which the
// existing BMI2 suites (RORX/SARX/SHLX/SHRX flag shifts, MULX div edges) do not
// exercise: PDEP/PEXT scatter/gather bits under a runtime mask, BZHI zeroes the
// high bits from a runtime index.  NeverD lifts these to the
// `@llvm.x86.bmi.{pdep,pext}.*` intrinsics and codegen re-selects them via the
// auto-detected `+bmi2` feature; the Unicorn default Haswell model implements
// them, so a value-dependent loop hash compares native vs lifted exactly.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BMI2DepRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BMI2DepRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kBMI2Dep = {
  // PDEP/PEXT 64-bit under a runtime mask, chained through an LCG.
  {"pdep_pext_q",
   "long pdep_pext_q(long a){\n"
   "  unsigned long long x=(unsigned long long)a|1ull, acc=0;\n"
   "  for(int i=0;i<200;i++){\n"
   "    unsigned long long m = x ^ 0xAAAAAAAAAAAAAAAAull;\n"
   "    unsigned long long d = __builtin_ia32_pdep_di(x, m);\n"
   "    unsigned long long e = __builtin_ia32_pext_di(x, m);\n"
   "    acc = acc*131 + d + (e<<1);\n"
   "    x = x*6364136223846793005ull + 1442695040888963407ull; }\n"
   "  return (long)acc; }\n",
   {0xA1ull}, "BMI2Dep", 2, "-mbmi2"},

  // PDEP/PEXT 32-bit (the _si form), zero-extended into the 64-bit accumulator.
  {"pdep_pext_d",
   "long pdep_pext_d(long a){\n"
   "  unsigned x=(unsigned)a|1u; unsigned long long acc=0;\n"
   "  for(int i=0;i<200;i++){\n"
   "    unsigned m = x ^ 0xA5A5A5A5u;\n"
   "    unsigned d = __builtin_ia32_pdep_si(x, m);\n"
   "    unsigned e = __builtin_ia32_pext_si(x, m);\n"
   "    acc = acc*131 + d + ((unsigned long long)e<<1);\n"
   "    x = x*1103515245u + 12345u; }\n"
   "  return (long)acc; }\n",
   {0xA2ull}, "BMI2Dep", 2, "-mbmi2"},

  // BZHI with a runtime index (zero bits at and above position n).
  {"bzhi_q",
   "long bzhi_q(long a){\n"
   "  unsigned long long x=(unsigned long long)a|1ull, acc=0;\n"
   "  for(int i=0;i<200;i++){\n"
   "    unsigned long long b = __builtin_ia32_bzhi_di(x, (x>>3)&63ull);\n"
   "    acc = acc*131 + b;\n"
   "    x = x*6364136223846793005ull + 1442695040888963407ull; }\n"
   "  return (long)acc; }\n",
   {0xA3ull}, "BMI2Dep", 2, "-mbmi2"},

  // MULX-style full 64x64->128 multiply (high half) via __int128, plus PDEP.
  {"mulx_pdep",
   "long mulx_pdep(long a){\n"
   "  unsigned long long x=(unsigned long long)a|1ull, acc=0;\n"
   "  for(int i=0;i<200;i++){\n"
   "    unsigned __int128 p = (unsigned __int128)x * 0x9E3779B97F4A7C15ull;\n"
   "    unsigned long long hi = (unsigned long long)(p>>64);\n"
   "    unsigned long long d = __builtin_ia32_pdep_di(hi, x|1ull);\n"
   "    acc = acc*131 + hi + d;\n"
   "    x = x*6364136223846793005ull + 1442695040888963407ull; }\n"
   "  return (long)acc; }\n",
   {0xA4ull}, "BMI2Dep", 2, "-mbmi2"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(BMI2Dep, X64BMI2DepRT, ::testing::ValuesIn(kBMI2Dep), rtTCName);
